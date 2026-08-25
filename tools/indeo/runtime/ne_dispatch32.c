/*
 * ne_dispatch32.c - dispatch inside the lifted 32-bit half, and the bridge in
 * from the 16-bit half.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp32.h"
#if defined(_WIN32)
#include <windows.h>
#endif

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
    /* The selector may be a writable copy of a code segment rather than the
     * segment itself, and CS has to keep saying the copy - see ne_call32 - so
     * the lookup resolves it here instead. */
    uint16_t orig = ne_code_alias(sel, 47);
    if (orig)
        sel = orig;
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

#ifdef IR32_WATCH
uint32_t g_watch[IR32_WATCH_N];
unsigned g_watch_n;
uint32_t g_watch_lo, g_watch_hi;
unsigned g_watch_hits;

void ir32_watch_dump(void)
{
    fprintf(stderr, "     accesses in the watched window: %u\n", g_watch_hits);
    fprintf(stderr, "     last %d addresses touched (oldest first):\n",
            IR32_WATCH_N);
    for (unsigned i = 0; i < IR32_WATCH_N; i++) {
        uint32_t a = g_watch[(g_watch_n + i) % IR32_WATCH_N];
        uint32_t base = (uint32_t)(uintptr_t)g_arena;
        fprintf(stderr, "       %08X  %s\n", a,
                a < NE_ARENA_GUARD ? "near null - no base added"
                : (a >= base && a < base + NE_ARENA_SIZE)
                      ? "in arena" : "OUTSIDE arena");
    }
}
#endif

#if defined(_WIN32)
/* Pull the faulting address out of the exception record. Which address was
 * touched identifies the bug; the register dump only hints at it. */
static void record_fault(EXCEPTION_POINTERS *ep, uint32_t *at, int *write)
{
    if (!ep || !ep->ExceptionRecord)
        return;
    if (ep->ExceptionRecord->NumberParameters >= 2) {
        *write = (int)ep->ExceptionRecord->ExceptionInformation[0];
        *at = (uint32_t)ep->ExceptionRecord->ExceptionInformation[1];
    }
}
#endif

unsigned ne_call32(uint16_t seg, uint32_t off, uint16_t ss, uint16_t sp,
                   uint16_t ds, uint16_t es, const ne_regs *r)
{
    g_bridge_calls++;
#ifdef IR32_WATCH
    /* Arm the low-region watch once the working buffer exists. */
    if (!g_watch_lo) {
        /* IR32_WATCH_SEL / _OFF / _LEN choose the window, so a run can ask
         * about any region without a rebuild. Defaults to the decoder's
         * globals in the first working buffer, which is where the damage
         * showed up. */
        const char *ws = getenv("IR32_WATCH_SEL");
        const char *wo = getenv("IR32_WATCH_OFF");
        const char *wl = getenv("IR32_WATCH_LEN");
        uint16_t wsel = ws ? (uint16_t)strtoul(ws, NULL, 16) : 0x0405;
        uint32_t woff = wo ? strtoul(wo, NULL, 16) : 0xE180;
        uint32_t wlen = wl ? strtoul(wl, NULL, 16) : 0x40;
        if (g_segoff[wsel]) {
            g_watch_lo = g_selbase[wsel] + woff;
            g_watch_hi = g_watch_lo + wlen;
        }
    }
#endif
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
    /* The decode thunk indexes its plane table with [ebp+0x0A]:
     *
     *     mov ax, [ebp+0x0A]
     *     mov ebx, ds:[eax*4 + 8]     -> [0xE198]
     *     mov ebx, ds:[eax*4 + 0xC]   -> [0xE1A8], later dereferenced as EDI
     *
     * The table itself is correct after BEGIN - 0xE2C0, 0xE318, 0x18C94 - so a
     * pointer that faults means the INDEX is wrong and the read fell off the
     * end of the table into whatever follows. Print the arguments rather than
     * inferring the index from the value it produced. */
    /* `seg == 3` is not enough: this one is reached through 0404, a
     * writable alias of segment 3, so the hook never fired. Resolve the
     * alias the way find() does. */
    /* All six colour converters take the same frame: the destination far
     * pointer at ss:[bp+0x0C] and ss:[bp+0x10]. Which one runs depends on
     * the output depth, and 2C10 alone hid that the 24bpp converter, 41F0,
     * is handed a selector that is not mapped. */
    if (getenv("IR32_TRACE") &&
        (off == 0x2800 || off == 0x2C10 || off == 0x2FB0 ||
         off == 0x3640 || off == 0x3CCF || off == 0x41F0) &&
        (seg == 3 || ne_code_alias(seg, 47) == 3)) {
        const unsigned char *f = g_arena + g_segoff[ss] + sp;
        unsigned idx = f[0x0A] | (f[0x0B] << 8);
        fprintf(stderr, "     converter %04X args: [bp+06]=%04X [bp+08]=%04X "
                        "[bp+0A]=%04X (table index)\n",
                (unsigned)off, f[6] | (f[7] << 8), f[8] | (f[9] << 8), idx);
        fprintf(stderr, "       reads ds:[%X] and ds:[%X]%s\n",
                idx * 4 + 8, idx * 4 + 0xC,
                idx > 8 ? "   <- past the table" : "");
        /* The destination the converter writes through: ss:[bp+0x0C] is
         * the offset and ss:[bp+0x10] the selector. The converter steps
         * rows by a hardcoded 0x100 BYTES - the same 256 at 8, 16 and
         * 24bpp - so it cannot be a DIB pitch, which would be 216, 432
         * and 648 for this frame. Whatever this pointer names has
         * 256-byte rows, and if it is the caller's DIB then the caller
         * is passing the wrong buffer. */
        {
            uint32_t dst = (uint32_t)f[0x0C] | ((uint32_t)f[0x0D] << 8) |
                           ((uint32_t)f[0x0E] << 16) | ((uint32_t)f[0x0F] << 24);
            uint16_t dsel = (uint16_t)(f[0x10] | (f[0x11] << 8));
            fprintf(stderr, "       destination %04X:%08X  (%s)\n",
                    dsel, dst,
                    g_segoff[dsel] ? "mapped" : "UNMAPPED");
        }
    }

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
            /* The decoder walks a sliding pair over this array - DS from
             * [ecx], FS from [ecx+2], ecx += 2 - and stops when one reads
             * zero. How many entries there are is how many planes it will
             * process, so the array itself says how much work it intends. */
            fprintf(stderr, "     selector array at %04X:%04X:", p_sel, p_off);
            for (int k = 0; k < 8; k++) {
                uint16_t v = (uint16_t)(q[k * 2] | (q[k * 2 + 1] << 8));
                fprintf(stderr, " %04X%s", v, g_segoff[v] ? "" : "!");
                if (!v)
                    break;
            }
            fprintf(stderr, "   (! = unmapped)\n");
        }
    }

    CPU c;
    memset(&c, 0, sizeof c);
    /* CS keeps the selector the caller actually used, which may be a writable
     * copy of the code segment rather than the segment itself.
     *
     * That distinction is the whole reason the copy exists. This codec keeps
     * mutable variables inside its own code:
     *
     *     cmp eax, dword ptr cs:[0x2F7D]     ; read through CS
     *     mov es, [ebp+4]
     *     mov dword ptr es:[0x2F7D], eax     ; write through a data alias
     *
     * A real code segment cannot be written, so it makes a writable alias and
     * writes there. Pointing CS at the pristine original would have it reading
     * one copy and writing the other, and every such variable would read as
     * whatever the file happened to contain. */
    c.cs = seg;
    c.ss = ss;
    c.esp = sp;
    c.ds = ds;
    c.es = es;
    c.fs = ds;
    c.gs = ds;
    /* Carried, not cleared: a far call does not touch the general registers,
     * and this callee saves EDX, EDI and ESI on entry because they are inputs.
     * Zeroing them here was handing the decoder null pointers and calling it a
     * fresh machine. */
    if (r) {
        c.eax = r->eax; c.ecx = r->ecx; c.edx = r->edx; c.ebx = r->ebx;
        c.ebp = r->ebp; c.esi = r->esi; c.edi = r->edi;
    }
