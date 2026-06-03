/*
 * decotrace - function-call tracer for DECO_32.DLL (Phase-2c golden references)
 *
 * Loads the real DLL, sets INT3 (0xCC) breakpoints on every known function
 * entry (from deco_funcs.txt), then runs a full FTC decode. A vectored
 * exception handler logs, at each function entry, the register state and the
 * top stack dwords (candidate args). Breakpoints are transparently stepped
 * over and re-armed, so the decode runs to completion and produces the same
 * pixels as decooracle. The trace is the ground truth the lifted C is diffed
 * against.
 *
 * Usage: decotrace <input.ftc> <deco_funcs.txt> <trace_out.txt> [cap] [dll]
 *   cap = max logged entries per function (default 64)
 *
 * Build: 32-bit. Single-threaded decode assumed (the re-arm dance is not
 * thread-safe by design).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define PREFERRED_BASE 0x11000000u
#define MAX_FUNCS      4096

typedef int  (__cdecl *pfnOpen)(int *);
typedef int  (__cdecl *pfnSetBuf)(int, const void *, int);
typedef int  (__cdecl *pfnGetRes)(int, int *, int *, int *);
typedef int  (__cdecl *pfnSetRes)(int, int, int);
typedef int  (__cdecl *pfnSetFmt)(int, int, int, int, int, int);
typedef int  (__cdecl *pfnDecomp)(int, void *, int, int, int, int, int);

/* breakpoint / function table */
static struct {
    uintptr_t addr;        /* live address (rebased) */
    uint32_t  rva;         /* RVA at preferred base */
    char      name[48];
    uint8_t   orig;        /* original first byte */
    uint32_t  hits;        /* total hits */
    uint32_t  logged;      /* logged hits */
} g_fn[MAX_FUNCS];
static int       g_nfn = 0;
static uintptr_t g_lo = ~(uintptr_t)0, g_hi = 0;   /* address range of entries */
static uintptr_t g_pending = 0;                    /* addr to re-arm after single-step */
static uint32_t  g_cap = 64;
static uint32_t  g_seq = 0;
static FILE     *g_log = NULL;

static int find_fn(uintptr_t addr)
{
    /* linear scan is fine; called only on our own breakpoints */
    for (int i = 0; i < g_nfn; i++)
        if (g_fn[i].addr == addr) return i;
    return -1;
}

static void arm(int i)
{
    DWORD old;
    VirtualProtect((void *)g_fn[i].addr, 1, PAGE_EXECUTE_READWRITE, &old);
    *(volatile uint8_t *)g_fn[i].addr = 0xCC;
    VirtualProtect((void *)g_fn[i].addr, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)g_fn[i].addr, 1);
}

static void disarm(int i)
{
    DWORD old;
    VirtualProtect((void *)g_fn[i].addr, 1, PAGE_EXECUTE_READWRITE, &old);
    *(volatile uint8_t *)g_fn[i].addr = g_fn[i].orig;
    VirtualProtect((void *)g_fn[i].addr, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)g_fn[i].addr, 1);
}

