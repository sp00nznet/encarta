/*
 * recomp_enc97_run.c - BOOT the lifted ENC97.EXE: execute the recompiled entry
 * point (start @0x50DB70) through the 7,326-function dispatch table, with the
 * full 914-import IAT wired to real Win32/MFC/CRT.
 *
 * Internal calls (direct original-VA or indirect relocated live-VA) route to the
 * lifted C function if present, else fall back to the real mapped original.
 * Imports (addresses outside the image) are real DLL functions called through an
 * esp-switch trampoline. The lifted entry runs on a big worker stack under a
 * watchdog; we report how far the recompiled code gets (call count, last lifted
 * function, whether a window appeared) before the watchdog stops it.
 *
 * Build 32-bit. Needs the generated enc97_full.c (the full lift).
 *
 * Env switches (diagnostics; all optional):
 *   R2L_VTABLES      route .rdata/.data function-pointer slots to lifted code
 *   R2L_LO / R2L_HI  route only slots [LO,HI) - binary-search a bad slot
 *   R2L_TEST         unit-test the real->lifted thiscall trampoline, then exit
 *   R2L_HEAPCHECK=1  HeapValidate after each real->lifted call; =2 after every call
 *   R2L_TRACE=1      log real->lifted entries; =2 also log every call inside one
 *   RUN_TRACE=N      log the first N import calls
 *   ENC97_REDIRECT=FROM=TO   serve the content tree from TO instead of
 *                    FROM, so it can live on a hard disk
 *   ENC97_CDROM=X    make drive X: answer as a CD-ROM, so content can live
 *                    on a hard disk (with ENC97_CDLABEL for its label)
 *   IR32_DLL         where IR32.DLL is, for the video codec bridge
 *   NO_VIDEO         do not register the recompiled Indeo decoder
 *   MSGBOX_LOG       log the app's message boxes and answer OK without showing them
 *   NO_PRINTDLG      stub PrintDlgA -> FALSE (the startup printer query can stall)
 *   HOLD             keep running after the app's window appears (to look at it)
 *   REG_LOG          log the registry keys/values the app looks for at startup
 *   FILE_LOG         log files the app FAILS to open (=all logs successes too)
 *   WATCH=va,va,...  log dispatches to these original VAs and what they return
 *   LIFT_LO/LIFT_HI  only table indices [LO,HI) run lifted, rest run real -
 *                    bisect this to find the function whose lift is wrong
 *   ENC97_PROFILE    "CodePath=...;DATPath=...;BookPath=..." - answer the app's
 *                    97Options lookups from here instead of the registry, so it
 *                    can be pointed at content without running Setup
 * Isolation modes for a slot that misbehaves - each removes one layer:
 *   R2L_PASSTHRU     rewritten slot jumps straight at the original (no trampoline)
 *   R2L_STUB         trampoline returns 0 without running the callee
 *   R2L_REAL=1       run the ORIGINAL code through the trampoline (not the lift)
 *   R2L_REAL=2       ...and without the esp switch
 * Together these say whether a failure is in the lift, the slot rewrite, the
 * trampoline's calling convention, or the stack switch.
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

static uint8_t *g_base;
static uint32_t g_imgsz;
static volatile unsigned long g_calls;
static volatile uint32_t g_last;        /* last internal target dispatched */
static volatile uint32_t g_last_import; /* last import target */
static volatile int g_done;
static int g_trace;   /* RUN_TRACE: log the first N import calls of the boot */
static uint32_t g_initterm;            /* resolved MSVCRT40!_initterm address */
static uint32_t g_acmdln;              /* resolved MSVCRT40!__p__acmdln address */
static unsigned long g_initfns;        /* C++/CRT init fns routed through lifted dispatch */
static unsigned long g_r2l_calls;      /* real->lifted (vtable/fn-ptr) calls */
static int g_heapcheck;                /* R2L_HEAPCHECK: 1=after each r2l call, 2=after every call */
static int g_r2l_trace;                /* R2L_TRACE: log each real->lifted entry */
static int g_r2l_real;                 /* R2L_REAL: run the ORIGINAL fn on the emulated frame */
static int g_r2l_stub;                 /* R2L_STUB: return 0 immediately (convention check) */
static int g_r2l_passthru;             /* R2L_PASSTHRU: rewritten slot jumps to the original */
static uint32_t g_watch[32]; static int g_nwatch;   /* WATCH: original VAs to log */
static char g_profile[1024];           /* ENC97_PROFILE: Name=Value;... overlay */
static const char *g_disp_kind = "call";   /* which dispatch entry point we are in */
#define TRAIL 16
static uint32_t g_trail[TRAIL]; static unsigned g_ntrail;   /* recent internal targets */
/* resolved-address -> "dll!name" for tracing which import the boot is in */
static struct { uint32_t addr; char name[64]; } g_inames[1024];
static int g_ninames;
static const char *imp_name(uint32_t addr)
{
    for (int i = 0; i < g_ninames; i++) if (g_inames[i].addr == addr) return g_inames[i].name;
    return "?";
}

static int cmp_entry(const void *a, const void *b)
{ uint32_t x=((const entry_t*)a)->va, y=((const entry_t*)b)->va; return (x>y)-(x<y); }
/* LIFT_LO/LIFT_HI: only functions whose index in the sorted table falls in
   [LO,HI) run lifted; the rest fall back to the real original. Bisecting this
   range finds the single function whose LIFT is wrong, the same way R2L_LO/HI
   finds the slot whose ROUTING is wrong. */
static int g_lift_lo, g_lift_hi = 1<<30;
static lfn lookup(uint32_t va)
{
    size_t lo=0, hi=NLIFTED;
    while (lo<hi){ size_t m=(lo+hi)/2;
        if (g_lifted[m].va<va) lo=m+1; else if (g_lifted[m].va>va) hi=m;
        else return ((int)m >= g_lift_lo && (int)m < g_lift_hi) ? g_lifted[m].fn : NULL; }
    return NULL;
}
static int lifted_index(uint32_t va)
{
    size_t lo=0, hi=NLIFTED;
    while (lo<hi){ size_t m=(lo+hi)/2;
        if (g_lifted[m].va<va) lo=m+1; else if (g_lifted[m].va>va) hi=m; else return (int)m; }
    return -1;
}

/* esp-switch trampoline: run real code (import or real internal) on the emulated
   stack, loading GP regs from the CPU and capturing eax+esp afterwards. */
static uint32_t T_eax,T_ecx,T_edx,T_ebx,T_esi,T_edi,T_ebp,T_espp4,T_tgt,T_fesp,T_sesp;
#pragma warning(disable:4731)   /* we clobber ebp on purpose; push/pop restores it */
static void call_machine(CPU *c, uint32_t target)
{
    /* Reentrant: the real code we call can call BACK into lifted code (vtable
       routing), which dispatches imports through call_machine again. The inner
       call would otherwise overwrite T_sesp - the outer's saved host esp - and
       the outer would restore a bogus esp and return into hyperspace. */
    uint32_t sv_eax=T_eax, sv_ecx=T_ecx, sv_edx=T_edx, sv_ebx=T_ebx, sv_esi=T_esi,
             sv_edi=T_edi, sv_ebp=T_ebp, sv_espp4=T_espp4, sv_tgt=T_tgt,
             sv_fesp=T_fesp, sv_sesp=T_sesp;
    T_eax=c->eax; T_ecx=c->ecx; T_edx=c->edx; T_ebx=c->ebx; T_esi=c->esi; T_edi=c->edi;
    /* ebp too: MSVC emits frameless funclets (SEH unwind / local-object dtor
       helpers, e.g. `lea ecx,[ebp-0x10]; jmp ~CString`) that address the CALLER's
       frame. Leaving the host's ebp there makes them operate on garbage. */
    T_ebp=c->ebp;
    T_espp4=c->esp+4; T_tgt=target;
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        mov T_sesp, esp
        mov eax, T_eax
        mov ecx, T_ecx
        mov edx, T_edx
        mov ebx, T_ebx
        mov esi, T_esi
        mov edi, T_edi
        mov ebp, T_ebp
        mov esp, T_espp4
        call dword ptr [T_tgt]
        mov T_fesp, esp
        mov T_eax, eax
        mov T_edx, edx
        mov esp, T_sesp
        pop ebp
        pop edi
        pop esi
        pop ebx
    }
    /* edx too: a 64-bit return comes back in edx:eax, and this codebase passes
       such pairs around constantly (`mov [ebp-8],eax; mov [ebp-4],edx`).
       Dropping edx silently truncates every one of them. */
    c->eax=T_eax; c->edx=T_edx; c->esp=T_fesp;
    T_eax=sv_eax; T_ecx=sv_ecx; T_edx=sv_edx; T_ebx=sv_ebx; T_esi=sv_esi;
    T_edi=sv_edi; T_ebp=sv_ebp; T_espp4=sv_espp4; T_tgt=sv_tgt;
    T_fesp=sv_fesp; T_sesp=sv_sesp;
}

