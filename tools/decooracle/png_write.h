/*
 * png_write.h - minimal dependency-free PNG writer (header-only).
 *
 * Emits a valid 8-bit RGB PNG using uncompressed ("stored") DEFLATE blocks,
 * so no zlib is required. Input is a BGR24 top-down buffer (as produced by
 * DECO_32's BGR24 output); channels are swapped to RGB on write.
 */
#ifndef PNG_WRITE_H
#define PNG_WRITE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint32_t pw__crc_table[256];
static int pw__crc_ready = 0;

static void pw__crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        pw__crc_table[n] = c;
    }
    pw__crc_ready = 1;
}

/* standard CRC32 over a buffer (regression key for decoded pixels) */
static uint32_t pw__crc_buf(const uint8_t *buf, size_t len)
{
    if (!pw__crc_ready) pw__crc_init();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = pw__crc_table[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void pw__be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static void pw__chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[8];
    pw__be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    /* CRC covers the 4 type bytes followed by the data, as one stream */
    if (!pw__crc_ready) pw__crc_init();
    uint32_t c = 0xFFFFFFFFu;
    for (int i = 0; i < 4; i++)
        c = pw__crc_table[(c ^ (uint8_t)type[i]) & 0xFF] ^ (c >> 8);
    for (uint32_t i = 0; i < len; i++)
        c = pw__crc_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    uint8_t cb[4];
    pw__be32(cb, c ^ 0xFFFFFFFFu);
    fwrite(cb, 1, 4, f);
}

/* write BGR24 top-down buffer as RGB PNG. returns 1 on success. */
static int write_png_bgr24(const char *path, int w, int h, const uint8_t *bgr, int stride)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;

    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);

    uint8_t ihdr[13];
    pw__be32(ihdr + 0, (uint32_t)w);
    pw__be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;   /* bit depth */
    ihdr[9] = 2;   /* color type 2 = truecolor RGB */
    ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    pw__chunk(f, "IHDR", ihdr, 13);

    /* raw filtered image: each row = filter byte (0) + RGB pixels */
    size_t row_raw = (size_t)w * 3 + 1;
    size_t raw_len = row_raw * h;
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (!raw) { fclose(f); return 0; }
    for (int y = 0; y < h; y++) {
        uint8_t *dst = raw + (size_t)y * row_raw;
        *dst++ = 0; /* filter: None */
        const uint8_t *src = bgr + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            dst[0] = src[2]; /* R */
            dst[1] = src[1]; /* G */
            dst[2] = src[0]; /* B */
            dst += 3; src += 3;
        }
    }

    /* zlib stream: 2-byte header + stored deflate blocks + adler32 */
    size_t max_blocks = raw_len / 65535 + 1;
    size_t zcap = 2 + raw_len + max_blocks * 5 + 4;
    uint8_t *z = (uint8_t *)malloc(zcap);
    if (!z) { free(raw); fclose(f); return 0; }
    size_t zi = 0;
    z[zi++] = 0x78; z[zi++] = 0x01; /* zlib header, no preset dict */
    size_t off = 0;
    while (off < raw_len) {
        size_t chunk = raw_len - off;
        if (chunk > 65535) chunk = 65535;
        int final = (off + chunk >= raw_len) ? 1 : 0;
        z[zi++] = (uint8_t)final;                 /* BFINAL + BTYPE=00 (stored) */
        z[zi++] = (uint8_t)(chunk & 0xFF);
        z[zi++] = (uint8_t)((chunk >> 8) & 0xFF);
        z[zi++] = (uint8_t)(~chunk & 0xFF);
        z[zi++] = (uint8_t)((~chunk >> 8) & 0xFF);
        memcpy(z + zi, raw + off, chunk);
        zi += chunk;
        off += chunk;
    }
    /* adler32 of raw */
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    pw__be32(z + zi, (b << 16) | a);
    zi += 4;

    pw__chunk(f, "IDAT", z, (uint32_t)zi);
    pw__chunk(f, "IEND", NULL, 0);

    free(z);
    free(raw);
    fclose(f);
    return 1;
}

#endif /* PNG_WRITE_H */
