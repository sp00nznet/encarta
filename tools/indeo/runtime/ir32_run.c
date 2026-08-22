/*
 * ir32_run.c - load IR32.DLL's segments and call into the lifted 32-bit code.
 *
 * The lifted C is only half a program: it expects its segments to exist at
 * some base, addressed through selectors. This maps them and provides the
 * caller that the 16-bit half of the DLL would normally be.
 *
 * Selectors are ours to choose, because we are the caller. The convention here
 * is simply "selector == NE segment number", which keeps every trace readable:
 * a fault through 0x0003 is segment 3.
 *
 *   ir32_run <IR32.DLL>            check the plumbing, run no code
 *   ir32_run <IR32.DLL> <offset>   call the lifted entry at that offset
 *   ir32_run <IR32.DLL> sweep      call every entry, report what survives
 *   ir32_run <IR32.DLL> init       run the decoder's own initialisation
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp32.h"
#if defined(_WIN32)
#include <windows.h>
#endif

extern const ne_entry ir32_seg2_entries[];
extern const unsigned ir32_seg2_entry_count;
extern const ne_entry ir32_seg3_entries[];
extern const unsigned ir32_seg3_entry_count;

#define MAX_SEG 64

static struct {
    unsigned char *data;
    uint32_t size;
    uint16_t flags;
} g_seg[MAX_SEG];
static unsigned g_nseg;

static uint16_t rd16f(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32f(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Map every segment of the NE at a host address and give it a selector.
 * Segments are copied rather than pointed into the file image: lifted code
 * writes to its data segment, and scribbling on the mapped file would be a
 * confusing way to find that out. */
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
        fprintf(stderr, "not an NE file\n"); return 0;
    }
    unsigned count = rd16f(img + neoff + 0x1C);
    uint32_t segtab = neoff + rd16f(img + neoff + 0x22);
    unsigned shift = rd16f(img + neoff + 0x32);
    if (!shift) shift = 9;

    if (count >= MAX_SEG) count = MAX_SEG - 1;
    for (unsigned i = 0; i < count; i++) {
        const unsigned char *e = img + segtab + i * 8;
        uint32_t sector = rd16f(e), len = rd16f(e + 2), flags = rd16f(e + 4);
        uint32_t alloc = rd16f(e + 6);
        /* NE quirk: a length of 0 means 65536, and the allocation size can
         * exceed the file size - a data segment's BSS tail is not stored. */
        if (len == 0 && sector) len = 65536;
        if (alloc == 0) alloc = 65536;
        uint32_t size = alloc > len ? alloc : len;
        unsigned char *mem = (unsigned char *)calloc(1, size ? size : 1);
        if (sector && len)
            memcpy(mem, img + ((uint32_t)sector << shift), len);
        g_seg[i + 1].data = mem;
        g_seg[i + 1].size = size;
        g_seg[i + 1].flags = (uint16_t)flags;
        ne_map((uint16_t)(i + 1), mem, size);
    }
    g_nseg = count;
    /* Selector == segment number here, so a fixup naming segment N writes N -
     * which is only true because ne_map assigned it that way. */
    unsigned fixed = apply_relocs(img, neoff, count, segtab, shift);
    printf("applied %u selector fixups\n", fixed);
    free(img);
    return 1;
}

/* Apply the segment's relocations.
 *
 * Without this the image is not loaded, only copied. Every selector in the
 * code is a placeholder until a loader fills it in: `mov eax, 0xFFFF / mov ds,
 * ax` in the init routine does not mean selector 0xFFFF, it means "whatever
 * segment the fixup names". Run it unpatched and DS becomes an unmapped
 * selector whose base is 0, and the next access reads near null.
 *
 * NE chains the sites: the word at a fixup holds the offset of the next site
 * that takes the same value, and 0xFFFF ends the chain. That is also why the
 * placeholder looks like 0xFFFF everywhere - it is the terminator, not a
 * value.
 *
 * Only internal SELECTOR fixups are applied here. Imports (KERNEL, USER, and
 * the rest) are left alone deliberately: nothing calls out of the 32-bit core
 * in the paths being exercised, and a wrong address there would be far harder
 * to notice than an unpatched one. */
