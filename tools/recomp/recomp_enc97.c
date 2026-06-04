/*
 * recomp_enc97.c - execute a lifted ENC97.EXE function (proves lifted main-app
 * code RUNS, not just compiles).
 *
 * ENC97.EXE can't be LoadLibrary'd here (its MFC40 import isn't available), so
 * we MANUALLY MAP its sections at the preferred base 0x400000 and populate only
 * the IAT slots the target function needs (LoadCursorA/SetCursor from USER32).
 * The lifted function then runs, calling the real Win32 APIs through the same
 * import trampoline proven on ENCAPI32. We also call the REAL mapped function
 * for a side-by-side check.
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

void L_00401D10(CPU *);
static uint32_t g_lo, g_hi;

/* import trampoline (as proven on ENCAPI32): run a real stdcall API on the
   emulated stack; capture the API's resulting esp (stdcall cleanup). */
static uint32_t T_tgt, T_eax, T_espp4, T_fesp, T_sesp;
static void call_import(CPU *c, uint32_t target)
{
    T_tgt = target; T_espp4 = c->esp + 4;
    __asm {
        mov T_sesp, esp
        mov esp, T_espp4
        call dword ptr [T_tgt]
        mov T_fesp, esp
        mov T_eax, eax
        mov esp, T_sesp
    }
    c->eax = T_eax; c->esp = T_fesp;
}

void dispatch(CPU *c, uint32_t target)
{
    if (target >= g_lo && target < g_hi) { fprintf(stderr, "unlifted internal %08X\n", target); abort(); }
    call_import(c, target);   /* out of image -> real Win32 API */
}
void dispatch_jmp(CPU *c, uint32_t t) { dispatch(c, t); }
void dispatch_indirect(CPU *c, uint32_t t) { dispatch(c, t); }

static uint8_t *g_estack;
#define EMU_STACK (1u << 20)
static uint32_t call_lifted(void (*fn)(CPU *), const uint32_t *args, int n)
{
    CPU c; memset(&c, 0, sizeof c);
    c.esp = (uint32_t)(uintptr_t)(g_estack + EMU_STACK - 256);
    for (int i = n - 1; i >= 0; i--) push32(&c, args[i]);
    push32(&c, 0xDEADBEEFu);
    fn(&c);
    return c.eax;
}

/* manual-map ENC97.EXE: OS picks a free base, then we apply .reloc fixups so
   absolute addresses (and the lifted code's GVA(), via g_image_delta) resolve. */
static uint32_t g_base;   /* actual mapped base */
static uint32_t g_nrelocs;
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

    g_base = (uint32_t)(uintptr_t)base;
    int32_t delta = (int32_t)(g_base - PREF_BASE);
    /* apply base relocations */
    IMAGE_DATA_DIRECTORY rd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (delta && rd.Size) {
        uint8_t *p = base + rd.VirtualAddress, *end = p + rd.Size;
        while (p < end) {
            PIMAGE_BASE_RELOCATION br = (PIMAGE_BASE_RELOCATION)p;
            uint32_t n = (br->SizeOfBlock - sizeof *br) / 2;
            uint16_t *e = (uint16_t *)(br + 1);
            for (uint32_t i = 0; i < n; i++) {
                if ((e[i] >> 12) == IMAGE_REL_BASED_HIGHLOW) {
                    *(uint32_t *)(base + br->VirtualAddress + (e[i] & 0xFFF)) += delta;
                    g_nrelocs++;
                }
            }
            p += br->SizeOfBlock;
        }
    }
    free(file);
    g_image_delta = delta;
    g_lo = g_base; g_hi = g_base + imgsz;
    /* populate the two IAT slots sub_401D10 uses (relocated addresses) */
    HMODULE u = LoadLibraryA("user32.dll");
    *(uint32_t *)(uintptr_t)(0x0058DC80u + delta) = (uint32_t)(uintptr_t)GetProcAddress(u, "LoadCursorA");
    *(uint32_t *)(uintptr_t)(0x0058DC84u + delta) = (uint32_t)(uintptr_t)GetProcAddress(u, "SetCursor");
    return 1;
}

int main(int argc, char **argv)
{
    const char *exe = (argc >= 2) ? argv[1] : "C:\\encarta\\analysis\\ENC97.EXE";
    if (!map_image(exe)) { fprintf(stderr, "map failed\n"); return 1; }
    g_estack = malloc(EMU_STACK);
    fprintf(stderr, "ENC97 mapped @%08X (delta %+d, %u relocs applied); IAT[LoadCursorA/SetCursor] populated\n",
            g_base, (int)g_image_delta, g_nrelocs);

    /* lifted sub_401D10(0,0,0): LoadCursorA(0,IDC_ARROW)+SetCursor -> return 1 */
    uint32_t a[] = { 0, 0, 0 };
    int lret = (int)call_lifted(L_00401D10, a, 3);

    /* real sub_401D10 at 0x401D10 (stdcall, 3 args), now that text is mapped+IAT set */
    typedef int(__stdcall *pf)(int, int, int);
    pf real = (pf)(uintptr_t)(g_base + 0x1D10u);
    int rret = real(0, 0, 0);

    fprintf(stderr, "lifted L_00401D10 ret=%d ; real sub_401D10 ret=%d\n", lret, rret);
    int ok = (lret == 1 && rret == 1 && lret == rret);
    printf("%s ENC97 sub_401D10 executes (lifted==real==1, real Win32 via trampoline)\n",
           ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
