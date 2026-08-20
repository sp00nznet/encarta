/*
 * compare.c - score decoded output against a reference YUV.
 *
 * The decoder is being built incrementally from the bitstream, so the thing
 * that matters most is a number that says whether a change made it better.
 * `ffmpeg -i clip.avi -pix_fmt yuv410p -f rawvideo ref.yuv` produces the
 * reference; this walks it frame by frame against ours and reports PSNR.
 *
 * Until the pixel decoding exists, the "decoder" here is deliberately the
 * dumbest thing that respects what we HAVE established - null frames hold the
 * previous picture, everything else is flat mid-grey. That is not a decoder;
 * it is a baseline, and its PSNR is the number to beat.
 */
#include "indeo3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* 4:1:0 - chroma is quartered on both axes */
static size_t plane_bytes(uint32_t w, uint32_t h, int *cw, int *ch)
{
    *cw = (int)((w + 3) / 4);
    *ch = (int)((h + 3) / 4);
    return (size_t)w * h + 2u * (size_t)(*cw) * (*ch);
}

static double psnr(const uint8_t *a, const uint8_t *b, size_t n)
{
    double mse = 0.0;
    size_t i;
    for (i = 0; i < n; i++) {
        double d = (double)a[i] - (double)b[i];
        mse += d * d;
    }
    mse /= (double)n;
    if (mse <= 0.0) return 99.0;               /* identical */
    return 10.0 * log10(255.0 * 255.0 / mse);
}

int compare_to_reference(const char *avi_path, const char *ref_path)
{
    avi_file a;
    FILE *rf;
    uint8_t *ours = NULL, *ref = NULL, *prev = NULL;
    int cw, ch;
    size_t fsz, i;
    double total = 0.0;
    size_t scored = 0, nulls = 0;
    int rc = 1;

    if (avi_open(avi_path, &a)) { fprintf(stderr, "cannot read %s\n", avi_path); return 1; }
    if (!a.frame_count)         { fprintf(stderr, "no video chunks\n"); avi_close(&a); return 1; }

    rf = fopen(ref_path, "rb");
    if (!rf) { fprintf(stderr, "cannot read reference %s\n", ref_path); avi_close(&a); return 1; }

    fsz = plane_bytes(a.width, a.height, &cw, &ch);
    ours = malloc(fsz); ref = malloc(fsz); prev = malloc(fsz);
    if (!ours || !ref || !prev) goto done;

    printf("%s  %ux%u  luma %ux%u  chroma %dx%d  %zu bytes/frame\n",
           avi_path, a.width, a.height, a.width, a.height, cw, ch, fsz);

    memset(prev, 128, fsz);
    for (i = 0; i < a.frame_count; i++) {
        iv3_frame_header h;
        const char *why = NULL;
        if (fread(ref, 1, fsz, rf) != fsz) break;      /* reference exhausted */

        if (iv3_parse_header(a.data + a.frames[i].offset, a.frames[i].size, &h, &why)) {
            printf("  frame %-4zu unparsable: %s\n", i, why);
            continue;
        }
        if (h.null_frame) {
            memcpy(ours, prev, fsz);                   /* hold previous picture */
            nulls++;
        } else {
            memset(ours, 128, fsz);                    /* BASELINE, not a decode */
        }
        total += psnr(ours, ref, fsz);
        scored++;
        memcpy(prev, ours, fsz);
    }

    printf("scored %zu frames (%zu null/held).  mean PSNR %.2f dB\n",
           scored, nulls, scored ? total / (double)scored : 0.0);
    printf("note: this is the flat-grey baseline - real decoding must beat it.\n");
    rc = 0;

done:
    free(ours); free(ref); free(prev);
    fclose(rf);
    avi_close(&a);
    return rc;
}
