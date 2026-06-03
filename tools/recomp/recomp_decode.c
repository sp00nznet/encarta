/*
 * recomp_decode.c - fully statically-recompiled DECO_32 FTC decoder.
 *
 * The ENTIRE pipeline (setup + decode) runs as lifted C: OpenDecompressor,
 * SetFIFBuffer (FTC/FIF parse), GetOriginalResolution, SetOutputResolution,
 * SetOutputFormat and DecompressToBuffer and their whole call trees. No
 * original code is executed; the DLL is mapped (LoadLibrary) only to provide
 * its static data image (constant tables + .bss instance table) at 0x11000000.
 *
 * The CRT boundary (malloc/free/calloc/ftol/heap-noop) is C stubs; the single
 * KERNEL32 indirect call (GetVersion) is serviced directly. Output is compared
 * (CRC32) to the oracle baseline. Build 32-bit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "cpu.h"
#include "png_write.h"

/* ---- lifted functions (lifted_codec.c + lifted_setup.c) ---- */
#define DECL(a) void L_##a(CPU *);
#define CODEC(X) X(11001000) X(11004AA0) X(11004BE0) X(11004D60) X(110053E0) \
  X(11006FD0) X(11008030) X(110080F0) X(1100C320) X(11012020) X(110121D0) \
  X(11012330) X(110124C0) X(11012990) X(11012A90) X(11013280) X(11013630) \
  X(110138B0) X(110141D0) X(11014650) X(11015FA0) X(11016750) X(11016840) \
  X(11016E00) X(11018C30) X(110191D0) X(11019780) X(11019800)
#define SETUP(X) X(110050C0) X(11005140) X(11005450) X(110054C0) X(110055C0) \
  X(11005C70) X(11006760) X(11006B50) X(11006D70) X(11006DC0) X(11007300) \
  X(110078A0) X(11008010) X(1100B250) X(1100B630) X(1100B7D0) X(1100BBF0) \
  X(1100BCD0) X(1100C170) X(1100EEB0) X(11011220) X(110116E0) X(11011AA0) X(1101A094)
CODEC(DECL) SETUP(DECL)
#undef DECL

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
static void stub_heap_noop(CPU *c) { c->eax = 0; c->esp += 4; }   /* 1101A844 */

/* ---- dispatch table ---- */
typedef void (*lfn)(CPU *);
typedef struct { uint32_t rva; lfn fn; } entry_t;

static entry_t g_lifted[] = {
#define E(a) { 0x##a, L_##a },
    CODEC(E) SETUP(E)
#undef E
};
static entry_t g_stubs[] = {
    { 0x1101AA58, stub_malloc }, { 0x11019909, stub_malloc },
    { 0x1101AA4B, stub_free },   { 0x11019830, stub_free },
    { 0x110198B9, stub_calloc }, { 0x11019A44, stub_ftol },
    { 0x1101A844, stub_heap_noop },
};

static uint32_t g_image_size = 0;
static uint32_t g_call_count = 0;
typedef DWORD (WINAPI *getver_t)(void);

void dispatch(CPU *c, uint32_t target)
{
    uint32_t rva = target - g_image_delta;   /* back to preferred-base VA */
    g_call_count++;
    if (rva < 0x11000000u || rva >= 0x11000000u + g_image_size) {
        /* out of image -> imported function. Only GetVersion is reached. */
        getver_t gv = (getver_t)(uintptr_t)target;
        c->eax = (uint32_t)gv();
        c->esp += 4;
        return;
    }
    for (size_t i = 0; i < sizeof g_lifted / sizeof *g_lifted; i++)
        if (g_lifted[i].rva == rva) { g_lifted[i].fn(c); return; }
    for (size_t i = 0; i < sizeof g_stubs / sizeof *g_stubs; i++)
        if (g_stubs[i].rva == rva) { g_stubs[i].fn(c); return; }
    fprintf(stderr, "\n*** dispatch: UNLIFTED %08X (rva %08X) — lift it\n", target, rva);
    fflush(stderr); abort();
}
void dispatch_jmp(CPU *c, uint32_t target) { dispatch(c, target); }
void dispatch_indirect(CPU *c, uint32_t target) { dispatch(c, target); }

/* ---- helpers to invoke a lifted cdecl function on the emulated stack ---- */
static uint8_t *g_estack;
#define EMU_STACK (8u * 1024 * 1024)

static uint32_t call_lifted(lfn fn, const uint32_t *args, int n)
{
    CPU c; memset(&c, 0, sizeof c);
    c.esp = (uint32_t)(uintptr_t)(g_estack + EMU_STACK - 1024);
    for (int i = n - 1; i >= 0; i--) push32(&c, args[i]);  /* right-to-left */
    push32(&c, 0xDEADBEEFu);                                /* return slot */
    fn(&c);
    return c.eax;
}

/* Map the DLL's sections as DATA only (no DllMain, no original code executed),
 * at its preferred base 0x11000000. Returns base or 0. Fixes the GetVersion
 * IAT slot so the lifted indirect call resolves. */
static uint32_t map_dll_image(const char *path)
{
    long sz = 0; FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc(sz);
    if (fread(file, 1, sz, f) != (size_t)sz) { fclose(f); free(file); return 0; }
    fclose(f);
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)file;
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(file + dos->e_lfanew);
    uint32_t imgsz = nt->OptionalHeader.SizeOfImage;
    void *base = VirtualAlloc((void *)0x11000000u, imgsz, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (base != (void *)0x11000000u) { free(file); return 0; }   /* need preferred base (delta=0) */
    memcpy(base, file, nt->OptionalHeader.SizeOfHeaders);
    PIMAGE_SECTION_HEADER s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (s[i].SizeOfRawData)
            memcpy((uint8_t *)base + s[i].VirtualAddress, file + s[i].PointerToRawData, s[i].SizeOfRawData);
    free(file);
    /* fix the one imported indirect target the lifted code uses */
    HMODULE k = GetModuleHandleA("kernel32.dll");
    *(uint32_t *)0x110220DCu = (uint32_t)(uintptr_t)GetProcAddress(k, "GetVersion");
    g_image_size = imgsz;
    return 0x11000000u;
}

