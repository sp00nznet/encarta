/*
 * indeodec - inspect and demux the Indeo 3 (IV32) video in Encarta 97.
 *
 *   indeodec -i <file.avi>              stream + per-frame header report
 *   indeodec -c <file.avi>              check every frame against the model
 *   indeodec -x <file.avi> -o <dir>     write each frame's payload out
 */
#include "indeo3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0)
{
    printf("indeodec - Indeo 3 (IV32) container/frame inspector\n\n"
           "  %s -i <file.avi>            stream info + frame headers\n"
           "  %s -c <file.avi>            validate every frame header\n"
           "  %s -x <file.avi> -o <dir>   extract frame payloads\n",
           argv0, argv0, argv0);
}

static int open_or_die(const char *path, avi_file *a)
{
    int rc = avi_open(path, a);
    if (rc) { fprintf(stderr, "cannot read AVI '%s' (%d)\n", path, rc); return 0; }
    if (strcmp(a->compression, "IV32") && strcmp(a->compression, "iv32"))
        fprintf(stderr, "warning: compression is '%s', not IV32\n", a->compression);
    return 1;
}

int main(int argc, char **argv)
{
    const char *path = NULL, *outdir = NULL;
    int mode = 0;
    avi_file a;
    size_t i;

    for (int k = 1; k < argc; k++) {
        if (!strcmp(argv[k], "-i") && k + 1 < argc) { mode = 'i'; path = argv[++k]; }
        else if (!strcmp(argv[k], "-c") && k + 1 < argc) { mode = 'c'; path = argv[++k]; }
        else if (!strcmp(argv[k], "-x") && k + 1 < argc) { mode = 'x'; path = argv[++k]; }
        else if (!strcmp(argv[k], "-o") && k + 1 < argc) { outdir = argv[++k]; }
    }
    if (!mode || !path) { usage(argv[0]); return 1; }
    if (!open_or_die(path, &a)) return 1;

    printf("%s: %ux%u  %s  %.2f fps  %zu video chunks\n", path, a.width, a.height,
           a.compression, a.us_per_frame ? 1e6 / a.us_per_frame : 0.0, a.frame_count);

    if (mode == 'i' || mode == 'c') {
        size_t bad = 0, keys = 0;
        for (i = 0; i < a.frame_count; i++) {
            const uint8_t *d = a.data + a.frames[i].offset;
            iv3_frame_header h;
            const char *why = NULL;
            int rc = iv3_parse_header(d, a.frames[i].size, &h, &why);
            if (rc) {
                bad++;
                printf("  frame %-4zu INVALID: %s\n", i, why);
                continue;
            }
            /* the payload's biggest plane is the luma; a frame carrying a large
               one is a keyframe in every clip examined */
            if (h.data_size - h.plane_off[0] > 4096) keys++;
            if (mode == 'i' && i < 8)
                printf("  frame %-4u size=%-6u flags=%08X  %ux%u  planes @%u/%u/%u\n",
                       h.frame_number, h.data_size, h.flags, h.width, h.height,
                       h.plane_off[2], h.plane_off[1], h.plane_off[0]);
        }
        if (a.frame_count == 0) {           /* 0/0 is not a pass */
            printf("FAIL: no video chunks found - demuxer did not understand this file\n");
            avi_close(&a);
            return 3;
        }
        printf("%s: %zu/%zu frames match the header model (%zu large-luma frames)\n",
               bad ? "FAIL" : "OK", a.frame_count - bad, a.frame_count, keys);
        avi_close(&a);
        return bad ? 2 : 0;
    }

    if (mode == 'x') {
        char name[1024];
        if (!outdir) { fprintf(stderr, "-x needs -o <dir>\n"); avi_close(&a); return 1; }
        for (i = 0; i < a.frame_count; i++) {
            FILE *o;
            snprintf(name, sizeof name, "%s/f%04zu.iv32", outdir, i);
            o = fopen(name, "wb");
            if (!o) { fprintf(stderr, "cannot write %s\n", name); avi_close(&a); return 1; }
            fwrite(a.data + a.frames[i].offset, 1, a.frames[i].size, o);
            fclose(o);
        }
        printf("wrote %zu frames to %s\n", a.frame_count, outdir);
    }

    avi_close(&a);
    return 0;
}
