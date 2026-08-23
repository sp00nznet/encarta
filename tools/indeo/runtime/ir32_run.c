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
    fprintf(stderr, "unknown command %s\n", argv[2]);
    return 2;
}
