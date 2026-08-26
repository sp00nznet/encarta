/*
 * ir32_run.c - load IR32.DLL into the arena and run the lifted code.
 *
 * Mentions no CPU type on purpose. pcrecomp's 16-bit and 32-bit runtimes each
 * define a struct called CPU and a function called push32, so anything that
 * builds a machine lives in ir32_reg16.c or ir32_reg32.c and is reached
 * through the plain functions declared below.
 *
 * Selectors are ours to choose, because we are the loader. The convention is
 * "selector == NE segment number", which keeps traces readable: a fault
 * through 0x0003 is segment 3.
 *
 *   ir32_run <IR32.DLL>            load and report
 *   ir32_run <IR32.DLL> init       run the decoder's initialisation
 *   ir32_run <IR32.DLL> sweep      call every lifted 32-bit entry
 *   ir32_run <IR32.DLL> driver     call DriverProc, the 16-bit entry point
 *   ir32_run <IR32.DLL> decode <frame> <w> <h> [out.ppm]
 *                                  decode one Indeo frame
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ne_mem.h"

/* implemented where the CPU types live */
void ir32_register16(void);
void ir32_register32(void);
int  ir32_init_test(uint16_t ds, uint16_t ss);
int  ir32_sweep32(uint16_t ds, uint16_t ss);
unsigned long ir32_call16(uint16_t seg, uint16_t off, uint16_t ds,
                          uint16_t ss, uint16_t sp);
uint32_t ir32_driver_call(uint32_t driver_id, uint16_t hdrv, uint16_t msg,
                          uint32_t lp1, uint32_t lp2,
                          uint16_t ds, uint16_t ss, uint16_t sp);
void ne_report_imports(void);
uint16_t ne_init(uint32_t stack_bytes);
extern unsigned long g_bridge_calls;
#ifdef IR32_WATCH
/* How many accesses fell in the watched window over the whole run. A
 * bitstream reader that stops after a handful of bytes and one that
 * consumes the frame look identical from the output alone. */
extern unsigned g_watch_hits;
#endif

#define MAX_SEG 64
static struct { uint32_t off, size; uint16_t flags; } g_seg[MAX_SEG];
static unsigned g_nseg;
static uint16_t g_autodata;

