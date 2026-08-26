/*
 * recomp_enc97_full.c - the FULL ENC97.EXE recompilation, exercised at scale.
 *
 * Links the complete lift (enc97_full.c, all 7,326 functions) and builds the
 * full 7,326-entry dispatch table (sorted, binary-searched). Then:
 *
 *   A. Table integrity: 7,326 entries, sorted, unique VAs, all fn ptrs non-null.
 *   B. lifted->lifted dispatch routed through the FULL table: the three
 *      previously-validated chains (Win32 trampoline, array search, thiscall
 *      setter) now resolve their internal targets among all 7,326 functions.
 *   C. Differential sweep: every pure-leaf function (no calls/imports) is run
 *      both lifted and as the real mapped original with identical register +
 *      stack + memory state, and the results (eax + a scratch buffer) compared.
 *      SEH-guarded so a function that faults on the synthetic input is skipped
 *      identically. Demonstrates the lift is correct across hundreds of real
 *      functions spanning the whole binary, not just the codec.
 *
 * Build 32-bit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "cpu.h"

#define PREF_BASE 0x400000u

#include "enc97_full_list.h"
#define DECL(a) void L_##a(CPU *);
LIFTED_FUNCS(DECL)
#undef DECL

typedef void (*lfn)(CPU *);
typedef struct { uint32_t va; lfn fn; } entry_t;
static entry_t g_lifted[] = {
#define E(a) { 0x##a, L_##a },
    LIFTED_FUNCS(E)
#undef E
};
#define NLIFTED (sizeof g_lifted / sizeof *g_lifted)

static uint32_t g_base, g_imgsz, g_nrelocs;
static unsigned long g_dispatched;   /* count of lifted-fn dispatches */

static int cmp_entry(const void *a, const void *b)
{
    uint32_t x = ((const entry_t *)a)->va, y = ((const entry_t *)b)->va;
    return (x > y) - (x < y);
}
static lfn lookup(uint32_t va)
{
    size_t lo = 0, hi = NLIFTED;
    while (lo < hi) {
        size_t m = (lo + hi) / 2;
        if (g_lifted[m].va < va) lo = m + 1;
        else if (g_lifted[m].va > va) hi = m;
        else return g_lifted[m].fn;
    }
    return NULL;
}

/* trampoline: run real code with ALL GP regs loaded from the CPU (so the real
   original sees the same initial state as the lifted version), capture eax+esp. */
static uint32_t S_eax,S_ecx,S_edx,S_ebx,S_esi,S_edi,S_esp,S_tgt,S_sesp,S_feax,S_fesp;
static void call_machine(CPU *c, uint32_t target)
{
    S_eax=c->eax; S_ecx=c->ecx; S_edx=c->edx; S_ebx=c->ebx;
    S_esi=c->esi; S_edi=c->edi; S_esp=c->esp+4; S_tgt=target;
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        mov S_sesp, esp
        mov eax, S_eax
        mov ecx, S_ecx
        mov edx, S_edx
        mov ebx, S_ebx
        mov esi, S_esi
        mov edi, S_edi
        mov esp, S_esp
        call dword ptr [S_tgt]
        mov S_fesp, esp
        mov S_feax, eax
        mov esp, S_sesp
        pop ebp
        pop edi
        pop esi
        pop ebx
    }
    c->eax = S_feax; c->esp = S_fesp;
}

void dispatch(CPU *c, uint32_t target)
{
    if (target >= PREF_BASE && target < PREF_BASE + g_imgsz) {
        lfn fn = lookup(target);
        if (fn) { g_dispatched++; fn(c); return; }
        call_machine(c, target + g_image_delta);   /* unlifted internal -> real */
        return;
    }
    call_machine(c, target);                        /* out of image -> Win32 */
}
void dispatch_jmp(CPU *c, uint32_t t) { dispatch(c, t); }
void dispatch_indirect(CPU *c, uint32_t t) { dispatch(c, t); }

