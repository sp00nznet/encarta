/*
 * ne_dispatch16.c - far calls out of the lifted 16-bit half.
 *
 * A far call here can go three places, and the selector says which:
 *
 *   0xF0mm   an import. mm is the NE module index, and the offset is the
 *            ordinal, both put there by lift_ir32.py's fixup pass.
 *   2 or 3   the 32-bit decode core. Crossing that boundary needs a 32-bit
 *            machine, which is why the bridge itself lives in the other
 *            translation unit: pcrecomp's two runtimes each define a struct
 *            called CPU and a function called push32, so no file can see both.
 *   anything else   another lifted 16-bit segment.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp16.h"

unsigned long g_dispatch16_misses;

#define MAX_CODE16 64
static struct {
    uint16_t sel;
    const ne16_entry *entries;
    unsigned count;
} g_code16[MAX_CODE16];
static unsigned g_code16_n;

void ne_register_code16(uint16_t sel, const ne16_entry *entries, unsigned count)
{
    if (g_code16_n >= MAX_CODE16) {
        fprintf(stderr, "ne_register_code16: too many segments\n");
        abort();
    }
    g_code16[g_code16_n].sel = sel;
    g_code16[g_code16_n].entries = entries;
    g_code16[g_code16_n].count = count;
    g_code16_n++;
}

static void (*find16(uint16_t sel, uint16_t off))(CPU *)
{
    for (unsigned i = 0; i < g_code16_n; i++) {
        if (g_code16[i].sel != sel)
            continue;
        unsigned lo = 0, hi = g_code16[i].count;
        while (lo < hi) {
            unsigned mid = (lo + hi) / 2;
            uint16_t k = g_code16[i].entries[mid].off;
            if (k == off)
                return g_code16[i].entries[mid].fn;
            if (k < off) lo = mid + 1; else hi = mid;
        }
        return NULL;
    }
    return NULL;
}

/* ---- imports ----------------------------------------------------------
 *
 * Nothing here implements Windows. The DLL imports 61 distinct entries across
 * KERNEL, USER, GDI, MMSYSTEM and WIN87EM, and most of them belong to the
 * dialog boxes rather than to decoding anything. Guessing which matter and
 * writing those first is how you end up with five wrong implementations and no
 * way to tell. So every import is recorded and the unimplemented ones say so,
 * and what a decode actually touches becomes an observation.
 */
static const char *MODULES[] = { "?", "WIN87EM", "KERNEL", "GDI", "USER", "MMSYSTEM" };
#define NMODULES ((int)(sizeof MODULES / sizeof MODULES[0]))

#define MAX_IMPORTS 128
static struct { uint16_t mod, ord; unsigned long hits; } g_imports[MAX_IMPORTS];
static unsigned g_imports_n;

static void note_import(uint16_t mod, uint16_t ord)
{
    for (unsigned i = 0; i < g_imports_n; i++)
        if (g_imports[i].mod == mod && g_imports[i].ord == ord) {
            g_imports[i].hits++;
            return;
        }
    if (g_imports_n < MAX_IMPORTS) {
        g_imports[g_imports_n].mod = mod;
        g_imports[g_imports_n].ord = ord;
        g_imports[g_imports_n].hits = 1;
        g_imports_n++;
    }
}

/* Win16 KERNEL ordinals the decode path plausibly needs. GlobalAlloc returns a
 * selector, not a pointer, which suits us: allocate in the arena and hand back
 * the selector it was bound to. */
enum { K_GLOBALALLOC = 15, K_GLOBALREALLOC = 16, K_GLOBALFREE = 17,
       K_GLOBALLOCK = 18, K_GLOBALUNLOCK = 19, K_GLOBALSIZE = 20,
       K_GETVERSION = 3 };

static uint16_t g_next_heap_sel = 0x0400;

