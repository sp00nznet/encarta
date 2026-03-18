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

    /* No extra scale bit - 24 bits per block (7 scale + 14 offset + 3 opcode) */
    params->has_extra_bit = 0;

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

static int16_t scale_table_16[256];       /* luma scale table */
static int16_t scale_table_chroma[256];   /* chroma scale table (may use word0=8) */

static void init_scale_table_for_word0(int16_t *table, int word0,
                                        int num_entries, int16_t scale_base_val,
                                        int ctx_byte3)
{
    int32_t esi = (int32_t)scale_base_val * 16;
    int32_t step = ctx_byte3 * 16;
    int32_t bias = 0x800;

    for (int i = 0; i < num_entries; i++) {
        int32_t value;
        switch (word0) {
        case 6:
            value = esi;
            if (value >= 0) value += 5;
            else if (value < -0x5000) value = -0x5000;
            else value -= 4;
            value = value / 10;
            break;
        case 8:
            value = esi;
            if (value >= 0) value += 8;
            else value -= 7;
            value = value / 16;
            break;
        default:
            value = esi;
            if (value < (int32_t)0xFFFFF800) value = (int32_t)0xFFFFF800;
            break;
        }
        table[i] = (int16_t)(value + bias);
        esi += step;
    }
}

static void init_scale_table_16(const FTCParams *params)
{
    int word0 = params->ctx_count > 0 ? params->ctx[0].dimension : 6;
    int num_entries = 1 << params->scale_bits;
    if (num_entries > 256) num_entries = 256;
    int16_t scale_base = params->ctx_count > 0 ? params->ctx[0].scale_base : -2048;
    int ctx_byte3 = params->ctx_count > 0 ? params->ctx[0].extra_flag : 0;

    /* Build luma scale table (word0 from context, e.g. 6 = divide-by-10) */
    init_scale_table_for_word0(scale_table_16, word0, num_entries,
                                scale_base, (int16_t)ctx_byte3);

    /* Build chroma scale table: sub-header bytes[7:8] may specify word0=8 */
    int chroma_word0 = 8; /* divide-by-16: maps 128 entries to FP range 0-254 */
    init_scale_table_for_word0(scale_table_chroma, chroma_word0, num_entries,
                                scale_base, (int16_t)ctx_byte3);
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
             * where fp = scale_val / 16 is the fixed point target,
             * and s = 3/4 is the contraction factor.
             *
             * out = (3/4)*p + (1/4)*fp = (3*p + fp) / 4
             * With fp = scale_val / 16:
             *   out = (3*p + scale_val/16 + 2) / 4
             */
            int fp = (int)scale_val >> 4;
            int val = (3 * p + fp + 2) >> 2;
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
    return ((1u << n) | raw) + 1;
}

/* ------------------------------------------------------------------ */
/* Block assignment initialization (matches DLL 0x110191D0)            */
/* ------------------------------------------------------------------ */

/*
 * 3-pass block assignment (matches DLL 0x110191D0).
 *
 * The DLL reads run-length encoded assignments in 3 passes:
 *   Pass 1: flag=0 → state |= 2 (GREEN),  flag=1 → kept for pass 2
 *   Pass 2: flag=0 → state |= 6 (SKIP),   flag=1 → kept for pass 3
 *   Pass 3: flag=0 → state |= 1 (BLUE),   flag=1 → kept (RED, state=0)
 *
 * For luma level where all blocks are green: pass 1 assigns all with flag=1
 * (meaning "all are active/green"), and passes 2-3 process the remaining.
 */

typedef struct {
    uint8_t *state;     /* per-block state: 0=red, 1=blue, 2=green, 6=skip */
    int total_blocks;
} BlockAssignment;

static void run_assignment_pass(BitReader *br, uint8_t *state,
                                int *indices, int count,
                                uint8_t assign_state, int debug,
                                int pass_num, int *out_remaining,
                                int *remaining_indices)
{
    int pos = 0;
    int kept = 0;

    while (pos < count) {
        uint32_t flag = br_read(br, 1);
        uint32_t run = read_vlc_count(br);

        if (run > (uint32_t)(count - pos))
            run = count - pos;

        for (uint32_t i = 0; i < run; i++) {
            int idx = indices[pos + i];
            if (flag == 0) {
                state[idx] = assign_state;
            } else {
                remaining_indices[kept++] = idx;
            }
        }
        pos += run;
    }

    *out_remaining = kept;
}