static uint8_t *g_tramp_pool; static size_t g_tramp_off;   /* (defined below; fwd for dispatch) */

/* HeapValidate over every process heap; 0 = corrupt. */
static int heaps_ok(void)
{
    HANDLE hs[64]; DWORD nh = GetProcessHeaps(64, hs);
    for (DWORD i = 0; i < nh; i++) if (!HeapValidate(hs[i], 0, NULL)) return 0;
    return 1;
}

static void dispatch_inner(CPU *c, uint32_t target);

/* R2L_HEAPCHECK=2: validate the heap after EVERY dispatched call made while
   inside a real->lifted call, so the corrupting call names itself. */
void dispatch(CPU *c, uint32_t target)
{
    if (g_r2l_trace > 1 && g_r2l_calls)
        fprintf(stderr, "   call[%lu] -> %08X ecx=%08X ebp=%08X esp=%08X [ebp-10]=%08X\n",
                g_calls + 1, target, c->ecx, c->ebp, c->esp, c->ebp ? rd32(c->ebp - 0x10) : 0),
        fflush(stderr);
    if (g_heapcheck < 2 || !g_r2l_calls) {
        unsigned long m = g_calls + 1;
        dispatch_inner(c, target);
        if (g_r2l_trace > 1 && g_r2l_calls)
            fprintf(stderr, "   ret [%lu] <- %08X eax=%08X esp=%08X\n", m, target, c->eax, c->esp),
            fflush(stderr);
        return;
    }
    unsigned long n = g_calls + 1;
    dispatch_inner(c, target);
    if (!heaps_ok()) {
        fprintf(stderr, "HEAP CORRUPT after call [%lu] -> 0x%08X (%s)\n",
                n, target, target >= PREF_BASE && target < PREF_BASE + g_imgsz ? "internal" : imp_name(target));
        fflush(stderr);
        printf("RESULT: BAD (heap corrupt after call %lu -> 0x%08X)\n", n, target);
        fflush(stdout);
        ExitProcess(2);
    }
}

static void dispatch_inner(CPU *c, uint32_t target)
{
    g_calls++;
    /* a redirected vtable/fn-pointer slot may hand us a trampoline address; the
       lifted target's original-VA is the `mov eax, ova` immediate at stub+1. */
    if (g_tramp_pool && target >= (uint32_t)(uintptr_t)g_tramp_pool &&
        target < (uint32_t)(uintptr_t)g_tramp_pool + g_tramp_off) {
        uint32_t ova = *(uint32_t *)(uintptr_t)(target + 1);
        lfn fn = lookup(ova);
        if (fn) { g_last = ova; fn(c); return; }
    }
    uint32_t ova = 0;
    if (target >= PREF_BASE && target < PREF_BASE + g_imgsz)
        ova = target;                                   /* direct internal (original VA) */
    else if (target >= (uint32_t)(uintptr_t)g_base &&
             target <  (uint32_t)(uintptr_t)g_base + g_imgsz)
        ova = target - g_image_delta;                   /* indirect relocated live internal */
    if (ova) {
        g_last = ova;
        g_trail[g_ntrail++ & (TRAIL - 1)] = ova;   /* recent internal targets */
        int watched = 0;                 /* WATCH=va,va,...: log these calls + result */
        for (int w = 0; w < g_nwatch; w++) if (g_watch[w] == ova) { watched = 1; break; }
        if (watched)
            fprintf(stderr, "  watch -> 0x%06X(ecx=%08X, %08X, %08X)\n",
                    ova, c->ecx, rd32(c->esp + 4), rd32(c->esp + 8)), fflush(stderr);
        lfn fn = lookup(ova);
        if (fn) fn(c); else call_machine(c, ova + g_image_delta);  /* else real original */
        if (watched)
            fprintf(stderr, "  watch <- 0x%06X returned %08X\n", ova, c->eax), fflush(stderr);
        return;
    }
    if (!target) {   /* indirect call through a null pointer - name the caller */
        /* the lifted call site pushed its return address, so [esp] is the
           instruction right after the call that went through the null. */
        fprintf(stderr, "NULL %s after %lu calls, from call site just before 0x%06X;"
                " last internal targets:\n", g_disp_kind, g_calls, rd32(c->esp));
        for (unsigned t = (g_ntrail >= TRAIL ? g_ntrail - TRAIL : 0); t < g_ntrail; t++)
            fprintf(stderr, "    0x%06X\n", g_trail[t & (TRAIL - 1)]);
        /* nearly always means the IAT (or a fn-ptr table) got written over */
        { int zero = 0, seen = 0;
          for (uint32_t a = 0x58D000; a < 0x58E000; a += 4) {
              uint32_t v = rd32(a + g_image_delta);
              if (v) seen++; else zero++;
          }
          fprintf(stderr, "  .idata window 0x58D000-0x58E000: %d live, %d zero\n", seen, zero);
          fprintf(stderr, "  [58D438]=%08X [58D440]=%08X [58168C]=%08X\n",
                  rd32(0x58D438 + g_image_delta), rd32(0x58D440 + g_image_delta),
                  rd32(0x58168C + g_image_delta)); }
        fflush(stderr);
        printf("RESULT: BAD (null indirect call from 0x%06X)\n", g_last); fflush(stdout);
        ExitProcess(3);
    }
    g_last_import = target;
    /* Intercept _initterm: walk the C++/CRT init-pointer table and route each
       entry through our dispatch so the static initializers run as LIFTED code
       (this is the real->lifted path that real MFC would otherwise call directly). */
    if (target == g_initterm && g_initterm) {
        uint32_t b = rd32(c->esp + 4), e = rd32(c->esp + 8);
        for (uint32_t p = b; p < e; p += 4) {
            uint32_t fn = rd32(p);
            if (fn) { g_initfns++; push32(c, 0xDEADBEEFu); dispatch(c, fn); }
        }
        c->eax = 0; c->esp += 4;     /* mimic cdecl ret; caller cleans the 2 args */
        return;
    }
    const char *nm = imp_name(target);
    if (g_trace && g_calls <= (unsigned)g_trace)
        fprintf(stderr, "  [%lu] import %s\n", g_calls, nm), fflush(stderr);
    /* the app terminating means the lifted entry drove CRT+MFC startup to its
       end; report the milestone cleanly instead of vanishing inside real exit(). */
    if (!strcmp(nm,"MSVCRT40.dll!exit") || !strcmp(nm,"MSVCRT40.dll!_exit") ||
        !strcmp(nm,"KERNEL32.dll!ExitProcess")) {
        uint32_t code = rd32(c->esp + 4);
        fprintf(stderr, "\n*** lifted ENC97 reached %s(%u) after %lu dispatched calls\n", nm, code, g_calls);
        fflush(stderr);
        printf("ENC97 lifted boot: ran CRT+MFC startup to %s(%u) — %lu calls dispatched "
               "(%lu init fns, %lu real->lifted vtable calls), last lifted fn 0x%06X\n",
               nm, code, g_calls, g_initfns, g_r2l_calls, g_last);
        printf("RESULT: OK\n");
        fflush(stdout);
        ExitProcess(0);
    }
    call_machine(c, target);                            /* import -> real DLL */
}
void dispatch_jmp(CPU *c, uint32_t t){ g_disp_kind = "jmp"; dispatch(c,t); g_disp_kind = "call"; }
void dispatch_indirect(CPU *c, uint32_t t){ g_disp_kind = "indirect"; dispatch(c,t); g_disp_kind = "call"; }