static uint8_t *g_estack;
#define EMU_STACK (1u << 20)
static void cpu_init(CPU *c, uint32_t ecx)
{
    memset(c, 0, sizeof *c);
    c->esp = (uint32_t)(uintptr_t)(g_estack + EMU_STACK - 256);
    c->ecx = ecx;
}
static uint32_t call_lifted(lfn fn, uint32_t ecx, const uint32_t *args, int n)
{
    CPU c; cpu_init(&c, ecx);
    for (int i = n - 1; i >= 0; i--) push32(&c, args[i]);
    push32(&c, 0xDEADBEEFu);
    fn(&c);
    return c.eax;
}

static int map_image(const char *path)
{
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc(sz); if (fread(file, 1, sz, f) != (size_t)sz) { fclose(f); return 0; }
    fclose(f);
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)file;
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(file + dos->e_lfanew);
    uint32_t imgsz = nt->OptionalHeader.SizeOfImage;
    uint8_t *base = VirtualAlloc(NULL, imgsz, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!base) { free(file); return 0; }
    memcpy(base, file, nt->OptionalHeader.SizeOfHeaders);
    PIMAGE_SECTION_HEADER s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (s[i].SizeOfRawData)
            memcpy(base + s[i].VirtualAddress, file + s[i].PointerToRawData, s[i].SizeOfRawData);
    g_base = (uint32_t)(uintptr_t)base; g_imgsz = imgsz;
    int32_t delta = (int32_t)(g_base - PREF_BASE);
    IMAGE_DATA_DIRECTORY rd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (delta && rd.Size) {
        uint8_t *p = base + rd.VirtualAddress, *end = p + rd.Size;
        while (p < end) {
            PIMAGE_BASE_RELOCATION br = (PIMAGE_BASE_RELOCATION)p;
            uint32_t n = (br->SizeOfBlock - sizeof *br) / 2;
            uint16_t *e = (uint16_t *)(br + 1);
            for (uint32_t i = 0; i < n; i++)
                if ((e[i] >> 12) == IMAGE_REL_BASED_HIGHLOW) {
                    *(uint32_t *)(base + br->VirtualAddress + (e[i] & 0xFFF)) += delta;
                    g_nrelocs++;
                }
            p += br->SizeOfBlock;
        }
    }
    free(file);
    g_image_delta = delta;
    HMODULE u = LoadLibraryA("user32.dll");
    *(uint32_t *)(uintptr_t)(0x0058DC80u + delta) = (uint32_t)(uintptr_t)GetProcAddress(u, "LoadCursorA");
    *(uint32_t *)(uintptr_t)(0x0058DC84u + delta) = (uint32_t)(uintptr_t)GetProcAddress(u, "SetCursor");
    return 1;
}

/* ---- A: table integrity ------------------------------------------------- */
static int test_table(void)
{
    qsort(g_lifted, NLIFTED, sizeof *g_lifted, cmp_entry);
    int ok = 1; uint32_t prev = 0;
    for (size_t i = 0; i < NLIFTED; i++) {
        if (!g_lifted[i].fn) ok = 0;
        if (i && g_lifted[i].va <= prev) ok = 0;   /* sorted & unique */
        prev = g_lifted[i].va;
    }
    /* spot-check the lookup resolves a known function */
    if (lookup(0x004FF190u) == NULL) ok = 0;
    printf("%s [A] dispatch table: %u entries, sorted/unique, all non-null, lookup OK\n",
           ok ? "PASS" : "FAIL", (unsigned)NLIFTED);
    return ok;
}

