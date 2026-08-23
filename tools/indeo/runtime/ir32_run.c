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

static int load_ne(const char *path)
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
        const void *src = (sector && len) ? img + ((uint32_t)sector << shift) : NULL;
        g_seg[i + 1].off = ne_alloc((uint16_t)(i + 1), src, len, size);
        g_seg[i + 1].size = size;
        g_seg[i + 1].flags = (uint16_t)flags;
    }
    g_nseg = count;
    printf("applied %u selector fixups in the loaded image\n",
           apply_relocs(img, count, segtab, shift));
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
    SEL_BIIN = 0x0300, SEL_BIOUT, SEL_IN, SEL_OUT, SEL_ICD
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

static int decode_frame(const char *path, int w, int h, const char *out_ppm,
                        uint16_t ds, uint16_t ss)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *frame = (unsigned char *)malloc(n);
    if (fread(frame, 1, n, f) != (size_t)n) { fclose(f); return 1; }
    fclose(f);

    uint32_t outsize = (uint32_t)w * h * 3;
    ne_alloc(SEL_BIIN,  NULL, 0, 64);
    ne_alloc(SEL_BIOUT, NULL, 0, 64);
    ne_alloc(SEL_IN,    frame, (uint32_t)n, (uint32_t)n + 64);
    ne_alloc(SEL_OUT,   NULL, 0, outsize + 64);
    ne_alloc(SEL_ICD,   NULL, 0, 64);
    free(frame);

    bih(g_arena + g_segoff[SEL_BIIN],  w, h, 24, "IV32", (uint32_t)n);
    bih(g_arena + g_segoff[SEL_BIOUT], w, h, 24, NULL,   outsize);

    /* ICDECOMPRESS, 24 bytes: flags, lpbiInput, lpInput, lpbiOutput,
     * lpOutput, ckid. */
    unsigned char *icd = g_arena + g_segoff[SEL_ICD];
    memset(icd, 0, 24);
    put32(icd + 0, 0);                       /* dwFlags */
    put32(icd + 4, farptr(SEL_BIIN));
    put32(icd + 8, farptr(SEL_IN));
    put32(icd + 12, farptr(SEL_BIOUT));
    put32(icd + 16, farptr(SEL_OUT));

    uint32_t id = 0;
    struct { uint16_t msg; const char *name; uint32_t p1, p2; } pre[] = {
        { 0x0001, "DRV_LOAD",   0, 0 },
        { 0x0002, "DRV_ENABLE", 0, 0 },
        { 0x0003, "DRV_OPEN",   0, 0 },
    };
    for (unsigned i = 0; i < 3; i++) {
        uint32_t r = ir32_driver_call(id, 1, pre[i].msg, pre[i].p1, pre[i].p2,
                                      ds, ss, 0xFF00);
        printf("   %-22s -> %08X\n", pre[i].name, r);
        if (pre[i].msg == 0x0003) id = r;
    }

    /* ICM_DECOMPRESS_QUERY and _BEGIN take the two headers; ICM_DECOMPRESS
     * takes the ICDECOMPRESS and its size. */
    struct { uint16_t msg; const char *name; uint32_t p1, p2; } seq[] = {
        { 0x400B, "ICM_DECOMPRESS_QUERY", farptr(SEL_BIIN), farptr(SEL_BIOUT) },
        { 0x400C, "ICM_DECOMPRESS_BEGIN", farptr(SEL_BIIN), farptr(SEL_BIOUT) },
        { 0x400D, "ICM_DECOMPRESS",       farptr(SEL_ICD),  24 },
    };
    for (unsigned i = 0; i < 3; i++) {
        uint32_t r = ir32_driver_call(id, 1, seq[i].msg, seq[i].p1, seq[i].p2,
                                      ds, ss, 0xFF00);
        printf("   %-22s -> %08X\n", seq[i].name, r);
    }

    /* Did anything land in the output buffer? A decoder that ran and wrote
     * nothing is the failure worth catching, and it looks identical to success
     * from the return code alone. */
    unsigned char *outp = g_arena + g_segoff[SEL_OUT];
    unsigned long nonzero = 0;
    for (uint32_t i = 0; i < outsize; i++)
        if (outp[i]) nonzero++;
    printf("output: %lu of %u bytes non-zero (%.1f%%)\n",
           nonzero, outsize, 100.0 * nonzero / outsize);

    if (out_ppm && nonzero) {
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
    if (!load_ne(argv[1])) return 1;
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
            fprintf(stderr, "usage: %s <DLL> decode <frame.bin> <w> <h> [out.ppm]\n",
                    argv[0]);
            return 2;
        }
        return decode_frame(argv[3], atoi(argv[4]), atoi(argv[5]),
                            argc > 6 ? argv[6] : NULL, ds, ss);
    }

    fprintf(stderr, "unknown command %s\n", argv[2]);
    return 2;
}