static uint8_t *g_estack;
#define EMU_STACK (4u << 20)
static uint8_t *g_r2l_arena; static uint32_t g_r2l_top;
#define R2L_ARENA (8u<<20)
#define R2L_FRAME 0x8000u
static uint32_t call_lifted(lfn fn, const uint32_t *args, int n)
{
    CPU c; memset(&c,0,sizeof c);
    c.esp = (uint32_t)(uintptr_t)(g_estack + EMU_STACK - 4096);
    for (int i=n-1;i>=0;i--) push32(&c,args[i]);
    push32(&c, 0xDEADBEEFu);
    fn(&c);
    return c.eax;
}

/* ================= real -> lifted thiscall trampoline =====================
 * The inverse of call_machine: when REAL code (e.g. MFC virtual dispatch) calls
 * a function pointer that we've redirected here, run the LIFTED function instead.
 * A per-target stub (`mov eax, ova; jmp r2l_common`) lands in r2l_common with
 * eax=target original-VA, ecx=this, [esp]=retaddr, args above. r2l_helper copies
 * the args onto a private emulated-stack frame (so the lifted code's pushes can't
 * collide with the real stack), runs the lifted fn, and reports how many arg
 * bytes its `ret N` cleaned so the trampoline returns thiscall-correctly.
 * Reentrant: nesting frames are LIFO on a dedicated arena; result+pop come back
 * in edx:eax (no globals). Note the arena can't be the real stack: the harness's
 * own C frames descend from there while the lifted code runs, and the two would
 * interleave. */

static uint64_t __cdecl r2l_helper(uint32_t ova, uint32_t this_, uint32_t *real_args,
                                   uint32_t ebx, uint32_t esi, uint32_t edi, uint32_t ebp)
{
    uint32_t save = g_r2l_top;
    g_r2l_top -= R2L_FRAME;
    uint32_t argsp = g_r2l_top + R2L_FRAME - 0x100;     /* args near frame top */
    for (int i = 0; i < 16; i++) wr32(argsp + 4 + i*4, real_args[i]);
    wr32(argsp, 0xDEADBEEFu);                            /* fake return slot */
    CPU c; memset(&c, 0, sizeof c);
    /* seed all GP regs from the real caller: some routed targets are thunks that
       use the CALLER's ebp (e.g. `lea ecx,[ebp-0x10]`) without a frame of own. */
    c.ecx = this_; c.ebx = ebx; c.esi = esi; c.edi = edi; c.ebp = ebp; c.esp = argsp;
    g_r2l_calls++;
    if (g_r2l_trace)
        fprintf(stderr, "  r2l #%lu -> 0x%06X this=%08X ret=%08X args=%08X %08X %08X\n",
                g_r2l_calls, ova, this_, real_args[-1], real_args[0], real_args[1], real_args[2]),
        fflush(stderr);
    /* R2L_REAL: run the ORIGINAL code on the emulated frame instead of the lift.
       Separates "the lift is wrong" from "the emulated-stack boundary is wrong". */
    /* R2L_STUB: return 0 without running anything - isolates the trampoline's
       return convention from what the callee actually did. */
    if (g_r2l_stub)      { c.eax = 0; c.esp = argsp + 4; }
    else if (g_r2l_real == 2) {        /* original, NO esp switch: plain 0-arg thiscall
                                          on our own stack - isolates the stack switch */
        uint32_t t = ova + g_image_delta, r;
        __asm { mov ecx, this_
                mov eax, t
                call eax
                mov  r, eax }
        c.eax = r; c.esp = argsp + 4;
    }
    else if (g_r2l_real) call_machine(&c, ova + g_image_delta);
    else                 dispatch(&c, ova + g_image_delta);   /* -> lifted */
    if (g_heapcheck) {                                   /* pinpoint the corrupting call */
        HANDLE hs[64]; DWORD nh = GetProcessHeaps(64, hs);
        for (DWORD i = 0; i < nh; i++)
            if (!HeapValidate(hs[i], 0, NULL)) {
                fprintf(stderr, "HEAP CORRUPT after lifted 0x%06X (r2l #%lu, heap %p)\n",
                        ova, g_r2l_calls, hs[i]); fflush(stderr);
                printf("RESULT: BAD (heap corrupt after 0x%06X)\n", ova); fflush(stdout);
                ExitProcess(2);            /* stop at the FIRST corrupting call */
            }
    }
    uint32_t pop = (c.esp >= argsp + 4) ? (c.esp - argsp - 4) : 0;
    g_r2l_top = save;
    if (g_r2l_trace)
        fprintf(stderr, "  r2l #%lu <- 0x%06X eax=%08X pop=%u ret=%08X\n",
                g_r2l_calls, ova, c.eax, pop, real_args[-1]), fflush(stderr);
    return (uint64_t)c.eax | ((uint64_t)(pop & 0xFFFF) << 32);
}

__declspec(naked) static void r2l_common(void)
{
    __asm {
        push ebp
        mov  ebp, esp                  /* [ebp+4]=retaddr, [ebp+8]=arg0, [ebp]=caller ebp */
        push dword ptr [ebp]           /* caller's ebp (for ebp-relative thunks) */
        push edi
        push esi
        push ebx
        lea  edx, [ebp+8]
        push edx                       /* real_args */
        push ecx                       /* this */
        push eax                       /* ova */
        call r2l_helper                /* edx:eax = pop:result */
        add  esp, 28                   /* clean 7 cdecl args */
        mov  ecx, edx                  /* ecx = pop bytes */
        mov  edx, [ebp+4]              /* retaddr */
        mov  esp, ebp
        pop  ebp
        add  esp, 4                    /* pop retaddr */
        add  esp, ecx                  /* thiscall: callee cleans args */
        jmp  edx                       /* return (eax=result) */
    }
}

static uint32_t make_tramp(uint32_t ova)
{
    uint8_t *s = g_tramp_pool + g_tramp_off; g_tramp_off += 16;
    s[0]=0xB8; *(uint32_t*)(s+1)=ova;                     /* mov eax, ova */
    /* R2L_PASSTHRU: jump straight at the original instead of into the r2l path -
       same slot rewrite, no trampoline machinery. Isolates one from the other. */
    s[5]=0xE9; *(int32_t*)(s+6)=(int32_t)((g_r2l_passthru
        ? (uint8_t*)(uintptr_t)(ova + g_image_delta)
        : (uint8_t*)r2l_common) - (s+10));                /* jmp target */
    return (uint32_t)(uintptr_t)s;
}

/* Redirect ENC97's vtable / function-pointer slots to real->lifted trampolines,
   so when real MFC virtual-dispatches into the app it lands in LIFTED code. Only
   slots whose target is a FUNCTION START (lookup succeeds) are rewritten — this
   excludes jump tables (their entries are mid-function labels, not fn starts).
   Scans .rdata + .data (where vtables and fn-ptr tables live). */
