/*
 * ne_dispatch32.c - dispatch inside the lifted 32-bit half, and the bridge in
 * from the 16-bit half.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp32.h"

uint32_t g_image_delta;        /* cpu.h's GVA(); zero, so GVA is the identity */
unsigned long g_dispatch_misses;
uint32_t g_last_miss;
int g_dispatch_soft;

#define MAX_CODE_SEGS 8
static struct {
    uint16_t sel;
    const ne_entry *entries;
    unsigned count;
} g_code[MAX_CODE_SEGS];
static unsigned g_code_n;

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
    for (unsigned i = 0; i < g_code_n; i++) {
        if (g_code[i].sel != sel)
            continue;
        unsigned lo = 0, hi = g_code[i].count;   /* entries are emitted sorted */
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

/* A miss is not automatically a bug. Descent recovers function ENTRY points,
 * and a target landing mid-function - a shared epilogue, a block two functions
 * tail-merge into - has no entry of its own and cannot be called from C at all.
 * Six of these are real and deliberate: jump-table slots for the invalid index,
 * pointing at an int3.
 *
 * Aborting is right for a real run, since continuing past a transfer that did
 * not happen produces wrong output rather than no output. It also makes the
 * whole program the unit of failure, so a survey cannot ask "how many entries
 * work" without the first miss ending the survey. Soft mode records and
 * returns: wrong execution, honest measurement. */
#define MAX_MISSES 64
static uint32_t g_miss_list[MAX_MISSES];
static unsigned g_miss_n;

static void miss(uint16_t cs, uint32_t target)
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
        return;
    fprintf(stderr, "dispatch: no entry for %04X:%08X\n", cs, target);
    abort();
}

void ne_report_misses(void)
{
    if (!g_miss_n) {
        printf("dispatch: every target resolved\n");
        return;
    }
    printf("dispatch: %lu misses, %u distinct targets with no lifted entry:\n",
           g_dispatch_misses, g_miss_n);
    for (unsigned i = 0; i < g_miss_n; i++)
        printf("   %08X\n", g_miss_list[i]);
}

void dispatch(CPU *c, uint32_t target)
{
    void (*fn)(CPU *) = find((uint16_t)c->cs, target);
    if (!fn) { miss((uint16_t)c->cs, target); return; }
    fn(c);
}

void dispatch_jmp(CPU *c, uint32_t target)
{
    void (*fn)(CPU *) = find((uint16_t)c->cs, target);
    if (!fn) { miss((uint16_t)c->cs, target); return; }
    fn(c);
}

uint16_t ne_init(uint32_t stack_bytes)
{
    static uint16_t stack_sel = 0x0100;
    ne_alloc(stack_sel, NULL, 0, stack_bytes);
    /* A real base, like every other segment: ESP is an offset into it, not an
     * address, because the callee does `mov bp, sp` and a host address does
     * not survive being truncated to 16 bits. See recomp32.h. */
    return stack_sel;
}

/* The 16-bit -> 32-bit bridge.
 *
 * This is what hardware does at a far call across the boundary, and it is
 * short only because the memory model already lines up: both halves index one
 * arena, so the arguments the 16-bit caller pushed onto SS are already where
 * the 32-bit callee looks for them. The machine is fresh - a far call does not
 * inherit the caller's general registers - and ESP points at the return
 * address the caller pushed, which is the frame the real thunk reads its
 * arguments from at [bp+4]. */
/* How many times the 16-bit half has crossed into the 32-bit core. Zero after
 * a decode means the driver never got as far as calling its own decoder,
 * which is a different problem from a decoder that ran and produced nothing -
 * and an empty output buffer looks identical either way. */
unsigned long g_bridge_calls;

unsigned ne_call32(uint16_t seg, uint32_t off, uint16_t ss, uint16_t sp,
                   uint16_t ds, uint16_t es)
{
    g_bridge_calls++;
    /* IR32_TRACE=1 prints each crossing. Which 32-bit entries a decode
     * reaches says more than the return code does: seven crossings with an
     * empty output buffer could be seven calls to setup routines that never
     * got as far as the decoder itself. */
    if (getenv("IR32_TRACE"))
        fprintf(stderr, "  -> 32-bit %04X:%08X\n", seg, off);
    void (*fn)(CPU *) = find(seg, off);
    if (!fn) {
        miss(seg, off);
        return 0;
    }
    /* The decode entry's first act is to find its instance data:
     *
     *     mov ax, [bp+0x1E]     ; a selector
     *     mov es, ax
     *     mov ax, es:[ecx]      ; ecx = [bp+4]; read a word through it
     *     mov ds, ax            ; that word IS the instance's data selector
     *
     * So a decode that returns without doing anything is most cheaply explained
     * by that chain not resolving. Walk it here, where both the arena and the
     * selector table are in reach, rather than inferring it from registers. */
    if (getenv("IR32_TRACE") && seg == 3 && off == 0x610) {
        const unsigned char *frame = g_arena + g_segoff[ss] + sp;
        uint16_t p_off = (uint16_t)(frame[4] | (frame[5] << 8));
        uint16_t p_sel = (uint16_t)(frame[0x1E] | (frame[0x1F] << 8));
        fprintf(stderr, "     args: [bp+04]=%04X [bp+1E]=%04X (%s)\n",
                p_off, p_sel, g_segoff[p_sel] ? "mapped" : "UNMAPPED");
        if (g_segoff[p_sel]) {
            const unsigned char *q = g_arena + g_segoff[p_sel] + p_off;
            uint16_t ds_sel = (uint16_t)(q[0] | (q[1] << 8));
            /* Two words, not one. The decoder takes DS from the first and FS
             * from the second, and bails out immediately if the second is
             * zero:  add ecx,2 / mov ax,es:[ecx] / test ax,ax / je. */
            uint16_t fs_sel = (uint16_t)(q[2] | (q[3] << 8));
            fprintf(stderr, "     ds from %04X:%04X = %04X (%s), "
                            "fs from +2 = %04X (%s)\n",
                    p_sel, p_off, ds_sel,
                    g_segoff[ds_sel] ? "mapped" : "UNMAPPED",
                    fs_sel,
                    fs_sel == 0 ? "ZERO - decoder bails here"
                                : (g_segoff[fs_sel] ? "mapped" : "UNMAPPED"));
        }
    }

    CPU c;
    memset(&c, 0, sizeof c);
    c.cs = seg;
    c.ss = ss;
    c.esp = sp;
    c.ds = ds;
    c.es = es;
    c.fs = ds;
    c.gs = ds;
    fn(&c);
    if (getenv("IR32_TRACE"))
        fprintf(stderr, "     returned eax=%08X ecx=%08X edx=%08X esi=%08X "
                        "edi=%08X\n", c.eax, c.ecx, c.edx, c.esi, c.edi);
    /* How many argument bytes the callee popped is its `retf N`, which the
     * lifted form does not carry yet - so the caller drops only the return
     * address and SP is left N bytes low. Wrong, but visibly wrong. */
    return 0;
}
