/*
 * indeo3.h - Indeo 3 (IV32) container + frame header, derived from the
 *            Encarta 97 video bitstream.
 *
 * Everything here was worked out by reading the bytes of the shipping .AVI
 * files and checking each field across every frame of a clip. Fields whose
 * meaning is not yet established are named `unk*` rather than guessed at.
 *
 * No third-party decoder source was used. Where a reference decode is needed
 * to check our output, ffmpeg is run as an external oracle to emit raw frames
 * (the same trick `tools/decooracle` uses against DECO_32.DLL) - its output is
 * data to compare against, not code to copy.
 */
#ifndef INDEO3_H
#define INDEO3_H

#include <stdint.h>
#include <stddef.h>

#define IV3_SIGNATURE   0x4652u     /* u16 at offset 10, constant */
#define IV3_CONST24     0x1200u     /* u16 at offset 24, constant */
#define IV3_HEADER_SIZE 48          /* frame header; plane offsets are from frame start */
#define IV3_TABLE_SIZE  16          /* fixed table that follows the header */

/* Frame header. Verified field-by-field across every frame of a clip:
 * frame numbers run sequentially, the signature and the constant at 24 never
 * vary, height/width agree with the AVI stream header, and the plane offsets
 * are strictly ordered and land inside data_size. */
typedef struct {
    uint32_t frame_number;
    uint32_t zero4;          /* always 0 */
    uint16_t checksum;       /* varies per frame */
    uint16_t signature;      /* IV3_SIGNATURE */
    uint32_t data_size;      /* payload bytes, <= chunk size */
    uint32_t flags;          /* low half always 0x0020; high half cycles per GOP */
    uint32_t unk20;
    uint16_t const24;        /* IV3_CONST24 */
    uint16_t unk26;
    uint16_t height;
    uint16_t width;
    uint32_t plane_off[3];   /* offsets at 32/36/40, descending; [2] == 48 */
    uint32_t zero44;         /* always 0 */
    int      null_frame;     /* header only, no payload: repeat the last frame.
                                Its plane offsets are stale and must be ignored. */
} iv3_frame_header;

/* Parse and validate. Returns 0 on success, non-zero on the first field that
 * fails its invariant - so a stream that is not what we think it is says so
 * rather than decoding into nonsense. */
int iv3_parse_header(const uint8_t *data, size_t len, iv3_frame_header *out,
                     const char **why);

/* ---- AVI demuxer (ours; no library) ---- */

typedef struct {
    uint32_t offset;         /* into the file buffer */
    uint32_t size;
} avi_chunk;

typedef struct {
    uint8_t   *data;
    size_t     size;
    uint32_t   width, height;
    uint32_t   us_per_frame;
    char       compression[5];
    avi_chunk *frames;
    size_t     frame_count;
} avi_file;

int  avi_open(const char *path, avi_file *out);
void avi_close(avi_file *a);

#endif /* INDEO3_H */