static void kernel_import(CPU *cpu, uint16_t ord)
{
    switch (ord) {
    case K_GLOBALALLOC: {
        /* GlobalAlloc(flags, dwBytes) -> handle, Pascal order: the last
         * argument pushed is nearest the top. lift16 pushed the far return
         * address after the arguments, so skip it. */
        uint32_t bytes = (uint32_t)mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 4))
                       | ((uint32_t)mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 6)) << 16);
        if (!bytes) bytes = 16;
        uint16_t sel = g_next_heap_sel++;
        ne_alloc(sel, NULL, 0, bytes);
        cpu->ax = sel;         /* handle == selector, which is true enough */
        cpu->dx = 0;
        break;
    }
    case K_GLOBALLOCK:
        /* GlobalLock(handle) -> far pointer. The segment is the handle and the
         * offset is zero, which is what a Win16 huge block looks like. */
        cpu->ax = 0;
        cpu->dx = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 4));
        break;
    case K_GLOBALREALLOC: {
        /* GlobalReAlloc(handle, dwBytes, flags). The handle is a selector and
         * stays one, so the block is re-bound at a fresh arena offset and the
         * handle is returned unchanged. Contents are not copied: nothing on
         * this path re-allocates a block it has already filled, and silently
         * copying the wrong length would be worse than not copying. */
        uint16_t h = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 8));
        uint32_t bytes = (uint32_t)mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 4))
                       | ((uint32_t)mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 6)) << 16);
        if (!bytes) bytes = 16;
        if (h) ne_alloc(h, NULL, 0, bytes);
        cpu->ax = h;
        cpu->dx = 0;
        break;
    }
    case K_GLOBALUNLOCK:
    case K_GLOBALFREE:
        cpu->ax = 0;
        break;
    case K_GLOBALSIZE:
        cpu->ax = 0; cpu->dx = 0;
        break;
    case K_GETVERSION:
        cpu->ax = 0x0A03;      /* Windows 3.10 */
        break;
    default:
        fprintf(stderr, "unimplemented KERNEL.%u\n", ord);
        break;
    }
}

void ne_import(CPU *cpu, const char *module, uint16_t ordinal)
{
    (void)module;
    (void)cpu;
    (void)ordinal;
}

/* Lifted far calls become real C calls, so a call cycle becomes real recursion
 * and runs the host stack out instead of looping. That death is silent - the
 * fault handler needs stack it no longer has - so bound the depth and print the
 * tail of the call chain, which is what identifies the cycle. */
#define MAX_DEPTH 256
static void recomp_dispatch_inner(CPU *cpu, uint16_t seg, uint16_t off);
static unsigned g_depth;
static uint32_t g_chain[MAX_DEPTH];

void recomp_dispatch(CPU *cpu, uint16_t seg, uint16_t off)
{
    if (g_depth >= MAX_DEPTH) {
        fprintf(stderr, "recomp_dispatch: %u deep, last 16 calls:\n", g_depth);
        for (unsigned i = g_depth - 16; i < g_depth; i++)
            fprintf(stderr, "   %04X:%04X\n",
                    (uint16_t)(g_chain[i] >> 16), (uint16_t)g_chain[i]);
        abort();
    }
    g_chain[g_depth++] = ((uint32_t)seg << 16) | off;
    recomp_dispatch_inner(cpu, seg, off);
    g_depth--;
}

static void recomp_dispatch_inner(CPU *cpu, uint16_t seg, uint16_t off)
{
    /* A `retf` in lifted code pops the return address and dispatches to it,
     * which is right when the caller was also lifted. When the caller is C -
     * this runtime, or any lifted function reached by a direct call - there is
     * no address to go back to, and lift16 marks that by pushing 0xFFFF as the
     * return offset. Dispatching to it looks like a missing function and is
     * really just the end of the call. */
    if (off == 0xFFFF)
        return;

    if ((seg & 0xFF00) == 0xF000) {
        uint16_t mod = seg & 0xFF;
        note_import(mod, off);
        if (mod < NMODULES && !strcmp(MODULES[mod], "KERNEL"))
            kernel_import(cpu, off);
        /* Pascal convention: the callee pops its arguments. We do not know how
         * many without a prototype per ordinal, so the far return address is
         * dropped and the arguments are left - which keeps SP wrong rather
         * than silently plausible, and the import report says who to blame. */
        cpu->sp += 4;
        return;
    }

    if (seg == 2 || seg == 3) {
        unsigned pops = ne_call32(seg, off, cpu->ss, cpu->sp,
                                  cpu->ds, cpu->es);
        cpu->sp += (uint16_t)(4 + pops);
        return;
    }

    void (*fn)(CPU *) = find16(seg, off);
    if (!fn) {
        g_dispatch16_misses++;
        fprintf(stderr, "recomp_dispatch: no lifted entry for %04X:%04X\n",
                seg, off);
        cpu->sp += 4;
        return;
    }
    fn(cpu);
}