static int g_r2l_lo, g_r2l_hi = 1<<30;   /* R2L_LO/R2L_HI: slot range to route */
static int g_nslots;                     /* total candidate slots seen */
static uint32_t g_slot_ova[65536];       /* slot index -> target original VA */
static int rewrite_fnptr_slots(void)
{
    PIMAGE_DOS_HEADER dos=(PIMAGE_DOS_HEADER)g_base;
    PIMAGE_NT_HEADERS nt=(PIMAGE_NT_HEADERS)(g_base+dos->e_lfanew);
    PIMAGE_SECTION_HEADER s=IMAGE_FIRST_SECTION(nt);
    uint32_t tlo=0,thi=0; uint32_t base=(uint32_t)(uintptr_t)g_base;
    for(int i=0;i<nt->FileHeader.NumberOfSections;i++)
        if(!memcmp(s[i].Name,".text",5)){ tlo=base+s[i].VirtualAddress;
            thi=tlo+s[i].Misc.VirtualSize; }
    /* A function-start pointer (points into .text at a lifted fn entry). */
    /* lifted_index, not lookup: "is this a function start" must not depend on
       LIFT_LO/LIFT_HI, or restricting the lifted set would silently change which
       slots get routed and confound the bisect. */
    #define IS_FNPTR(val) ((val)>=tlo && (val)<thi && lifted_index((val)-g_image_delta)>=0)
    const int MINVT=3;       /* only rewrite runs of >=3 (real vtables); a run of */
                             /* 3 consecutive valid fn-starts is not coincidence,  */
                             /* so this avoids clobbering data that merely looks    */
                             /* like a pointer. */
    int n=0, nr=0;      /* n = candidate slots seen (stable bisect index); nr = routed */
    for(int i=0;i<nt->FileHeader.NumberOfSections;i++){
        if(memcmp(s[i].Name,".rdata",6) && memcmp(s[i].Name,".data",5)) continue;
        uint32_t a=base+s[i].VirtualAddress, e=a+s[i].Misc.VirtualSize;
        for(uint32_t p=a; p+4<=e; ){
            uint32_t v=*(uint32_t*)(uintptr_t)p;
            if(IS_FNPTR(v)){
                uint32_t q=p; int run=0;             /* measure the run length */
                while(q+4<=e && IS_FNPTR(*(uint32_t*)(uintptr_t)q)){ run++; q+=4; }
                if(run>=MINVT)
                    for(uint32_t r=p;r<q;r+=4){
                        uint32_t ova=*(uint32_t*)(uintptr_t)r - g_image_delta;
                        /* R2L_LO/R2L_HI bisect: route only slots [lo,hi) of the
                           scan order, so a binary search can name the slot whose
                           routing breaks the boot. */
                        if(n<(int)(sizeof g_slot_ova/4)) g_slot_ova[n]=ova;
                        if(n>=g_r2l_lo && n<g_r2l_hi){
                            *(uint32_t*)(uintptr_t)r=make_tramp(ova); nr++;
                        }
                        n++;
                    }
                p=q;
            } else p+=4;
        }
    }
    g_nslots=n;
    return nr;
}

/* ---- map + relocate ENC97, then wire its full import table to real code ---- */
static char g_exedir[MAX_PATH];   /* dir of the ENC97.EXE we mapped */
static HMODULE load_for(const char *dll)
{
    char path[MAX_PATH];
    /* Encarta-private DLLs (EEUIL10, DECO_32, ENCAPI32) sit next to the exe. */
    snprintf(path,sizeof path,"%s%s",g_exedir,dll);
    if (GetFileAttributesA(path)!=INVALID_FILE_ATTRIBUTES) return LoadLibraryA(path);
    if (!_stricmp(dll,"MSVCRT40.dll")) return LoadLibraryA("msvcrt.dll");
    return LoadLibraryA(dll);
}
/* FILE_LOG: log the files the app opens, and loudly the ones it fails to find.
   An old application that says "not installed properly" is nearly always
   looking for a path its installer would have written; this says which. Set
   FILE_LOG=all to see successes too. */
static int g_file_log;
static HANDLE (WINAPI *real_createfile)(LPCSTR,DWORD,DWORD,void*,DWORD,DWORD,HANDLE);
static HFILE  (WINAPI *real_openfile)(LPCSTR,OFSTRUCT*,UINT);
static HFILE  (WINAPI *real_lopen)(LPCSTR,int);
static HANDLE (WINAPI *real_findfirst)(LPCSTR,LPWIN32_FIND_DATAA);

/* ENC97_REDIRECT=FROM=TO: serve content from somewhere other than the CD.
 *
 * Pointing BookPath at a local copy does not work, because the app never asks
 * for BookPath - it locates its books itself and opens absolute paths on the
 * drive it decided on. Whatever it decided, those opens come through here, so
 * rewriting the prefix is what actually moves the content.
 *
 *   ENC97_REDIRECT="H:\ENCYC97=G:\encarta97\ENCYC97"
 *
 * Prefix match, case-insensitive, first match wins. A path that does not start
 * with FROM is passed through untouched, so this cannot affect anything but
 * the content tree it is aimed at.
 */
static char g_redir_from[MAX_PATH], g_redir_to[MAX_PATH];
static int  g_redir_hits;

static const char *redirect(const char *name, char *buf, size_t bufsz)
{
    if (!g_redir_from[0] || !name)
        return name;
    size_t n = strlen(g_redir_from);
    if (_strnicmp(name, g_redir_from, n) != 0)
        return name;
    _snprintf(buf, bufsz, "%s%s", g_redir_to, name + n);
    buf[bufsz - 1] = 0;
    if (g_file_log > 1 && g_redir_hits < 20)
        fprintf(stderr, "  redir  %s -> %s\n", name, buf);
    g_redir_hits++;
    return buf;
}

static void file_note(const char *api, const char *name, int ok)
{
    if (!name) return;
    if (ok && g_file_log < 2) return;             /* misses only, unless FILE_LOG=all */
    fprintf(stderr, "  %s %-9s %s\n", ok ? "file  " : "MISS >", api, name);
    fflush(stderr);
}
static HANDLE WINAPI createfile_hook(LPCSTR name, DWORD acc, DWORD share, void *sa,
                                     DWORD disp, DWORD flags, HANDLE tmpl)
{
    char rb[MAX_PATH];
    name = redirect(name, rb, sizeof rb);
    HANDLE h = real_createfile(name, acc, share, sa, disp, flags, tmpl);
    file_note("CreateFile", name, h != INVALID_HANDLE_VALUE);
    return h;
}
static HFILE WINAPI openfile_hook(LPCSTR name, OFSTRUCT *of, UINT style)
{
    char rb[MAX_PATH];
    name = redirect(name, rb, sizeof rb);
    HFILE h = real_openfile(name, of, style);
    file_note("OpenFile", name, h != HFILE_ERROR);
    return h;
}
static HFILE WINAPI lopen_hook(LPCSTR name, int mode)
{
    char rb[MAX_PATH];
    name = redirect(name, rb, sizeof rb);
    HFILE h = real_lopen(name, mode);
    file_note("_lopen", name, h != HFILE_ERROR);
    return h;
}
static HANDLE WINAPI findfirst_hook(LPCSTR name, LPWIN32_FIND_DATAA fd)
{
    char rb[MAX_PATH];
    name = redirect(name, rb, sizeof rb);
    HANDLE h = real_findfirst(name, fd);
    file_note("FindFirst", name, h != INVALID_HANDLE_VALUE);
    return h;
}

/* ENC97_CDROM: make a local content directory answer as the CD.
 *
 * Encarta checks that its content lives on a CD-ROM and that the disc has the
 * right volume label, and refuses to run otherwise. That is the last thing
 * tying the whole project to a physical disc: point BookPath at a copy on a
 * hard drive and the app opens every file happily, then declines because the
 * drive is fixed and unlabelled.
 *
 *   ENC97_CDROM=G          answer for that drive letter
 *   ENC97_CDLABEL=<label>  the label to report (default CD1ENC97ENC, which is
 *                          what CD1 of Encarta 97 actually carries)
 *
 * Scoped to one drive letter and one process. Nothing is mounted, no drive is
 * emulated, and the machine is not touched - the app is simply told the truth
 * it expects about where its content is.
 */
static char g_cdrom_drive;                       /* 'G', or 0 when unset */
static char g_cdlabel[64] = "CD1ENC97ENC";

static int is_cdrom_path(const char *p)
{
    return g_cdrom_drive && p && p[0] && p[1] == ':' &&
           (p[0] | 32) == (g_cdrom_drive | 32);
}