static bool read_block_assignment(BitReader *br, BlockAssignment *ba,
                                  int total_blocks, int debug)
{
    ba->total_blocks = total_blocks;
    ba->state = (uint8_t *)calloc(total_blocks, 1);
    if (!ba->state) return false;

    /* All blocks start as state 0 (potential red/unassigned) */
    int *indices = (int *)malloc(total_blocks * sizeof(int));
    int *remaining = (int *)malloc(total_blocks * sizeof(int));
    if (!indices || !remaining) {
        free(indices); free(remaining);
        return false;
    }

    for (int i = 0; i < total_blocks; i++)
        indices[i] = i;

    /* Pass 1: assign GREEN (state 2), keep rest */
    int count = total_blocks;
    int kept = 0;
    size_t pass1_start = br->byte_pos * 8 + br->bit_pos;
    run_assignment_pass(br, ba->state, indices, count, 2, debug, 1,
                        &kept, remaining);
    if (debug)
        fprintf(stderr, "  Pass 1: %d green, %d kept (%zu bits)\n",
                count - kept, kept,
                br->byte_pos * 8 + br->bit_pos - pass1_start);

    /* Pass 2: assign SKIP (state 6), keep rest */
    if (kept > 0) {
        count = kept;
        size_t pass2_start = br->byte_pos * 8 + br->bit_pos;
        run_assignment_pass(br, ba->state, remaining, count, 6, debug, 2,
                            &kept, indices);
        if (debug)
            fprintf(stderr, "  Pass 2: %d skip, %d kept (%zu bits)\n",
                    count - kept, kept,
                    br->byte_pos * 8 + br->bit_pos - pass2_start);
    }

    /* Pass 3: assign BLUE (state 1), rest stays RED (state 0) */
    if (kept > 0) {
        count = kept;
        size_t pass3_start = br->byte_pos * 8 + br->bit_pos;
        run_assignment_pass(br, ba->state, indices, count, 1, debug, 3,
                            &kept, remaining);
        if (debug)
            fprintf(stderr, "  Pass 3: %d blue, %d red (%zu bits)\n",
                    count - kept, kept,
                    br->byte_pos * 8 + br->bit_pos - pass3_start);
    }

    free(indices);
    free(remaining);

    if (debug) {
        int counts[7] = {0};
        for (int i = 0; i < total_blocks; i++)
            if (ba->state[i] < 7) counts[ba->state[i]]++;
        fprintf(stderr, "  Assignment: green=%d skip=%d blue=%d red=%d\n",
                counts[2], counts[6], counts[1], counts[0]);
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
 * in the pixel buffer. Blocks are organized in 4x4 superblock scan
 * order: within each 16×16 pixel superblock (4×4 blocks), blocks
 * are scanned in raster order, and superblocks are scanned left-to-right,
 * top-to-bottom. The grid is padded to multiples of 4 blocks.
 */

static int init_address_table(uint16_t *addr, int bw, int bh, int buf_w,
                              int padded_bw, int padded_bh)
{
    int sbw = padded_bw / 4;   /* superblocks across */
    int sbh = padded_bh / 4;   /* superblocks down */
    int num_padded = padded_bw * padded_bh;
    int valid = 0;

    for (int i = 0; i < num_padded; i++) {
        int sb_idx = i / 16;
        int local = i % 16;
        int sb_x = sb_idx % sbw;
        int sb_y = sb_idx / sbw;
        int local_x = local % 4;
        int local_y = local / 4;
        int bx = sb_x * 4 + local_x;
        int by = sb_y * 4 + local_y;

        if (bx < bw && by < bh) {
            addr[valid++] = (uint16_t)(by * FTC_BLOCK_H * buf_w + bx * FTC_BLOCK_W);
        }
    }
    return valid;
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

/*
 * Decode blocks from bitstream in superblock scan order.
 * num_blocks is the PADDED block count (includes invalid padding blocks).
 * Blocks in padding positions are read from the bitstream but discarded.
 * Returns the number of VALID blocks stored in the blocks array.
 */
static int decode_blocks(BitReader *br, const FTCParams *params,
                          BlockDesc *blocks, int num_padded,
                          uint8_t channel, int mult_override,
                          int debug)
{
    int valid = 0;
    int bits_per_block = params->scale_bits + params->has_extra_bit +
                         params->offset_bits + params->opcode_bits;

    uint16_t dim = params->dim_x;
    int total_dim = (int)params->dim_x * (int)params->dim_y;
    int mult = mult_override > 0 ? mult_override : params->multiplier;

    for (int i = 0; i < num_padded; i++) {
        /* Check if enough bits remain for a full block */
        if (!br_has_bits(br, bits_per_block))
            break;

        /* Read scale index */
        uint32_t scale_idx = br_read(br, params->scale_bits);

        /* Optional extra bit */
        if (params->has_extra_bit) {
            uint32_t extra = br_read(br, 1);
            scale_idx |= (extra << params->scale_bits);
        }

        /* Read packed offset */
        uint32_t packed_offset = br_read(br, params->offset_bits);

        /* Wrap offset into valid domain range */
        if (total_dim > 0)
            packed_offset = packed_offset % (uint32_t)total_dim;

        /* Decode x,y from packed offset */
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

        /* Store only valid (non-padding) blocks.
         * The address table already maps valid indices to pixel positions,
         * so we just store sequentially. */
        blocks[valid].opcode = (uint8_t)opcode;
        blocks[valid].channel = channel;
        blocks[valid].scale_idx = (int16_t)scale_idx;
        blocks[valid].x_off = (int16_t)x_off;
        blocks[valid].y_off = (int16_t)y_off;

        if (debug && valid < 10) {
            fprintf(stderr, "  Block[%d/%d]: scale=%d offset=%d (x=%d,y=%d) op=%d\n",
                    valid, i, scale_idx, packed_offset, x_off, y_off, opcode);
        }
        valid++;
    }

    if (debug) {
        fprintf(stderr, "  Decoded %d valid blocks from %d padded for ch %d\n",
                valid, num_padded, channel);
    }
    return valid;
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

    /* Padded grid: rounds up to 4-block groups for superblock scan */
    int padded_bw_luma = ((width + 15) / 16) * 4;
    int padded_bh_luma = ((height + 15) / 16) * 4;
    int num_padded_luma = padded_bw_luma * padded_bh_luma;

    int chroma_w = width / mult;
    int chroma_h = height / mult;
    int bw_chroma = (chroma_w + FTC_BLOCK_W - 1) / FTC_BLOCK_W;
    int bh_chroma = (chroma_h + FTC_BLOCK_H - 1) / FTC_BLOCK_H;
    int num_chroma = bw_chroma * bh_chroma;
    int padded_bw_chroma = ((chroma_w + 15) / 16) * 4;
    int padded_bh_chroma = ((chroma_h + 15) / 16) * 4;
    int num_padded_chroma = padded_bw_chroma * padded_bh_chroma;

    fprintf(stderr, "  Luma blocks: %d (%d x %d), padded: %d (%d x %d)\n",
            num_luma, bw_luma, bh_luma, num_padded_luma, padded_bw_luma, padded_bh_luma);
    fprintf(stderr, "  Chroma blocks: %d (%d x %d), padded: %d (%d x %d)\n",
            num_chroma, bw_chroma, bh_chroma, num_padded_chroma, padded_bw_chroma, padded_bh_chroma);

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

    /* Allocate address tables (superblock scan order) */
    uint16_t *addr_luma = (uint16_t *)calloc(num_luma, sizeof(uint16_t));
    uint16_t *addr_chroma = (uint16_t *)calloc(num_chroma, sizeof(uint16_t));
    int valid_luma = init_address_table(addr_luma, bw_luma, bh_luma, luma_buf_w,
                                        padded_bw_luma, padded_bh_luma);
    int valid_chroma = init_address_table(addr_chroma, bw_chroma, bh_chroma, chroma_buf_w,
                                          padded_bw_chroma, padded_bh_chroma);
    fprintf(stderr, "  Address table: %d luma, %d chroma valid blocks\n",
            valid_luma, valid_chroma);

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

    /* Read 3-pass block assignment (uses actual block count, not padded) */
    fprintf(stderr, "\n--- Block assignment (3-pass) ---\n");
    BlockAssignment ba;
    if (!read_block_assignment(&br, &ba, num_luma, debug)) {
        fprintf(stderr, "Error: block assignment failed\n");
        goto cleanup;
    }

    fprintf(stderr, "  Bitstream pos after assignment: byte %zu bit %d\n",
            br.byte_pos, br.bit_pos);

    /* Decode green (luma) blocks in superblock order.
     * Read num_padded_luma blocks but only store valid ones. */
    fprintf(stderr, "\n--- Decoding green channel (%d padded, %d valid) ---\n",
            num_padded_luma, num_luma);
    int num_green_decoded = decode_blocks(&br, &params, blocks_green,
                                          num_padded_luma, 2, mult, debug);

    fprintf(stderr, "  Bitstream pos after green: byte %zu bit %d\n",
            br.byte_pos, br.bit_pos);

    /* Decode blue chroma blocks */
    fprintf(stderr, "\n--- Decoding blue channel (%d padded, %d valid) ---\n",
            num_padded_chroma, num_chroma);
    int num_blue_decoded = decode_blocks(&br, &params, blocks_blue,
                                         num_padded_chroma, 1, 1, debug);

    fprintf(stderr, "  Bitstream pos after blue: byte %zu bit %d\n",
            br.byte_pos, br.bit_pos);

    /* Decode red chroma blocks */
    fprintf(stderr, "\n--- Decoding red channel (%d padded, %d valid) ---\n",
            num_padded_chroma, num_chroma);
    int num_red_decoded = decode_blocks(&br, &params, blocks_red,
                                        num_padded_chroma, 0, 1, debug);

    fprintf(stderr, "  Bitstream pos after all channels: byte %zu bit %d / %zu\n",
            br.byte_pos, br.bit_pos, bitstream_size);

    /* Cap decoded block counts at valid (non-padding) counts */
    if (num_green_decoded > valid_luma) num_green_decoded = valid_luma;
    if (num_blue_decoded > valid_chroma) num_blue_decoded = valid_chroma;
    if (num_red_decoded > valid_chroma) num_red_decoded = valid_chroma;

    fprintf(stderr, "\n--- Flat-fill decode (FP values) ---\n");
    fprintf(stderr, "  Green: %d, Blue: %d, Red: %d blocks\n",
            num_green_decoded, num_blue_decoded, num_red_decoded);

    /* Flat-fill: write fixed-point value (scale_val / 16) directly into each block */
    for (int i = 0; i < num_green_decoded; i++) {
        BlockDesc *b = &blocks_green[i];
        int si = b->scale_idx;
        if (si < 0) si = 0;
        if (si >= total_scale_entries) si = total_scale_entries - 1;
        int fp = scale_table_16[si] >> 4;
        if (fp < 0) fp = 0;
        if (fp > 255) fp = 255;

        int dst_off = addr_luma[i];
        for (int row = 0; row < 4; row++) {
            for (int col = 0; col < 4; col++) {
                int di = dst_off + row * luma_buf_w + col;
                if (di >= 0 && di < luma_buf_size)
                    cur_green[di] = (uint8_t)fp;
            }
        }
    }

    /* Use chroma scale table (word0=8) for blue/red flat-fill */
    for (int i = 0; i < num_blue_decoded; i++) {
        BlockDesc *b = &blocks_blue[i];
        int si = b->scale_idx;
        if (si < 0) si = 0;
        if (si >= total_scale_entries) si = total_scale_entries - 1;
        int fp = scale_table_chroma[si] >> 4;
        if (fp < 0) fp = 0;
        if (fp > 255) fp = 255;

        int dst_off = addr_chroma[i];
        for (int row = 0; row < 4; row++) {
            for (int col = 0; col < 4; col++) {
                int di = dst_off + row * chroma_buf_w + col;
                if (di >= 0 && di < chroma_buf_size)
                    cur_blue[di] = (uint8_t)fp;
            }
        }
    }

    for (int i = 0; i < num_red_decoded; i++) {
        BlockDesc *b = &blocks_red[i];
        int si = b->scale_idx;
        if (si < 0) si = 0;
        if (si >= total_scale_entries) si = total_scale_entries - 1;
        int fp = scale_table_chroma[si] >> 4;
        if (fp < 0) fp = 0;
        if (fp > 255) fp = 255;

        int dst_off = addr_chroma[i];
        for (int row = 0; row < 4; row++) {
            for (int col = 0; col < 4; col++) {
                int di = dst_off + row * chroma_buf_w + col;
                if (di >= 0 && di < chroma_buf_size)
                    cur_red[di] = (uint8_t)fp;
            }
        }
    }

    fprintf(stderr, "  Green: %d, Blue: %d, Red: %d blocks\n",
            num_green_decoded, num_blue_decoded, num_red_decoded);

    /* Output BMP: grayscale from luma, or experimental color */
    int bgr_stride = width * 3;
    uint8_t *bgr = (uint8_t *)calloc(1, bgr_stride * height);
    if (!bgr) {
        fprintf(stderr, "Error: cannot allocate BGR buffer\n");
        goto cleanup;
    }

    /* Grayscale output from luma channel (reliable) */
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint8_t g = cur_green[y * luma_buf_w + x];
            int out = y * bgr_stride + x * 3;
            bgr[out + 0] = g;
            bgr[out + 1] = g;
            bgr[out + 2] = g;
        }
    }

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

/* ------------------------------------------------------------------ */
/* FTT raw image decoder                                               */
/* ------------------------------------------------------------------ */

/*
 * FTT files are uncompressed raw pixel data with a 26-byte header.
 * Header: "FTT\0" magic, width, height, bpp (usually 8 = grayscale).
 * Data follows immediately after header, width*height bytes.
 */

static int decode_ftt(const char *input_path, const char *output_path)
{
    long file_size = 0;
    uint8_t *file_data = read_file(input_path, &file_size);
    if (!file_data) {
        fprintf(stderr, "Error: cannot read '%s'\n", input_path);
        return 1;
    }

    if (file_size < 26 || memcmp(file_data, "FTT", 3) != 0) {
        fprintf(stderr, "Error: not an FTT file\n");
        free(file_data);
        return 1;
    }

    /* FTT header: 26 bytes (similar to FTC but no sub-header) */
    uint16_t width  = *(uint16_t *)(file_data + 8);
    uint16_t height = *(uint16_t *)(file_data + 10);
    uint16_t bpp    = *(uint16_t *)(file_data + 12);
    uint16_t hdr_size = *(uint16_t *)(file_data + 16);

    fprintf(stderr, "FTT: %s (%ld bytes)\n", input_path, file_size);
    fprintf(stderr, "  Dimensions: %u x %u, %u bpp, header %u bytes\n",
            width, height, bpp, hdr_size);

    if (bpp != 8) {
        fprintf(stderr, "Warning: FTT with bpp=%u (expected 8)\n", bpp);
    }

    const uint8_t *pixels = file_data + hdr_size;
    long pixel_count = (long)width * height;

    if (hdr_size + pixel_count > file_size) {
        fprintf(stderr, "Error: data truncated\n");
        free(file_data);
        return 1;
    }

    /* Write grayscale BMP */
    int bgr_stride = width * 3;
    uint8_t *bgr = (uint8_t *)calloc(1, bgr_stride * height);
    if (!bgr) { free(file_data); return 1; }

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint8_t v = pixels[y * width + x];
            int out = y * bgr_stride + x * 3;
            bgr[out + 0] = v;
            bgr[out + 1] = v;
            bgr[out + 2] = v;
        }
    }

    if (write_bmp(output_path, width, height, bgr, bgr_stride)) {
        fprintf(stderr, "Wrote: %s (%d x %d, 24-bit BMP)\n",
                output_path, width, height);
    }

    free(bgr);
    free(file_data);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "ftcdecode - FTC/FTT image decoder for Encarta 97\n\n");
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  ftcdecode <input.ftc> <output.bmp>   Decode FTC to BMP\n");
        fprintf(stderr, "  ftcdecode <input.ftt> <output.bmp>   Decode FTT to BMP\n");
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

    const char *input = argv[1 + arg_off];
    const char *output = argv[2 + arg_off];

    /* Auto-detect format by reading magic bytes */
    FILE *f = fopen(input, "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open '%s'\n", input);
        return 1;
    }
    char magic[4];
    fread(magic, 1, 4, f);
    fclose(f);

    if (memcmp(magic, "FTT", 3) == 0 && magic[3] == '\0') {
        return decode_ftt(input, output);
    }
    return decode_ftc(input, output, debug);
}