static uint16_t rd16f(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32f(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Apply SELECTOR fixups in the loaded copy.
 *
 * Code fixups are applied earlier, in lift_ir32.py, and have to be: lifting
 * turns an immediate into a C constant, so patching loaded bytes afterwards
 * changes nothing. Data fixups belong here, because data really is read from
 * memory at run time. Both are needed; neither substitutes for the other.
 *
 * NE chains the sites - the word at a fixup holds the offset of the next one
 * taking the same value, 0xFFFF ends it - which is also why an unpatched slot
 * reads as 0xFFFF rather than as anything meaningful. */
static unsigned apply_relocs(const unsigned char *img, unsigned count,
                             uint32_t segtab, unsigned shift)
{
    unsigned applied = 0;
    for (unsigned i = 0; i < count; i++) {
        const unsigned char *e = img + segtab + i * 8;
        uint32_t sector = rd16f(e), len = rd16f(e + 2), flags = rd16f(e + 4);
        if (!(flags & 0x0100) || !sector)
            continue;
        if (len == 0) len = 65536;
        const unsigned char *rel = img + ((uint32_t)sector << shift) + len;
        unsigned nrel = rd16f(rel);
        rel += 2;
        unsigned char *seg = g_arena + g_seg[i + 1].off;
        uint32_t seglen = g_seg[i + 1].size;
        for (unsigned r = 0; r < nrel; r++, rel += 8) {
            unsigned src = rel[0], typ = rel[1];
            uint32_t off = rd16f(rel + 2), tseg = rd16f(rel + 4);
            if (src != 2 || (typ & 3) != 0)
                continue;
            for (unsigned guard = 0; guard < 4096; guard++) {
                if (off + 2 > seglen) break;
                uint32_t next = (uint32_t)seg[off] | ((uint32_t)seg[off + 1] << 8);
                seg[off] = (unsigned char)(tseg & 0xFF);
                seg[off + 1] = (unsigned char)(tseg >> 8);
                applied++;
                if (next == 0xFFFF) break;
                off = next;
            }
        }
    }
    return applied;
}

/* Not static any more: ir32_vfw.c loads the same image to answer VFW.
 * g_ne_autodata goes with it - the DS every driver call needs. */
uint16_t g_ne_autodata;

int ir32_load_ne(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 0; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *img = (unsigned char *)malloc(n);
    if (!img || fread(img, 1, n, f) != (size_t)n) {
        fprintf(stderr, "cannot read %s\n", path); fclose(f); return 0;
    }
    fclose(f);

    uint32_t neoff = rd32f(img + 0x3C);
    if (neoff + 0x40 > (uint32_t)n || img[neoff] != 'N' || img[neoff + 1] != 'E') {
        fprintf(stderr, "not an NE file\n"); free(img); return 0;
    }
    unsigned count = rd16f(img + neoff + 0x1C);
    uint32_t segtab = neoff + rd16f(img + neoff + 0x22);
    unsigned shift = rd16f(img + neoff + 0x32);
    if (!shift) shift = 9;
    g_autodata = rd16f(img + neoff + 0x0E);
    g_ne_autodata = g_autodata;
    uint16_t heapsz = rd16f(img + neoff + 0x10);

    ne_mem_init();
    if (count >= MAX_SEG) count = MAX_SEG - 1;
    for (unsigned i = 0; i < count; i++) {
        const unsigned char *e = img + segtab + i * 8;
        uint32_t sector = rd16f(e), len = rd16f(e + 2), flags = rd16f(e + 4);
        uint32_t alloc = rd16f(e + 6);
        /* NE quirks: a stored length of 0 means 65536, and the allocation size
         * can exceed it, because a data segment's BSS tail is not in the file. */
        if (len == 0 && sector) len = 65536;
        if (alloc == 0) alloc = 65536;
        uint32_t size = alloc > len ? alloc : len;
        /* The automatic data segment carries a local heap past its data, which
         * the header asks for and the loader appends. LocalAlloc hands out near
         * offsets into it, so without the room those calls have nowhere to go. */
        if (i + 1 == g_autodata && heapsz) {
            /* ne_heap is the INITIAL local heap; Windows grows it on demand
             * within the 64K the segment can address, and IR32 asks for 1116
             * bytes in one call against a declared 1024. So give it the rest
             * of the segment, which is what a real DGROUP has available. */
            g_local_base = (uint16_t)size;
            g_local_next = g_local_base;
            g_local_end = 0xFFF0u;
            size = 0x10000u;
        }
        const void *src = (sector && len) ? img + ((uint32_t)sector << shift) : NULL;
        g_seg[i + 1].off = ne_alloc((uint16_t)(i + 1), src, len, size);
        g_seg[i + 1].size = size;
        g_seg[i + 1].flags = (uint16_t)flags;
    }
    g_nseg = count;
    printf("applied %u selector fixups in the loaded image\n",
           apply_relocs(img, count, segtab, shift));
    if (g_local_base)
        printf("local heap: %u bytes at DS:%04X\n",
               (unsigned)(g_local_end - g_local_base), g_local_base);
    free(img);
    return 1;
}


/* ---- ICM_DECOMPRESS ----------------------------------------------------
 *
 * The structures are the documented ones, laid out here by hand because a
 * 16-bit BITMAPINFOHEADER and ICDECOMPRESS are packed and use far pointers,
 * neither of which a host struct would reproduce. A far pointer is
 * selector:offset packed as a DWORD, and each object gets its own selector so
 * that a stray write lands somewhere identifiable rather than in a neighbour.
 */
enum {
    SEL_BIIN = 0x0300, SEL_BIOUT, SEL_IN, SEL_OUT, SEL_ICD, SEL_ICOPEN,
    /* GET_FORMAT fills in a header. Give it its own object: asked
     * against SEL_BIOUT it overwrites the format being requested, so
     * every run silently became whatever the codec preferred. */
    SEL_BIFMT
};

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;         p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static void put16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
}

/* BITMAPINFOHEADER: 40 bytes, packed. */
static void bih(unsigned char *p, int32_t w, int32_t h, uint16_t bits,
                const char *fourcc, uint32_t sizeimage)
{
    memset(p, 0, 40);
    put32(p + 0, 40);
    put32(p + 4, (uint32_t)w);
    put32(p + 8, (uint32_t)h);
    put16(p + 12, 1);                       /* biPlanes */
    put16(p + 14, bits);
    if (fourcc)
        memcpy(p + 16, fourcc, 4);          /* biCompression */
    put32(p + 20, sizeimage);
}

static uint32_t farptr(uint16_t sel) { return ((uint32_t)sel << 16); }

/* The decompress worker gates on two bytes of the driver's instance:
 *
 *     cmp byte ds:[bx+8], 0 / je no-decode
 *     cmp byte ds:[bx+6], 0 / je no-decode
 *
 * Both are zero when it runs, so something that should set them has not. The
 * instance is a LocalAlloc block in the automatic data segment, so watching
 * that region across the message sequence says which message is meant to. */
static void dump_instance(uint16_t ds, const char *when)
{
    if (!getenv("IR32_TRACE") || !g_local_base)
        return;
    const unsigned char *heap = g_arena + g_segoff[ds] + g_local_base;
    printf("   instance after %-22s:", when);
    for (int i = 0x168; i < 0x180; i++)
        printf(" %02X", heap[i]);
    printf("\n");

    /* The 32-bit core's plane table lives at the start of whichever selector
     * the thunk loads into DS, written by 3:0000 - `[0x0C]=0xE2C0`,
     * `[0x18]=0xE2EC`, stride 0x2C. If it holds pointers after BEGIN and
     * pixels after DECOMPRESS, the decoder is writing over its own table,
     * which is a very different bug from never having built one. */
    /* Two selectors that are supposed to alias must share an arena offset.
     * Printing them settles whether a difference in their contents means the
     * decoder wrote to one, or that they were never the same memory. */
    printf("   arena offsets: 0400=%06X 0401=%06X 0402=%06X 0403=%06X "
           "0404=%06X 0405=%06X 0406=%06X\n",
           g_segoff[0x400], g_segoff[0x401], g_segoff[0x402], g_segoff[0x403],
           g_segoff[0x404], g_segoff[0x405], g_segoff[0x406]);
    for (uint16_t sel = 0x0405; sel <= 0x0406; sel++) {
        if (!g_segoff[sel])
            continue;
        const unsigned char *q = g_arena + g_segoff[sel];
        printf("   sel %04X head:", sel);
        for (int i = 0; i < 0x1C; i += 4)
            printf(" %08X", (unsigned)(q[i] | (q[i+1] << 8) |
                                       (q[i+2] << 16) | ((unsigned)q[i+3] << 24)));
        printf("\n");
    }
}

static int decode_frame(const char *path, int w, int h, const char *out_ppm,
                        uint16_t ds, uint16_t ss, int outbits)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *frame = (unsigned char *)malloc(n);
    if (fread(frame, 1, n, f) != (size_t)n) { fclose(f); return 1; }
    fclose(f);

    /* The codec decodes into its own buffers and then converts to whatever
     * output format was asked for, so the format is not a formality - a depth
     * it does not support is a decode that runs and then declines to hand
     * anything back. Indeo 3 supports several, and which one this build wants
     * is a question for the driver rather than for a guess. */
    /* outbits 9 means YVU9, Indeo 3's own planar format, not nine bits in a
     * DIB. Asking for it skips the colour conversion entirely: the codec hands
     * back the planes it just decoded instead of packing them into RGB or into
     * palette indices. That matters for checking the decode, because ffmpeg
     * decodes the same frame to yuv410p - the same layout - so the two can be
     * compared directly, with no palette and no colour matrix in between.
     *
     * YVU9 is 4:1:0: a full luma plane and two chroma planes at a quarter
     * resolution in each direction, so nine bits per pixel on average. */
    int planar = (outbits == 9);
    uint32_t outsize = planar
        ? (uint32_t)w * h + 2 * (((uint32_t)w / 4) * ((uint32_t)h / 4))
        : (uint32_t)w * h * ((outbits + 7) / 8);
    /* A BITMAPINFOHEADER for a palettised format is followed by its colour
     * table - 256 RGBQUADs - and the codec both reads and fills it. 64 bytes
     * holds the header alone, so at 8bpp the palette would land outside the
     * object. Give both headers room for a full BITMAPINFO. */
    ne_alloc(SEL_BIIN,  NULL, 0, 40 + 256 * 4);
    ne_alloc(SEL_BIOUT, NULL, 0, 40 + 256 * 4);
    ne_alloc(SEL_BIFMT, NULL, 0, 40 + 256 * 4);
    /* IR32_BIAS: add a constant to the three plane offsets at +0x20 before
     * handing the frame over. The decoder reads at frame+offset and finds zero
     * bytes; the plane header signature 61 F7 08 actually sits at
     * frame+0x14+offset. If the decoder is simply missing that 0x14, biasing
     * the offsets makes it read the real data and the access count jumps from
     * 16 to thousands. If nothing changes, the base is not the problem. */
    const char *bias = getenv("IR32_BIAS");
    if (bias && n >= 0x2C) {
        uint32_t b = (uint32_t)strtoul(bias, NULL, 0);
        for (int i = 0; i < 3; i++) {
            uint32_t v;
            memcpy(&v, frame + 0x20 + i * 4, 4);
            v += b;
            memcpy(frame + 0x20 + i * 4, &v, 4);
        }
        fprintf(stderr, "plane offsets biased by +0x%X\n", b);
    }
    ne_alloc(SEL_IN,    frame, (uint32_t)n, (uint32_t)n + 64);
    /* Generous slack, not a tight fit. The codec's row pitch is its own
     * decision, and if it exceeds the DIB width the tail of the picture lands
     * past a tightly-sized buffer - which reads as "the decoder stopped early"
     * rather than as "the buffer was too small". 64K covers any plausible
     * pitch for this frame size, and the dump shows how much was really used. */
    /* Sized for the widest format the codec can choose, not for the depth
     * asked for. Its preferred output is 24bpp - 124,416 bytes for this
     * frame - and an 8bpp-sized buffer is smaller than that, so the
     * decode wrote past the end and faulted. */
    ne_alloc(SEL_OUT,   NULL, 0, (uint32_t)w * h * 4 + 65536);
    ne_alloc(SEL_ICD,   NULL, 0, 64);
    free(frame);

    bih(g_arena + g_segoff[SEL_BIIN],  w, h, 24, "IV32", (uint32_t)n);
    /* IR32_OUTFOURCC names the output format directly, so which formats this
     * build accepts is a question for the driver rather than for a guess.
     * ICM_DECOMPRESS_QUERY answers it: ICERR_BADFORMAT (-2) means no. */
    const char *fcc = getenv("IR32_OUTFOURCC");
    if (fcc && strlen(fcc) != 4)
        fcc = NULL;
    /* IR32_OUTW declares a different width for the OUTPUT header only, leaving
     * the input header saying what the frame really is. The codec writes
     * 256-byte rows for a 216-wide DIB; this says whether that pitch follows
     * the width it is given or is a constant of the codec's own. */
    const char *ow = getenv("IR32_OUTW");
    int32_t outw = ow ? atoi(ow) : w;
    bih(g_arena + g_segoff[SEL_BIOUT], outw, h, (uint16_t)outbits,
        fcc ? fcc : (planar ? "YVU9" : NULL), outsize);
    if (outbits <= 8) {
        /* biClrUsed: how many entries the codec should fill in. Left zero it
         * means "all of them" for some callers and "none" for others, and the
         * difference is a palette that never gets written. */
        put32(g_arena + g_segoff[SEL_BIOUT] + 32, 1u << outbits);
    }

    /* ICDECOMPRESS, 24 bytes: flags, lpbiInput, lpInput, lpbiOutput,
     * lpOutput, ckid. */
    unsigned char *icd = g_arena + g_segoff[SEL_ICD];
    memset(icd, 0, 24);
    put32(icd + 0, 0);                       /* dwFlags */
    put32(icd + 4, farptr(SEL_BIIN));
    put32(icd + 8, farptr(SEL_IN));
    put32(icd + 12, farptr(SEL_BIOUT));
    put32(icd + 16, farptr(SEL_OUT));

    /* DRV_OPEN on an installable driver takes no argument, but a codec is
     * opened through ICM and gets an ICOPEN in lParam2 saying what it is being
     * opened FOR. Without it the driver has no reason to believe it is being
     * asked to decompress, which is the obvious first suspect when the messages
     * after it answer ICERR_BADFORMAT.
     *
     *   0  dwSize    4  fccType 'vidc'   8  fccHandler 'IV32'
     *  12  dwVersion 16  dwFlags        20  dwError
     *  24  pV1Reserved  28  pV2Reserved  32  dnDevNode      = 36 bytes
     */
    ne_alloc(SEL_ICOPEN, NULL, 0, 64);
    unsigned char *ico = g_arena + g_segoff[SEL_ICOPEN];
    memset(ico, 0, 36);
    put32(ico + 0, 36);
    memcpy(ico + 4, "vidc", 4);
    memcpy(ico + 8, "IV32", 4);
    put32(ico + 16, 4);          /* ICMODE_DECOMPRESS */

    uint32_t id = 0;
    struct { uint16_t msg; const char *name; uint32_t p1, p2; } pre[] = {
        { 0x0001, "DRV_LOAD",   0, 0 },
        { 0x0002, "DRV_ENABLE", 0, 0 },
        { 0x0003, "DRV_OPEN",   0, farptr(SEL_ICOPEN) },
    };
    for (unsigned i = 0; i < sizeof pre / sizeof pre[0]; i++) {
        uint32_t r = ir32_driver_call(id, 1, pre[i].msg, pre[i].p1, pre[i].p2,
                                      ds, ss, 0xFF00);
        printf("   %-22s -> %08X\n", pre[i].name, r);
        if (pre[i].msg == 0x0003) id = r;
        dump_instance(ds, pre[i].name);
    }

    /* ICM_DECOMPRESS_QUERY and _BEGIN take the two headers; ICM_DECOMPRESS
     * takes the ICDECOMPRESS and its size. */
    struct { uint16_t msg; const char *name; uint32_t p1, p2; } seq[] = {
        /* Ask the codec what output format it wants, rather than inventing
         * one. Its converters step rows by a hardcoded 0x100 bytes and
         * advance bands by 0x400 - width, i.e. four rows of 256, so they
         * only write a correct DIB when its pitch is 256. That is not a
         * number to guess at: ICM_DECOMPRESS_GET_FORMAT (DRV_USER+10) has
         * the codec fill in the header it actually wants. */
        { 0x400A, "ICM_DECOMPRESS_GET_FORMAT",
                                          farptr(SEL_BIIN), farptr(SEL_BIFMT) },
        { 0x400B, "ICM_DECOMPRESS_QUERY", farptr(SEL_BIIN), farptr(SEL_BIOUT) },
        { 0x400C, "ICM_DECOMPRESS_BEGIN", farptr(SEL_BIIN), farptr(SEL_BIOUT) },
        { 0x400D, "ICM_DECOMPRESS",       farptr(SEL_ICD),  24 },
        /* At 8bpp the decoded bytes are palette indices. Nothing supplies that
         * palette unless it is asked for - ICM_DECOMPRESS_GET_PALETTE,
         * DRV_USER+30 - and without it the output is indices into a table of
         * zeroes, which compares to a reference decoder's luma as pure noise
         * no matter how correct the decode is.
         *
         * After the decode, not before: asked between BEGIN and DECOMPRESS it
         * returns success and leaves the decoder unable to produce anything,
         * so the frame came out empty. The palette is a property of the
         * stream, so reading it afterwards costs nothing. */
        { 0x401E, "ICM_DECOMPRESS_GET_PALETTE",
                                          farptr(SEL_BIIN), farptr(SEL_BIOUT) },
    };
    /* Over the table, not a literal 3. It was 3 when the sequence was
     * QUERY/BEGIN/DECOMPRESS, and every message added after that was
     * silently never sent - which is why GET_PALETTE looked like a codec
     * that answers ICERR_OK and fills nothing. It was never asked. */
    for (unsigned i = 0; i < sizeof seq / sizeof seq[0]; i++) {
        uint32_t r = ir32_driver_call(id, 1, seq[i].msg, seq[i].p1, seq[i].p2,
                                      ds, ss, 0xFF00);
        printf("   %-22s -> %08X\n", seq[i].name, r);
        dump_instance(ds, seq[i].name);
        /* What the codec wants the output to be, after every message.
         * GET_FORMAT fills this in and BEGIN may adjust it, and the
         * number that matters is the row pitch: the converters advance
         * bands by 0x400 - width, four rows of 256, so they only write a
         * correct DIB when its pitch is 256 bytes. */
        {
            const unsigned char *bo = g_arena + g_segoff[
                seq[i].msg == 0x400A ? SEL_BIFMT : SEL_BIOUT];
            int32_t bw = (int32_t)rd32f(bo + 4), bh = (int32_t)rd32f(bo + 8);
            uint16_t bc = rd16f(bo + 14);
            printf("        %s: %dx%d %ubpp comp=%08X size=%u"
                   "  -> pitch %d\n",
                   seq[i].msg == 0x400A ? "wants" : "biOut",
                   bw, bh, bc, rd32f(bo + 16), rd32f(bo + 20),
                   ((bw * bc / 8) + 3) & ~3);
        }
    }

    /* Did anything land in the output buffer? A decoder that ran and wrote
     * nothing is the failure worth catching, and it looks identical to success
     * from the return code alone. */
    unsigned char *outp = g_arena + g_segoff[SEL_OUT];
    unsigned long nonzero = 0;
    for (uint32_t i = 0; i < outsize; i++)
        if (outp[i]) nonzero++;
    printf("output (%d bpp): %lu of %u bytes non-zero (%.1f%%)\n",
           outbits, nonzero, outsize, 100.0 * nonzero / outsize);
    /* How far past the DIB the codec actually wrote. The row pitch is the
     * codec's decision, and if it exceeds the DIB width the tail of the
     * picture lands beyond a tightly-sized buffer - which reads as "the
     * decoder stopped early" instead of "the buffer was too small". */
    {
        uint32_t last = 0;
        for (uint32_t i = 0; i < outsize + 65536; i++)
            if (outp[i]) last = i;
        if (last >= outsize)
            printf("   wrote %u bytes past the %u-byte DIB "
                   "(pitch is wider than the width)\n",
                   last + 1 - outsize, outsize);
        else
            printf("   highest byte touched: %u of %u\n", last, outsize);
    }
    /* Zero crossings means the driver never called its own decoder, which is a
     * different problem from a decoder that ran and wrote nothing - and an
     * empty output buffer looks identical either way. */
    printf("crossings into the 32-bit core: %lu\n", g_bridge_calls);
#ifdef IR32_WATCH
    /* How much of the watched region was actually touched. A bitstream reader
     * that stops after a handful of bytes and one that consumes the whole
     * frame produce output that looks the same. */
    printf("accesses in the watched window: %u\n", g_watch_hits);
#endif

    /* Did the decoder write anywhere at all?
     *
     * An empty output buffer has two very different explanations: nothing
     * decoded, or something decoded into the driver's own working buffers and
     * was never copied out. Those need opposite fixes, and the output buffer
     * alone cannot tell them apart. The driver's buffers are the ones it got
     * from GlobalAlloc, which this runtime hands out from 0x0400 up. */
    printf("driver buffers written:\n");
    unsigned any = 0;
    for (uint16_t sel = 0x0400; sel < 0x0420; sel++) {
        if (!g_segoff[sel])
            continue;
        const unsigned char *p = g_arena + g_segoff[sel];
        /* The real size, not 64K. These blocks are GlobalAlloc'd and most
         * are bigger: 0405 and 0406 are 137,024 bytes each, and the codec's
         * luma plane pointer is 0x18B34 - past the 64K that used to be
         * reported and dumped. Every earlier conclusion about "the plane
         * buffers" was drawn from the first half of them. */
        uint32_t sz = g_selsize[sel] ? g_selsize[sel] : 0x10000u;
        unsigned long nz = 0;
        for (uint32_t i = 0; i < sz; i++)
            if (p[i]) nz++;
        printf("   selector %04X: %lu of %u bytes non-zero\n", sel, nz, sz);
        any += (nz != 0);
    }
    if (!any)
        printf("   (none - nothing decoded anywhere, not just not copied out)\n");

    /* IR32_DUMP=<prefix> writes each of those buffers out.
     *
     * The point is to check the decode against a known-good decoder rather than
     * against itself. ffmpeg decodes the same frame to yuv410p, and if the
     * planes the lifted codec produced are in here, they will match it - which
     * is the difference between "it ran and wrote plausible bytes" and "it
     * decoded the frame". Layout inside the buffer is unknown, so dump whole
     * and let the comparison find the planes. */
    const char *dump = getenv("IR32_DUMP");
    if (dump) {
        char op[512];
        snprintf(op, sizeof op, "%s_out.bin", dump);
        FILE *oo = fopen(op, "wb");
        if (oo) {
            fwrite(g_arena + g_segoff[SEL_OUT], 1, outsize + 65536, oo);
            fclose(oo);
            printf("   dumped %s (%u bytes)\n", op, outsize);
        }
        for (uint16_t sel = 0x0400; sel < 0x0420; sel++) {
            if (!g_segoff[sel])
                continue;
            char path[512];
            snprintf(path, sizeof path, "%s_%04X.bin", dump, sel);
            FILE *o = fopen(path, "wb");
            if (o) {
                fwrite(g_arena + g_segoff[sel], 1,
                       g_selsize[sel] ? g_selsize[sel] : 0x10000u, o);
                fclose(o);
                printf("   dumped %s\n", path);
            }
        }
    }

    /* Write the decoded luma plane, which is the part that is known correct.
     *
     * The codec decodes into its own working buffers and only afterwards
     * converts to whatever DIB was asked for. The decode is byte-exact
     * against ffmpeg on every frame tested; the conversion is not, because
     * the six colour converters write at a hardcoded 256-byte row stride
     * against a base computed at the DIB pitch. So read the plane rather
     * than the DIB.
     *
     * It arrives in two vertical strips. The plane workspace is 176 bytes
     * wide - 0xB0, hardcoded in 65 places - so a 216-wide frame does not fit
     * in one pass: the left 168 columns land in the first plane buffer and
     * the rest in the second, eight bytes further in, both at a 176-byte
     * stride. The base is the codec's own [E1AC], read here rather than
     * assumed, so a different frame size still finds it.
     *
     * Doubling is the domain, not a correction: Indeo 3 works in six bits,
     * and ffmpeg scales by four on output where this plane holds twice. */
    if (out_ppm && strstr(out_ppm, ".pgm")) {
        enum { PLANE_STRIDE = 176, LEFT_COLS = 168, CTX_PLANE = 0xE1AC };
        uint16_t s_left = 0x0405, s_right = 0x0406;
        if (g_segoff[s_left] && g_segoff[s_right]) {
            const unsigned char *L = g_arena + g_segoff[s_left];
            const unsigned char *R = g_arena + g_segoff[s_right];
            uint32_t base = rd32f(L + CTX_PLANE);
            int lw = w < LEFT_COLS ? w : LEFT_COLS;
            int rw = w - lw;
            uint32_t need = base + (uint32_t)PLANE_STRIDE * (h - 1) + lw;
            if (base && need < g_selsize[s_left]) {
                FILE *o = fopen(out_ppm, "wb");
                if (o) {
                    fprintf(o, "P5\n%d %d\n255\n", w, h);
                    for (int y = 0; y < h; y++) {
                        for (int x = 0; x < lw; x++)
                            fputc(L[base + (size_t)y * PLANE_STRIDE + x] * 2, o);
                        for (int x = 0; x < rw; x++)
                            fputc(R[base + 8 + (size_t)y * PLANE_STRIDE + x] * 2, o);
                    }
                    fclose(o);
                    printf("wrote %s (%dx%d luma, from the plane at 0x%X)\n",
                           out_ppm, w, h, base);
                }
            } else {
                printf("   plane pointer 0x%X does not fit selector %04X\n",
                       base, s_left);
            }
        }
    }

    /* At 8bpp the output bytes are palette indices, not intensities, so
     * comparing them to a reference decoder's luma compares two different
     * things and reports noise however right the decode is. The codec fills
     * the output BITMAPINFO's colour table itself, so resolve through it.
     *
     * IR32_STRIDE overrides the row pitch. The buffer's own autocorrelation
     * says the codec writes 256-byte rows for a 216-wide DIB, and reading it
     * back at 216 shears the picture into something no comparison can match. */
    if (out_ppm && nonzero && outbits == 8 && !strstr(out_ppm, ".pgm")) {
        const unsigned char *pal = g_arena + g_segoff[SEL_BIOUT] + 40;
        const char *sv = getenv("IR32_STRIDE");
        int stride = sv ? atoi(sv) : w;
        FILE *o = fopen(out_ppm, "wb");
        if (o) {
            fprintf(o, "P6\n%d %d\n255\n", w, h);
            for (int y = h - 1; y >= 0; y--) {
                const unsigned char *row = outp + (size_t)y * stride;
                for (int x = 0; x < w; x++) {
                    const unsigned char *e = pal + row[x] * 4;
                    fputc(e[2], o); fputc(e[1], o); fputc(e[0], o);
                }
            }
            fclose(o);
            printf("wrote %s (8bpp through the codec's palette, stride %d)\n",
                   out_ppm, stride);
        }
        int palnz = 0;
        for (int i = 0; i < 256 * 4; i++)
            palnz += pal[i] != 0;
        printf("   output palette: %d of 1024 bytes non-zero\n", palnz);
    }

    if (out_ppm && nonzero && outbits == 24) {
        FILE *o = fopen(out_ppm, "wb");
        if (o) {
            fprintf(o, "P6\n%d %d\n255\n", w, h);
            /* DIB rows run bottom-up, and the byte order is BGR. */
            for (int y = h - 1; y >= 0; y--) {
                const unsigned char *row = outp + (size_t)y * w * 3;
                for (int x = 0; x < w; x++) {
                    fputc(row[x * 3 + 2], o);
                    fputc(row[x * 3 + 1], o);
                    fputc(row[x * 3 + 0], o);
                }
            }
            fclose(o);
            printf("wrote %s\n", out_ppm);
        }
    }
    return nonzero ? 0 : 1;
}

