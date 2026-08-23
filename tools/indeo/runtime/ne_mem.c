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
    bind_sel(sel, off);
    return off;
}

void ne_alias(uint16_t sel, uint32_t arena_off)
{
    if (!g_arena)
        ne_mem_init();
    bind_sel(sel, arena_off);
}
