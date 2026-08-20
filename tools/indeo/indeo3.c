/*
 * indeo3.c - AVI demuxer and Indeo 3 frame-header parser.
 * See indeo3.h for provenance.
 */
#include "indeo3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int iv3_parse_header(const uint8_t *d, size_t len, iv3_frame_header *h,
                     const char **why)
{
    const char *dummy;
    if (!why) why = &dummy;

    /* A chunk too small to hold a header carries no picture: AVI's "no new
       data for this stream" marker, which real clips use for held frames. Zero
       length is the common form; a few frames use a short stub instead. */
    if (len < IV3_HEADER_SIZE) { memset(h, 0, sizeof *h); h->null_frame = 1; return 0; }

    h->frame_number = rd32(d + 0);
    h->zero4        = rd32(d + 4);
    h->checksum     = rd16(d + 8);
    h->signature    = rd16(d + 10);
    h->data_size    = rd32(d + 12);
    h->flags        = rd32(d + 16);
    h->unk20        = rd32(d + 20);
    h->const24      = rd16(d + 24);
    h->unk26        = rd16(d + 26);
    h->height       = rd16(d + 28);
    h->width        = rd16(d + 30);
    h->plane_off[0] = rd32(d + 32);
    h->plane_off[1] = rd32(d + 36);
    h->plane_off[2] = rd32(d + 40);
    h->zero44       = rd32(d + 44);

    /* Invariants, each checked against every frame of the sample clips. */
    if (h->signature != IV3_SIGNATURE)  { *why = "bad signature";            return 2; }
    if (h->const24   != IV3_CONST24)    { *why = "bad constant at 24";       return 3; }
    if (h->zero4 || h->zero44)          { *why = "reserved field not zero";  return 4; }
    if (h->data_size > len)             { *why = "data_size exceeds chunk";  return 5; }

    /* data_size == header size means the frame carries no plane data at all:
       a null frame telling the decoder to hold the previous picture. Real
       clips are full of them, and their plane offsets are leftovers from an
       earlier frame, so the plane invariants below do not apply. */
    h->null_frame = (h->data_size <= IV3_HEADER_SIZE);
    if (h->null_frame) return 0;

    if (len < IV3_HEADER_SIZE + IV3_TABLE_SIZE) { *why = "shorter than header+table"; return 10; }
    if (h->plane_off[2] != IV3_HEADER_SIZE) { *why = "plane[2] not at 48";   return 6; }
    if (!(h->plane_off[2] < h->plane_off[1] && h->plane_off[1] < h->plane_off[0])) {
        *why = "plane offsets not ordered"; return 7;
    }
    if (h->plane_off[0] >= h->data_size) { *why = "plane[0] beyond data";    return 8; }
    if (!h->width || !h->height)         { *why = "zero dimensions";         return 9; }
    return 0;
}

/* ---------------- AVI ---------------- */

static void avi_walk(avi_file *a, size_t off, size_t end, int *have_strf)
{
    while (off + 8 <= end) {
        const uint8_t *p = a->data + off;
        uint32_t size = rd32(p + 4);
        size_t body = off + 8;

        if (!memcmp(p, "RIFF", 4) || !memcmp(p, "LIST", 4)) {
            size_t sub_end = body + size;
            if (sub_end > end) sub_end = end;
            avi_walk(a, body + 4, sub_end, have_strf);
        } else if (!memcmp(p, "avih", 4) && body + 40 <= end) {
            a->us_per_frame = rd32(a->data + body);
            a->width  = rd32(a->data + body + 32);
            a->height = rd32(a->data + body + 36);
        } else if (!memcmp(p, "strf", 4) && !*have_strf && body + 20 <= end) {
            memcpy(a->compression, a->data + body + 16, 4);
            a->compression[4] = 0;
            *have_strf = 1;
        } else if (p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9' &&
                   /* Encarta's clips do not stick to the spec's "dc"/"db" video
                      suffixes - they variously use "iv" and "32" as well. So
                      rather than whitelisting video suffixes and silently
                      finding zero frames in a file that uses a new one, exclude
                      the non-video ones: "wb" audio, "tx" text, "pc" palette. */
                   memcmp(p + 2, "wb", 2) && memcmp(p + 2, "tx", 2) &&
                   memcmp(p + 2, "pc", 2)) {
            if (body + size <= a->size) {
                a->frames = (avi_chunk *)realloc(a->frames,
                                (a->frame_count + 1) * sizeof *a->frames);
                a->frames[a->frame_count].offset = (uint32_t)body;
                a->frames[a->frame_count].size = size;
                a->frame_count++;
            }
        }
        off = body + size + (size & 1);   /* chunks are word-aligned */
    }
}

int avi_open(const char *path, avi_file *a)
{
    FILE *f = fopen(path, "rb");
    long n;
    int have_strf = 0;

    memset(a, 0, sizeof *a);
    if (!f) return 1;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 12) { fclose(f); return 2; }
    a->data = (uint8_t *)malloc((size_t)n);
    if (!a->data) { fclose(f); return 3; }
    if (fread(a->data, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(a->data); return 4; }
    fclose(f);
    a->size = (size_t)n;

    if (memcmp(a->data, "RIFF", 4) || memcmp(a->data + 8, "AVI ", 4)) {
        free(a->data); a->data = NULL; return 5;
    }
    avi_walk(a, 12, a->size, &have_strf);
    return 0;
}

void avi_close(avi_file *a)
{
    free(a->data); free(a->frames);
    memset(a, 0, sizeof *a);
}