void ne_report_imports(void)
{
    if (!g_imports_n) {
        printf("imports: none reached\n");
        return;
    }
    printf("imports reached (%u distinct):\n", g_imports_n);
    for (unsigned i = 0; i < g_imports_n; i++) {
        const char *m = g_imports[i].mod < NMODULES
                      ? MODULES[g_imports[i].mod] : "?";
        printf("   %-9s ordinal %-5u x%lu\n", m, g_imports[i].ord,
               g_imports[i].hits);
    }
}

/* ---- helpers the 16-bit lifter emits calls to -------------------------
 *
 * Both are reached only from code that should not run. `int 0x21` is a DOS
 * call in a Windows DLL, which means the sweep walked into data; divide by
 * zero is the same story or a genuine bug. Neither is silently survivable, so
 * they say what happened and stop rather than returning a plausible value. */
void dos_int21(CPU *cpu)
{
    fprintf(stderr, "dos_int21: INT 21h reached (ax=%04X) - this is data being "
                    "executed, not code\n", cpu->ax);
    abort();
}

void catz_div0(void)
{
    fprintf(stderr, "catz_div0: divide by zero in lifted code\n");
    abort();
}



/* ---- call-cycle detection --------------------------------------------
 *
 * lift16 compiles a tail jump as a call, so a loop spanning two carved
 * functions is recursion, and a cycle runs the host stack out. That death is
 * silent: the structured handler needs stack it no longer has, and raising the
 * stack to half a gigabyte only makes it take longer. So count entries and
 * print the tail of the trail while there is still room.
 *
 * The budget is per top-level call, reset by ir32_enter_reset(). */
#define TRAIL 32
static unsigned long g_calls, g_budget = 2000000;
static uint16_t g_trail[TRAIL];
static unsigned g_trail_n;

void ir32_enter_reset(unsigned long budget)
{
    g_calls = 0;
    g_trail_n = 0;
    if (budget) g_budget = budget;
}

int ir32_enter(unsigned off)
{
    g_trail[g_trail_n++ % TRAIL] = (uint16_t)off;
    if (++g_calls < g_budget)
        return 0;
    if (g_calls == g_budget) {
        fprintf(stderr, "ir32_enter: %lu calls without returning - the last "
                        "%d entered:\n", g_calls, TRAIL);
        for (unsigned i = 0; i < TRAIL; i++)
            fprintf(stderr, "   %04X\n", g_trail[(g_trail_n + i) % TRAIL]);
    }
    return 1;      /* unwind: every frame returns at once */
}

/* ---- DPMI (INT 31h) ---------------------------------------------------
 *
 * A Win16 program in protected mode manipulates its own descriptors through
 * DPMI, and this driver uses it for exactly one thing:
 *
 *     mov bx, [bp+6]        ; a selector
 *     lea di, [bp-8]        ; an 8-byte descriptor buffer
 *     mov es, ax            ; ES:DI = buffer
 *     mov ax, 0x000B        ; get descriptor
 *     int 31h
 *     or byte [bp-2], 0x40  ; byte 6, bit 6 - the D/B bit
 *     mov ax, 0x000C        ; set descriptor
 *     int 31h
 *
 * It is marking its own segments 32-bit at load time. That is the same fact
 * the lift had to establish by decoding each segment both ways and comparing
 * junk rates, arrived at from the other direction - and it is why nothing in
 * the file records the width statically: the DLL sets it itself, at run time.
 *
 * Descriptors are kept as an 8-byte image per selector. Nothing here consults
 * them for addressing - g_segoff does that - so setting the D/B bit changes
 * no behaviour. Refusing the call, though, would stop the driver dead.
 */
