/*
 * fifdecode - FIF image decoder using DECO_32.DLL bridge
 *
 * Loads Iterated Systems' DECO_32.DLL at runtime and uses its exported
 * functions to decode FIF (Fractal Image Format) images to BMP files.
 *
 * Usage:
 *   fifdecode <input.fif> <output.bmp> [deco_32.dll path]
 *   fifdecode -e                        List DLL exports (diagnostic)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "fifdecode.h"

/* Vectored exception handler for debugging DLL crashes */
static LONG CALLBACK crash_handler(PEXCEPTION_POINTERS info)
{
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        fprintf(stderr, "  CRASH at EIP=0x%08lX\n",
                (unsigned long)info->ContextRecord->Eip);
        fprintf(stderr, "  Access type: %s address 0x%08lX\n",
                info->ExceptionRecord->ExceptionInformation[0] ? "WRITE" : "READ",
                (unsigned long)info->ExceptionRecord->ExceptionInformation[1]);
        fprintf(stderr, "  EAX=0x%08lX EBX=0x%08lX ECX=0x%08lX EDX=0x%08lX\n",
                info->ContextRecord->Eax, info->ContextRecord->Ebx,
                info->ContextRecord->Ecx, info->ContextRecord->Edx);
        fprintf(stderr, "  ESI=0x%08lX EDI=0x%08lX ESP=0x%08lX EBP=0x%08lX\n",
                info->ContextRecord->Esi, info->ContextRecord->Edi,
                info->ContextRecord->Esp, info->ContextRecord->Ebp);
        fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Default DLL search paths */
static const char *dll_search_paths[] = {
    "DECO_32.DLL",
    ".\\DECO_32.DLL",
    "C:\\encarta\\analysis\\DECO_32.DLL",
    "F:\\AAMSSTP\\ENCARTA\\DECO_32.DLL",
    NULL
};

/* ------------------------------------------------------------------ */
/* DLL loading                                                         */
/* ------------------------------------------------------------------ */

static HMODULE find_and_load_dll(const char *explicit_path)
{
    HMODULE h;

    if (explicit_path) {
        h = LoadLibraryA(explicit_path);
        if (h) return h;
        fprintf(stderr, "Warning: cannot load '%s' (error %lu)\n",
                explicit_path, GetLastError());
    }

    for (int i = 0; dll_search_paths[i]; i++) {
        h = LoadLibraryA(dll_search_paths[i]);
        if (h) {
            fprintf(stderr, "Loaded DLL from: %s\n", dll_search_paths[i]);
            return h;
        }
    }

    return NULL;
}

#define RESOLVE(funcs, name) do { \
    (funcs)->name = (pfn##name)GetProcAddress((funcs)->hDLL, #name); \
    if (!(funcs)->name) { \
        fprintf(stderr, "Warning: export '%s' not found\n", #name); \
    } \
} while(0)

#define REQUIRE(funcs, name) do { \
    RESOLVE(funcs, name); \
    if (!(funcs)->name) { \
        fprintf(stderr, "Error: required export '%s' not found\n", #name); \
        return false; \
    } \
} while(0)

static bool resolve_exports(DecoFuncs *f)
{
    REQUIRE(f, OpenDecompressor);
    REQUIRE(f, CloseDecompressor);
    REQUIRE(f, SetFIFBuffer);
    REQUIRE(f, ClearFIFBuffer);
    REQUIRE(f, DecompressToBuffer);

    /* Optional but useful */
    RESOLVE(f, GetFIFFTTFileName);
    RESOLVE(f, SetFTTBuffer);
    RESOLVE(f, ClearFTTBuffer);
    RESOLVE(f, GetOriginalResolution);
    RESOLVE(f, SetOutputResolution);
    RESOLVE(f, GetOutputResolution);
    RESOLVE(f, SetOutputFormat);
    RESOLVE(f, GetOutputFormat);
    RESOLVE(f, GetFIFColorTable);
    RESOLVE(f, SetOutputColorTable);
    RESOLVE(f, GetOutputColorTable);
    RESOLVE(f, GetPhysicalDimensions);
    RESOLVE(f, DecompressToYUV);
    RESOLVE(f, GetDecoVersion);

    return true;
}

/* ------------------------------------------------------------------ */
/* BMP writer                                                          */
/* ------------------------------------------------------------------ */

#pragma pack(push, 1)
typedef struct {
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
} BMPFILEHEADER;

typedef struct {
    uint32_t biSize;
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
} BMPINFOHEADER;
#pragma pack(pop)

static bool write_bmp(const char *path, int width, int height,
                      int bpp, const uint8_t *pixels, int stride)
{
    int row_bytes = ((width * bpp + 31) / 32) * 4; /* BMP row alignment */
    int image_size = row_bytes * abs(height);

    BMPFILEHEADER fh;
    BMPINFOHEADER ih;

    memset(&fh, 0, sizeof(fh));
    memset(&ih, 0, sizeof(ih));

    fh.bfType = 0x4D42; /* 'BM' */
    fh.bfOffBits = sizeof(BMPFILEHEADER) + sizeof(BMPINFOHEADER);
    fh.bfSize = fh.bfOffBits + image_size;

    ih.biSize = sizeof(BMPINFOHEADER);
    ih.biWidth = width;
    ih.biHeight = height; /* Positive = bottom-up */
    ih.biPlanes = 1;
    ih.biBitCount = (uint16_t)bpp;
    ih.biCompression = 0; /* BI_RGB */
    ih.biSizeImage = image_size;
    ih.biXPelsPerMeter = 2835; /* 72 DPI */
    ih.biYPelsPerMeter = 2835;

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "Error: cannot create '%s'\n", path);
        return false;
    }

    fwrite(&fh, 1, sizeof(fh), f);
    fwrite(&ih, 1, sizeof(ih), f);

    /* Write pixel rows (BMP is bottom-up, FIF output may already be) */
    int src_stride = (stride > 0) ? stride : width * (bpp / 8);
    for (int y = 0; y < abs(height); y++) {
        const uint8_t *row = pixels + y * src_stride;
        fwrite(row, 1, width * (bpp / 8), f);
        /* Pad to 4-byte alignment */
        int pad = row_bytes - width * (bpp / 8);
        if (pad > 0) {
            uint8_t zeros[4] = {0};
            fwrite(zeros, 1, pad, f);
        }
    }

    fclose(f);
    return true;
}