#if defined(_WIN32)
    /* Catching it here rather than at the 16-bit boundary is the difference
     * between "the message faulted" and knowing which selector was being read
     * through when it did. An unmapped selector's base is 0 and the arena's
     * first page is deliberately not committed, so a fault near null is a
     * selector nobody mapped - which is a specific, findable bug rather than a
     * crash. */
    uint32_t fault_at = 0;
    int fault_write = 0;
    __try {
        fn(&c);
    } __except (record_fault(GetExceptionInformation(), &fault_at, &fault_write),
                EXCEPTION_EXECUTE_HANDLER) {
        /* The address that was touched says more than any register does. Near
         * zero means an unmapped selector - base 0 plus a small offset - and
         * the arena's first page is left uncommitted so exactly that faults.
         * Far outside the arena means an offset that is not an offset. */
        fprintf(stderr, "     FAULT in %04X:%08X (code 0x%08lX) %s %08X %s\n",
                seg, off, (unsigned long)GetExceptionCode(),
                fault_write ? "writing" : "reading", fault_at,
                fault_at < NE_ARENA_GUARD ? "- UNMAPPED SELECTOR (near null)"
                : (fault_at >= (uint32_t)(uintptr_t)g_arena &&
                   fault_at < (uint32_t)(uintptr_t)g_arena + NE_ARENA_SIZE)
                      ? "- inside the arena" : "- OUTSIDE the arena");
        fprintf(stderr, "       eax=%08X ebx=%08X ecx=%08X edx=%08X\n",
                c.eax, c.ebx, c.ecx, c.edx);
        fprintf(stderr, "       esi=%08X edi=%08X ebp=%08X esp=%08X\n",
                c.esi, c.edi, c.ebp, c.esp);
        fprintf(stderr, "       ds=%04X%s es=%04X%s fs=%04X%s gs=%04X%s "
                        "ss=%04X%s\n",
                c.ds, g_segoff[c.ds] ? "" : "!",
                c.es, g_segoff[c.es] ? "" : "!",
                c.fs, g_segoff[c.fs] ? "" : "!",
                c.gs, g_segoff[c.gs] ? "" : "!",
                c.ss, g_segoff[c.ss] ? "" : "!");
        fprintf(stderr, "       (! = selector not mapped)\n");
#ifdef IR32_WATCH
        ir32_watch_dump();
#endif
        return 0;
    }
#else
    fn(&c);
#endif
    if (getenv("IR32_TRACE"))
        fprintf(stderr, "     returned eax=%08X ecx=%08X edx=%08X esi=%08X "
                        "edi=%08X\n", c.eax, c.ecx, c.edx, c.esi, c.edi);
    /* How many argument bytes the callee popped is its `retf N`, which the
     * lifted form does not carry yet - so the caller drops only the return
     * address and SP is left N bytes low. Wrong, but visibly wrong. */
    return 0;
}
