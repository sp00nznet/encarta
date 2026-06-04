/*
 * recomp_enc97_iat.c - wire ENC97.EXE's full import table (the "914 imports"),
 * and report exactly what resolves on a modern system.
 *
 * Maps ENC97 (with .reloc applied), walks the import directory, and for every
 * imported symbol resolves a real address and writes it into the IAT slot:
 *
 *   - system DLLs (KERNEL32/USER32/GDI32/WINMM/comdlg32/ADVAPI32/SHELL32/LZ32):
 *     LoadLibrary by name.
 *   - local Encarta DLLs we ship-with / have lifted (DECO_32/ENCAPI32/EEUIL10):
 *     LoadLibrary by full path from analysis\.
 *   - MSVCRT40 (absent on modern Windows, all by-name): redirect to msvcrt.dll.
 *   - anything unresolved (notably MFC40.DLL's 398 by-ordinal imports — absent
 *     and ordinal-bound): point at a shared logging stub so the slot is valid.
 *
 * Prints a per-DLL resolved/stubbed tally. This makes the import wiring concrete
 * and pins down precisely what blocks a full run (MFC40 + a few CRT internals).
 *
 * Build 32-bit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define PREF_BASE 0x400000u
static uint8_t *g_base;
static int32_t g_delta;

static int map_image(const char *path, uint32_t *imgsz_out)
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
    g_base = base; g_delta = (int32_t)((uint32_t)(uintptr_t)base - PREF_BASE);
    IMAGE_DATA_DIRECTORY rd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (g_delta && rd.Size) {
        uint8_t *p = base + rd.VirtualAddress, *end = p + rd.Size;
        while (p < end) {
            PIMAGE_BASE_RELOCATION br = (PIMAGE_BASE_RELOCATION)p;
            uint32_t n = (br->SizeOfBlock - sizeof *br) / 2;
            uint16_t *e = (uint16_t *)(br + 1);
            for (uint32_t i = 0; i < n; i++)
                if ((e[i] >> 12) == IMAGE_REL_BASED_HIGHLOW)
                    *(uint32_t *)(base + br->VirtualAddress + (e[i] & 0xFFF)) += g_delta;
            p += br->SizeOfBlock;
        }
    }
    free(file);
    if (imgsz_out) *imgsz_out = imgsz;
    return 1;
}

/* generic stub for unresolved imports: log first hit per slot, then return 0. */
static unsigned long g_stub_hits;
static int __stdcall import_stub(void) { g_stub_hits++; return 0; }

static HMODULE load_for(const char *dll)
{
    /* local Encarta DLLs: load from analysis\ by full path */
    char path[MAX_PATH];
    if (!_stricmp(dll, "DECO_32.DLL") || !_stricmp(dll, "ENCAPI32.dll") ||
        !_stricmp(dll, "EEUIL10.dll")) {
        snprintf(path, sizeof path, "C:\\encarta\\analysis\\%s", dll);
        return LoadLibraryA(path);
    }
    if (!_stricmp(dll, "MSVCRT40.dll")) return LoadLibraryA("msvcrt.dll");  /* redirect */
    return LoadLibraryA(dll);                                              /* system */
}

int main(int argc, char **argv)
{
    const char *exe = (argc >= 2) ? argv[1] : "C:\\encarta\\analysis\\ENC97.EXE";
    uint32_t imgsz;
    if (!map_image(exe, &imgsz)) { fprintf(stderr, "map failed\n"); return 1; }
    printf("ENC97 mapped @%p (delta %+d)\n\n", (void *)g_base, (int)g_delta);

    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)g_base;
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(g_base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY id = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    PIMAGE_IMPORT_DESCRIPTOR imp = (PIMAGE_IMPORT_DESCRIPTOR)(g_base + id.VirtualAddress);

    int tot = 0, tot_res = 0, tot_stub = 0, ndll = 0, ndll_load = 0;
    printf("%-16s%9s%9s%9s  %-7s\n", "DLL", "imports", "resolved", "stubbed", "loaded?");
    for (; imp->Name; imp++) {
        const char *dll = (const char *)(g_base + imp->Name);
        HMODULE h = load_for(dll);
        ndll++; if (h) ndll_load++;
        /* OriginalFirstThunk holds the import names/ordinals; FirstThunk is the
           IAT we overwrite with resolved addresses. */
        uint32_t oft = imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk;
        uint32_t *lookup = (uint32_t *)(g_base + oft);
        uint32_t *iat = (uint32_t *)(g_base + imp->FirstThunk);
        int n = 0, res = 0, stub = 0;
        for (; lookup[n]; n++) {
            uint32_t ent = lookup[n];
            FARPROC p = NULL;
            if (h) {
                if (ent & 0x80000000u)
                    p = GetProcAddress(h, (LPCSTR)(uintptr_t)(ent & 0xFFFF));   /* by ordinal */
                else
                    p = GetProcAddress(h, (LPCSTR)(g_base + (ent & 0x7FFFFFFF) + 2)); /* by name */
            }
            if (p) { iat[n] = (uint32_t)(uintptr_t)p; res++; }
            else   { iat[n] = (uint32_t)(uintptr_t)import_stub; stub++; }
        }
        tot += n; tot_res += res; tot_stub += stub;
        printf("%-16s%9d%9d%9d  %-7s\n", dll, n, res, stub, h ? "YES" : "NO");
    }
    printf("\nTOTAL: %d imports across %d DLLs (%d loaded) -> %d resolved, %d stubbed\n",
           tot, ndll, ndll_load, tot_res, tot_stub);
    printf("%.1f%% of imports wired to real code\n", 100.0 * tot_res / tot);

    /* OS-level check: can the real Windows loader map+bind ENC97 itself now?
       (MFC40.DLL ships in SysWOW64 on Win11, so every dependency is present.) */
    HMODULE self = LoadLibraryExA(exe, NULL, 0);
    if (self) {
        printf("\nOS loader: LoadLibrary(ENC97.EXE) SUCCEEDED @%p — every dependency "
               "(incl. MFC40 from SysWOW64) is satisfiable on this Win11 system.\n", (void *)self);
        FreeLibrary(self);
    } else {
        printf("\nOS loader: LoadLibrary(ENC97.EXE) failed (gle=%lu)\n", GetLastError());
    }
    return 0;
}