/* IR32_NO_MAIN: link this file for its NE loader without its command line.
 * The VFW test has a main of its own and needs ir32_load_ne from here. */
#ifndef IR32_NO_MAIN
int main(int argc, char **argv)
{
    /* Unbuffered, because the interesting runs are the ones that fault: a
     * buffered stdout loses everything printed before the crash, which is
     * exactly the part that says how far it got. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (argc < 2) {
        fprintf(stderr, "usage: %s <IR32.DLL> [init|sweep|driver]\n", argv[0]);
        return 2;
    }
    if (!ir32_load_ne(argv[1])) return 1;
    ir32_register16();
    ir32_register32();

    uint16_t ds = g_autodata ? g_autodata : 41;
    uint16_t ss = ne_init(256 * 1024);
    printf("loaded %u segments; auto-data is segment %u\n", g_nseg, ds);

    if (argc < 3) {
        printf("loaded; no command given\n");
        return 0;
    }
    if (!strcmp(argv[2], "init"))
        return ir32_init_test(ds, ss);
    if (!strcmp(argv[2], "sweep"))
        return ir32_sweep32(ds, ss);
    if (!strcmp(argv[2], "driver")) {
        /* The sequence Video for Windows actually performs on an installable
         * codec. Doing it in order matters: DRV_OPEN is what returns the driver
         * ID every later message is addressed to, and a decode cannot be asked
         * for before ICM_DECOMPRESS_BEGIN has set the format up.
         *
         * Values are from the installable-driver and ICM interfaces, not from
         * guesswork - and the DLL's own dispatch agrees, routing 0x4005..0x402A
         * through a 38-entry table, which is where the ICM_DECOMPRESS_* range
         * sits. */
        struct { uint16_t msg; const char *name; } seq[] = {
            { 0x0001, "DRV_LOAD" },
            { 0x0002, "DRV_ENABLE" },
            { 0x0003, "DRV_OPEN" },
        };
        uint32_t id = 0;
        for (unsigned i = 0; i < sizeof seq / sizeof seq[0]; i++) {
            uint32_t r = ir32_driver_call(id, 1, seq[i].msg, 0, 0, ds, ss, 0xFF00);
            printf("   %-22s -> %08X\n", seq[i].name, r);
            if (seq[i].msg == 0x0003 && r != 0xFFFFFFFFu)
                id = r;            /* DRV_OPEN returns the driver ID */
        }
        printf("driver id: %08X\n", id);
        ne_report_imports();
        return 0;
    }

    if (!strcmp(argv[2], "driver-old")) {
        /* DriverProc is segment 6 offset 0, ordinal 2 in the entry table.
         * Calling it with an empty frame is not a decode - it is the first
         * question the runtime can ask the 16-bit half at all, and what it
         * answers with is which imports a real driver call would need. */
        printf("calling DriverProc (6:0000) ...\n");
        unsigned long misses = ir32_call16(6, 0, ds, ss, 0xFF00);
        printf("far-call misses: %lu\n", misses);
        ne_report_imports();
        return 0;
    }
    if (!strcmp(argv[2], "decode")) {
        if (argc < 6) {
            fprintf(stderr, "usage: %s <DLL> decode <frame.bin> <w> <h> "
                            "[out.ppm] [outbits]\n", argv[0]);
            return 2;
        }
        return decode_frame(argv[3], atoi(argv[4]), atoi(argv[5]),
                            argc > 6 ? argv[6] : NULL, ds, ss,
                            argc > 7 ? atoi(argv[7]) : 24);
    }

    fprintf(stderr, "unknown command %s\n", argv[2]);
    return 2;
}
#endif  /* IR32_NO_MAIN */
