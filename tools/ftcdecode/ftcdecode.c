/*
 * ftcdecode - Clean-room FTC fractal image decoder
 *
 * Decodes FTC (Fractal Transform Codec) images used by Encarta 97.
 * Based on reverse engineering of DECO_32.DLL binary analysis.
 *
 * FTC uses LSB-first packed bitstream encoding (NOT arithmetic coding).
 * The fractal transform uses affine IFS with 8 symmetry modes.
 *
 * Usage:
 *   ftcdecode <input.ftc> <output.bmp>    Decode FTC to BMP
 *   ftcdecode -i <input.ftc>              Show header info
 *   ftcdecode -d <input.ftc> <out.bmp>    Decode with debug
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "ftcdecode.h"

/* ------------------------------------------------------------------ */
/* File I/O helpers                                                    */
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

/* ------------------------------------------------------------------ */
/* BMP writer                                                          */
/* ------------------------------------------------------------------ */

static bool write_bmp(const char *path, int width, int height,
                      const uint8_t *bgr_pixels, int src_stride)
{
    int row_bytes = ((width * 3 + 3) / 4) * 4;
    int image_size = row_bytes * height;

    BMPFileHeader fh;
    BMPInfoHeader ih;

    memset(&fh, 0, sizeof(fh));
    memset(&ih, 0, sizeof(ih));

    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(BMPFileHeader) + sizeof(BMPInfoHeader);
    fh.bfSize = fh.bfOffBits + image_size;

    ih.biSize = sizeof(BMPInfoHeader);
    ih.biWidth = width;
    ih.biHeight = height;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biCompression = 0;
    ih.biSizeImage = image_size;
    ih.biXPelsPerMeter = 2835;
    ih.biYPelsPerMeter = 2835;

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "Error: cannot create '%s'\n", path);
        return false;
    }

    fwrite(&fh, 1, sizeof(fh), f);
    fwrite(&ih, 1, sizeof(ih), f);

    for (int y = height - 1; y >= 0; y--) {
        const uint8_t *row = bgr_pixels + y * src_stride;
        fwrite(row, 1, width * 3, f);
        int pad = row_bytes - width * 3;
        if (pad > 0) {
            uint8_t zeros[4] = {0};
            fwrite(zeros, 1, pad, f);
        }
    }

    fclose(f);
    return true;
}

/* ------------------------------------------------------------------ */
/* Header parsing                                                      */
/* ------------------------------------------------------------------ */

static bool parse_header(const uint8_t *data, long file_size,
                         FTCHeader *hdr, FTCSubHeader *sub)
{
    if (file_size < (long)sizeof(FTCHeader)) {
        fprintf(stderr, "Error: file too small for FTC header\n");
        return false;
    }

    memcpy(hdr, data, sizeof(FTCHeader));

    if (memcmp(hdr->magic, "FTC", 3) != 0 || hdr->magic[3] != '\0') {
        fprintf(stderr, "Error: not an FTC file (magic: %02X %02X %02X %02X)\n",
                (uint8_t)hdr->magic[0], (uint8_t)hdr->magic[1],
                (uint8_t)hdr->magic[2], (uint8_t)hdr->magic[3]);
        return false;
    }

    if (hdr->hdr_size > (uint16_t)file_size) {
        fprintf(stderr, "Error: header size %u exceeds file size %ld\n",
                hdr->hdr_size, file_size);
        return false;
    }

    if (hdr->hdr_size >= sizeof(FTCHeader) + sizeof(FTCSubHeader)) {
        memcpy(sub, data + sizeof(FTCHeader), sizeof(FTCSubHeader));
    } else {
        memset(sub, 0, sizeof(FTCSubHeader));
        sub->block_w = FTC_BLOCK_W;
        sub->block_h = FTC_BLOCK_H;
        sub->chroma_subsample = 2;
        sub->channels = 3;
        sub->iterations = 7;
    }

    return true;
}

