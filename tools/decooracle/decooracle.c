/*
 * decooracle - faithful DECO_32.DLL bridge (the "oracle")
 *
 * Loads the original Iterated Systems DECO_32.DLL and drives its export API
 * with the CORRECT signatures and call sequence reverse-engineered from the
 * disassembly (Hex-Rays). Produces full-colour BGR24 output for FTC/FIF
 * images. This is the ground-truth reference for the static-recompilation
 * effort.
 *
 * Build: 32-bit (Win32). The DLL is PE32 i386 and runs natively under WOW64 —
 * no DEP tricks, no manual mapping, no RWX hacks required. The DLL was never
 * the problem; the previous bridge just called the exports with wrong arg
 * counts.
 *
 * Verified DECO_32 API (offsets are decimal as Hex-Rays prints them):
 *   int  OpenDecompressor(int *out_handle);                 // handle in *out, 1..256
 *   int  SetFIFBuffer(int h, const void *buf, int size);    // magic "FTC\0"/"FIF\0"
 *   int  SetFTTBuffer(int h, const void *buf, int size);    // optional (external FTT)
 *   int  GetOriginalResolution(int h, int *w, int *hgt, int *extra);  // THREE outs
 *   int  SetOutputFormat(int h, int c0,int c1,int c2,int c3, int dither);
 *        // channel selector per output byte: 1=R 2=G 3=B 4=skip 5=pad
 *        // BGR24 == (3,2,1,4, 0)
 *   int  DecompressToBuffer(int h, void *buf, int x,int y, int w,int hgt, int stride);
 *        // x,y must be even; w/hgt default to image size when 0
 *   void ClearFIFBuffer(int h);
 *   void CloseDecompressor(int h);
 *
 * Usage: decooracle <input.ftc|.fif> <output.bmp> [DECO_32.DLL path]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "png_write.h"

typedef int  (__cdecl *pfnOpen)(int *);
typedef int  (__cdecl *pfnSetBuf)(int, const void *, int);
typedef int  (__cdecl *pfnGetRes)(int, int *, int *, int *);
typedef int  (__cdecl *pfnSetRes)(int, int, int);
typedef int  (__cdecl *pfnSetFmt)(int, int, int, int, int, int);
typedef int  (__cdecl *pfnDecomp)(int, void *, int, int, int, int, int);
typedef void (__cdecl *pfnClear)(int);
typedef void (__cdecl *pfnClose)(int);

static const char *dll_paths[] = {
    "DECO_32.DLL", ".\\DECO_32.DLL",
    "C:\\encarta\\analysis\\DECO_32.DLL", NULL
};

static uint8_t *read_file(const char *path, long *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc(size ? size : 1);
    if (data && fread(data, 1, size, f) != (size_t)size) { free(data); data = NULL; }
    fclose(f);
    if (data) *out_size = size;
    return data;
}

#pragma pack(push, 1)
typedef struct { uint16_t t; uint32_t sz; uint16_t r1, r2; uint32_t off; } BMPFH;
typedef struct {
    uint32_t size; int32_t w, h; uint16_t planes, bpp;
    uint32_t comp, imgsz; int32_t xppm, yppm; uint32_t used, important;
} BMPIH;
#pragma pack(pop)

/* pixels are BGR24, top-down, src_stride bytes/row; written bottom-up to BMP */
static int write_bmp(const char *path, int w, int h, const uint8_t *px, int src_stride)
{
    int row = ((w * 3 + 3) & ~3);
    int imgsz = row * h;
    BMPFH fh; BMPIH ih;
    memset(&fh, 0, sizeof fh); memset(&ih, 0, sizeof ih);
    fh.t = 0x4D42; fh.off = sizeof(BMPFH) + sizeof(BMPIH); fh.sz = fh.off + imgsz;
    ih.size = sizeof(BMPIH); ih.w = w; ih.h = h; ih.planes = 1; ih.bpp = 24;
    ih.imgsz = imgsz; ih.xppm = ih.yppm = 2835;
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot create %s\n", path); return 0; }
    fwrite(&fh, 1, sizeof fh, f);
    fwrite(&ih, 1, sizeof ih, f);
    uint8_t pad[4] = {0};
    for (int y = h - 1; y >= 0; y--) {     /* BMP is bottom-up */
        fwrite(px + (size_t)y * src_stride, 1, w * 3, f);
        if (row - w * 3 > 0) fwrite(pad, 1, row - w * 3, f);
    }
    fclose(f);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: decooracle <input.ftc|.fif> <output.bmp> [deco_32.dll]\n");
        return 2;
    }
    const char *in = argv[1], *out = argv[2], *dll_arg = (argc >= 4) ? argv[3] : NULL;

    HMODULE h = dll_arg ? LoadLibraryA(dll_arg) : NULL;
    for (int i = 0; !h && dll_paths[i]; i++) h = LoadLibraryA(dll_paths[i]);
    if (!h) { fprintf(stderr, "cannot load DECO_32.DLL\n"); return 1; }

    pfnOpen   Open   = (pfnOpen)  GetProcAddress(h, "OpenDecompressor");
    pfnSetBuf SetFIF = (pfnSetBuf)GetProcAddress(h, "SetFIFBuffer");
    pfnGetRes GetRes = (pfnGetRes)GetProcAddress(h, "GetOriginalResolution");
    pfnSetRes SetRes = (pfnSetRes)GetProcAddress(h, "SetOutputResolution");
    pfnSetFmt SetFmt = (pfnSetFmt)GetProcAddress(h, "SetOutputFormat");
    pfnDecomp Decomp = (pfnDecomp)GetProcAddress(h, "DecompressToBuffer");
    pfnClear  Clear  = (pfnClear) GetProcAddress(h, "ClearFIFBuffer");
    pfnClose  Close  = (pfnClose) GetProcAddress(h, "CloseDecompressor");
    if (!Open || !SetFIF || !GetRes || !SetFmt || !Decomp || !Close) {
        fprintf(stderr, "missing required export\n"); return 1;
    }

    long fsz = 0;
    uint8_t *data = read_file(in, &fsz);
    if (!data) { fprintf(stderr, "cannot read %s\n", in); return 1; }
    fprintf(stderr, "input: %s (%ld bytes), magic %02x %02x %02x %02x\n",
            in, fsz, data[0], data[1], data[2], data[3]);

    int hd = 0, rc;
    rc = Open(&hd);
    fprintf(stderr, "OpenDecompressor -> %d (handle %d)\n", rc, hd);
    if (rc != 0 || hd == 0) return 1;

    rc = SetFIF(hd, data, (int)fsz);
    fprintf(stderr, "SetFIFBuffer -> %d\n", rc);
    if (rc != 0) { fprintf(stderr, "  (input rejected: not FTC\\0/FIF\\0 or too small)\n"); return 1; }

    int w = 0, hgt = 0, extra = 0;
    rc = GetRes(hd, &w, &hgt, &extra);
    fprintf(stderr, "GetOriginalResolution -> %d  (%d x %d, extra=%d)\n", rc, w, hgt, extra);
    if (rc != 0 || w <= 0 || hgt <= 0) return 1;

    if (SetRes) {
        rc = SetRes(hd, w, hgt);      /* populate instance output w/h (offset 0x15/0x17) */
        fprintf(stderr, "SetOutputResolution(%d,%d) -> %d\n", w, hgt, rc);
        if (rc != 0) return 1;
    }

    rc = SetFmt(hd, 3, 2, 1, 4, 0);   /* BGR24 */
    fprintf(stderr, "SetOutputFormat(3,2,1,4,0) -> %d\n", rc);
    if (rc != 0) return 1;

    int stride = w * 3;
    uint8_t *px = (uint8_t *)calloc(1, (size_t)stride * hgt);
    if (!px) { fprintf(stderr, "oom\n"); return 1; }

    rc = Decomp(hd, px, 0, 0, w, hgt, stride);
    fprintf(stderr, "DecompressToBuffer(0,0,%d,%d,stride=%d) -> %d\n", w, hgt, stride, rc);

    if (rc == 0) {
        long total = (long)stride * hgt, nz = 0;
        /* deterministic CRC32 of the raw decoded buffer — the regression key */
        uint32_t pixcrc = pw__crc_buf(px, (size_t)total);
        for (long i = 0; i < total; i++) if (px[i]) nz++;
        fprintf(stderr, "decoded: %ld/%ld non-zero bytes\n", nz, total);
        printf("pixcrc=%08X size=%dx%d bytes=%ld file=%s\n", pixcrc, w, hgt, total, in);

        size_t n = strlen(out);
        int is_png = (n >= 4 && (out[n-1]=='g'||out[n-1]=='G') &&
                      (out[n-2]=='n'||out[n-2]=='N') && (out[n-3]=='p'||out[n-3]=='P'));
        int ok = is_png ? write_png_bgr24(out, w, hgt, px, stride)
                        : write_bmp(out, w, hgt, px, stride);
        if (ok) fprintf(stderr, "wrote %s (%dx%d %s)\n", out, w, hgt, is_png ? "RGB PNG" : "BGR24 BMP");
    } else {
        fprintf(stderr, "decode failed (rc=%d)\n", rc);
    }

    free(px);
    if (Clear) Clear(hd);
    Close(hd);
    free(data);
    FreeLibrary(h);
    return rc == 0 ? 0 : 1;
}
