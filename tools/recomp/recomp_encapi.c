/*
 * recomp_encapi.c - harness for the statically-recompiled ENCAPI32.DLL.
 *
 * ENCAPI32 is mostly Win32 glue (CD/volume checks, IPC via window messages,
 * registry, article-ID strings, ACM audio). Unlike DECO_32 (KERNEL32-only),
 * it imports standard APIs (KERNEL32/USER32/ADVAPI32/MSVCRT40) that STILL
 * EXIST on Win11 — so the lifted code calls the REAL APIs through an import
 * trampoline (lifted emulated-stack -> real stdcall/cdecl Win32). This proves
 * the lifted<->Win32 boundary needed for the larger ENC97.EXE.
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

#define PREF_BASE 0x10000000u

#include "encapi_lifted_list.h"
#define DECL(a) void L_##a(CPU *);
LIFTED_FUNCS(DECL)
#undef DECL

typedef void (*lfn)(CPU *);
typedef struct { uint32_t rva; lfn fn; } entry_t;
static entry_t g_lifted[] = {
#define E(a) { 0x##a, L_##a },
    LIFTED_FUNCS(E)
#undef E
};

static uint32_t g_lo, g_hi;            /* live image address range */

/* ---- import trampoline: call a real Win32 API on the emulated stack ----
 * Loads regs from the CPU, points machine esp at cpu.esp+4 (so the call's
 * pushed return addr lands on the lifted ret-slot and args line up), invokes
 * the API, then captures the API's resulting esp — which auto-accounts for
 * stdcall arg cleanup — and writes it back to cpu.esp. */
static uint32_t T_tgt, T_eax, T_espp4, T_fesp, T_sesp;
static void call_import(CPU *c, uint32_t target)
{
    /* Standard Win32 APIs are stdcall with args on the stack — we only need to
       point esp at the lifted arg area, call, then capture the API's resulting
       esp (which reflects its own stdcall arg cleanup). ebp/callee-saved regs
       are left to the C frame / preserved by the API, so the C frame is intact. */
    T_tgt = target; T_espp4 = c->esp + 4;
    __asm {
        mov T_sesp, esp
        mov esp, T_espp4
        call dword ptr [T_tgt]
        mov T_fesp, esp
        mov T_eax, eax
        mov esp, T_sesp
    }
    c->eax = T_eax;
    c->esp = T_fesp;     /* API's resulting esp (handles stdcall cleanup) */
}

void dispatch(CPU *c, uint32_t target)
{
    uint32_t rva = target - g_image_delta;
    if (rva >= PREF_BASE && rva < g_hi - g_image_delta) {
        for (size_t i = 0; i < sizeof g_lifted / sizeof *g_lifted; i++)
            if (g_lifted[i].rva == rva) { g_lifted[i].fn(c); return; }
        fprintf(stderr, "\n*** unlifted internal target %08X\n", target); abort();
    }
    call_import(c, target);   /* out of image -> real Win32 API */
}
void dispatch_jmp(CPU *c, uint32_t t) { dispatch(c, t); }
void dispatch_indirect(CPU *c, uint32_t t) { dispatch(c, t); }

#define EMU_STACK (1u << 20)
static uint8_t *g_estack;

static uint32_t call_lifted(lfn fn, const uint32_t *args, int n)
{
    CPU c; memset(&c, 0, sizeof c);
    c.esp = (uint32_t)(uintptr_t)(g_estack + EMU_STACK - 256);
    for (int i = n - 1; i >= 0; i--) push32(&c, args[i]);
    push32(&c, 0xDEADBEEFu);
    fn(&c);
    return c.eax;
}

/* OutputDebugStringA raises a benign, continuable DBG_PRINTEXCEPTION_C; continue it. */
static LONG CALLBACK veh_dbgprint(PEXCEPTION_POINTERS ep)
{
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (c == 0x40010006 /*DBG_PRINTEXCEPTION_C*/ || c == 0x4001000A /*WIDE*/)
        return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char **argv)
{
    AddVectoredExceptionHandler(1, veh_dbgprint);
    const char *dll = (argc >= 2) ? argv[1] : "C:\\encarta\\analysis\\ENCAPI32.DLL";
    HMODULE h = LoadLibraryA(dll);
    if (!h) { fprintf(stderr, "cannot load %s\n", dll); return 1; }
    uint32_t base = (uint32_t)(uintptr_t)h;
    g_image_delta = base - PREF_BASE;
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)h;
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((uint8_t *)h + dos->e_lfanew);
    g_lo = base; g_hi = base + nt->OptionalHeader.SizeOfImage;
    g_estack = malloc(EMU_STACK);
    fprintf(stderr, "ENCAPI32 @ %08X (delta %+d)\n", base, (int)g_image_delta);

    /* Validate fGetArticleID: it lstrcpyA's the String1 global (@0x100042D8)
       into the caller's buffer. Set the global, run the LIFTED export, and
       compare to the REAL export — exercises lifted code + the import
       trampoline (lstrcpyA, OutputDebugStringA). */
    char *gstr = (char *)(uintptr_t)(0x100042D8u + g_image_delta);
    strcpy(gstr, "Article_42:Mathematics");

    char lbuf[128] = {0}, rbuf[128] = {0};
    uint32_t a[] = { (uint32_t)(uintptr_t)lbuf };
    int lret = (int)call_lifted(L_100021E0, a, 1);   /* lifted fGetArticleID */

    typedef int(__stdcall *pf)(char *);
    pf real = (pf)GetProcAddress(h, "fGetArticleID");
    int rret = real(rbuf);                            /* real fGetArticleID */

    fprintf(stderr, "lifted: ret=%d buf=\"%s\"\n", lret, lbuf);
    fprintf(stderr, "real:   ret=%d buf=\"%s\"\n", rret, rbuf);
    int ok = (lret == rret && strcmp(lbuf, rbuf) == 0 && strcmp(lbuf, gstr) == 0);
    printf("%s fGetArticleID  (lifted==real==global)\n", ok ? "PASS" : "FAIL");

    /* also a null-arg path: fGetArticleID(NULL) must return 0 without crashing */
    uint32_t a0[] = { 0 };
    int lnull = (int)call_lifted(L_100021E0, a0, 1);
    int rnull = real(NULL);
    printf("%s fGetArticleID(NULL) lifted=%d real=%d\n",
           (lnull == rnull && lnull == 0) ? "PASS" : "FAIL", lnull, rnull);

    FreeLibrary(h);
    return ok ? 0 : 1;
}