static UINT (WINAPI *real_getdrivetype)(LPCSTR);
static UINT WINAPI getdrivetype_hook(LPCSTR root)
{
    if (is_cdrom_path(root)) {
        if (g_file_log)
            fprintf(stderr, "  cd     GetDriveType %s -> CDROM\n", root);
        return DRIVE_CDROM;
    }
    return real_getdrivetype(root);
}

static BOOL (WINAPI *real_getvolinfo)(LPCSTR, LPSTR, DWORD, LPDWORD, LPDWORD,
                                      LPDWORD, LPSTR, DWORD);
static BOOL WINAPI getvolinfo_hook(LPCSTR root, LPSTR name, DWORD namelen,
                                   LPDWORD serial, LPDWORD maxcomp,
                                   LPDWORD flags, LPSTR fs, DWORD fslen)
{
    if (is_cdrom_path(root)) {
        if (name && namelen) {
            strncpy(name, g_cdlabel, namelen - 1);
            name[namelen - 1] = 0;
        }
        if (serial)  *serial  = 0x1E97CD01;
        if (maxcomp) *maxcomp = 255;
        if (flags)   *flags   = FS_CASE_IS_PRESERVED;
        if (fs && fslen) {
            strncpy(fs, "CDFS", fslen - 1);
            fs[fslen - 1] = 0;
        }
        if (g_file_log)
            fprintf(stderr, "  cd     GetVolumeInformation %s -> %s (CDFS)\n",
                    root, g_cdlabel);
        return TRUE;
    }
    return real_getvolinfo(root, name, namelen, serial, maxcomp, flags, fs, fslen);
}

/* MSGBOX_LOG: log the app's message boxes and answer OK without showing them.
   Encarta's startup failures are reported this way, and the text names exactly
   what it thinks is missing - which a modal dialog under a watchdog does not. */
static int g_msgbox_log;
static int WINAPI msgbox_hook(HWND h, LPCSTR text, LPCSTR cap, UINT type)
{
    (void)h; (void)type;
    fprintf(stderr, "MessageBox [%s]: %s\n", cap ? cap : "", text ? text : "");
    fflush(stderr);
    return IDOK;
}

/* NO_PRINTDLG: answer "no printer" without asking the spooler. Encarta queries
   the default printer during startup; on a machine where that call stalls it
   never gets to its UI, which has nothing to do with the recompilation. */
static int g_no_printdlg;
static BOOL WINAPI printdlg_hook(void *pd)
{
    (void)pd;
    fprintf(stderr, "PrintDlgA stubbed -> FALSE\n"); fflush(stderr);
    return FALSE;
}

/* REG_LOG: log the registry the app consults at startup. Encarta expects its
   Setup to have recorded where the content lives; this says which keys and
   values it wants, and which ones came back empty. */
static int g_reg_log;
static LONG (WINAPI *real_regcreate)(HKEY,LPCSTR,DWORD,LPSTR,DWORD,REGSAM,void*,PHKEY,LPDWORD);
static LONG (WINAPI *real_regopen)(HKEY,LPCSTR,DWORD,REGSAM,PHKEY);
static LONG (WINAPI *real_regquery)(HKEY,LPCSTR,LPDWORD,LPDWORD,LPBYTE,LPDWORD);
static LONG WINAPI regcreate_hook(HKEY k, LPCSTR sub, DWORD r, LPSTR cls, DWORD o,
                                  REGSAM sam, void *sa, PHKEY out, LPDWORD disp)
{
    LONG rc = real_regcreate(k, sub, r, cls, o, sam, sa, out, disp);
    /* Encarta keeps its install paths under HKLM, which we can neither read
       (Setup never ran) nor create (no admin). With a profile overlay in play,
       fall back to HKCU so the open succeeds and the value query reaches it. */
    if (rc != ERROR_SUCCESS && k == HKEY_LOCAL_MACHINE && g_profile[0])
        rc = real_regcreate(HKEY_CURRENT_USER, sub, r, cls, o, sam, sa, out, disp);
    if (g_reg_log) fprintf(stderr, "  reg create %-60s -> %ld\n", sub ? sub : "(null)", rc);
    fflush(stderr); return rc;
}
static LONG WINAPI regopen_hook(HKEY k, LPCSTR sub, DWORD o, REGSAM sam, PHKEY out)
{
    LONG rc = real_regopen(k, sub, o, sam, out);
    fprintf(stderr, "  reg open   %-60s -> %ld\n", sub ? sub : "(null)", rc);
    fflush(stderr); return rc;
}
/* ENC97_PROFILE="Name=Value;Name=Value": answer the app's profile lookups from
   here instead of the registry, so it can be told where its content lives
   without running Setup or writing anything to the machine. Encarta stores
   these under 97Options - CodePath, DATPath and BookPath are the ones its
   startup refuses to run without. */
static const char *profile_lookup(const char *name)
{
    if (!name || !g_profile[0]) return NULL;
    size_t n = strlen(name);
    for (const char *p = g_profile; *p; ) {
        const char *eq = strchr(p, '='), *end = strchr(p, ';');
        if (!end) end = p + strlen(p);
        if (eq && eq < end && (size_t)(eq - p) == n && !_strnicmp(p, name, n))
            { static char v[512]; size_t vl = end - eq - 1;
              if (vl >= sizeof v) vl = sizeof v - 1;
              memcpy(v, eq + 1, vl); v[vl] = 0; return v; }
        p = *end ? end + 1 : end;
    }
    return NULL;
}
/* Encarta reads CodePath/DATPath/BookPath out of encarta.ini, not the registry,
   so the overlay has to answer here too - otherwise pointing it at content would
   mean dropping a file in %WINDIR%. */
static DWORD (WINAPI *real_getprofstr)(LPCSTR,LPCSTR,LPCSTR,LPSTR,DWORD,LPCSTR);
static DWORD WINAPI getprofstr_hook(LPCSTR sect, LPCSTR key, LPCSTR def,
                                    LPSTR ret, DWORD size, LPCSTR file)
{
    const char *ov = key ? profile_lookup(key) : NULL;
    if (ov && ret && size) {
        size_t n = strlen(ov);
        if (n >= size) n = size - 1;
        memcpy(ret, ov, n); ret[n] = 0;
        if (g_reg_log)
            fprintf(stderr, "  ini   [%s] %-14s -> profile \"%s\"\n",
                    sect ? sect : "", key, ret), fflush(stderr);
        return (DWORD)n;
    }
    DWORD rc = real_getprofstr(sect, key, def, ret, size, file);
    if (g_reg_log)
        fprintf(stderr, "  ini   [%s] %-14s -> \"%s\" (%s)\n", sect ? sect : "",
                key ? key : "(all)", ret ? ret : "", file ? file : ""), fflush(stderr);
    return rc;
}

static LONG WINAPI regquery_hook(HKEY k, LPCSTR name, LPDWORD res, LPDWORD type,
                                 LPBYTE data, LPDWORD len)
{
    const char *ov = profile_lookup(name);
    if (ov && data && len) {
        size_t n = strlen(ov) + 1;
        if (*len < n) { *len = (DWORD)n; return ERROR_MORE_DATA; }
        memcpy(data, ov, n); *len = (DWORD)n;
        if (type) *type = REG_SZ;
        if (g_reg_log) fprintf(stderr, "  reg query  %-30s -> profile \"%s\"\n", name, ov), fflush(stderr);
        return ERROR_SUCCESS;
    }
    LONG rc = real_regquery(k, name, res, type, data, len);
    if (g_reg_log) fprintf(stderr, "  reg query  %-30s -> %ld%s%s\n", name ? name : "(default)", rc,
            (rc == 0 && data && type && *type == REG_SZ) ? " = " : "",
            (rc == 0 && data && type && *type == REG_SZ) ? (char *)data : "");
    fflush(stderr); return rc;
}

