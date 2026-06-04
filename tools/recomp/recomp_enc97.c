/*
 * recomp_enc97.c - execute lifted ENC97.EXE code (proves lifted main-app code
 * RUNS, not just compiles), including lifted->lifted internal dispatch.
 *
 * ENC97.EXE can't be LoadLibrary'd here (its MFC40 import isn't available), so
 * we MANUALLY MAP its sections at an OS-chosen base and apply its .reloc fixups
 * (so absolute refs, and the lifted code's GVA()/g_image_delta, resolve). We
 * then run lifted functions and compare to the REAL mapped originals:
 *
 *   1. sub_401D10  - LoadCursorA+SetCursor->1, exercises the Win32 import
 *      trampoline (lifted emulated-stack -> real USER32 API).
 *   2. sub_4E3F40 -> sub_4FF190 - a self-contained array search (real loop +
 *      lea-based *36 indexing). Exercises lifted->lifted dispatch and a
 *      differential check over present/absent keys against a synthetic table.
 *   3. sub_4AD870 -> sub_4ADD10 - a __thiscall setter chain (this in ecx,
 *      memory write). Exercises lifted->lifted + register-arg passing.
 *
 * The dispatch table routes internal (original-VA) targets to the lifted C
 * function when present, else falls back to executing the real mapped original;
 * out-of-image targets are real Win32 APIs via the import trampoline.
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

#include "enc97_one_list.h"
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

static uint32_t g_base, g_imgsz, g_nrelocs;

/* import trampoline: run a real stdcall API (or real internal fn) on the
   emulated stack; capture the resulting esp (handles stdcall cleanup). */
static uint32_t T_tgt, T_eax, T_ecx, T_espp4, T_fesp, T_sesp;
static void call_machine(CPU *c, uint32_t target)
{
    T_tgt = target; T_espp4 = c->esp + 4; T_ecx = c->ecx;
    __asm {
        mov T_sesp, esp
        mov ecx, T_ecx          /* thiscall `this` (harmless for stdcall) */
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
    /* internal targets arrive as ORIGINAL VAs (lift.py emits absolute call
       addresses); live API addresses (read from the relocated IAT) arrive as
       real addresses outside the original image range. */
    if (target >= PREF_BASE && target < PREF_BASE + g_imgsz) {
        for (size_t i = 0; i < sizeof g_lifted / sizeof *g_lifted; i++)
            if (g_lifted[i].va == target) { g_lifted[i].fn(c); return; }
        call_machine(c, target + g_image_delta);   /* unlifted internal -> real original */
        return;
    }
    call_machine(c, target);                        /* out of image -> real Win32 API */
}
void dispatch_jmp(CPU *c, uint32_t t) { dispatch(c, t); }
void dispatch_indirect(CPU *c, uint32_t t) { dispatch(c, t); }

static uint8_t *g_estack;
#define EMU_STACK (1u << 20)
static uint32_t call_lifted(lfn fn, uint32_t ecx, const uint32_t *args, int n)
{
    CPU c; memset(&c, 0, sizeof c);
    c.esp = (uint32_t)(uintptr_t)(g_estack + EMU_STACK - 256);
    c.ecx = ecx;
    for (int i = n - 1; i >= 0; i--) push32(&c, args[i]);
    push32(&c, 0xDEADBEEFu);
    fn(&c);
    return c.eax;
}

/* manual-map ENC97.EXE: OS picks a free base, then apply .reloc fixups. */
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
    /* populate the two IAT slots sub_401D10 uses (relocated addresses) */
    HMODULE u = LoadLibraryA("user32.dll");
    *(uint32_t *)(uintptr_t)(0x0058DC80u + delta) = (uint32_t)(uintptr_t)GetProcAddress(u, "LoadCursorA");
    *(uint32_t *)(uintptr_t)(0x0058DC84u + delta) = (uint32_t)(uintptr_t)GetProcAddress(u, "SetCursor");
    return 1;
}

/* a real ENC97 internal function pointer (original VA -> live address) */
#define REALFN(va, sig) ((sig)(uintptr_t)(g_base + ((va) - PREF_BASE)))

