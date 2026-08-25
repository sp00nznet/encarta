/*
 * ne_mem.c - the shared arena. See ne_mem.h for why it exists.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ne_mem.h"

#if defined(_WIN32)
#include <windows.h>
#endif

unsigned char *g_arena;
uint32_t g_segoff[65536];
uint32_t g_selbase[65536];
uint32_t g_selsize[65536];

static uint32_t g_next = NE_ARENA_GUARD;

uint16_t g_local_base, g_local_next, g_local_end;

void ne_mem_init(void)
{
    if (g_arena)
        return;
#if defined(_WIN32)
    /* Reserve the whole arena, then commit everything past the guard. The
     * guard stays NOACCESS so a read through an unmapped selector - offset 0 -
     * faults at once rather than returning plausible bytes. */
    g_arena = (unsigned char *)VirtualAlloc(NULL, NE_ARENA_SIZE,
                                            MEM_RESERVE, PAGE_NOACCESS);
    if (g_arena) {
        if (!VirtualAlloc(g_arena + NE_ARENA_GUARD,
                          NE_ARENA_SIZE - NE_ARENA_GUARD,
                          MEM_COMMIT, PAGE_READWRITE))
            g_arena = NULL;
    }
#else
    g_arena = (unsigned char *)calloc(1, NE_ARENA_SIZE);
#endif
    if (!g_arena) {
        fprintf(stderr, "ne_mem_init: cannot reserve %u bytes\n", NE_ARENA_SIZE);
        abort();
    }
    /* Every selector starts unmapped, which is offset 0, which is the guard. */
    memset(g_segoff, 0, sizeof g_segoff);
    memset(g_selbase, 0, sizeof g_selbase);
}

static void bind_sel(uint16_t sel, uint32_t off)
{
    g_segoff[sel] = off;
    g_selbase[sel] = (uint32_t)(uintptr_t)(g_arena + off);
}

#define NE_AHINCR 8
#define NE_MAX_SEGMENT 64   /* selectors at or below this are NE
                             * segments, numbered by index */      /* the protected-mode __AHINCR, patched into the
                          * code by lift_ir32's fixup pass */

uint32_t ne_huge_alias(uint16_t sel)
{
    /* Walk back one 64K step at a time looking for the block this selector
     * is a window into. Bounded by how many steps a real block could span,
     * so an genuinely bogus selector costs a short loop and still reads as
     * unmapped. */
    for (unsigned k = 1; k <= 256; k++) {
        uint16_t base_sel = (uint16_t)(sel - k * NE_AHINCR);
        if (!g_segoff[base_sel])
            continue;
        uint32_t need = k * 65536u;
        if (g_selsize[base_sel] <= need)
            continue;          /* the block does not reach this far */
        uint32_t off = g_segoff[base_sel] + need;
        bind_sel(sel, off);
        g_selsize[sel] = g_selsize[base_sel] - need;
        return off;
    }
    /* The other way a selector gets stepped: across NE segments.
     *
     * Windows hands consecutive segments consecutive selectors, 8 apart, so
     * `sel + 8` is the NEXT SEGMENT - and code that treats several 64K data
     * segments as one buffer walks them exactly that way. This runtime numbers
     * a selector by its segment index instead, which is what makes traces
     * readable, so segment 44 is 0x2C and 0x2C+8 is 0x34 rather than segment
     * 45. Map it back.
     *
     * IR32 has three of these in a row - segments 43, 44 and 45, all data,
     * all with an allocation size of zero meaning a full 64K - and DRV_LOAD
     * copies across them. */
    for (unsigned k = 1; k <= 32; k++) {
        uint16_t base_sel = (uint16_t)(sel - k * NE_AHINCR);
        if (base_sel == 0 || base_sel > NE_MAX_SEGMENT)
            continue;
        uint16_t step_to = (uint16_t)(base_sel + k);
        if (step_to > NE_MAX_SEGMENT || !g_segoff[base_sel] || !g_segoff[step_to])
            continue;
        bind_sel(sel, g_segoff[step_to]);
        g_selsize[sel] = g_selsize[step_to];
        return g_segoff[step_to];
    }
    return 0;
}

uint32_t ne_alloc(uint16_t sel, const void *src, uint32_t copy, uint32_t size)
{
    if (!g_arena)
        ne_mem_init();
    /* 16-byte alignment costs nothing and keeps dumps readable. */
    uint32_t off = (g_next + 15u) & ~15u;
    if (off + size > NE_ARENA_SIZE) {
        fprintf(stderr, "ne_alloc: arena full at selector %04X (%u bytes)\n",
                sel, size);
        abort();
    }
    memset(g_arena + off, 0, size);
    if (src && copy)
        memcpy(g_arena + off, src, copy > size ? size : copy);
    g_next = off + size;
    g_selsize[sel] = size;
    bind_sel(sel, off);
    return off;
}

void ne_alias(uint16_t sel, uint32_t arena_off)
{
    if (!g_arena)
        ne_mem_init();
    bind_sel(sel, arena_off);
}

/* See ne_mem.h. The comparison is 256 bytes: enough that two different
 * segments cannot agree by accident, short enough to not matter. The first
 * bytes of a code segment are its entry thunk, which differs between segments,
 * so this is a strong discriminator in practice. */
#define ALIAS_CMP 256

static uint16_t g_alias[65536];
static unsigned char g_alias_known[65536];

uint16_t ne_code_alias(uint16_t sel, unsigned nseg)
{
    if (g_alias_known[sel])
        return g_alias[sel];
    g_alias_known[sel] = 1;
    g_alias[sel] = 0;
    if (!g_segoff[sel] || !g_arena)
        return 0;
    const unsigned char *a = g_arena + g_segoff[sel];
    for (unsigned i = 1; i <= nseg && i < 64; i++) {
        if (i == sel || !g_segoff[i])
            continue;
        const unsigned char *b = g_arena + g_segoff[i];
        unsigned same = 0;
        for (unsigned k = 0; k < ALIAS_CMP; k++)
            same += (a[k] == b[k]);
        /* Not memcmp: the loader patches selectors into the original after the
         * copy was taken, so a handful of bytes legitimately differ. */
        if (same >= ALIAS_CMP - 16) {
            g_alias[sel] = (uint16_t)i;
            break;
        }
    }
    return g_alias[sel];
}
