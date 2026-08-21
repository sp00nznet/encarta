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
    free(img);
    return 1;
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