#define DESC_MAX 4096
static unsigned char g_desc[DESC_MAX][8];
static unsigned char g_desc_init[DESC_MAX];
static uint16_t g_next_dpmi_sel = 0x0800;

static void desc_default(uint16_t sel)
{
    if (sel >= DESC_MAX || g_desc_init[sel])
        return;
    uint32_t base = g_selbase[sel];
    unsigned char *d = g_desc[sel];
    d[0] = 0xFF; d[1] = 0xFF;                    /* limit 15:0 */
    d[2] = (unsigned char)(base & 0xFF);
    d[3] = (unsigned char)((base >> 8) & 0xFF);
    d[4] = (unsigned char)((base >> 16) & 0xFF);
    d[5] = 0xF3;                                 /* present, ring 3, data, rw */
    d[6] = 0x40;                                 /* D/B set, limit 19:16 = 0 */
    d[7] = (unsigned char)((base >> 24) & 0xFF);
    g_desc_init[sel] = 1;
}

static void set_cf(CPU *cpu, int on)
{
    if (on) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF;
}

static void dpmi(CPU *cpu)
{
    uint16_t fn = cpu->ax;
    switch (fn) {
    case 0x0000: {                 /* allocate LDT descriptors, CX of them */
        uint16_t n = cpu->cx ? cpu->cx : 1;
        uint16_t first = g_next_dpmi_sel;
        for (uint16_t i = 0; i < n; i++)
            desc_default((uint16_t)(first + i));
        g_next_dpmi_sel = (uint16_t)(first + n);
        cpu->ax = first;
        set_cf(cpu, 0);
        break;
    }
    case 0x000B: {                 /* get descriptor: BX -> ES:DI */
        desc_default(cpu->bx);
        if (cpu->bx < DESC_MAX)
            for (int i = 0; i < 8; i++)
                mem_write8(cpu, cpu->es, (uint16_t)(cpu->di + i), g_desc[cpu->bx][i]);
        set_cf(cpu, 0);
        break;
    }
    case 0x000C: {                 /* set descriptor: ES:DI -> BX */
        if (cpu->bx < DESC_MAX) {
            for (int i = 0; i < 8; i++)
                g_desc[cpu->bx][i] = mem_read8(cpu, cpu->es, (uint16_t)(cpu->di + i));
            g_desc_init[cpu->bx] = 1;
        }
        set_cf(cpu, 0);
        break;
    }
    case 0x0006:                   /* get segment base -> CX:DX */
        cpu->cx = (uint16_t)(g_selbase[cpu->bx] >> 16);
        cpu->dx = (uint16_t)(g_selbase[cpu->bx] & 0xFFFF);
        set_cf(cpu, 0);
        break;
    case 0x0007:                   /* set segment base, 0008 limit, 0009 rights */
    case 0x0008:
    case 0x0009:
        /* Accepted and ignored: the base a program asks for is meaningless
         * here, since g_segoff already says where the segment is. */
        set_cf(cpu, 0);
        break;
    default:
        fprintf(stderr, "DPMI %04X not implemented\n", fn);
        set_cf(cpu, 1);
        break;
    }
}

/* `int N` from lifted 16-bit code. INT 31h is DPMI and expected; anything else
 * in a Windows DLL means execution walked into data. */
void int_handler(CPU *cpu, unsigned char n)
{
    if (n == 0x31) {
        dpmi(cpu);
        return;
    }
    fprintf(stderr, "int_handler: INT %02Xh reached (ax=%04X) - data being "
                    "executed, not code\n", n, cpu->ax);
    abort();
}