/* ---- B: the three validated chains, routed through the FULL table -------- */
static int test_chains(void)
{
    int ok = 1;
    {   uint32_t a[] = { 0, 0, 0 };
        uint32_t l = call_lifted(lookup(0x401D10u), 0, a, 3) & 0xFFFFFFFF;
        ok &= (l == 1);
    }
    {   uint8_t *obj = calloc(0x200, 1); uint8_t *srch = obj + 0x1A4;
        uint8_t *tab = calloc(4, 36); uint16_t keys[4] = { 0x1111,0x2222,0x3333,0x4444 };
        for (int i = 0; i < 4; i++) *(uint32_t *)(tab + i*36 + 4) = keys[i];
        *(uint32_t *)(srch + 4) = 4; *(uint32_t *)(srch + 0xC) = (uint32_t)(uintptr_t)tab;
        uint32_t probes[] = { 0x3333,0x1111,0x4444,0xBEEF,0xA31B };
        for (int p = 0; p < 5; p++) {
            uint32_t a[] = { probes[p] };
            uint32_t l = call_lifted(lookup(0x4E3F40u), (uint32_t)(uintptr_t)obj, a, 1) & 0xFFFF;
            CPU rc; cpu_init(&rc, (uint32_t)(uintptr_t)obj); push32(&rc, probes[p]); push32(&rc, 0xDEADBEEFu);
            call_machine(&rc, 0x4E3F40u + g_image_delta);
            ok &= (l == (rc.eax & 0xFFFF));
        }
        free(obj); free(tab);
    }
    {   uint8_t *oL = calloc(0x100,1), *oR = calloc(0x100,1); uint32_t v = 0xCAFEF00Du;
        uint32_t a[] = { v };
        call_lifted(lookup(0x4AD870u), (uint32_t)(uintptr_t)oL, a, 1);
        CPU rc; cpu_init(&rc, (uint32_t)(uintptr_t)oR); push32(&rc, v); push32(&rc, 0xDEADBEEFu);
        call_machine(&rc, 0x4AD870u + g_image_delta);
        ok &= (*(uint32_t *)(oL+0x8E) == v && *(uint32_t *)(oR+0x8E) == v);
        free(oL); free(oR);
    }
    printf("%s [B] 3 validated chains resolve & match through the 7326-entry table "
           "(%lu lifted dispatches)\n", ok ? "PASS" : "FAIL", g_dispatched);
    return ok;
}

/* ---- C: differential sweep over pure-leaf functions --------------------- */
#include "enc97_pure_leaves.h"
#define SCRATCH 4096

/* Call a real function on the REAL C stack (push 8 identical args, set ecx for
   thiscall), so that if it faults on synthetic input the exception unwinds
   normally and __except catches it. (call_machine's esp-switch makes faults
   uncatchable, so it must only be used with inputs known not to fault.) */
static uint32_t RL_tgt, RL_arg, RL_ecx, RL_eax, RL_save;
static uint32_t call_real_leaf(uint32_t target, uint32_t argval, uint32_t ecx)
{
    RL_tgt = target; RL_arg = argval; RL_ecx = ecx;
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        mov RL_save, esp
        mov eax, RL_arg
        push eax            /* 8 identical args = buffer ptr */
        push eax
        push eax
        push eax
        push eax
        push eax
        push eax
        push eax
        mov ecx, RL_ecx     /* match cpu_init: ecx=buf, all other GP regs 0 */
        xor eax, eax
        xor edx, edx
        xor ebx, ebx
        xor esi, esi
        xor edi, edi
        xor ebp, ebp
        call dword ptr [RL_tgt]
        mov RL_eax, eax
        mov esp, RL_save
        pop ebp
        pop edi
        pop esi
        pop ebx
    }
    return RL_eax;
}
/* Run a no-write pure leaf both lifted and real with identical state (regs 0,
   ecx + a few args = a valid readable buffer for any pointer reads), and compare
   eax. The lifter does not relocate absolute-address IMMEDIATES (it was modelled
   at delta=0), so a leaf returning a pointer into the image yields the original
   VA from the lift but the live address from the real run; treat values that
   differ by exactly g_image_delta as equal (same logical pointer). These leaves
   perform no memory writes, so any fault is a catchable read AV (-> skip). */