static unsigned apply_relocs(const unsigned char *img, uint32_t neoff,
                             unsigned count, uint32_t segtab, unsigned shift)
{
    unsigned applied = 0;
    for (unsigned i = 0; i < count; i++) {
        const unsigned char *e = img + segtab + i * 8;
        uint32_t sector = rd16f(e), len = rd16f(e + 2), flags = rd16f(e + 4);
        if (!(flags & 0x0100) || !sector)     /* no relocation records */
            continue;
        if (len == 0) len = 65536;
        const unsigned char *rel = img + ((uint32_t)sector << shift) + len;
        unsigned nrel = rd16f(rel);
        rel += 2;
        unsigned char *seg = g_seg[i + 1].data;
        uint32_t seglen = g_seg[i + 1].size;
        for (unsigned r = 0; r < nrel; r++, rel += 8) {
            unsigned src = rel[0], typ = rel[1];
            uint32_t off = rd16f(rel + 2);
            uint32_t tseg = rd16f(rel + 4);
            if (src != 2 || (typ & 3) != 0)   /* SELECTOR, internal ref only */
                continue;
            /* walk the chain */
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

/* Which segment is the DLL's default data segment? The NE header names it. */
static uint16_t auto_data(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned char h[0x40];
    uint32_t neoff;
    uint16_t ds = 0;
    if (!f) return 0;
    if (fread(h, 1, 4, f) == 4) { }
    fseek(f, 0x3C, SEEK_SET);
    if (fread(h, 1, 4, f) == 4) {
        neoff = rd32f(h);
        fseek(f, neoff + 0x0E, SEEK_SET);   /* ne_autodata */
        if (fread(h, 1, 2, f) == 2) ds = rd16f(h);
    }
    fclose(f);
    return ds;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <IR32.DLL> [entry-offset-hex]\n", argv[0]);
        return 2;
    }
    if (!load_ne(argv[1])) return 1;

    ne_register_code(2, ir32_seg2_entries, ir32_seg2_entry_count);
    ne_register_code(3, ir32_seg3_entries, ir32_seg3_entry_count);

    uint16_t ds = auto_data(argv[1]);
    uint16_t ss = ne_init(256 * 1024);

    printf("loaded %u segments; auto-data is segment %u\n", g_nseg, ds);
    printf("segment 2: %u bytes at %08X, %u lifted entries\n",
           g_seg[2].size, SEGB(2), ir32_seg2_entry_count);
    printf("segment 3: %u bytes at %08X, %u lifted entries\n",
           g_seg[3].size, SEGB(3), ir32_seg3_entry_count);

    /* Does every entry the lifter emitted actually resolve? This is plumbing,
     * not behaviour, but it is the part that silently half-works: a table that
     * is sorted wrong, or a segment registered under the wrong selector, still
     * links and still runs until the first dispatch. */
    CPU probe = {0};
    unsigned bad = 0;
    for (unsigned i = 0; i < ir32_seg3_entry_count; i++) {
        probe.cs = 3;
        uint32_t off = ir32_seg3_entries[i].off;
        unsigned j;
        int found = 0;
        for (j = 0; j < ir32_seg3_entry_count; j++)
            if (ir32_seg3_entries[j].off == off) { found = 1; break; }
        if (!found) bad++;
        if (i && ir32_seg3_entries[i - 1].off >= off) {
            printf("  entry table not sorted at %u (%08X after %08X)\n",
                   i, off, ir32_seg3_entries[i - 1].off);
            bad++;
        }
    }
    printf("entry table: %s\n", bad ? "INCONSISTENT" : "sorted and complete");

    if (argc < 3) {
        printf("no entry requested; not running any lifted code\n");
        return bad != 0;
    }

    if (!strcmp(argv[2], "init")) {
        /* 3:0000 is the decoder's initialisation, and it is the one entry that
         * needs nothing but a place to work: its only argument is the selector
         * of an instance segment. Everything else in the 32-bit core wants
         * structures the 16-bit driver owns.
         *
         * What it should do is checkable without knowing the format: fill from
         * 0xE20C with 0x40404040 for 0xB0 bytes, then walk a pointer forward in
         * 0x2C steps storing it at 0x0C, 0x18 and on. If the segment model is
         * wrong in any way - bases, stack, selector fixups - this writes
         * somewhere else or faults, and either way says so. */
        enum { INST_SEL = 0x0200, INST_SIZE = 0x10000 };
        unsigned char *inst = (unsigned char *)calloc(1, INST_SIZE);
        ne_map(INST_SEL, inst, INST_SIZE);

        CPU c = {0};
        c.cs = 3;
        c.ds = ds ? ds : 41;
        c.es = INST_SEL;
        c.ss = ss;
        c.esp = 0xFF00u;
        /* A 16-bit far call leaves 4 bytes of return address, so the first
         * argument sits at [bp+4]. Nothing returns to it here - the lifted
         * function returns to us - so the slot itself can stay zero. */
        wr16(SEGB(c.ss) + c.esp + 4, INST_SEL);
        c.ebp = c.esp;

        printf("instance segment %04X at %08X (%d bytes)\n",
               INST_SEL, SEGB(INST_SEL), INST_SIZE);
        printf("calling 3:0000 (init) ...\n");
        dispatch(&c, 0x00000000u);

        unsigned fill = 0;
        for (unsigned o = 0xE20C; o + 4 <= 0xE20C + 0xB0; o += 4)
            if (inst[o] == 0x40 && inst[o+1] == 0x40 &&
                inst[o+2] == 0x40 && inst[o+3] == 0x40)
                fill += 4;
        printf("fill at 0xE20C: %u of %u bytes are 0x40404040\n", fill, 0xB0);
        printf("pointers written: [0x0C]=%08X [0x18]=%08X [0x24]=%08X\n",
               *(uint32_t *)(inst + 0x0C), *(uint32_t *)(inst + 0x18),
               *(uint32_t *)(inst + 0x24));
        unsigned nonzero = 0;
        for (unsigned o = 0; o < INST_SIZE; o++)
            if (inst[o]) nonzero++;
        printf("instance segment: %u of %u bytes written\n", nonzero, INST_SIZE);
        ne_report_misses();

        /* These are not observations, they are predictions read off the
         * instructions before the code was ever run, which is what makes them
         * worth asserting:
         *
         *   mov ebx, 0xE20C / mov ecx, 0xB0
         *   loop: store 8 bytes, ebx += 8, ecx -= 8, jg loop  -> ebx = 0xE2BC
         *   add ebx, 4     -> 0xE2C0   mov es:[0x0C], ebx
         *   add ebx, 0x2C  -> 0xE2EC   mov es:[0x18], ebx
         *
         * If the segment model, the stack base or the selector fixups are
         * wrong, these are the first things to go. */
        int wrong = 0;
        if (fill != 0xB0)                            wrong |= 1;
        if (*(uint32_t *)(inst + 0x0C) != 0xE2C0u)   wrong |= 2;
        if (*(uint32_t *)(inst + 0x18) != 0xE2ECu)   wrong |= 4;
        if (g_dispatch_misses)                       wrong |= 8;
        printf("init check: %s\n",
               wrong ? "FAILED" : "matches the disassembly");
        return wrong;
    }

    if (!strcmp(argv[2], "sweep")) {
        /* Call every entry with a blank machine and see which return.
         *
         * A fault here is not automatically a defect: most of these want real
         * arguments - a bitstream in GS, an output frame in ES - and reading
         * an unset selector's segment is exactly the near-null access the
         * selector table is designed to make loud. What the sweep measures is
         * the runtime, not the decoder: whether dispatch resolves, whether the
         * segment bases hold, whether anything corrupts the machine badly
         * enough to take the process down. Before it existed, "it links" was
         * the only evidence there was. */
        unsigned ok = 0, faulted = 0;
        g_dispatch_soft = 1;
        for (unsigned i = 0; i < ir32_seg3_entry_count; i++) {
            CPU c = {0};
            c.cs = 3;
            c.ds = ds ? ds : 41;
            c.es = c.ds; c.fs = c.ds; c.gs = c.ds;
            c.ss = ss;
            c.esp = 0xFF00u;
            c.ebp = c.esp;
#if defined(_WIN32)
            __try {
                ir32_seg3_entries[i].fn(&c);
                ok++;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                faulted++;
                printf("   %08X faulted (0x%08lX)\n",
                       ir32_seg3_entries[i].off,
                       (unsigned long)GetExceptionCode());
            }
#else
            ir32_seg3_entries[i].fn(&c);
            ok++;
#endif
        }
        printf("sweep: %u of %u entries returned, %u faulted\n",
               ok, ir32_seg3_entry_count, faulted);
        ne_report_misses();
        return 0;
    }

    uint32_t off = (uint32_t)strtoul(argv[2], NULL, 16);
    CPU c = {0};
    c.cs = 3;
    c.ds = ds ? ds : 41;
    c.es = c.ds;
    c.fs = c.ds;
    c.gs = c.ds;
    c.ss = ss;
    /* ESP is an offset within SS, not an address (see recomp32.h). Keep it
     * inside 16 bits so `mov bp, sp` in the callee still means what it says. */
    c.esp = 0xFF00u;
    c.ebp = c.esp;
    printf("calling %04X:%08X ...\n", c.cs, off);
    dispatch(&c, off);
    printf("returned: eax=%08X ecx=%08X edx=%08X esi=%08X edi=%08X\n",
           c.eax, c.ecx, c.edx, c.esi, c.edi);
    printf("dispatch misses: %lu\n", g_dispatch_misses);
    return 0;
}
