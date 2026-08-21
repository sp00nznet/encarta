/*
 * ne_runtime.c - selector table and dispatch for lifted 32-bit NE code.
 *
 * See recomp32.h for the memory model. This file is the whole of it: a
 * selector -> base array, a per-selector table of lifted entry points, and the
 * dispatch that turns a segment offset into a call.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp32.h"

uint32_t g_selbase[65536];
uint32_t g_image_delta;        /* cpu.h's GVA(); zero, so GVA is the identity */

unsigned long g_dispatch_misses;
uint32_t g_last_miss;

/* Code segments are few - IR32 has two that are 32-bit - so a linear scan over
 * registered segments costs nothing next to a binary search inside one. */
#define MAX_CODE_SEGS 8
static struct {
    uint16_t sel;
    const ne_entry *entries;
    unsigned count;
} g_code[MAX_CODE_SEGS];
static unsigned g_code_n;

void ne_map(uint16_t sel, void *host, uint32_t size)
{
    (void)size;
    g_selbase[sel] = (uint32_t)(uintptr_t)host;
}

void ne_register_code(uint16_t sel, const ne_entry *entries, unsigned count)
{
    if (g_code_n >= MAX_CODE_SEGS) {
        fprintf(stderr, "ne_register_code: too many code segments\n");
        abort();
    }
    g_code[g_code_n].sel = sel;
    g_code[g_code_n].entries = entries;
    g_code[g_code_n].count = count;
    g_code_n++;
}

static void (*find(uint16_t sel, uint32_t off))(CPU *)
{
    unsigned i;
    for (i = 0; i < g_code_n; i++) {
        if (g_code[i].sel != sel)
            continue;
        /* entries are emitted sorted by offset */
        unsigned lo = 0, hi = g_code[i].count;
        while (lo < hi) {
            unsigned mid = (lo + hi) / 2;
            uint32_t k = g_code[i].entries[mid].off;
            if (k == off)
                return g_code[i].entries[mid].fn;
            if (k < off) lo = mid + 1; else hi = mid;
        }
        return NULL;
    }
    return NULL;
}

/* A miss is not automatically a bug, and it is worth being precise about why.
 * Descent recovers function ENTRY points; a target that lands in the middle of
 * a function - a shared epilogue, a block two functions tail-merge into - has
 * no entry of its own and cannot be called from C at all. Fixing that means
 * re-lifting the containing function with the target as a label, which the
 * lifter already does for branches it can see within one function.
 *
 * Aborting is right for a real run: continuing past a transfer that did not
 * happen produces wrong output rather than no output. But it makes the whole
 * program the unit of failure, so a survey cannot ask "how many entries work"
 * without the first miss ending the survey. In soft mode the miss is recorded
 * and dispatch returns, which is wrong execution and honest measurement. */
int g_dispatch_soft;

#define MAX_MISSES 64
static uint32_t g_miss_list[MAX_MISSES];
static unsigned g_miss_n;

static int miss(CPU *c, uint32_t target)
{
    unsigned i;
    g_dispatch_misses++;
    g_last_miss = target;
    for (i = 0; i < g_miss_n; i++)
        if (g_miss_list[i] == target)
            break;
    if (i == g_miss_n && g_miss_n < MAX_MISSES)
        g_miss_list[g_miss_n++] = target;
    if (g_dispatch_soft)
        return 0;
    fprintf(stderr, "dispatch: no entry for %04X:%08X\n", c->cs, target);
    abort();
    return 0;
}

void ne_report_misses(void)
{
    unsigned i;
    if (!g_miss_n) {
        printf("dispatch: every target resolved\n");
        return;
    }
    printf("dispatch: %lu misses, %u distinct targets with no lifted entry:\n",
           g_dispatch_misses, g_miss_n);
    for (i = 0; i < g_miss_n; i++)
        printf("   %08X\n", g_miss_list[i]);
}

void dispatch(CPU *c, uint32_t target)
{
    void (*fn)(CPU *) = find((uint16_t)c->cs, target);
    if (!fn) { miss(c, target); return; }
    fn(c);
}

void dispatch_jmp(CPU *c, uint32_t target)
{
    void (*fn)(CPU *) = find((uint16_t)c->cs, target);
    if (!fn) { miss(c, target); return; }
    fn(c);
}

uint16_t ne_init(uint32_t stack_bytes)
{
    static uint16_t stack_sel = 0x0100;
    void *stack = calloc(1, stack_bytes);
    if (!stack) {
        fprintf(stderr, "ne_init: cannot allocate %u byte stack\n", stack_bytes);
        abort();
    }
    /* A real base, like every other segment. The caller then sets ESP to an
     * OFFSET near the top of it - see recomp32.h for why that matters. */
    g_selbase[stack_sel] = (uint32_t)(uintptr_t)stack;
    return stack_sel;
}
