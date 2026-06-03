/*
 * recomp_decode.c - integration harness for the statically-recompiled
 * DECO_32 decoder.
 *
 * The real DLL performs setup (Open/SetFIF/SetRes/SetFmt builds the decoder
 * instance at 0x11000000); we then hand the instance to the LIFTED
 * DecompressToBuffer (L_1100C320) running on an emulated stack. Lifted code
 * uses real 32-bit pointers (same address space), dispatches sub-calls to other
 * lifted functions, and routes the CRT boundary (malloc/free/calloc/ftol/...)
 * to C stubs — which severs the heap subtree so no KERNEL32 is reached.
 *
 * Output pixels are compared (CRC32) against the oracle baseline. Build 32-bit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "cpu.h"
#include "png_write.h"   /* pw__crc_buf */

/* ---- lifted functions (from lifted_codec.c) ---- */
#define L(a) void L_##a(CPU *);
L(11001000) L(11004AA0) L(11004BE0) L(11004D60) L(110053E0) L(11006FD0)
L(11008030) L(110080F0) L(1100C320) L(11012020) L(110121D0) L(11012330)
L(110124C0) L(11012990) L(11012A90) L(11013280) L(11013630) L(110138B0)
L(110141D0) L(11014650) L(11015FA0) L(11016750) L(11016840) L(11016E00)
L(11018C30) L(110191D0) L(11019780) L(11019800)
#undef L

/* ---- CRT stubs (read args off the emulated stack, set eax, pop the ret) ---- */
static void stub_malloc(CPU *c) {            /* 1101AA58(size), 11019909(size) */
    uint32_t size = rd32(c->esp + 4);
    c->eax = (uint32_t)(uintptr_t)malloc(size ? size : 1);
    c->esp += 4;
}
static void stub_free(CPU *c) {              /* 1101AA4B(p), 11019830(p) */
    uint32_t p = rd32(c->esp + 4);
    if (p) free((void *)(uintptr_t)p);
    c->eax = 0; c->esp += 4;
}
static void stub_calloc(CPU *c) {            /* 110198B9(n, sz): alloc + zero */
    uint32_t n = rd32(c->esp + 4), sz = rd32(c->esp + 8);
    uint32_t total = n * sz, rsz = (total + 3) & ~3u;
    void *p = malloc(rsz ? rsz : 1);
    if (p) memset(p, 0, rsz);
    c->eax = (uint32_t)(uintptr_t)p; c->esp += 4;
}
static void stub_ftol(CPU *c) {              /* 11019A44: (int64)st0 -> edx:eax, pop */
    int64_t v = (int64_t)*fst(c, 0); fpop(c);
    c->eax = (uint32_t)v; c->edx = (uint32_t)((uint64_t)v >> 32);
    c->esp += 4;
}
static void stub_heap_noop(CPU *c) {         /* 1101A844: heap maintenance -> 0 */
    c->eax = 0; c->esp += 4;
}

/* ---- dispatch table ---- */
typedef void (*lfn)(CPU *);
typedef struct { uint32_t rva; lfn fn; } entry_t;   /* rva at preferred base */

static entry_t g_lifted[] = {
#define E(a) { 0x##a, L_##a },
    E(11001000) E(11004AA0) E(11004BE0) E(11004D60) E(110053E0) E(11006FD0)
    E(11008030) E(110080F0) E(1100C320) E(11012020) E(110121D0) E(11012330)
    E(110124C0) E(11012990) E(11012A90) E(11013280) E(11013630) E(110138B0)
    E(110141D0) E(11014650) E(11015FA0) E(11016750) E(11016840) E(11016E00)
    E(11018C30) E(110191D0) E(11019780) E(11019800)
#undef E
};
static entry_t g_stubs[] = {
    { 0x1101AA58, stub_malloc }, { 0x11019909, stub_malloc },
    { 0x1101AA4B, stub_free },   { 0x11019830, stub_free },
    { 0x110198B9, stub_calloc }, { 0x11019A44, stub_ftol },
    { 0x1101A844, stub_heap_noop },
};

static uint32_t g_call_count = 0;

void dispatch(CPU *c, uint32_t target)
{
    uint32_t rva = target - g_image_delta;   /* back to preferred-base VA */
    g_call_count++;
    for (size_t i = 0; i < sizeof g_lifted / sizeof *g_lifted; i++)
        if (g_lifted[i].rva == rva) { g_lifted[i].fn(c); return; }
    for (size_t i = 0; i < sizeof g_stubs / sizeof *g_stubs; i++)
        if (g_stubs[i].rva == rva) { g_stubs[i].fn(c); return; }
    fprintf(stderr, "\n*** dispatch: UNLIFTED target %08X (rva %08X) — need to lift it\n",
            target, rva);
    fflush(stderr);
    abort();
}

void dispatch_jmp(CPU *c, uint32_t target)
{
    /* tail call: run target, then it returns to our caller */
    dispatch(c, target);
}