static uint8_t *read_file(const char *p, long *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(*n); if (d && fread(d, 1, *n, f) != (size_t)*n) { free(d); d = NULL; }
    fclose(f); return d;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: recomp_decode <in.ftc> [crc_hex] [out.png] [dll]\n"); return 2; }
    const char *in = argv[1];
    uint32_t expect = (argc >= 3) ? (uint32_t)strtoul(argv[2], NULL, 16) : 0;
    const char *outpng = (argc >= 4) ? argv[3] : NULL;
    const char *dll = (argc >= 5) ? argv[4] : "C:\\encarta\\analysis\\DECO_32.DLL";

    int use_map = (getenv("RECOMP_MAP") != NULL);
    if (use_map) {
        uint32_t base = map_dll_image(dll);
        if (!base) { fprintf(stderr, "manual map failed (need base 0x11000000 free)\n"); return 1; }
        g_image_delta = 0;
        fprintf(stderr, "data-only map at 0x11000000 (no DllMain, no original code)\n");
    } else {
        HMODULE h = LoadLibraryA(dll);
        if (!h) { fprintf(stderr, "cannot load %s\n", dll); return 1; }
        uint32_t base = (uint32_t)(uintptr_t)h;
        g_image_delta = base - 0x11000000u;
        PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)h;
        PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((uint8_t *)h + dos->e_lfanew);
        g_image_size = nt->OptionalHeader.SizeOfImage;
    }

    long fsz = 0; uint8_t *data = read_file(in, &fsz);
    if (!data) { fprintf(stderr, "cannot read %s\n", in); return 1; }
    g_estack = malloc(EMU_STACK);

    /* ---- fully lifted setup + decode ---- */
    uint32_t hbuf = 0;  /* OpenDecompressor writes the handle here */
    uint32_t a_open[]  = { (uint32_t)(uintptr_t)&hbuf };
    int rc = (int)call_lifted(L_1100B630, a_open, 1);
    int hd = (int)hbuf;
    fprintf(stderr, "L_OpenDecompressor rc=%d handle=%d\n", rc, hd);
    if (rc || !hd) { fprintf(stderr, "open failed\n"); return 1; }

    uint32_t a_fif[] = { (uint32_t)hd, (uint32_t)(uintptr_t)data, (uint32_t)fsz };
    rc = (int)call_lifted(L_1100B7D0, a_fif, 3);
    fprintf(stderr, "L_SetFIFBuffer rc=%d\n", rc);
    if (rc) { fprintf(stderr, "setfif failed (rc=%d)\n", rc); return 1; }

    uint32_t w = 0, hgt = 0, ex = 0;
    uint32_t a_res[] = { (uint32_t)hd, (uint32_t)(uintptr_t)&w, (uint32_t)(uintptr_t)&hgt, (uint32_t)(uintptr_t)&ex };
    rc = (int)call_lifted(L_1100BBF0, a_res, 4);
    fprintf(stderr, "L_GetOriginalResolution rc=%d %ux%u extra=%u\n", rc, w, hgt, ex);
    if (rc || !w || !hgt) { fprintf(stderr, "getres failed\n"); return 1; }

    uint32_t a_sr[] = { (uint32_t)hd, w, hgt };
    rc = (int)call_lifted(L_1100BCD0, a_sr, 3);
    fprintf(stderr, "L_SetOutputResolution rc=%d\n", rc);
    if (rc) { fprintf(stderr, "setres failed\n"); return 1; }

    uint32_t a_fmt[] = { (uint32_t)hd, 3, 2, 1, 4, 0 };
    rc = (int)call_lifted(L_1100C170, a_fmt, 6);
    fprintf(stderr, "L_SetOutputFormat rc=%d\n", rc);
    if (rc) { fprintf(stderr, "setfmt failed\n"); return 1; }

    uint32_t stride = w * 3;
    uint8_t *px = calloc(1, (size_t)stride * hgt);
    uint32_t a_dec[] = { (uint32_t)hd, (uint32_t)(uintptr_t)px, 0, 0, w, hgt, stride };
    fprintf(stderr, "running fully lifted pipeline...\n"); fflush(stderr);
    rc = (int)call_lifted(L_1100C320, a_dec, 7);
    fprintf(stderr, "L_DecompressToBuffer rc=%d (%u dispatched calls)\n", rc, g_call_count);

    uint32_t crc = pw__crc_buf(px, (size_t)stride * hgt);
    long nz = 0; for (long i = 0; i < (long)stride * hgt; i++) if (px[i]) nz++;
    fprintf(stderr, "pixcrc=%08X nonzero=%ld/%ld\n", crc, nz, (long)stride * hgt);

    int ok = (expect && crc == expect);
    if (expect) printf("%s pixcrc=%08X expected=%08X\n", ok ? "PASS" : "FAIL", crc, expect);
    else        printf("pixcrc=%08X\n", crc);
    if (outpng) { write_png_bgr24(outpng, w, hgt, px, stride); fprintf(stderr, "wrote %s\n", outpng); }

    return (expect && !ok) ? 1 : 0;
}