/* ------------------------------------------------------------------ */
/* List DLL exports (diagnostic)                                       */
/* ------------------------------------------------------------------ */

static void list_exports(const char *dll_path)
{
    HMODULE h = find_and_load_dll(dll_path);
    if (!h) {
        fprintf(stderr, "Error: cannot load DECO_32.DLL\n");
        return;
    }

    /* Parse PE export table manually */
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)h;
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((BYTE*)h + dos->e_lfanew);
    PIMAGE_EXPORT_DIRECTORY exports = (PIMAGE_EXPORT_DIRECTORY)(
        (BYTE*)h + nt->OptionalHeader.DataDirectory[0].VirtualAddress);

    DWORD *names = (DWORD*)((BYTE*)h + exports->AddressOfNames);
    WORD  *ordinals = (WORD*)((BYTE*)h + exports->AddressOfNameOrdinals);
    DWORD *functions = (DWORD*)((BYTE*)h + exports->AddressOfFunctions);

    printf("DECO_32.DLL exports (%lu functions):\n", exports->NumberOfNames);
    printf("%-4s  %-30s  %-10s\n", "Ord", "Name", "RVA");

    for (DWORD i = 0; i < exports->NumberOfNames; i++) {
        const char *name = (const char*)((BYTE*)h + names[i]);
        WORD ordinal = ordinals[i] + (WORD)exports->Base;
        DWORD rva = functions[ordinals[i]];
        printf("%-4u  %-30s  0x%08lX\n", ordinal, name, rva);
    }

    FreeLibrary(h);
}