static LONG CALLBACK veh(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    CONTEXT *c = ep->ContextRecord;

    if (code == EXCEPTION_BREAKPOINT) {
        /* INT3: Eip points just past the 0xCC; the bp is at Eip-1 (or Eip). */
        uintptr_t bp = (uintptr_t)c->Eip - 1;
        int i = find_fn(bp);
        if (i < 0) { bp = (uintptr_t)c->Eip; i = find_fn(bp); }
        if (i < 0) return EXCEPTION_CONTINUE_SEARCH;   /* not ours */

        g_fn[i].hits++;
        if (g_fn[i].logged < g_cap) {
            g_fn[i].logged++;
            const uint32_t *sp = (const uint32_t *)c->Esp;
            fprintf(g_log,
                "%u %08X %-24s ret=%08X eax=%08X ecx=%08X edx=%08X ebx=%08X "
                "esp=%08X ebp=%08X esi=%08X edi=%08X | %08X %08X %08X %08X %08X %08X\n",
                g_seq++, g_fn[i].rva, g_fn[i].name,
                sp[0], (uint32_t)c->Eax, (uint32_t)c->Ecx, (uint32_t)c->Edx,
                (uint32_t)c->Ebx, (uint32_t)c->Esp, (uint32_t)c->Ebp,
                (uint32_t)c->Esi, (uint32_t)c->Edi,
                sp[1], sp[2], sp[3], sp[4], sp[5], sp[6]);
        }

        /* step over: restore original byte, rewind Eip, set trap flag */
        disarm(i);
        c->Eip = (DWORD)bp;
        c->EFlags |= 0x100;       /* TF */
        g_pending = bp;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (code == EXCEPTION_SINGLE_STEP) {
        if (g_pending) {
            int i = find_fn(g_pending);
            /* re-arm only while still under the cap; once capped, leave the
               breakpoint off so hot functions don't pay 2 exceptions/call */
            if (i >= 0 && g_fn[i].logged < g_cap) arm(i);
            g_pending = 0;
            c->EFlags &= ~0x100u;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

static int load_funcs(const char *path, uintptr_t delta)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 0; }
    char line[256];
    while (fgets(line, sizeof line, f) && g_nfn < MAX_FUNCS) {
        uint32_t rva; char name[48];
        if (sscanf(line, "%x %47s", &rva, name) != 2) continue;
        uintptr_t addr = (uintptr_t)rva + delta;
        g_fn[g_nfn].addr = addr;
        g_fn[g_nfn].rva = rva;
        strncpy(g_fn[g_nfn].name, name, sizeof g_fn[g_nfn].name - 1);
        g_fn[g_nfn].orig = *(uint8_t *)addr;
        g_fn[g_nfn].hits = g_fn[g_nfn].logged = 0;
        if (addr < g_lo) g_lo = addr;
        if (addr > g_hi) g_hi = addr;
        g_nfn++;
    }
    fclose(f);
    return g_nfn;
}

static uint8_t *read_file(const char *path, long *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(n ? n : 1);
    if (d && fread(d, 1, n, f) == (size_t)n) *out = n; else { free(d); d = NULL; }
    fclose(f);
    return d;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: decotrace <in.ftc> <deco_funcs.txt> <trace_out.txt> [cap] [dll]\n");
        return 2;
    }
    const char *in = argv[1], *funcs = argv[2], *trace = argv[3];
    if (argc >= 5) g_cap = (uint32_t)strtoul(argv[4], NULL, 10);
    const char *dll = (argc >= 6) ? argv[5] : "C:\\encarta\\analysis\\DECO_32.DLL";

    HMODULE h = LoadLibraryA(dll);
    if (!h) { fprintf(stderr, "cannot load %s\n", dll); return 1; }
    uintptr_t base = (uintptr_t)h;
    uintptr_t delta = base - PREFERRED_BASE;
    fprintf(stderr, "DECO_32 loaded at %p (delta %+lld)\n", (void *)base, (long long)delta);

    pfnOpen   Open   = (pfnOpen)  GetProcAddress(h, "OpenDecompressor");
    pfnSetBuf SetFIF = (pfnSetBuf)GetProcAddress(h, "SetFIFBuffer");
    pfnGetRes GetRes = (pfnGetRes)GetProcAddress(h, "GetOriginalResolution");
    pfnSetRes SetRes = (pfnSetRes)GetProcAddress(h, "SetOutputResolution");
    pfnSetFmt SetFmt = (pfnSetFmt)GetProcAddress(h, "SetOutputFormat");
    pfnDecomp Decomp = (pfnDecomp)GetProcAddress(h, "DecompressToBuffer");

    long fsz = 0; uint8_t *data = read_file(in, &fsz);
    if (!data) { fprintf(stderr, "cannot read %s\n", in); return 1; }

    if (!load_funcs(funcs, delta)) return 1;
    fprintf(stderr, "loaded %d function entries (range %p..%p)\n",
            g_nfn, (void *)g_lo, (void *)g_hi);

    g_log = fopen(trace, "w");
    if (!g_log) { fprintf(stderr, "cannot create %s\n", trace); return 1; }
    fprintf(g_log, "# decotrace %s cap=%u funcs=%d\n", in, g_cap, g_nfn);

    /* set up the decode BEFORE arming, so only the core decode is traced */
    int hd = 0, w = 0, hgt = 0, ex = 0;
    if (Open(&hd) || !hd) { fprintf(stderr, "open failed\n"); return 1; }
    if (SetFIF(hd, data, (int)fsz)) { fprintf(stderr, "setfif failed\n"); return 1; }
    {   /* load referenced FTT (mode-04 images) from the input's directory */
        typedef int (__cdecl *pfnGN)(int, char *);
        typedef int (__cdecl *pfnSF)(int, const void *, int);
        pfnGN GetFttName = (pfnGN)GetProcAddress(h, "GetFIFFTTFileName");
        pfnSF SetFtt     = (pfnSF)GetProcAddress(h, "SetFTTBuffer");
        char fttname[300] = {0};
        if (GetFttName && SetFtt) {
            GetFttName(hd, fttname);
            if (fttname[0]) {
                char *b1 = strrchr(fttname, '\\'), *b2 = strrchr(fttname, '/');
                const char *bn = fttname;
                if (b1 && b1 + 1 > bn) bn = b1 + 1;
                if (b2 && b2 + 1 > bn) bn = b2 + 1;
                char p[512]; const char *s1 = strrchr(in, '\\'), *s2 = strrchr(in, '/');
                const char *sep = (s2 > s1) ? s2 : s1;
                if (sep) snprintf(p, sizeof p, "%.*s%s", (int)(sep - in + 1), in, bn);
                else snprintf(p, sizeof p, "%s", bn);
                long ftsz = 0; uint8_t *ftt = read_file(p, &ftsz);
                if (ftt) { fprintf(stderr, "FTT '%s' -> SetFTTBuffer %d\n", bn, SetFtt(hd, ftt, (int)ftsz)); }
                else fprintf(stderr, "FTT '%s' not found at %s\n", bn, p);
            }
        }
    }
    if (GetRes(hd, &w, &hgt, &ex) || w <= 0 || hgt <= 0) { fprintf(stderr, "getres failed\n"); return 1; }
    SetRes(hd, w, hgt);
    SetFmt(hd, 3, 2, 1, 4, 0);
    int stride = w * 3;
    uint8_t *px = calloc(1, (size_t)stride * hgt);

    PVOID hveh = AddVectoredExceptionHandler(1, veh);
    for (int i = 0; i < g_nfn; i++) arm(i);

    int rc = Decomp(hd, px, 0, 0, w, hgt, stride);

    for (int i = 0; i < g_nfn; i++) disarm(i);
    RemoveVectoredExceptionHandler(hveh);

    fprintf(stderr, "decode rc=%d, traced %u entries\n", rc, g_seq);
    fprintf(g_log, "# decode rc=%d total_entries=%u\n", rc, g_seq);

    /* per-function hit summary (sorted by hits desc would be nicer; keep simple) */
    fprintf(g_log, "# --- hit counts ---\n");
    for (int i = 0; i < g_nfn; i++)
        if (g_fn[i].hits)
            fprintf(g_log, "# %08X %-24s hits=%u logged=%u\n",
                    g_fn[i].rva, g_fn[i].name, g_fn[i].hits, g_fn[i].logged);
    fclose(g_log);

    fprintf(stderr, "wrote trace %s\n", trace);
    return rc == 0 ? 0 : 1;
}
