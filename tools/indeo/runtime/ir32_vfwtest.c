/*
 * ir32_vfwtest.c - decode a frame the way an application does.
 *
 * The point is that nothing here calls the recompiled codec. It installs it,
 * then asks Video for Windows for a decompressor for `IV32` and uses whatever
 * VFW hands back. If the frame comes out, the bridge works for any caller that
 * goes through ICM - which is what MCIAVI does on Encarta's behalf.
 *
 *   ir32_vfwtest <IR32.DLL> <frame.bin> <w> <h> [out.ppm] [bits]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <vfw.h>

int ir32_vfw_install(const char *ne_path);
void ir32_vfw_remove(void);

static void bih(BITMAPINFOHEADER *b, LONG w, LONG h, WORD bits, DWORD comp,
                DWORD sz)
{
    memset(b, 0, sizeof *b);
    b->biSize = sizeof *b;
    b->biWidth = w;
    b->biHeight = h;
    b->biPlanes = 1;
    b->biBitCount = bits;
    b->biCompression = comp;
    b->biSizeImage = sz;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 5) {
        fprintf(stderr, "usage: %s <IR32.DLL> <frame.bin> <w> <h> "
                        "[out.ppm] [bits]\n", argv[0]);
        return 2;
    }
    const char *ne = argv[1], *fr = argv[2];
    LONG w = atol(argv[3]), h = atol(argv[4]);
    const char *ppm = argc > 5 && argv[5][0] ? argv[5] : NULL;
    WORD bits = argc > 6 ? (WORD)atoi(argv[6]) : 24;

    if (ir32_vfw_install(ne)) {
        printf("install: failed\n");
        return 1;
    }
    printf("installed IV32 for this process\n");

    FILE *f = fopen(fr, "rb");
    if (!f) { printf("cannot open %s\n", fr); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc(n);
    if (fread(data, 1, n, f) != (size_t)n) { fclose(f); return 1; }
    fclose(f);

    BITMAPINFOHEADER in, out;
    bih(&in, w, h, 24, mmioFOURCC('I','V','3','2'), (DWORD)n);
    bih(&out, w, h, bits, BI_RGB,
        (DWORD)w * h * ((bits + 7) / 8));

    /* ICLocate is the call that matters: it is VFW's own search, and it only
     * finds this codec because ICInstall put it in the list. */
    HIC hic = ICLocate(ICTYPE_VIDEO, mmioFOURCC('I','V','3','2'),
                       &in, &out, ICMODE_DECOMPRESS);
    if (!hic) {
        printf("ICLocate: no decompressor found\n");
        return 1;
    }
    printf("ICLocate: found a decompressor\n");

    ICINFO ii; memset(&ii, 0, sizeof ii);
    if (ICGetInfo(hic, &ii, sizeof ii))
        printf("ICGetInfo: %ls\n", ii.szDescription);

    LRESULT r = ICDecompressBegin(hic, &in, &out);
    printf("ICDecompressBegin: %ld\n", (long)r);
    if (r != ICERR_OK) { ICClose(hic); return 1; }

    unsigned char *pix = calloc(1, out.biSizeImage + 0x10000);
    r = ICDecompress(hic, 0, &in, data, &out, pix);
    printf("ICDecompress: %ld\n", (long)r);
    if (r != ICERR_OK) { ICClose(hic); return 1; }

    /* IR32_REPEAT: decode the same frame N times and report the rate. The
     * question it answers is whether the codec can keep up with playback -
     * MCIAVI drives the clock and calls ICDecompress per frame, so all this
     * has to do is come back before the next one is due. */
    const char *rep = getenv("IR32_REPEAT");
    if (rep) {
        int n = atoi(rep);
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        for (int k = 0; k < n; k++)
            ICDecompress(hic, 0, &in, data, &out, pix);
        QueryPerformanceCounter(&b);
        double sec = (double)(b.QuadPart - a.QuadPart) / f.QuadPart;
        printf("decoded %d frames in %.3f s = %.0f fps (%.2f ms/frame)\n",
               n, sec, n / sec, 1000.0 * sec / n);
    }

    unsigned long nz = 0;
    for (DWORD i = 0; i < out.biSizeImage; i++)
        if (pix[i]) nz++;
    printf("output: %lu of %lu bytes non-zero (%.1f%%)\n",
           nz, (unsigned long)out.biSizeImage,
           100.0 * nz / out.biSizeImage);

    if (ppm && bits == 24) {
        FILE *o = fopen(ppm, "wb");
        if (o) {
            fprintf(o, "P6\n%ld %ld\n255\n", w, h);
            for (LONG y = h - 1; y >= 0; y--) {          /* DIB is bottom-up */
                const unsigned char *row = pix + (size_t)y * w * 3;
                for (LONG x = 0; x < w; x++) {
                    fputc(row[x * 3 + 2], o);
                    fputc(row[x * 3 + 1], o);
                    fputc(row[x * 3 + 0], o);
                }
            }
            fclose(o);
            printf("wrote %s\n", ppm);
        }
    }
    ICDecompressEnd(hic);
    ICClose(hic);
    ir32_vfw_remove();
    return 0;
}