/* ------------------------------------------------------------------ */
/* FIF decode pipeline                                                 */
/* ------------------------------------------------------------------ */

static uint8_t *read_file(const char *path, long *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *data = (uint8_t *)malloc(size);
    if (!data) { fclose(f); return NULL; }

    fread(data, 1, size, f);
    fclose(f);

    *out_size = size;
    return data;
}

static int decode_fif(const char *input_path, const char *output_path,
                      const char *dll_path)
{
    DecoFuncs funcs;
    memset(&funcs, 0, sizeof(funcs));

    /* Try to disable DEP for this process so DECO_32.DLL can execute
       generated code from data pages (common in Win95-era DLLs) */
    {
        typedef BOOL (WINAPI *pfnSetProcessDEPPolicy)(DWORD);
        HMODULE hKernel = GetModuleHandleA("kernel32.dll");
        if (hKernel) {
            pfnSetProcessDEPPolicy pSetDEP = (pfnSetProcessDEPPolicy)
                GetProcAddress(hKernel, "SetProcessDEPPolicy");
            if (pSetDEP) {
                if (pSetDEP(0))
                    fprintf(stderr, "DEP disabled for process\n");
                else
                    fprintf(stderr, "Warning: cannot disable DEP (error %lu)\n",
                            GetLastError());
            }
        }
    }

    AddVectoredExceptionHandler(1, crash_handler);

    /* Load DLL */
    funcs.hDLL = find_and_load_dll(dll_path);
    if (!funcs.hDLL) {
        fprintf(stderr, "Error: cannot find DECO_32.DLL\n");
        fprintf(stderr, "Searched in:\n");
        for (int i = 0; dll_search_paths[i]; i++)
            fprintf(stderr, "  %s\n", dll_search_paths[i]);
        if (dll_path)
            fprintf(stderr, "  %s\n", dll_path);
        return 1;
    }

    if (!resolve_exports(&funcs)) {
        FreeLibrary(funcs.hDLL);
        return 1;
    }

    fprintf(stderr, "Exports resolved successfully\n");

    /* Make ALL DLL pages RWX to work around DEP issues */
    {
        PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)funcs.hDLL;
        PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((BYTE*)funcs.hDLL + dos->e_lfanew);
        DWORD image_size = nt->OptionalHeader.SizeOfImage;
        DWORD old_protect;
        if (VirtualProtect(funcs.hDLL, image_size,
                           PAGE_EXECUTE_READWRITE, &old_protect)) {
            fprintf(stderr, "DLL pages set to RWX (%lu bytes)\n", image_size);
        } else {
            fprintf(stderr, "Warning: VirtualProtect failed (error %lu)\n",
                    GetLastError());
        }
    }

    /* Skip GetDecoVersion — may crash if called before OpenDecompressor */

    /* Read FIF file */
    long fif_size = 0;
    uint8_t *fif_data = read_file(input_path, &fif_size);
    if (!fif_data) {
        fprintf(stderr, "Error: cannot read '%s'\n", input_path);
        FreeLibrary(funcs.hDLL);
        return 1;
    }

    fprintf(stderr, "FIF file: %s (%ld bytes)\n", input_path, fif_size);

    /* Open decompressor — returns error code, writes handle to output param */
    fprintf(stderr, "Calling OpenDecompressor...\n");
    fflush(stderr);
    HDECOMP hd = NULL;
    int open_ret = funcs.OpenDecompressor(&hd);
    fprintf(stderr, "OpenDecompressor returned %d, handle=%p\n", open_ret, hd);
    fflush(stderr);
    if (open_ret != 0 || !hd) {
        fprintf(stderr, "Error: OpenDecompressor() failed (ret=%d)\n", open_ret);
        free(fif_data);
        FreeLibrary(funcs.hDLL);
        return 1;
    }

    /* Set FIF buffer */
    fprintf(stderr, "Calling SetFIFBuffer (%ld bytes)...\n", fif_size);
    fflush(stderr);
    int ret = funcs.SetFIFBuffer(hd, fif_data, (int)fif_size);
    fprintf(stderr, "SetFIFBuffer returned %d\n", ret);
    fflush(stderr);

    /* Check for FTT file */
    if (funcs.GetFIFFTTFileName) {
        const char *ftt_name = funcs.GetFIFFTTFileName(hd);
        fprintf(stderr, "GetFIFFTTFileName returned: %s\n",
                ftt_name ? ftt_name : "(null)");
    }

    /* Always try to load FTT file — the DLL may need it even when
     * GetFIFFTTFileName returns null (FTC files embed FTT references) */
    {
        const char *ftt_names[] = {"AAA.FTT", "aaa.ftt", NULL};
        char ftt_path[512];
        int ftt_loaded = 0;

        for (int n = 0; !ftt_loaded && ftt_names[n]; n++) {
            /* Try same directory as input */
            const char *last_sep = strrchr(input_path, '\\');
            if (!last_sep) last_sep = strrchr(input_path, '/');
            if (last_sep) {
                int dir_len = (int)(last_sep - input_path + 1);
                snprintf(ftt_path, sizeof(ftt_path), "%.*s%s",
                         dir_len, input_path, ftt_names[n]);
            } else {
                snprintf(ftt_path, sizeof(ftt_path), "%s", ftt_names[n]);
            }

            long ftt_size = 0;
            uint8_t *ftt_data = read_file(ftt_path, &ftt_size);
            if (ftt_data && funcs.SetFTTBuffer) {
                ret = funcs.SetFTTBuffer(hd, ftt_data, (int)ftt_size);
                fprintf(stderr, "Loaded FTT: %s (%ld bytes), SetFTTBuffer returned %d\n",
                        ftt_path, ftt_size, ret);
                free(ftt_data);
                ftt_loaded = 1;
            }
        }
        if (!ftt_loaded)
            fprintf(stderr, "Warning: no FTT file found\n");
    }

    /* Get original resolution */
    int width = 0, height = 0;
    if (funcs.GetOriginalResolution) {
        funcs.GetOriginalResolution(hd, &width, &height);
        fprintf(stderr, "Original resolution: %d x %d\n", width, height);
    }

    if (width <= 0 || height <= 0) {
        fprintf(stderr, "Error: invalid image dimensions %d x %d\n",
                width, height);
        funcs.ClearFIFBuffer(hd);
        funcs.CloseDecompressor(hd);
        free(fif_data);
        FreeLibrary(funcs.hDLL);
        return 1;
    }

    /* Set output resolution — use half resolution (the "fast" 1:1 path
     * in the DLL, which avoids scaling computation issues) */
    int out_w = width / 2;
    int out_h = height / 2;
    if (funcs.SetOutputResolution) {
        int sr_ret = funcs.SetOutputResolution(hd, out_w, out_h);
        fprintf(stderr, "SetOutputResolution(%d,%d) returned %d\n",
                out_w, out_h, sr_ret);
        if (sr_ret != 0) {
            /* Fall back to full resolution */
            out_w = width;
            out_h = height;
            sr_ret = funcs.SetOutputResolution(hd, out_w, out_h);
            fprintf(stderr, "SetOutputResolution(%d,%d) retry returned %d\n",
                    out_w, out_h, sr_ret);
        }
    }

    /* Set output format: BGR24 via SetOutputFormat(handle, B=3, G=2, R=1)
     * Each arg is a 1-based channel ID: 1=R, 2=G, 3=B
     * Args are listed in byte offset order: byte0=B(3), byte1=G(2), byte2=R(1)
     */
    /* SetOutputFormat(handle, ch1, ch2, ch3, ch4, dither)
     * ch1-ch3 = channel IDs (1=R, 2=G, 3=B) in byte offset order
     * ch4 = 4 means "no 4th channel" (3 bytes/pixel)
     *        5 means "padding byte" (4 bytes/pixel)
     * dither = 0 (off) or 1 (on) */
    if (funcs.SetOutputFormat) {
        ret = funcs.SetOutputFormat(hd, 3, 2, 1, 4, 0);
        fprintf(stderr, "SetOutputFormat(3,2,1,4,0) returned %d\n", ret);
    }

    /* Allocate output buffer */
    int stride = ((out_w * 3 + 3) & ~3); /* 4-byte aligned row stride */
    int buf_size = stride * out_h;
    uint8_t *pixels = (uint8_t *)calloc(1, buf_size);
    if (!pixels) {
        fprintf(stderr, "Error: cannot allocate %d bytes for image\n", buf_size);
        funcs.ClearFIFBuffer(hd);
        funcs.CloseDecompressor(hd);
        free(fif_data);
        FreeLibrary(funcs.hDLL);
        return 1;
    }

    /* Set output stride via SetDecompressCallback.
     * SetDecompressCallback(handle, stride_or_callback, mode)
     * When mode=0: [4Ah]=stride (used by output copy), no progress callback.
     * When mode=1: bottom-up stride, [4Ah]=stride.
     * When mode=2: [4Ah]=callback function pointer.
     */
    {
        typedef int (__cdecl *pfnSetDecompressCallback)(HDECOMP, int, int);
        pfnSetDecompressCallback pSetCB = (pfnSetDecompressCallback)
            GetProcAddress(funcs.hDLL, "SetDecompressCallback");
        if (pSetCB) {
            /* mode=0 (top-down), stride value */
            ret = pSetCB(hd, stride, 0);
            fprintf(stderr, "SetDecompressCallback(stride=%d, mode=0) returned %d\n",
                    stride, ret);
        } else {
            fprintf(stderr, "Warning: SetDecompressCallback not found\n");
        }
    }

    /* Dump instance state for debugging */
    {
        uintptr_t handle_val = (uintptr_t)hd;
        uintptr_t *instance_table = (uintptr_t *)((uint8_t *)funcs.hDLL + 0x1E000);
        uint8_t *inst = (uint8_t *)instance_table[handle_val];
        if (inst) {
            fprintf(stderr, "Instance dump (handle=%u, ptr=%p):\n", (unsigned)handle_val, inst);
            fprintf(stderr, "  [00h] sub-obj  = %p\n", *(void**)(inst));
            fprintf(stderr, "  [04h]          = %p\n", *(void**)(inst+4));
            fprintf(stderr, "  [08h]          = 0x%08X\n", *(uint32_t*)(inst+8));
            fprintf(stderr, "  [0Ch]          = 0x%08X\n", *(uint32_t*)(inst+0xC));
            fprintf(stderr, "  [14h]          = 0x%02X\n", inst[0x14]);
            fprintf(stderr, "  [15h] width    = %u\n", *(uint16_t*)(inst+0x15));
            fprintf(stderr, "  [17h] height   = %u\n", *(uint16_t*)(inst+0x17));
            fprintf(stderr, "  [3Bh]          = 0x%08X\n", *(uint32_t*)(inst+0x3B));
            fprintf(stderr, "  [3Fh] R_off    = %u\n", *(uint16_t*)(inst+0x3F));
            fprintf(stderr, "  [41h] G_off    = %u\n", *(uint16_t*)(inst+0x41));
            fprintf(stderr, "  [43h] B_off    = %u\n", *(uint16_t*)(inst+0x43));
            fprintf(stderr, "  [45h] n_comp   = %u\n", *(uint16_t*)(inst+0x45));
            fprintf(stderr, "  [47h] fmt_type = %u\n", inst[0x47]);
            fprintf(stderr, "  [48h] dither   = %u\n", *(uint16_t*)(inst+0x48));
            fprintf(stderr, "  [4Ah] stride/cb= 0x%08X (%u)\n",
                    *(uint32_t*)(inst+0x4A), *(uint32_t*)(inst+0x4A));
            fprintf(stderr, "  [4Eh] cb_mode  = %u\n", *(uint16_t*)(inst+0x4E));

            /* Check the sub-object at [00h] */
            uint8_t *sub = *(uint8_t**)(inst);
            if (sub) {
                fprintf(stderr, "  Sub-obj [89Ch]= 0x%08X\n", *(uint32_t*)(sub+0x89C));
                fprintf(stderr, "  Sub-obj [8A0h]= 0x%08X\n", *(uint32_t*)(sub+0x8A0));
                fprintf(stderr, "  Sub-obj [8B9h]= %p\n", *(void**)(sub+0x8B9));
                /* Dump the info structure pointed to by [8B9h] */
                uint8_t *info = *(uint8_t**)(sub+0x8B9);
                if (info) {
                    fprintf(stderr, "  Info [00h-03h]= %02x %02x %02x %02x\n",
                            info[0], info[1], info[2], info[3]);
                    fprintf(stderr, "  Info [04h]    = %u\n", *(uint16_t*)(info+4));
                    fprintf(stderr, "  Info [18h]    = %u (width?)\n", *(uint32_t*)(info+0x18));
                    fprintf(stderr, "  Info [1Ch]    = %u (height?)\n", *(uint32_t*)(info+0x1C));
                    fprintf(stderr, "  Info [38h]    = %u\n", *(uint32_t*)(info+0x38));
                    fprintf(stderr, "  Info [3Ch]    = %u\n", *(uint32_t*)(info+0x3C));
                    fprintf(stderr, "  Info [41h]    = %p\n", *(void**)(info+0x41));
                }
            }
            /* Instance [19h] and [1Dh] = resolution scaling floats */
            fprintf(stderr, "  [19h] x_scale  = %f\n", *(float*)(inst+0x19));
            fprintf(stderr, "  [1Dh] y_scale  = %f\n", *(float*)(inst+0x1D));
        }
    }

    /* The DLL has a pointer arithmetic bug: it computes output addresses as
     * buffer + 0x767C3B20 (a ~2GB offset). Pre-allocate memory at that
     * target range so the writes succeed. This requires LARGE_ADDRESS_AWARE. */
    {
        uintptr_t target = (uintptr_t)pixels + 0x767C3B20UL;
        uintptr_t page_base = target & ~0xFFFFUL;
        void *mapped = VirtualAlloc((void*)page_base, 0x100000,
                                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        fprintf(stderr, "VirtualAlloc at 0x%08lX: %p\n",
                (unsigned long)page_base, mapped);
    }

    /* Pre-allocate large address ranges the DLL may try to write to.
     * The DLL computes output addresses with large offsets that can
     * land anywhere in the 32-bit address space. */
    for (uintptr_t addr = 0x10000000UL; addr < 0xFFF00000UL; addr += 0x10000000UL) {
        VirtualAlloc((void*)addr, 0x100000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }

    /* Try DecompressToYUV first — more reliable than DecompressToBuffer
     * which has pointer arithmetic issues on modern Windows.
     * Note: DLL internally decodes at original resolution for FTC,
     * so we use out_w/out_h for the output buffers. The DLL's
     * DecompressToYUV uses the resolution set via SetOutputResolution. */
    ret = -1;
    if (funcs.DecompressToYUV) {
        fprintf(stderr, "Trying DecompressToYUV(%d x %d)...\n", out_w, out_h);
        int yuv_stride = ((out_w + 3) & ~3);
        int y_size = yuv_stride * out_h;
        int uv_stride = ((out_w / 2 + 3) & ~3);
        int uv_size = uv_stride * (out_h / 2);
        uint8_t *y_buf = (uint8_t *)calloc(1, y_size);
        uint8_t *u_buf = (uint8_t *)calloc(1, uv_size);
        uint8_t *v_buf = (uint8_t *)calloc(1, uv_size);
        if (y_buf && u_buf && v_buf) {
            __try {
                ret = funcs.DecompressToYUV(hd, y_buf, u_buf, v_buf,
                                             yuv_stride, uv_stride);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                fprintf(stderr, "DecompressToYUV crashed (exception 0x%08lX)\n",
                        GetExceptionCode());
                ret = -1;
            }
            fprintf(stderr, "DecompressToYUV returned %d\n", ret);
            if (ret == 0) {
                /* DLL outputs in GBR plane order (not YUV!) for FTC:
                 * y_buf = Green, u_buf = Blue, v_buf = Red */
                for (int row = 0; row < out_h; row++) {
                    for (int col = 0; col < out_w; col++) {
                        int pi = row * yuv_stride + col;
                        int ci = (row / 2) * uv_stride + (col / 2);
                        uint8_t g_val = y_buf[pi];
                        uint8_t b_val = u_buf[ci];
                        uint8_t r_val = v_buf[ci];
                        int idx = row * stride + col * 3;
                        pixels[idx + 0] = b_val;
                        pixels[idx + 1] = g_val;
                        pixels[idx + 2] = r_val;
                    }
                }
            }
        }
        free(y_buf); free(u_buf); free(v_buf);
    }

    /* Fallback: try DecompressToBuffer with SEH protection */
    if (ret != 0) {
        fprintf(stderr, "Trying DecompressToBuffer(%d x %d)...\n", out_w, out_h);
        __try {
            ret = funcs.DecompressToBuffer(hd, pixels, stride);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            fprintf(stderr, "DecompressToBuffer crashed (exception 0x%08lX)\n",
                    GetExceptionCode());
            ret = -1;
        }
        fprintf(stderr, "DecompressToBuffer returned %d\n", ret);
    }

    /* Check if pixels have data */
    {
        int nonzero = 0;
        for (int i = 0; i < buf_size && nonzero < 10; i++)
            if (pixels[i]) nonzero++;
        fprintf(stderr, "Pixel buffer: %d non-zero bytes in first scan "
                        "(buf_size=%d)\n", nonzero, buf_size);
        /* Dump first 32 bytes */
        fprintf(stderr, "First 32 bytes: ");
        for (int i = 0; i < 32 && i < buf_size; i++)
            fprintf(stderr, "%02x ", pixels[i]);
        fprintf(stderr, "\n");
    }

    /* Write BMP */
    if (write_bmp(output_path, out_w, out_h, 24, pixels, stride)) {
        fprintf(stderr, "Wrote: %s (%d x %d, 24-bit BMP)\n",
                output_path, out_w, out_h);
    }

    /* Cleanup */
    free(pixels);
    funcs.ClearFIFBuffer(hd);
    funcs.CloseDecompressor(hd);
    free(fif_data);
    FreeLibrary(funcs.hDLL);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "fifdecode - FIF image decoder (DECO_32.DLL bridge)\n\n");
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  fifdecode <input.fif> <output.bmp> [deco_32.dll]\n");
        fprintf(stderr, "  fifdecode -e [deco_32.dll]   List DLL exports\n");
        return 1;
    }

    if (strcmp(argv[1], "-e") == 0) {
        list_exports(argc >= 3 ? argv[2] : NULL);
        return 0;
    }

    if (argc < 3) {
        fprintf(stderr, "Error: need both input and output paths\n");
        return 1;
    }

    const char *dll_path = (argc >= 4) ? argv[3] : NULL;
    return decode_fif(argv[1], argv[2], dll_path);
}