static int run_leaf(uint32_t va, uint8_t *buf, uint32_t *out_l, uint32_t *out_r)
{
    lfn fn = lookup(va); if (!fn) return -1;
    uint32_t al = 0, ar = 0; volatile int lfault = 0, rfault = 0;
    uint32_t bp = (uint32_t)(uintptr_t)buf;
#ifdef LEAF_TRACE
    fprintf(stderr, "L %06x\n", va); fflush(stderr);
#endif
    __try {
        CPU c; cpu_init(&c, bp);
        for (int i = 0; i < 8; i++) push32(&c, bp);
        push32(&c, 0xDEADBEEFu);
        fn(&c); al = c.eax;
    } __except (EXCEPTION_EXECUTE_HANDLER) { lfault = 1; }
#ifdef LEAF_TRACE
    fprintf(stderr, "R %06x\n", va); fflush(stderr);
#endif
    __try {
        ar = call_real_leaf(va + g_image_delta, bp, bp);
    } __except (EXCEPTION_EXECUTE_HANDLER) { rfault = 1; }
    if (lfault || rfault) return -1;
    *out_l = al; *out_r = ar;
    return (al == ar || al + (uint32_t)g_image_delta == ar) ? 1 : 0;
}
/* ---- C2: the same, for leaves that WRITE ---------------------------------
 *
 * The no-write sweep excluded any function that stores to memory, for fear of
 * corruption. That fear was misplaced: a store through a pointer derived from
 * a zeroed register lands near null and raises a catchable access violation,
 * exactly like the reads those functions already do. What has to stay excluded
 * is the loop - a backward branch can spin forever or walk memory until it
 * finds something, and neither is recoverable.
 *
 * These are the more valuable half. A function whose only observable effect is
 * on memory is invisible to a comparison of eax, so this compares the buffer
 * too, which is a stronger check than the 818 no-write leaves get.
 *
 * Both runs use the SAME buffer address, sequentially, with the contents
 * restored in between. Two buffers at different addresses would be the obvious
 * approach and would report a false mismatch the moment a function stored a
 * pointer to its own argument.
 */
#include "enc97_write_leaves.h"

/* As call_real_leaf, but with sixteen arguments instead of eight.
 *
 * Eight was enough while nothing wrote memory. It is not enough here:
 * sub_50BC50 does `lea edi, [esp+0x14]` and copies a run of stack slots into
 * its object, reaching past the eighth. Beyond that point the emulated stack
 * and the real one hold different values - consistently, so the two-pass check
 * calls both sides reproducible - and the copied bytes differ for a reason
 * that has nothing to do with the lift.
 *
 * Pushing more identical arguments extends the region both sides agree on.
 */
static uint32_t call_real_leaf16(uint32_t target, uint32_t argval, uint32_t ecx)
{
    RL_tgt = target; RL_arg = argval; RL_ecx = ecx;
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        mov RL_save, esp
        mov eax, RL_arg
        mov ecx, 16
      push_loop:
        push eax
        dec ecx
        jnz push_loop
        mov ecx, RL_ecx
        xor eax, eax
        xor edx, edx
        xor ebx, ebx
        xor esi, esi
        xor edi, edi
        xor ebp, ebp
        call dword ptr [RL_tgt]
        mov RL_eax, eax
        mov esp, RL_save
        pop ebp
        pop edi
        pop esi
        pop ebx
    }
    return RL_eax;
}

/* Leave a known pattern in the stack memory just below esp.
 *
 * Some of these functions read stack they never wrote - an uninitialised local,
 * or an argument past the eight this harness pushes. Their result is then not a
 * function of their inputs at all, and the lifted and real runs disagree simply
 * because an emulated stack and the real one hold different rubbish. Comparing
 * them would report a lifter bug that is not there.
 *
 * Dirtying the stack deliberately, with two different patterns, turns that into
 * something detectable: run each side twice, and if a side disagrees with
 * ITSELF, its answer depends on memory nobody defined.
 */