static void print_header_info(const FTCHeader *hdr, const FTCSubHeader *sub)
{
    printf("FTC Header:\n");
    printf("  Magic:           %.3s\\0\n", hdr->magic);
    printf("  Version:         %u.%u\n", hdr->version_a, hdr->version_b);
    printf("  Format:          0x%04X\n", hdr->format);
    printf("  Width:           %u\n", hdr->width);
    printf("  Height:          %u\n", hdr->height);
    printf("  BPP:             %u\n", hdr->bpp);
    printf("  Planes:          %u\n", hdr->planes);
    printf("  Header size:     %u\n", hdr->hdr_size);
    printf("  Data size:       %u\n", hdr->data_size);
    printf("\n");
    printf("Sub-Header:\n");
    printf("  Sub version:     %u.%u\n", sub->sub_ver_a, sub->sub_ver_b);
    printf("  Sub field:       %u\n", sub->sub_field);
    printf("  Width2:          %u\n", sub->width2);
    printf("  Height2:         %u\n", sub->height2);
    printf("  Block size:      %ux%u\n", sub->block_w, sub->block_h);
    printf("  Chroma subsamp:  %u\n", sub->chroma_subsample);
    printf("  Channels:        %u\n", sub->channels);
    printf("  Iterations:      %u\n", sub->iterations);
    printf("  Remaining:");
    for (int i = 0; i < 15; i++)
        printf(" %02X", sub->remaining[i]);
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Sub-header context parsing (matches DLL 0x11005CA0)                 */
/* ------------------------------------------------------------------ */

/*
 * The sub-header "remaining" bytes encode parameters for the bitstream
 * decoder. In "small mode" (p2*10+p3 <= 31, which is our case with
 * p2=1, p3=1 giving 11), the layout is:
 *
 * remaining[0]   = extra_byte (used as scale_bits for ctx entry)
 * remaining[1]   = extra_byte2 (multiplied by 8, used as opcode shift)
 * remaining[2:3] = extra_word (LE, multiplied by 8 minus 0x800 = scale_base)
 * remaining[3:4] = context_count (LE)
 * remaining[5+]  = per-context: 2-byte word (dimension)
 * After contexts: more fields for block grid setup
 *
 * For our test file (remaining = 04 00 00 01 00 06 00 ...):
 *   extra_byte=4, extra_byte2=0, extra_word=0
 *   context_count=1, ctx[0].dimension=6
 */

typedef struct {
    uint16_t dimension;     /* ctx[0:1] - for offset div/mod */
    uint8_t  scale_bits;    /* ctx[2] - number of scale index bits */
    uint8_t  extra_flag;    /* ctx[3] - if nonzero, read extra bit */
    int16_t  scale_base;    /* ctx[4:5] - scale base (extra_word*8 - 0x800) */
} CtxEntry;

typedef struct {
    /* Parsed from sub-header remaining bytes */
    int p2, p3;
    int multiplier;         /* chroma_subsample */
    int iterations;
    int channels;

    /* Context table */
    int ctx_count;
    CtxEntry ctx[16];

    /* Computed decode parameters */
    uint16_t dim_x;         /* ceil(width / multiplier) */
    uint16_t dim_y;         /* ceil(height / multiplier) */
    int scale_bits;         /* bits for scale field */
    int offset_bits;        /* bits for offset field */
    int opcode_bits;        /* bits for opcode field */
    int has_extra_bit;      /* whether to read extra scale bit */
} FTCParams;

static int count_bits(int value)
{
    int bits = 0;
    while (value > 0) {
        value >>= 1;
        bits++;
    }
    return bits;
}

static bool parse_ftc_params(const FTCSubHeader *sub, int width, int height,
                             FTCParams *params)
{
    memset(params, 0, sizeof(*params));

    params->p2 = sub->sub_ver_a;
    params->p3 = sub->sub_ver_b;
    params->multiplier = sub->chroma_subsample > 0 ? sub->chroma_subsample : 2;
    params->iterations = sub->iterations > 0 ? sub->iterations : 7;
    params->channels = sub->channels > 0 ? sub->channels : 3;

    int small_mode = (params->p2 * 10 + params->p3) <= 31;

    if (!small_mode) {
        fprintf(stderr, "Warning: large mode not yet supported (p2=%d, p3=%d)\n",
                params->p2, params->p3);
        return false;
    }

    const uint8_t *r = sub->remaining;

    /*
     * Small mode parsing (matches DLL 0x11005EA8).
     *
     * The DLL reads the sub-header sequentially. By the time it reaches
     * the small-mode code, it has already read iterations into register bl.
     * Our struct stores iterations separately, so "remaining" starts after it.
     *
     * Mapping from DLL's sequential read to our remaining[] offsets:
     *   bl = iterations (already parsed into sub->iterations = 7)
     *   remaining[0] = extra_byte2 (DLL: [edi+1], saved as [esp+14h])
     *   remaining[1:2] = extra_word (LE)
     *   remaining[3:4] = context_count (LE)
     *   remaining[5+] = per-context 2-byte dimension words
     *
     * Context entry fields (12-byte internal struct):
     *   ctx[0:1] = dimension word (from file)
     *   ctx[2] = bl = iterations = 7 (scale_bits)
     *   ctx[3] = extra_byte2 * 8 = 32 (opcode_shift / extra_flag)
     *   ctx[4:5] = extra_word * 8 - 0x800 (scale_base)
     */

    int extra_byte2 = r[0];
    uint16_t extra_word = r[1] | (r[2] << 8);
    uint16_t ctx_count = r[3] | (r[4] << 8);

    params->ctx_count = ctx_count;
    if (ctx_count > 16 || ctx_count == 0) {
        fprintf(stderr, "Error: invalid context count (%d)\n", ctx_count);
        return false;
    }

    int16_t scale_base = (int16_t)((int)(extra_word) * 8 - 0x800);
    uint8_t opcode_shift = (uint8_t)(extra_byte2 * 8);

    for (int i = 0; i < ctx_count; i++) {
        int off = 5 + i * 2;
        if (off + 1 >= 15) break;
        params->ctx[i].dimension = r[off] | (r[off + 1] << 8);
        params->ctx[i].scale_bits = params->iterations; /* bl = iterations */
        params->ctx[i].extra_flag = opcode_shift;
        params->ctx[i].scale_base = scale_base;
    }

    /* Compute decode parameters (matches DLL 0x11008030) */
    int mult = params->multiplier;

    /* dim_x, dim_y = ceil(width/mult), ceil(height/mult) */
    params->dim_x = (uint16_t)((width - 1) / mult + 1);
    params->dim_y = (uint16_t)((height - 1) / mult + 1);

    /* Scale bits from decoder[52h] = ctx_array[0].byte2 = iterations */
    params->scale_bits = params->iterations; /* = 7 */

    /* Extra bit: ctx[3] (opcode_shift) nonzero means read 1 extra scale bit */
    params->has_extra_bit = (opcode_shift != 0) ? 1 : 0;

    /* Offset bits: ceil(log2(dim_x * dim_y)) */
    int total_dim = (int)params->dim_x * (int)params->dim_y;
    params->offset_bits = count_bits(total_dim - 1);

    /* Opcode bits = channels = 3 */
    params->opcode_bits = params->channels;

    return true;
}

/* ------------------------------------------------------------------ */
/* 16-bit scale table (matches DLL 0x1100B250, word0=6 case)           */
/* ------------------------------------------------------------------ */

static int16_t scale_table_16[256]; /* indexed by combined scale index */

static void init_scale_table_16(const FTCParams *params)
{
    int word0 = params->ctx_count > 0 ? params->ctx[0].dimension : 6;
    int scale_bits = params->scale_bits;
    int num_entries = 1 << scale_bits;
    if (params->has_extra_bit) num_entries <<= 1;
    if (num_entries > 256) num_entries = 256;

    int16_t scale_base_val = params->ctx_count > 0 ? params->ctx[0].scale_base : -2048;

    /* Compute scale table based on word0 (dimension parameter) */
    /* The DLL has a switch on word0-4 for different formulas */
    int32_t esi = (int32_t)scale_base_val * 16;
    int32_t step = 16;  /* default step per entry */

    /* For small mode without extra bits, step = 1*16 = 16.
     * Actually step = ebx * 16 where ebx was initialized to 0 or 1 depending on mode.
     * For our case (no extra bits, info[101h]=0): ebx=0 at 1100B2D2, but that gives step=0.
     * Wait, ebx is set to [ecx+3] = ctx[3] = opcode_shift = 32.
     * Hmm, let me re-read: 1100B2D8: bl = [ecx+3] which is ctx.byte3.
     * In small mode ctx.byte3 = opcode_shift = 32.
     * So ebx = 32. And step = 32 * 16 = 512.
     */

    /* Actually the DLL code flow (for info[101h]=0 path at 1100B2C8):
     * ebp = 1 << ctx.byte2 = 1 << 7 = 128 (num entries)
     * ebx = ctx.byte3 = 32
     * Then at 1100B3D6 (word0=6 case):
     *   shl esi, 4 → esi *= 16 (esi was ctx.word4 = -2048 → -32768)
     *   shl ebx, 4 → ebx *= 16 (ebx was 32 → 512)
     */
    esi = (int32_t)scale_base_val * 16;
    step = 32 * 16;  /* This needs to use the actual ctx.byte3 value */

    /* Actually, let me use the values from the analysis directly */
    /* For our test file: ctx.byte3 = opcode_shift = extra_byte2*8 = 4*8 = 32 */
    int ctx_byte3 = params->has_extra_bit ? (params->ctx[0].extra_flag) : 0;
    /* Wait, ctx.byte3 IS the extra_flag. For our file it's 32. */
    ctx_byte3 = params->ctx_count > 0 ? params->ctx[0].extra_flag : 0;

    esi = (int32_t)scale_base_val * 16;
    step = ctx_byte3 * 16;

    /* ecx (bias) = 0x800 (the neutral point) */
    int32_t bias = 0x800;

    for (int i = 0; i < num_entries; i++) {
        int32_t value;

        switch (word0) {
        case 4: {
            /* Linear with clamp, divide by 4 */
            value = esi;
            if (value < (int32_t)0xFFFFF800) value = (int32_t)0xFFFFF800;
            value += bias;
            scale_table_16[i] = (int16_t)value;
            esi += step / 4;  /* approximation */
            break;
        }
        case 5: {
            /* Divide by 6 */
            value = esi;
            if (value >= 0) value += 3;
            else if (value < -0x3C00) value = -0x3C00;
            else value -= 2;
            value = value / 6;
            scale_table_16[i] = (int16_t)(value + bias);
            esi += step;
            break;
        }
        case 6: {
            /* Divide by 10 (matches 1100B3CB path) */
            value = esi;
            if (value >= 0)
                value = value + 5;
            else if (value < -0x5000)
                value = -0x5000;
            else
                value = value - 4;
            value = value / 10;
            scale_table_16[i] = (int16_t)(value + bias);
            esi += step;
            break;
        }
        case 7: {
            /* Divide by 12 */
            value = esi;
            if (value >= 0) value += 6;
            else if (value < -0x6000) value = -0x6000;
            else value -= 5;
            value = value / 12;
            scale_table_16[i] = (int16_t)(value + bias);
            esi += step;
            break;
        }
        case 8: {
            /* Divide by 16 */
            value = esi;
            if (value >= 0) value += 8;
            else value -= 7;
            value = value / 16;
            scale_table_16[i] = (int16_t)(value + bias);
            esi += step;
            break;
        }
        default: {
            /* Fallback: simple linear */
            value = esi;
            if (value < (int32_t)0xFFFFF800) value = (int32_t)0xFFFFF800;
            scale_table_16[i] = (int16_t)(value + bias);
            esi += step;
            break;
        }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Pixel transform from 16-bit scale                                   */
/* ------------------------------------------------------------------ */

/*
 * The 16-bit scale value from the scale table encodes the affine
 * transform parameter. 0x800 is the neutral value.
 *
 * The transform applied to each pixel is approximately:
 *   output = (input * contrast + offset) clamped to [0,255]
 *
 * Where the 16-bit scale encodes: scale = contrast * 2048 + offset
 * (or similar). For now, we use a simplified model:
 *   contrast = 0.75 (fixed, as in FVF)
 *   bias = (scale_16 - 0x800) * some_factor
 *   output = input * 0.75 + bias
 */

static uint8_t pixel_transform[256][256]; /* [scale_idx][pixel] */

static void init_pixel_transform(int num_entries)
{
    if (num_entries > 256) num_entries = 256;

    for (int si = 0; si < num_entries; si++) {
        int16_t scale_val = scale_table_16[si];

        for (int p = 0; p < 256; p++) {
            /*
             * Fractal affine transform: out = s * in + (1-s) * fp
             * where fp = scale_val / 16 is the fixed point,
             * and s = 0.75 is the contraction factor.
             *
             * This converges to fp after enough iterations.
             * Integer math: out = (p * 3 + scale_val / 4 + 2) >> 2
             * (since 0.75*p + 0.25*(sv/16) = (3p + sv/16) / 4)
             */
            int fp4 = (int)scale_val >> 2; /* = (scale_val/16) * 4 = fp*4 */
            int val = (p * 3 + fp4 + 2) >> 2;
            if (val < 0) val = 0;
            if (val > 255) val = 255;
            pixel_transform[si][p] = (uint8_t)val;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Variable-length count reader (matches DLL 0x11019780)               */
/* ------------------------------------------------------------------ */

/*
 * Exponential-Golomb-like coding:
 * 1. Read leading 1-bits until a 0-bit (count = N)
 * 2. If N=0: return 1
 * 3. Read (N-1) raw bits
 * 4. Return (1 << (N-1)) | raw_bits + 1
 */
static uint32_t read_vlc_count(BitReader *br)
{
    int leading_ones = 0;

    while (br_has_bits(br, 1)) {
        uint32_t bit = br_read(br, 1);
        if (bit == 1)
            leading_ones++;
        else
            break;
    }

    if (leading_ones == 0)
        return 1;

    int n = leading_ones - 1;
    if (n == 0)
        return 2;

    uint32_t raw = br_read(br, n);
    return (1u << n) | raw + 1;

    /* Wait, the DLL does: result = (1 << (N-1)) | raw + 1
     * where N = leading_ones, and raw has (N-1) bits.
     * So: (1 << n) | raw, then +1 */
}

/* ------------------------------------------------------------------ */
/* Block assignment initialization (matches DLL 0x110191D0)            */
/* ------------------------------------------------------------------ */

/*
 * For a still FTC image, this reads run-length encoded block assignments.
 * Each run has a 1-bit flag followed by a VLC count:
 *   flag=1: count blocks are "active" (need decode)
 *   flag=0: count blocks are "skip" (keep previous / zero)
 *
 * The DLL also reads 16-bit address indices for each block, but for
 * our simplified decoder we just need to know which blocks are active.
 */

typedef struct {
    uint8_t *state;     /* per-block state: 0=active, 2=skip */
    int total_blocks;
} BlockAssignment;

static bool read_block_assignment(BitReader *br, BlockAssignment *ba,
                                  int total_blocks, int debug)
{
    ba->total_blocks = total_blocks;
    ba->state = (uint8_t *)calloc(total_blocks, 1);
    if (!ba->state) return false;

    int remaining = total_blocks;
    int pos = 0;

    while (remaining > 0) {
        uint32_t flag = br_read(br, 1);
        uint32_t count = read_vlc_count(br);

        if (count > (uint32_t)remaining)
            count = remaining;

        if (debug && pos < 100)
            fprintf(stderr, "  BlockAssign: flag=%u count=%u at pos=%d\n",
                    flag, count, pos);

        for (uint32_t i = 0; i < count; i++) {
            if (flag == 0) {
                ba->state[pos] = 2; /* skip */
            }
            /* flag == 1: state stays 0 = active */
            pos++;
        }

        remaining -= count;
    }

    if (debug) {
        int active = 0, skip = 0;
        for (int i = 0; i < total_blocks; i++) {
            if (ba->state[i] == 0) active++;
            else skip++;
        }
        fprintf(stderr, "  BlockAssign: %d active, %d skip out of %d total\n",
                active, skip, total_blocks);
    }

    return true;
}

/* ------------------------------------------------------------------ */
/* Block descriptor                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  opcode;      /* affine transform mode (0-7) */
    uint8_t  channel;     /* which channel this block belongs to */
    int16_t  scale_idx;   /* scale table index */
    int16_t  x_off;       /* source x offset */
    int16_t  y_off;       /* source y offset */
} BlockDesc;

/* ------------------------------------------------------------------ */
/* Address table (block index → pixel offset)                          */
/* ------------------------------------------------------------------ */

/*
 * The address table maps each block's linear index to its position
 * in the pixel buffer. The blocks are organized in a specific scan
 * order that groups 4x4 pixel blocks into 16-block superblocks.
 *
 * For simplicity, we use a straightforward raster scan for now.
 */

static void init_address_table(uint16_t *addr, int bw, int bh, int buf_w)
{
    int idx = 0;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            addr[idx++] = (uint16_t)(by * FTC_BLOCK_H * buf_w + bx * FTC_BLOCK_W);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Affine transform (4x4 block from 8x8 source)                       */
/* ------------------------------------------------------------------ */

/*
 * The fractal transform copies a downsampled 8x8 source region to a
 * 4x4 destination block, applying contrast scaling and one of 8
 * symmetry operations (identity, mirrors, flips, rotations).
 *
 * Source addressing: source pixel at (sx, sy) maps to
 *   buf[y_off + sy*2 + x_off + sx*2] (2:1 downsampling)
 *
 * The 8 symmetry modes correspond to the dihedral group D4:
 *   0: identity
 *   1: mirror horizontal
 *   2: flip vertical
 *   3: mirror + flip (180° rotation)
 *   4: transpose (90° CW rotation)
 *   5: transpose + mirror
 *   6: transpose + flip
 *   7: transpose + mirror + flip (270° CW rotation)
 */

static void apply_transform(uint8_t *dst_buf, const uint8_t *src_buf,
                            int dst_off, int src_x, int src_y,
                            int opcode, int scale_idx,
                            int buf_w, int buf_h, int buf_size)
{
    const uint8_t *scale = pixel_transform[scale_idx & 0xFF];

    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            int sx, sy;

            switch (opcode & 7) {
            case 0: sx = col; sy = row; break;
            case 1: sx = 3 - col; sy = row; break;
            case 2: sx = col; sy = 3 - row; break;
            case 3: sx = 3 - col; sy = 3 - row; break;
            case 4: sx = row; sy = col; break;
            case 5: sx = row; sy = 3 - col; break;
            case 6: sx = 3 - row; sy = col; break;
            case 7: sx = 3 - row; sy = 3 - col; break;
            default: sx = col; sy = row; break;
            }

            /* Source is 2:1 downsampled from prev buffer */
            int src_px = src_x + sx * 2;
            int src_py = src_y + sy * 2;
            int si = src_py * buf_w + src_px;

            int di = dst_off + row * buf_w + col;

            if (si >= 0 && si < buf_size && di >= 0 && di < buf_size) {
                dst_buf[di] = scale[(uint8_t)src_buf[si]];
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Channel decode (matches DLL 0x11004BE0 / 0x11004D60)                */
/* ------------------------------------------------------------------ */

static void decode_blocks(BitReader *br, const FTCParams *params,
                          BlockDesc *blocks, int num_blocks,
                          uint8_t channel, int debug)
{
    int decoded = 0;

    for (int i = 0; i < num_blocks; i++) {
        /* Read scale index */
        uint32_t scale_idx = br_read(br, params->scale_bits);

        /* Optional extra bit */
        if (params->has_extra_bit) {
            uint32_t extra = br_read(br, 1);
            scale_idx |= (extra << params->scale_bits);
        }

        /* Read packed offset */
        uint32_t packed_offset = br_read(br, params->offset_bits);

        /* Decode x,y from packed offset */
        uint16_t dim = params->dim_x;
        int mult = params->multiplier;
        int x_off, y_off;

        if (dim > 0) {
            x_off = (int)(packed_offset % dim) * mult;
            y_off = (int)(packed_offset / dim) * mult;
        } else {
            x_off = 0;
            y_off = 0;
        }

        /* Read opcode */
        uint32_t opcode = br_read(br, params->opcode_bits);

        blocks[i].opcode = (uint8_t)opcode;
        blocks[i].channel = channel;
        blocks[i].scale_idx = (int16_t)scale_idx;
        blocks[i].x_off = (int16_t)x_off;
        blocks[i].y_off = (int16_t)y_off;

        if (debug && decoded < 10) {
            fprintf(stderr, "  Block[%d]: scale=%d offset=%d (x=%d,y=%d) op=%d\n",
                    i, scale_idx, packed_offset, x_off, y_off, opcode);
        }
        decoded++;
    }
}

/* ------------------------------------------------------------------ */
/* Convert GBR 4:2:0 planar to BGR interleaved                        */
/* ------------------------------------------------------------------ */

static void convert_to_bgr(const uint8_t *green, const uint8_t *blue,
                            const uint8_t *red,
                            int width, int height, int buf_w,
                            int chroma_w, int chroma_h,
                            int mult,
                            uint8_t *bgr, int bgr_stride)
{
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int gi = y * buf_w + x;
            int cx = x / mult;
            int cy = y / mult;
            if (cx >= chroma_w) cx = chroma_w - 1;
            if (cy >= chroma_h) cy = chroma_h - 1;
            int bi = cy * chroma_w + cx; /* blue channel index */
            int ri = bi;                 /* red channel index (same layout) */

            int out_idx = y * bgr_stride + x * 3;
            bgr[out_idx + 0] = blue[bi];
            bgr[out_idx + 1] = green[gi];
            bgr[out_idx + 2] = red[ri];
        }
    }
}

/* ------------------------------------------------------------------ */
/* Main decode function                                                */
/* ------------------------------------------------------------------ */

static int decode_ftc(const char *input_path, const char *output_path, int debug)
{
    long file_size = 0;
    uint8_t *file_data = read_file(input_path, &file_size);
    if (!file_data) {
        fprintf(stderr, "Error: cannot read '%s'\n", input_path);
        return 1;
    }

    FTCHeader hdr;
    FTCSubHeader sub;
    if (!parse_header(file_data, file_size, &hdr, &sub)) {
        free(file_data);
        return 1;
    }

    int width = hdr.width;
    int height = hdr.height;

    fprintf(stderr, "FTC: %s (%ld bytes)\n", input_path, file_size);
    fprintf(stderr, "  Dimensions: %u x %u, %u bpp\n", hdr.width, hdr.height, hdr.bpp);

    /* Parse FTC-specific parameters from sub-header */
    FTCParams params;
    if (!parse_ftc_params(&sub, width, height, &params)) {
        free(file_data);
        return 1;
    }

    fprintf(stderr, "  Iterations: %d, channels: %d, multiplier: %d\n",
            params.iterations, params.channels, params.multiplier);
    fprintf(stderr, "  Dimension: %d x %d, scale_bits: %d, offset_bits: %d, opcode_bits: %d\n",
            params.dim_x, params.dim_y, params.scale_bits, params.offset_bits, params.opcode_bits);
    fprintf(stderr, "  Has extra bit: %d\n", params.has_extra_bit);

    if (params.ctx_count > 0) {
        fprintf(stderr, "  Context[0]: dim=%d scale_bits=%d extra_flag=%d scale_base=%d\n",
                params.ctx[0].dimension, params.ctx[0].scale_bits,
                params.ctx[0].extra_flag, params.ctx[0].scale_base);
    }

    /* Initialize scale table */
    init_scale_table_16(&params);
    int total_scale_entries = 1 << params.scale_bits;
    if (params.has_extra_bit) total_scale_entries <<= 1;
    init_pixel_transform(total_scale_entries);

    if (debug) {
        fprintf(stderr, "  Scale table (first 16): ");
        for (int i = 0; i < 16 && i < total_scale_entries; i++)
            fprintf(stderr, "%d ", scale_table_16[i]);
        fprintf(stderr, "\n");
        fprintf(stderr, "  Scale table (mid): ");
        for (int i = 60; i < 68 && i < total_scale_entries; i++)
            fprintf(stderr, "%d ", scale_table_16[i]);
        fprintf(stderr, "\n");
    }

    /* Block grid dimensions */
    int mult = params.multiplier;
    int bw_luma = (width + FTC_BLOCK_W - 1) / FTC_BLOCK_W;
    int bh_luma = (height + FTC_BLOCK_H - 1) / FTC_BLOCK_H;
    int num_luma = bw_luma * bh_luma;

    int chroma_w = width / mult;
    int chroma_h = height / mult;
    int bw_chroma = (chroma_w + FTC_BLOCK_W - 1) / FTC_BLOCK_W;
    int bh_chroma = (chroma_h + FTC_BLOCK_H - 1) / FTC_BLOCK_H;
    int num_chroma = bw_chroma * bh_chroma;

    int total_blocks = num_luma + num_chroma * 2; /* green + blue + red */

    fprintf(stderr, "  Luma blocks: %d (%d x %d)\n", num_luma, bw_luma, bh_luma);
    fprintf(stderr, "  Chroma blocks: %d (%d x %d) per channel\n",
            num_chroma, bw_chroma, bh_chroma);
    fprintf(stderr, "  Total blocks: %d\n", total_blocks);

    /* Allocate pixel buffers */
    int luma_buf_w = bw_luma * FTC_BLOCK_W;
    int luma_buf_h = bh_luma * FTC_BLOCK_H;
    int luma_buf_size = luma_buf_w * luma_buf_h;

    int chroma_buf_w = bw_chroma * FTC_BLOCK_W;
    int chroma_buf_h = bh_chroma * FTC_BLOCK_H;
    int chroma_buf_size = chroma_buf_w * chroma_buf_h;

    uint8_t *cur_green  = (uint8_t *)calloc(1, luma_buf_size);
    uint8_t *cur_blue   = (uint8_t *)calloc(1, chroma_buf_size);
    uint8_t *cur_red    = (uint8_t *)calloc(1, chroma_buf_size);
    uint8_t *prev_green = (uint8_t *)calloc(1, luma_buf_size);
    uint8_t *prev_blue  = (uint8_t *)calloc(1, chroma_buf_size);
    uint8_t *prev_red   = (uint8_t *)calloc(1, chroma_buf_size);

    if (!cur_green || !cur_blue || !cur_red ||
        !prev_green || !prev_blue || !prev_red) {
        fprintf(stderr, "Error: cannot allocate pixel buffers\n");
        goto cleanup;
    }

    /* Initialize to mid-gray */
    memset(cur_green, 0x80, luma_buf_size);
    memset(cur_blue, 0x80, chroma_buf_size);
    memset(cur_red, 0x80, chroma_buf_size);
    memset(prev_green, 0x80, luma_buf_size);
    memset(prev_blue, 0x80, chroma_buf_size);
    memset(prev_red, 0x80, chroma_buf_size);

    /* Allocate address tables */
    uint16_t *addr_luma = (uint16_t *)calloc(num_luma, sizeof(uint16_t));
    uint16_t *addr_chroma = (uint16_t *)calloc(num_chroma, sizeof(uint16_t));
    init_address_table(addr_luma, bw_luma, bh_luma, luma_buf_w);
    init_address_table(addr_chroma, bw_chroma, bh_chroma, chroma_buf_w);

    /* Allocate block descriptors */
    BlockDesc *blocks_green = (BlockDesc *)calloc(num_luma, sizeof(BlockDesc));
    BlockDesc *blocks_blue  = (BlockDesc *)calloc(num_chroma, sizeof(BlockDesc));
    BlockDesc *blocks_red   = (BlockDesc *)calloc(num_chroma, sizeof(BlockDesc));

    /* Set up bitstream */
    size_t data_offset = hdr.hdr_size;
    const uint8_t *bitstream = file_data + data_offset;
    size_t bitstream_size = file_size - data_offset;

    fprintf(stderr, "  Bitstream: %zu bytes at offset %zu\n",
            bitstream_size, data_offset);

    if (debug) {
        fprintf(stderr, "  First 16 bytes: ");
        for (int i = 0; i < 16 && i < (int)bitstream_size; i++)
            fprintf(stderr, "%02X ", bitstream[i]);
        fprintf(stderr, "\n");
    }

    BitReader br;
    br_init(&br, bitstream, bitstream_size);

    /* Read block assignment */
    fprintf(stderr, "\n--- Block assignment ---\n");
    BlockAssignment ba;
    if (!read_block_assignment(&br, &ba, num_luma, debug)) {
        fprintf(stderr, "Error: block assignment failed\n");
        goto cleanup;
    }

    fprintf(stderr, "  Bitstream pos after assignment: byte %zu bit %d\n",
            br.byte_pos, br.bit_pos);

    /* Decode green (luma) channel blocks */
    fprintf(stderr, "\n--- Decoding green channel (%d blocks) ---\n", num_luma);
    decode_blocks(&br, &params, blocks_green, num_luma, 2, debug);

    fprintf(stderr, "  Bitstream pos after green: byte %zu bit %d\n",
            br.byte_pos, br.bit_pos);

    /* Decode blue chroma blocks */
    fprintf(stderr, "\n--- Decoding blue channel (%d blocks) ---\n", num_chroma);
    decode_blocks(&br, &params, blocks_blue, num_chroma, 1, debug);

    fprintf(stderr, "  Bitstream pos after blue: byte %zu bit %d\n",
            br.byte_pos, br.bit_pos);

    /* Decode red chroma blocks */
    fprintf(stderr, "\n--- Decoding red channel (%d blocks) ---\n", num_chroma);
    decode_blocks(&br, &params, blocks_red, num_chroma, 0, debug);

    fprintf(stderr, "  Bitstream pos after all channels: byte %zu bit %d / %zu\n",
            br.byte_pos, br.bit_pos, bitstream_size);

    /* Iterative fractal decode */
    fprintf(stderr, "\n--- Iterating %d times ---\n", params.iterations);

    for (int iter = 0; iter < params.iterations; iter++) {
        /* Swap current and previous */
        uint8_t *tmp;
        tmp = prev_green; prev_green = cur_green; cur_green = tmp;
        tmp = prev_blue;  prev_blue = cur_blue;   cur_blue = tmp;
        tmp = prev_red;   prev_red = cur_red;     cur_red = tmp;

        /* Clear current buffers */
        memset(cur_green, 0x80, luma_buf_size);
        memset(cur_blue, 0x80, chroma_buf_size);
        memset(cur_red, 0x80, chroma_buf_size);

        /* Apply green transforms */
        for (int i = 0; i < num_luma; i++) {
            if (ba.state[i] == 2) continue; /* skip */
            BlockDesc *b = &blocks_green[i];
            int dst_off = addr_luma[i];
            apply_transform(cur_green, prev_green, dst_off,
                           b->x_off, b->y_off, b->opcode, b->scale_idx,
                           luma_buf_w, luma_buf_h, luma_buf_size);
        }

        /* Apply blue transforms */
        for (int i = 0; i < num_chroma; i++) {
            BlockDesc *b = &blocks_blue[i];
            int dst_off = addr_chroma[i];
            apply_transform(cur_blue, prev_blue, dst_off,
                           b->x_off, b->y_off, b->opcode, b->scale_idx,
                           chroma_buf_w, chroma_buf_h, chroma_buf_size);
        }

        /* Apply red transforms */
        for (int i = 0; i < num_chroma; i++) {
            BlockDesc *b = &blocks_red[i];
            int dst_off = addr_chroma[i];
            apply_transform(cur_red, prev_red, dst_off,
                           b->x_off, b->y_off, b->opcode, b->scale_idx,
                           chroma_buf_w, chroma_buf_h, chroma_buf_size);
        }

        if (iter == 0 || iter == params.iterations - 1) {
            /* Count non-128 pixels */
            int non_mid = 0;
            for (int p = 0; p < luma_buf_size; p++)
                if (cur_green[p] != 128) non_mid++;
            fprintf(stderr, "  Iter %d: green non-128: %d/%d  green[0]=%d green[1000]=%d\n",
                    iter, non_mid, luma_buf_size, cur_green[0],
                    1000 < luma_buf_size ? cur_green[1000] : 0);
        }
    }

    /* Convert to BGR and write BMP */
    int bgr_stride = width * 3;
    uint8_t *bgr = (uint8_t *)calloc(1, bgr_stride * height);
    if (!bgr) {
        fprintf(stderr, "Error: cannot allocate BGR buffer\n");
        goto cleanup;
    }

    convert_to_bgr(cur_green, cur_blue, cur_red,
                    width, height, luma_buf_w,
                    chroma_buf_w, chroma_buf_h,
                    mult, bgr, bgr_stride);

    if (write_bmp(output_path, width, height, bgr, bgr_stride)) {
        fprintf(stderr, "Wrote: %s (%d x %d, 24-bit BMP)\n",
                output_path, width, height);
    }

    free(bgr);

cleanup:
    free(blocks_green); free(blocks_blue); free(blocks_red);
    free(addr_luma); free(addr_chroma);
    free(ba.state);
    free(cur_green); free(cur_blue); free(cur_red);
    free(prev_green); free(prev_blue); free(prev_red);
    free(file_data);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Header info mode                                                    */
/* ------------------------------------------------------------------ */

static int show_info(const char *input_path)
{
    long file_size = 0;
    uint8_t *file_data = read_file(input_path, &file_size);
    if (!file_data) {
        fprintf(stderr, "Error: cannot read '%s'\n", input_path);
        return 1;
    }

    FTCHeader hdr;
    FTCSubHeader sub;
    if (!parse_header(file_data, file_size, &hdr, &sub)) {
        free(file_data);
        return 1;
    }

    print_header_info(&hdr, &sub);

    printf("\nFile: %s (%ld bytes)\n", input_path, file_size);
    printf("Compressed data offset: %u\n", hdr.hdr_size);
    printf("Compressed data size:   %u (file has %ld)\n",
           hdr.data_size, file_size - hdr.hdr_size);

    /* Also show computed parameters */
    FTCParams params;
    if (parse_ftc_params(&sub, hdr.width, hdr.height, &params)) {
        printf("\nComputed parameters:\n");
        printf("  dim_x=%d dim_y=%d\n", params.dim_x, params.dim_y);
        printf("  scale_bits=%d offset_bits=%d opcode_bits=%d\n",
               params.scale_bits, params.offset_bits, params.opcode_bits);
        printf("  has_extra_bit=%d\n", params.has_extra_bit);
        if (params.ctx_count > 0)
            printf("  ctx[0]: dim=%d scale_base=%d\n",
                   params.ctx[0].dimension, params.ctx[0].scale_base);
    }

    free(file_data);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "ftcdecode - Clean-room FTC fractal image decoder\n\n");
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  ftcdecode <input.ftc> <output.bmp>   Decode FTC to BMP\n");
        fprintf(stderr, "  ftcdecode -i <input.ftc>             Show header info\n");
        fprintf(stderr, "  ftcdecode -d <input.ftc> <out.bmp>   Decode with debug\n");
        return 1;
    }

    if (strcmp(argv[1], "-i") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: -i requires an input file\n");
            return 1;
        }
        return show_info(argv[2]);
    }

    int debug = 0;
    int arg_off = 0;
    if (strcmp(argv[1], "-d") == 0) {
        debug = 1;
        arg_off = 1;
    }

    if (argc < 3 + arg_off) {
        fprintf(stderr, "Error: need both input and output paths\n");
        return 1;
    }

    return decode_ftc(argv[1 + arg_off], argv[2 + arg_off], debug);
}