static int g_imports, g_imports_res;
static int map_and_wire(const char *path)
{
    FILE *f=fopen(path,"rb"); if(!f) return 0;
    {   const char *b=strrchr(path,'\\'), *b2=strrchr(path,'/');
        if(b2>b) b=b2;
        size_t n = b ? (size_t)(b-path)+1 : 0;
        if(n>=sizeof g_exedir) n=sizeof g_exedir-1;
        memcpy(g_exedir,path,n); g_exedir[n]=0; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t *file=malloc(sz); if(fread(file,1,sz,f)!=(size_t)sz){fclose(f);return 0;} fclose(f);
    PIMAGE_DOS_HEADER dos=(PIMAGE_DOS_HEADER)file;
    PIMAGE_NT_HEADERS nt=(PIMAGE_NT_HEADERS)(file+dos->e_lfanew);
    uint32_t imgsz=nt->OptionalHeader.SizeOfImage;
    /* OS-chosen base; relocs are applied below and the lifter now GVA-wraps
       address immediates, so the lifted code is correct at any base. */
    uint8_t *base=VirtualAlloc(NULL,imgsz,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
    if(!base){free(file);return 0;}
    memcpy(base,file,nt->OptionalHeader.SizeOfHeaders);
    PIMAGE_SECTION_HEADER s=IMAGE_FIRST_SECTION(nt);
    for(int i=0;i<nt->FileHeader.NumberOfSections;i++)
        if(s[i].SizeOfRawData) memcpy(base+s[i].VirtualAddress,file+s[i].PointerToRawData,s[i].SizeOfRawData);
    g_base=base; g_imgsz=imgsz; g_image_delta=(int32_t)((uint32_t)(uintptr_t)base-PREF_BASE);
    IMAGE_DATA_DIRECTORY rd=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if(g_image_delta && rd.Size){
        uint8_t *p=base+rd.VirtualAddress,*end=p+rd.Size;
        while(p<end){ PIMAGE_BASE_RELOCATION br=(PIMAGE_BASE_RELOCATION)p;
            uint32_t n=(br->SizeOfBlock-sizeof *br)/2; uint16_t *e=(uint16_t*)(br+1);
            for(uint32_t i=0;i<n;i++) if((e[i]>>12)==IMAGE_REL_BASED_HIGHLOW)
                *(uint32_t*)(base+br->VirtualAddress+(e[i]&0xFFF))+=g_image_delta;
            p+=br->SizeOfBlock; }
    }
    /* wire imports to real code (read descriptors from `file` headers, write the
       mapped IAT in `base`); free `file` only after we're done with `nt`. */
    IMAGE_DATA_DIRECTORY id=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    PIMAGE_IMPORT_DESCRIPTOR imp=(PIMAGE_IMPORT_DESCRIPTOR)(base+id.VirtualAddress);
    for(;imp->Name;imp++){
        const char *dll=(const char*)(base+imp->Name);
        HMODULE h=load_for(dll);
        uint32_t oft=imp->OriginalFirstThunk?imp->OriginalFirstThunk:imp->FirstThunk;
        uint32_t *lookup_t=(uint32_t*)(base+oft);
        uint32_t *iat=(uint32_t*)(base+imp->FirstThunk);
        for(int i=0;lookup_t[i];i++){
            uint32_t ent=lookup_t[i]; FARPROC p=NULL;
            if(h){ if(ent&0x80000000u) p=GetProcAddress(h,(LPCSTR)(uintptr_t)(ent&0xFFFF));
                   else p=GetProcAddress(h,(LPCSTR)(base+(ent&0x7FFFFFFF)+2)); }
            g_imports++;
            if(p){ iat[i]=(uint32_t)(uintptr_t)p; g_imports_res++;
                if(g_ninames < 1024){ g_inames[g_ninames].addr=(uint32_t)(uintptr_t)p;
                    if(ent&0x80000000u) snprintf(g_inames[g_ninames].name,64,"%s#%u",dll,ent&0xFFFF);
                    else snprintf(g_inames[g_ninames].name,64,"%s!%s",dll,(const char*)(base+(ent&0x7FFFFFFF)+2));
                    if(!_stricmp(dll,"MSVCRT40.dll") && !(ent&0x80000000u) &&
                       !strcmp((const char*)(base+(ent&0x7FFFFFFF)+2),"_initterm"))
                        g_initterm=(uint32_t)(uintptr_t)p;
                    if(!_stricmp(dll,"MSVCRT40.dll") && !(ent&0x80000000u) &&
                       !strcmp((const char*)(base+(ent&0x7FFFFFFF)+2),"__p__acmdln"))
                        g_acmdln=(uint32_t)(uintptr_t)p;
                    if(g_msgbox_log && !(ent&0x80000000u) &&
                       !strncmp((const char*)(base+(ent&0x7FFFFFFF)+2),"MessageBox",10))
                        iat[i]=(uint32_t)(uintptr_t)msgbox_hook;
                    if((g_file_log || g_redir_from[0]) && !(ent&0x80000000u)){
                        const char *fn=(const char*)(base+(ent&0x7FFFFFFF)+2);
                        if(!strcmp(fn,"CreateFileA")){ *(void**)&real_createfile=(void*)p;
                            iat[i]=(uint32_t)(uintptr_t)createfile_hook; }
                        else if(!strcmp(fn,"OpenFile")){ *(void**)&real_openfile=(void*)p;
                            iat[i]=(uint32_t)(uintptr_t)openfile_hook; }
                        else if(!strcmp(fn,"_lopen")){ *(void**)&real_lopen=(void*)p;
                            iat[i]=(uint32_t)(uintptr_t)lopen_hook; }
                        else if(!strcmp(fn,"FindFirstFileA")){ *(void**)&real_findfirst=(void*)p;
                            iat[i]=(uint32_t)(uintptr_t)findfirst_hook; }
                    }
                    if(g_cdrom_drive && !(ent&0x80000000u)){
                        const char *fn=(const char*)(base+(ent&0x7FFFFFFF)+2);
                        if(!strcmp(fn,"GetDriveTypeA")){ *(void**)&real_getdrivetype=(void*)p;
                            iat[i]=(uint32_t)(uintptr_t)getdrivetype_hook; }
                        else if(!strcmp(fn,"GetVolumeInformationA")){ *(void**)&real_getvolinfo=(void*)p;
                            iat[i]=(uint32_t)(uintptr_t)getvolinfo_hook; }
                    }
                    if(g_no_printdlg && !(ent&0x80000000u) &&
                       !strncmp((const char*)(base+(ent&0x7FFFFFFF)+2),"PrintDlg",8))
                        iat[i]=(uint32_t)(uintptr_t)printdlg_hook;
                    if((g_reg_log||g_profile[0]) && !(ent&0x80000000u)){
                        const char *n=(const char*)(base+(ent&0x7FFFFFFF)+2);
                        if(!strcmp(n,"RegCreateKeyExA")){ *(void**)&real_regcreate=(void*)p;
                            iat[i]=(uint32_t)(uintptr_t)regcreate_hook; }
                        else if(!strcmp(n,"RegOpenKeyExA")){ *(void**)&real_regopen=(void*)p;
                            iat[i]=(uint32_t)(uintptr_t)regopen_hook; }
                        else if(!strcmp(n,"RegQueryValueExA")){ *(void**)&real_regquery=(void*)p;
                            iat[i]=(uint32_t)(uintptr_t)regquery_hook; }
                        else if(!strcmp(n,"GetPrivateProfileStringA")){ *(void**)&real_getprofstr=(void*)p;
                            iat[i]=(uint32_t)(uintptr_t)getprofstr_hook; }
                    }
                    g_ninames++; }
            }
        }
    }
    free(file);
    return 1;
}

static volatile DWORD g_fault_code;
/* Vectored handler: faults inside real code called on the switched trampoline
   stack are uncatchable by SEH, so log diagnostics here (then continue search). */
static LONG CALLBACK veh_diag(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == 0x40010006 || code == 0x4001000A) return EXCEPTION_CONTINUE_EXECUTION; /* OutputDebugString */
    static int nlog;
    if (code < 0xC0000000u) {          /* first-chance / C++ / debug exceptions */
        if (nlog++ < 20)
            fprintf(stderr, "VEH first-chance 0x%08lX @%p (calls=%lu)\n",
                    code, ep->ExceptionRecord->ExceptionAddress, g_calls), fflush(stderr);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (1) {
        uint32_t fa = (uint32_t)(uintptr_t)ep->ExceptionRecord->ExceptionAddress;
        uint32_t ova = (fa >= (uint32_t)(uintptr_t)g_base && fa < (uint32_t)(uintptr_t)g_base + g_imgsz)
                     ? fa - g_image_delta : 0;
        fprintf(stderr, "VEH fault 0x%08lX @%08X%s after %lu calls; last lifted=0x%06X last import=0x%08X (%s)\n",
                code, fa, ova ? "" : " (external)", g_calls, g_last, g_last_import, imp_name(g_last_import));
        if (ova) fprintf(stderr, "  faulting addr is ENC97 original-VA 0x%06X\n", ova);
        if (code == 0xC0000005u && ep->ExceptionRecord->NumberParameters >= 2)
            fprintf(stderr, "  %s %08X\n",
                    ep->ExceptionRecord->ExceptionInformation[0] ? "bad WRITE to" : "bad READ from",
                    (uint32_t)ep->ExceptionRecord->ExceptionInformation[1]);
        { CONTEXT *x = ep->ContextRecord;
          fprintf(stderr, "  eip=%08lX esp=%08lX ebp=%08lX eax=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX\n",
                  x->Eip, x->Esp, x->Ebp, x->Eax, x->Ecx, x->Edx, x->Esi, x->Edi);
          /* crude backtrace: code-looking dwords near esp. Off by default - it
             burns stack (MAX_PATH buffer + fprintf) at a moment when the stack is
             usually the thing that's broken, and can fault the handler itself. */
          if (x->Esp && g_r2l_trace) {
              fprintf(stderr, "  stack near esp:\n");
              for (int k = -4; k < 48; k++) {
                  uint32_t sp = x->Esp + k*4, v;
                  if (IsBadReadPtr((void*)(uintptr_t)sp, 4)) continue;
                  v = *(uint32_t*)(uintptr_t)sp;
                  HMODULE m = NULL; char nm[MAX_PATH] = "";
                  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                     GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     (LPCSTR)(uintptr_t)v, &m);
                  if (m) GetModuleFileNameA(m, nm, MAX_PATH);
                  int in_img = v >= (uint32_t)(uintptr_t)g_base && v < (uint32_t)(uintptr_t)g_base + g_imgsz;
                  if (m || in_img) {
                      const char *b = strrchr(nm, '\\');
                      fprintf(stderr, "    [esp%+d] %08X  %s%s\n", k*4, v, b ? b+1 : nm,
                              in_img ? " (ENC97 image)" : "");
                  }
              } } }
        fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

struct winscan { DWORD pid; char *title; HWND hwnd; };
static BOOL CALLBACK find_proc_window(HWND h, LPARAM lp)
{
    struct winscan *w = (struct winscan *)lp; DWORD p = 0;
    GetWindowThreadProcessId(h, &p);
    if (p == w->pid && IsWindowVisible(h) && GetWindowTextLengthA(h) > 0) {
        GetWindowTextA(h, w->title, 250); w->hwnd = h; return FALSE;
    }
    return TRUE;
}
/* the title alone is generic ("... cannot start"); the child statics carry the
   actual complaint, which is what says WHAT the app is missing. */
static BOOL CALLBACK dump_child_text(HWND h, LPARAM lp)
{
    char buf[512];
    (void)lp;
    if (GetWindowTextA(h, buf, sizeof buf) > 0)
        fprintf(stderr, "  dialog text: %s\n", buf);
    return TRUE;
}

static DWORD WINAPI run_thread(LPVOID arg)
{
    (void)arg;
    __try {
        call_lifted(L_0050DB70, NULL, 0);   /* lifted CRT/MFC entry: start() */
        g_done = 1;
    } __except (g_fault_code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        fprintf(stderr, "FAULT 0x%08lX after %lu calls; last lifted fn=0x%06X last import=0x%08X (%s)\n",
                g_fault_code, g_calls, g_last, g_last_import, imp_name(g_last_import));
        fflush(stderr);
    }
    return 0;
}

/* Give the app a video codec.
 *
 * Encarta plays its clips through MCI, so the chain is mciSendCommand ->
 * MCIAVI -> ICLocate("vidc", "IV32"). All 68 clips are Indeo 3.2, Microsoft
 * removed that codec from Windows years ago, and the copy on the CD is a
 * 16-bit NE this process cannot load - which is why it was recompiled.
 *
 * ir32vfw.dll registers the recompiled decoder with ICInstall for this process
 * only, so the ICLocate MCIAVI makes on the app's behalf finds it. The app is
 * unchanged and unaware; nothing is hooked.
 *
 * Loaded rather than linked so the two builds stay independent - the codec is
 * forty generated segments built by its own script. Absent, video simply fails
 * the way it did before, and everything else runs.
 *
 *   IR32_DLL=<path>   where IR32.DLL is (default: the CD's SYSTEM16)
 *   NO_VIDEO=1        skip it
 */
static void install_video_codec(void)
{
    if (getenv("NO_VIDEO"))
        return;
    HMODULE h = LoadLibraryA("ir32vfw.dll");
    if (!h) {
        fprintf(stderr, "video: ir32vfw.dll not found; clips will not play\n");
        return;
    }
    int (*install)(const char *) =
        (int (*)(const char *))(void *)GetProcAddress(h, "ir32_vfw_install");
    if (!install) {
        fprintf(stderr, "video: ir32vfw.dll has no ir32_vfw_install\n");
        return;
    }
    const char *dll = getenv("IR32_DLL");
    if (!dll) dll = "H:\\AAMSSTP\\SYSTEM16\\IR32.DLL";
    int r = install(dll);
    if (r) {
        fprintf(stderr, "video: codec install failed (%d) using %s\n", r, dll);
        return;
    }
    /* Installed is not the same as findable. Ask VFW to locate an IV32
     * decompressor the way MCIAVI will when the app plays a clip - the
     * same ICLocate, from inside the same process. */
    int (*probe)(int, int) =
        (int (*)(int, int))(void *)GetProcAddress(h, "ir32_vfw_probe");
    int found = probe ? probe(216, 192) == 0 : -1;
    fprintf(stderr, "video: IV32 registered (%s); ICLocate %s\n",
            dll, found == 1 ? "finds it" :
                 found == 0 ? "DOES NOT find it" : "not probed");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
    { const char *t = getenv("RUN_TRACE"); g_trace = t ? atoi(t) : 0; }
    { const char *t2 = getenv("R2L_TRACE"); g_r2l_trace = t2 ? (atoi(t2) ? atoi(t2) : 1) : 0; }
    { const char *r = getenv("R2L_REAL"); g_r2l_real = r ? (atoi(r) ? atoi(r) : 1) : 0; }
    g_r2l_stub  = getenv("R2L_STUB")  != NULL;
    g_r2l_passthru = getenv("R2L_PASSTHRU") != NULL;
    { const char *v; if((v=getenv("LIFT_LO"))) g_lift_lo=atoi(v);
                     if((v=getenv("LIFT_HI"))) g_lift_hi=atoi(v);
      if(g_lift_lo || g_lift_hi != (1<<30))
          fprintf(stderr,"lifting only table indices [%d,%d) of %u; rest run real\n",
                  g_lift_lo, g_lift_hi < (int)NLIFTED ? g_lift_hi : (int)NLIFTED, (unsigned)NLIFTED); }
    { const char *w = getenv("WATCH");            /* comma/space separated VAs */
      while (w && *w && g_nwatch < 32) {
          char *end; uint32_t v = (uint32_t)strtoul(w, &end, 16);
          if (end == w) break;
          g_watch[g_nwatch++] = v; w = end; while (*w == ',' || *w == ' ') w++;
      } }
    g_msgbox_log   = getenv("MSGBOX_LOG")   != NULL;
    g_no_printdlg  = getenv("NO_PRINTDLG")  != NULL;
    { const char *fl = getenv("FILE_LOG");
      g_file_log = fl ? (!strcmp(fl, "all") ? 2 : 1) : 0; }
    g_reg_log      = getenv("REG_LOG")      != NULL;
    { const char *rd = getenv("ENC97_REDIRECT");
      if (rd) { const char *eq = strchr(rd, '=');
          if (eq) { size_t n = eq - rd;
              if (n >= sizeof g_redir_from) n = sizeof g_redir_from - 1;
              memcpy(g_redir_from, rd, n); g_redir_from[n] = 0;
              strncpy(g_redir_to, eq + 1, sizeof g_redir_to - 1);
              g_redir_to[sizeof g_redir_to - 1] = 0;
              fprintf(stderr, "content: %s -> %s\n",
                      g_redir_from, g_redir_to); } } }
    { const char *cd = getenv("ENC97_CDROM");
      if (cd && cd[0]) g_cdrom_drive = cd[0];
      const char *lb = getenv("ENC97_CDLABEL");
      if (lb && lb[0]) { strncpy(g_cdlabel, lb, sizeof g_cdlabel - 1);
                         g_cdlabel[sizeof g_cdlabel - 1] = 0; }
      if (g_cdrom_drive)
          fprintf(stderr, "content: %c: answers as CD-ROM labelled %s\n",
                  g_cdrom_drive, g_cdlabel); }
    { const char *pf = getenv("ENC97_PROFILE");
      if (pf) { strncpy(g_profile, pf, sizeof g_profile - 1); g_profile[sizeof g_profile - 1] = 0; } }
    { const char *h = getenv("R2L_HEAPCHECK"); g_heapcheck = h ? (atoi(h) ? atoi(h) : 1) : 0; }
    AddVectoredExceptionHandler(1, veh_diag);
    fprintf(stderr,"[1] start\n");
    const char *exe = (argc>=2)?argv[1]:"C:\\encarta\\analysis\\ENC97.EXE";
    int timeout_ms = (argc>=3)?atoi(argv[2]):8000;
    if(!map_and_wire(exe)){ fprintf(stderr,"map/wire failed\n"); return 1; }
    /* LoadLibrary calls the app makes at runtime (InitInstance pulls in
       encres97.dll) search OUR directory, not the mapped app's. */
    SetDllDirectoryA(g_exedir);
    /* The lifted CRT parses _acmdln - which is OUR command line, so ENC97 sees
       our argv as its own parameters and refuses to start ("The command line is
       improperly formatted"). Give it its own. ENC97_CMDLINE overrides. */
    if (g_acmdln) {
        static char cmd[1024];
        const char *extra = getenv("ENC97_CMDLINE");
        snprintf(cmd, sizeof cmd, "\"%s\"%s%s", exe, extra ? " " : "", extra ? extra : "");
        *((char **(*)(void))(uintptr_t)g_acmdln)() = cmd;
        fprintf(stderr, "app command line: %s\n", cmd);
    }
    install_video_codec();
    fprintf(stderr,"[2] mapped+wired\n");
    qsort(g_lifted,NLIFTED,sizeof *g_lifted,cmp_entry);
    if(g_lift_hi - g_lift_lo == 1 && g_lift_lo < (int)NLIFTED)
        fprintf(stderr,"lift index %d = 0x%06X\n", g_lift_lo, g_lifted[g_lift_lo].va);
    g_estack=VirtualAlloc(NULL,EMU_STACK,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    g_r2l_arena=VirtualAlloc(NULL,R2L_ARENA,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    g_r2l_top=(uint32_t)(uintptr_t)(g_r2l_arena+R2L_ARENA);
    g_tramp_pool=VirtualAlloc(NULL,0x200000,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);

    if(getenv("R2L_TEST")){   /* unit-test the real->lifted thiscall trampoline */
        uint8_t *buf=calloc(0x200,1);
        uint32_t t=make_tramp(0x4AD870u);   /* lifted __thiscall(this,v): *(this+0x8E)=v */
        FlushInstructionCache(GetCurrentProcess(),NULL,0);
        uint32_t this_=(uint32_t)(uintptr_t)buf, val=0xCAFEF00Du, tgt=t;
        __asm {                              /* real thiscall call: ecx=this, arg on stack */
            mov  ecx, this_
            push val
            mov  eax, tgt
            call eax                         /* trampoline does thiscall ret 4 cleanup */
        }
        uint32_t got=*(uint32_t*)(buf+0x8E);
        printf("%s real->lifted thiscall trampoline: sub_4AD870(this,0xCAFEF00D) via REAL call -> this+0x8E=%08X\n",
               got==0xCAFEF00Du?"PASS":"FAIL",got);
        return got==0xCAFEF00Du?0:1;
    }
    fprintf(stderr,"ENC97 mapped @%p (delta %+d); IAT %d/%d wired; %u lifted fns\n",
            (void*)g_base,(int)g_image_delta,g_imports_res,g_imports,(unsigned)NLIFTED);
    if(getenv("R2L_VTABLES")){
        { const char *v; if((v=getenv("R2L_LO"))) g_r2l_lo=atoi(v);
                         if((v=getenv("R2L_HI"))) g_r2l_hi=atoi(v); }
        int nv=rewrite_fnptr_slots();
        FlushInstructionCache(GetCurrentProcess(),NULL,0);
        fprintf(stderr,"routed %d of %d vtable/fn-ptr slots (index range [%d,%d)) -> real->lifted\n",
                nv,g_nslots,g_r2l_lo,g_r2l_hi<g_nslots?g_r2l_hi:g_nslots);
        if(nv==1) fprintf(stderr,"  slot %d -> lifted fn 0x%06X\n",g_r2l_lo,g_slot_ova[g_r2l_lo]);
    }
    fprintf(stderr,"booting lifted entry start@0x50DB70 (watchdog %d ms)...\n",timeout_ms);

    /* run the lifted entry on a worker with a big stack (lifted calls recurse on
       the real C stack); watchdog + window poll from the main thread. */
    HANDLE th=CreateThread(NULL, 64u<<20, run_thread, NULL, 0, NULL);
    char wintitle[256] = {0};
    int elapsed = 0, step = 100;
    while (elapsed < timeout_ms) {
        if (WaitForSingleObject(th, step) == WAIT_OBJECT_0) break;
        elapsed += step;
        /* did the booting app create a top-level window in our process? */
        struct winscan wp = { GetCurrentProcessId(), wintitle, NULL };
        EnumWindows(find_proc_window, (LPARAM)&wp);
        if (wintitle[0] && !getenv("HOLD")) {
            if (wp.hwnd) {   /* let the dialog finish filling its statics */
                Sleep(700); EnumChildWindows(wp.hwnd, dump_child_text, 0); fflush(stderr);
            }
            break;
        }
    }

    if(g_done)
        fprintf(stderr,"lifted entry RETURNED after %lu dispatched calls\n", g_calls);
    else
        fprintf(stderr,"%s: %lu calls; last lifted fn=0x%06X last import=0x%08X (%s)\n",
                wintitle[0]?"window appeared":"watchdog stop",
                g_calls, g_last, g_last_import, imp_name(g_last_import));

    /* a window means the app got far enough to put UI up - that's a pass, not a
       crash; only a fault or a silent stall is BAD. */
    printf("RESULT: %s\n", (g_done || wintitle[0]) ? "OK" : "BAD (no clean exit)");
    printf("ENC97 lifted boot: %lu calls dispatched (%lu init fns, %lu real->lifted "
           "vtable calls), last lifted fn 0x%06X%s%s%s\n",
           g_calls, g_initfns, g_r2l_calls, g_last,
           g_done?" (entry returned)":" (running)",
           wintitle[0]?" — window: " : "", wintitle);
    TerminateThread(th, 0);   /* bounded: stop whatever the boot is doing */
    return 0;
}