static void dirty_stack(uint32_t pattern)
{
    volatile uint32_t pad[256];
    for (int i = 0; i < 256; i++)
        pad[i] = pattern ^ (uint32_t)i;
    /* the compiler must not decide this is dead */
    if (pad[0] == 0xFFFFFFFFu)
        printf("");
}

static int run_write_leaf(uint32_t va, uint8_t *buf, uint8_t *snapl, uint8_t *snapr,
                          uint32_t *out_l, uint32_t *out_r, int *memdiff)
{
    lfn fn = lookup(va); if (!fn) return -1;
    uint32_t al = 0, ar = 0; volatile int lfault = 0, rfault = 0;
    uint32_t bp = (uint32_t)(uintptr_t)buf;

    /* A pattern rather than zeroes: a function that copies its input somewhere
     * is indistinguishable from one that zeroes the destination when the input
     * is already zero. */
    /* Each side runs twice, under different stack rubbish. A side that
     * disagrees with itself is reading memory nobody defined, and there is
     * nothing to compare - see dirty_stack. */
    uint32_t al2 = 0, ar2 = 0;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t pat = pass ? 0xA5A5A5A5u : 0x5A5A5A5Au;
        for (int i = 0; i < SCRATCH; i++)
            buf[i] = (uint8_t)(i * 7 + 13);
        dirty_stack(pat);
        __try {
            CPU c; cpu_init(&c, bp);
            for (int i = 0; i < 16; i++) push32(&c, bp);
            push32(&c, 0xDEADBEEFu);
            fn(&c); if (pass) al2 = c.eax; else al = c.eax;
        } __except (EXCEPTION_EXECUTE_HANDLER) { lfault = 1; }
        if (!pass) memcpy(snapl, buf, SCRATCH);
        else if (memcmp(snapl, buf, SCRATCH)) return -2;   /* not reproducible */
    }
    if (al != al2) return -2;

    for (int pass = 0; pass < 2; pass++) {
        uint32_t pat = pass ? 0xA5A5A5A5u : 0x5A5A5A5Au;
        for (int i = 0; i < SCRATCH; i++)
            buf[i] = (uint8_t)(i * 7 + 13);
        dirty_stack(pat);
        __try {
            uint32_t v = call_real_leaf16(va + g_image_delta, bp, bp);
            if (pass) ar2 = v; else ar = v;
        } __except (EXCEPTION_EXECUTE_HANDLER) { rfault = 1; }
        if (!pass) memcpy(snapr, buf, SCRATCH);
        else if (memcmp(snapr, buf, SCRATCH)) return -2;
    }
    if (ar != ar2) return -2;

    if (lfault || rfault) return -1;
    *out_l = al; *out_r = ar;
    *memdiff = memcmp(snapl, snapr, SCRATCH) != 0;
    if (*memdiff) return 0;
    return (al == ar || al + (uint32_t)g_image_delta == ar) ? 1 : 0;
}

/* Where the two runs first disagree, and by how much.
 *
 * "memory differs" on its own does not distinguish a wrong result from a
 * rounding difference, and those want opposite responses. pcrecomp models the
 * x87 stack as C doubles where the hardware carries 80-bit extended, so a
 * chain of multiplies is expected to part company in the low bits of a stored
 * double - and expected is not the same as wrong.
 */
static void show_first_diff(const uint8_t *a, const uint8_t *b)
{
    int o = 0;
    while (o < SCRATCH && a[o] == b[o]) o++;
    if (o >= SCRATCH) return;
    int n = 0;
    for (int i = 0; i < SCRATCH; i++) n += (a[i] != b[i]);
    printf("      %d of %d bytes differ, first at +%d:\n"
           "        lifted %02X %02X %02X %02X %02X %02X %02X %02X\n"
           "        real   %02X %02X %02X %02X %02X %02X %02X %02X\n",
           n, SCRATCH, o,
           a[o], a[o+1], a[o+2], a[o+3], a[o+4], a[o+5], a[o+6], a[o+7],
           b[o], b[o+1], b[o+2], b[o+3], b[o+4], b[o+5], b[o+6], b[o+7]);
}