int main(int argc, char **argv)
{
    const char *exe = (argc >= 2) ? argv[1] : "C:\\encarta\\analysis\\ENC97.EXE";
    if (!map_image(exe)) { fprintf(stderr, "map failed\n"); return 1; }
    g_estack = malloc(EMU_STACK);
    fprintf(stderr, "ENC97 mapped @%08X (delta %+d, %u relocs); IAT[LoadCursorA/SetCursor] set\n",
            g_base, (int)g_image_delta, g_nrelocs);
    int all_ok = 1;

    /* ---- (1) Win32 import trampoline: sub_401D10 (LoadCursorA+SetCursor->1) -- */
    {
        uint32_t a[] = { 0, 0, 0 };
        int lret = (int)call_lifted(L_00401D10, 0, a, 3);
        int rret = REALFN(0x401D10u, int(__stdcall*)(int,int,int))(0, 0, 0);
        int ok = (lret == 1 && rret == 1);
        all_ok &= ok;
        printf("%s [1] sub_401D10  lifted=%d real=%d (Win32 trampoline)\n",
               ok ? "PASS" : "FAIL", lret, rret);
    }

    /* ---- (2) lifted->lifted: sub_4E3F40 -> sub_4FF190 (array search) --------
       Build a synthetic object: +0x1A4 is the struct sub_4FF190 walks; it reads
       count at +4, array ptr at +0xC; entries are 36 bytes, key at +4 of entry.
       sub_4E3F40(key): key==0xA31B -> 0 (low 16 = 0); else search -> index|0xFFFF.
       We compare lifted vs the real original over several keys. */
    {
        uint8_t *obj = calloc(0x1A4 + 64, 1);
        uint8_t *srch = obj + 0x1A4;
        #define NTAB 4
        uint8_t *tab = calloc(NTAB, 36);
        uint16_t keys[NTAB] = { 0x1111, 0x2222, 0x3333, 0x4444 };
        for (int i = 0; i < NTAB; i++) *(uint32_t *)(tab + i * 36 + 4) = keys[i];
        *(uint32_t *)(srch + 4) = NTAB;                       /* count   */
        *(uint32_t *)(srch + 0xC) = (uint32_t)(uintptr_t)tab; /* array   */
        uint32_t obj_va = (uint32_t)(uintptr_t)obj;           /* ecx = this */

        uint32_t probes[] = { 0x3333, 0x1111, 0x4444, 0xBEEF, 0xA31B };
        int ok = 1;
        for (int p = 0; p < (int)(sizeof probes / sizeof *probes); p++) {
            uint32_t a[] = { probes[p] };
            uint32_t lret = call_lifted(L_004E3F40, obj_va, a, 1) & 0xFFFF;
            /* call the real original via the same machine trampoline (thiscall: ecx=this) */
            CPU rc; memset(&rc, 0, sizeof rc);
            rc.esp = (uint32_t)(uintptr_t)(g_estack + EMU_STACK - 256);
            rc.ecx = obj_va; push32(&rc, probes[p]); push32(&rc, 0xDEADBEEFu);
            call_machine(&rc, 0x4E3F40u + g_image_delta);
            uint32_t real16 = rc.eax & 0xFFFF;
            if (lret != real16) { ok = 0;
                printf("    key %04X: lifted=%04X real=%04X MISMATCH\n", probes[p], lret, real16); }
        }
        all_ok &= ok;
        printf("%s [2] sub_4E3F40->sub_4FF190  array search, 5 keys (lifted==real)\n",
               ok ? "PASS" : "FAIL");
        free(obj); free(tab);
    }

    /* ---- (3) lifted->lifted __thiscall: sub_4AD870 -> sub_4ADD10 (setter) ----
       sub_4AD870(this, v): ecx=this+0x84; sub_4ADD10 writes v to *(ecx+0xA),
       i.e. *(this+0x8E) = v. Verify the memory write matches the real original. */
    {
        uint8_t *objL = calloc(0x100, 1), *objR = calloc(0x100, 1);
        uint32_t v = 0xCAFEF00Du;
        uint32_t a[] = { v };
        call_lifted(L_004AD870, (uint32_t)(uintptr_t)objL, a, 1);

        CPU rc; memset(&rc, 0, sizeof rc);
        rc.esp = (uint32_t)(uintptr_t)(g_estack + EMU_STACK - 256);
        rc.ecx = (uint32_t)(uintptr_t)objR; push32(&rc, v); push32(&rc, 0xDEADBEEFu);
        call_machine(&rc, 0x4AD870u + g_image_delta);

        uint32_t wl = *(uint32_t *)(objL + 0x8E), wr = *(uint32_t *)(objR + 0x8E);
        int ok = (wl == v && wr == v);
        all_ok &= ok;
        printf("%s [3] sub_4AD870->sub_4ADD10  this+0x8E: lifted=%08X real=%08X (==%08X)\n",
               ok ? "PASS" : "FAIL", wl, wr, v);
        free(objL); free(objR);
    }

    printf("%s ENC97 lifted code executes (Win32 trampoline + lifted->lifted dispatch)\n",
           all_ok ? "ALL PASS:" : "FAIL:");
    return all_ok ? 0 : 1;
}