void dispatch_indirect(CPU *c, uint32_t target)
{
    /* gated callbacks (progress cb) should be skipped for this decode */
    fprintf(stderr, "\n*** dispatch_indirect: target %08X reached unexpectedly\n", target);
    abort();
}

/* ---- real DLL setup (decooracle sequence) ---- */
typedef int (__cdecl *pfnOpen)(int *);
typedef int (__cdecl *pfnSetBuf)(int, const void *, int);
typedef int (__cdecl *pfnGetRes)(int, int *, int *, int *);
typedef int (__cdecl *pfnSetRes)(int, int, int);
typedef int (__cdecl *pfnSetFmt)(int, int, int, int, int, int);

static uint8_t *read_file(const char *p, long *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(*n); if (d && fread(d, 1, *n, f) != (size_t)*n) { free(d); d = NULL; }
    fclose(f); return d;
}

#define EMU_STACK (4u * 1024 * 1024)

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: recomp_decode <in.ftc> [expected_crc_hex] [out.png] [dll]\n"); return 2; }
    const char *in = argv[1];
    uint32_t expect = (argc >= 3) ? (uint32_t)strtoul(argv[2], NULL, 16) : 0;
    const char *outpng = (argc >= 4) ? argv[3] : NULL;
    const char *dll = (argc >= 5) ? argv[4] : "C:\\encarta\\analysis\\DECO_32.DLL";

    HMODULE h = LoadLibraryA(dll);
    if (!h) { fprintf(stderr, "cannot load %s\n", dll); return 1; }
    g_image_delta = (uint32_t)(uintptr_t)h - 0x11000000u;

    pfnOpen   Open   = (pfnOpen)  GetProcAddress(h, "OpenDecompressor");
    pfnSetBuf SetFIF = (pfnSetBuf)GetProcAddress(h, "SetFIFBuffer");
    pfnGetRes GetRes = (pfnGetRes)GetProcAddress(h, "GetOriginalResolution");
    pfnSetRes SetRes = (pfnSetRes)GetProcAddress(h, "SetOutputResolution");
    pfnSetFmt SetFmt = (pfnSetFmt)GetProcAddress(h, "SetOutputFormat");

    long fsz = 0; uint8_t *data = read_file(in, &fsz);
    if (!data) { fprintf(stderr, "cannot read %s\n", in); return 1; }

    int hd = 0, w = 0, hgt = 0, ex = 0;
    if (Open(&hd) || !hd) { fprintf(stderr, "open failed\n"); return 1; }
    if (SetFIF(hd, data, (int)fsz)) { fprintf(stderr, "setfif failed\n"); return 1; }
    if (GetRes(hd, &w, &hgt, &ex) || w <= 0) { fprintf(stderr, "getres failed\n"); return 1; }
    SetRes(hd, w, hgt);
    SetFmt(hd, 3, 2, 1, 4, 0);
    int stride = w * 3;
    uint8_t *px = calloc(1, (size_t)stride * hgt);
    fprintf(stderr, "setup ok: handle=%d %dx%d stride=%d\n", hd, w, hgt, stride);

    /* run the LIFTED DecompressToBuffer(handle, px, 0, 0, w, hgt, stride) */
    uint8_t *estack = malloc(EMU_STACK);
    CPU c; memset(&c, 0, sizeof c);
    c.esp = (uint32_t)(uintptr_t)(estack + EMU_STACK - 256);
    /* cdecl args pushed right-to-left, then the call's return slot */
    push32(&c, (uint32_t)stride);
    push32(&c, (uint32_t)hgt);
    push32(&c, (uint32_t)w);
    push32(&c, 0);                 /* y */
    push32(&c, 0);                 /* x */
    push32(&c, (uint32_t)(uintptr_t)px);
    push32(&c, (uint32_t)hd);
    push32(&c, 0xDEADBEEFu);       /* return slot */
    fprintf(stderr, "running lifted L_1100C320...\n"); fflush(stderr);
    L_1100C320(&c);
    int rc = (int)c.eax;
    fprintf(stderr, "lifted DecompressToBuffer rc=%d (%u dispatched calls)\n", rc, g_call_count);

    uint32_t crc = pw__crc_buf(px, (size_t)stride * hgt);
    long nz = 0; for (long i = 0; i < (long)stride * hgt; i++) if (px[i]) nz++;
    fprintf(stderr, "pixcrc=%08X nonzero=%ld/%ld\n", crc, nz, (long)stride * hgt);

    int ok = (expect && crc == expect);
    if (expect) printf("%s pixcrc=%08X expected=%08X\n", ok ? "PASS" : "FAIL", crc, expect);
    else        printf("pixcrc=%08X\n", crc);

    if (outpng) { write_png_bgr24(outpng, w, hgt, px, stride); fprintf(stderr, "wrote %s\n", outpng); }

    FreeLibrary(h);
    return (expect && !ok) ? 1 : 0;
}