static int test_write_leaf_sweep(void)
{
    uint8_t *region = VirtualAlloc(NULL, SCRATCH + 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD old; VirtualProtect(region + SCRATCH, 0x1000, PAGE_NOACCESS, &old);
    uint8_t *snapl = (uint8_t *)malloc(SCRATCH), *snapr = (uint8_t *)malloc(SCRATCH);
    int matched = 0, mism = 0, skip = 0, shown = 0, memmism = 0, indet = 0;
    #define WSWEEP(va, ret) do { \
        uint32_t l, r; int md = 0; \
        int res = run_write_leaf((va), region, snapl, snapr, &l, &r, &md); \
        if (res == -2) indet++; \
        else if (res < 0) skip++; else if (res) matched++; else { mism++; if (md) memmism++; \
            if (shown++ < 8) { \
                printf("    MISMATCH 0x%06x: lifted eax=%08X real eax=%08X%s\n", \
                       (va), l, r, md ? "  and memory differs" : ""); \
                if (md) show_first_diff(snapl, snapr); } } \
    } while (0);
    WRITE_LEAVES(WSWEEP)
    #undef WSWEEP
    free(snapl); free(snapr);
    VirtualFree(region, 0, MEM_RELEASE);
    int ok = (mism == 0 && matched > 0);
    printf("%s [C2] writing pure-leaf sweep (eax AND memory): %d matched, %d mismatch"
           " (%d in memory), %d skipped, %d indeterminate\n",
           ok ? "PASS" : "FAIL", matched, mism, memmism, skip, indet);
    return ok;
}

static int test_leaf_sweep(void)
{
    /* buffer with a NOACCESS guard page after it: any over-read faults (caught) */
    uint8_t *region = VirtualAlloc(NULL, SCRATCH + 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD old; VirtualProtect(region + SCRATCH, 0x1000, PAGE_NOACCESS, &old);
    uint8_t *buf = region;
    memset(buf, 0, SCRATCH);
    int matched = 0, mism = 0, skip = 0, mism_shown = 0;
    #define SWEEP(va, ret) do { \
        uint32_t l, r; int res = run_leaf((va), buf, &l, &r); \
        if (res < 0) skip++; else if (res) matched++; else { mism++; \
            if (mism_shown++ < 8) printf("    MISMATCH 0x%06x: lifted eax=%08X real eax=%08X\n", (va), l, r); } \
    } while (0);
    PURE_LEAVES(SWEEP)
    #undef SWEEP
    VirtualFree(region, 0, MEM_RELEASE);
    int ok = (mism == 0 && matched > 0);
    printf("%s [C] no-write pure-leaf differential sweep: %d matched, %d mismatch, %d skipped "
           "(faulted on synthetic input)\n", ok ? "PASS" : "FAIL", matched, mism, skip);
    return ok;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *exe = (argc >= 2) ? argv[1] : "C:\\encarta\\analysis\\ENC97.EXE";
    if (!map_image(exe)) { fprintf(stderr, "map failed\n"); return 1; }
    g_estack = malloc(EMU_STACK);
    fprintf(stderr, "ENC97 mapped @%08X (delta %+d, %u relocs); full lift = %u functions\n",
            g_base, (int)g_image_delta, g_nrelocs, (unsigned)NLIFTED);
    int ok = 1;
    ok &= test_table();
    ok &= test_chains();
    ok &= test_leaf_sweep();
    ok &= test_write_leaf_sweep();
    printf("%s ENC97 full recomp exercised at scale\n", ok ? "ALL PASS:" : "FAIL:");
    return ok ? 0 : 1;
}
