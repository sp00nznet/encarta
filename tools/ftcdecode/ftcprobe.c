/*
 * ftcprobe - FTC bitstream diagnostic tool
 *
 * Tries multiple hypotheses for the FTC bitstream format:
 * 1. Opcodes at bit 0 (no header)
 * 2. Skip first N bytes
 * 3. Different buffer dimensions
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "ftcdecode.h"

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

/*
 * Parse FVF-style opcodes from given bit offset.
 * Returns number of valid ops parsed. Sets *out_blk to blocks covered.
 * If verbose, prints detailed trace.
 */
static int parse_ops(const uint8_t *data, size_t data_size,
                     int start_bit, int max_src, int max_blk,
                     int *out_blk, int verbose)
{
    BitReader br;
    br_init(&br, data, data_size);
    br.byte_pos = start_bit / 8;
    br.bit_pos = start_bit % 8;

    int blk = 0;
    int op_count = 0;

    while (blk < max_blk && br_has_bits(&br, 3)) {
        size_t pos_byte = br.byte_pos;
        int pos_bit = br.bit_pos;
        int pos_total = (int)(pos_byte * 8 + pos_bit);

        uint32_t mode = br_read(&br, 3);

        if (mode <= 3) {
            if (!br_has_bits(&br, 21)) break;
            uint32_t scale = br_read(&br, 7);
            uint32_t src = br_read(&br, 14);
            if (verbose && (op_count < 100 || src >= (uint32_t)max_src))
                printf("  [blk=%4d op=%4d bit=%5d] AFFINE_%u scale=%3u src=%5u%s\n",
                       blk, op_count, pos_total, mode, scale, src,
                       src >= (uint32_t)max_src ? " **OOB**" : "");
            if (src >= (uint32_t)max_src) break;
            blk++;
            op_count++;
        } else if (mode == 4) {
            if (!br_has_bits(&br, 5)) break;
            uint32_t count = br_read(&br, 5) + 1;
            if (verbose && op_count < 100)
                printf("  [blk=%4d op=%4d bit=%5d] SKIP %u\n", blk, op_count, pos_total, count);
            blk += count;
            op_count++;
        } else if (mode == 5) {
            if (!br_has_bits(&br, 5)) break;
            uint32_t flag = br_read(&br, 1);
            uint32_t mv_idx = br_read(&br, 4);
            uint32_t cnt = 0;
            if (flag) {
                if (!br_has_bits(&br, 8)) break;
                cnt = br_read(&br, 8) + 1;
            }
            if (verbose && op_count < 100)
                printf("  [blk=%4d op=%4d bit=%5d] MV idx=%u count=%u\n",
                       blk, op_count, pos_total, mv_idx, cnt + 1);
            blk += cnt + 1;
            op_count++;
        } else if (mode == 6) {
            br_align(&br);
            if (!br_has_bits(&br, 128)) break;
            if (verbose && op_count < 100)
                printf("  [blk=%4d op=%4d bit=%5d] RAW_4x4 @byte%zu\n",
                       blk, op_count, pos_total, br.byte_pos);
            for (int i = 0; i < 16; i++) br_read(&br, 8);
            blk++;
            op_count++;
        } else { /* mode == 7 */
            if (!br_has_bits(&br, 4)) break;
            uint32_t emode = br_read(&br, 4);

            if (emode <= 7) {
                br_align(&br);
                if (!br_has_bits(&br, 24)) break;
                uint32_t scale = br_read(&br, 7);
                uint32_t ref_flag = br_read(&br, 1);
                uint32_t src = br_read(&br, 14);
                uint32_t sub_off = br_read(&br, 2);
                if (verbose && op_count < 100)
                    printf("  [blk=%4d op=%4d bit=%5d] EXT8x8_%u scale=%3u ref=%u src=%5u sub=%u%s\n",
                           blk, op_count, pos_total, emode, scale, ref_flag, src, sub_off,
                           src >= (uint32_t)max_src ? " **OOB**" : "");
                if (src >= (uint32_t)max_src) break;

                if (ref_flag) {
                    if (!br_has_bits(&br, 3)) break;
                    uint32_t rf_mode = br_read(&br, 3);
                    if (rf_mode <= 3) {
                        if (!br_has_bits(&br, 21)) break;
                        uint32_t rs = br_read(&br, 7);
                        uint32_t rsrc = br_read(&br, 14);
                        (void)rs;
                        if (verbose && op_count < 100)
                            printf("           ref: AFFINE_%u scale=%u src=%u%s\n",
                                   rf_mode, rs, rsrc, rsrc >= (uint32_t)max_src ? " **OOB**" : "");
                        if (rsrc >= (uint32_t)max_src) break;
                    } else if (rf_mode == 4) {
                        br_align(&br);
                        if (verbose && op_count < 100)
                            printf("           ref: SKIP\n");
                    } else if (rf_mode == 5) {
                        if (!br_has_bits(&br, 5)) break;
                        uint32_t marker = br_read(&br, 1);
                        uint32_t ridx = br_read(&br, 4);
                        if (verbose && op_count < 100)
                            printf("           ref: MV marker=%u idx=%u\n", marker, ridx);
                    } else if (rf_mode == 6) {
                        br_align(&br);
                        if (!br_has_bits(&br, 128)) break;
                        for (int i = 0; i < 16; i++) br_read(&br, 8);
                        if (verbose && op_count < 100)
                            printf("           ref: RAW_4x4\n");
                    } else {
                        if (verbose)
                            printf("  ABORT: invalid ref mode %u\n", rf_mode);
                        break;
                    }
                }
                blk += 4;
                op_count++;
            } else if (emode == 12) {
                if (!br_has_bits(&br, 1)) break;
                uint32_t flag = br_read(&br, 1);
                uint32_t cnt;
                if (!flag) {
                    if (!br_has_bits(&br, 8)) break;
                    cnt = br_read(&br, 8) + 0x20;
                } else {
                    if (!br_has_bits(&br, 16)) break;
                    cnt = br_read(&br, 16) + 0x120;
                }
                if (verbose && op_count < 100)
                    printf("  [blk=%4d op=%4d bit=%5d] EXT_SKIP %u\n",
                           blk, op_count, pos_total, cnt + 1);
                blk += cnt + 1;
                op_count++;
            } else if (emode == 15) {
                br_align(&br);
                if (!br_has_bits(&br, 48)) break;
                uint32_t dst_off = br_read(&br, 16);
                uint32_t p0 = br_read(&br, 8);
                uint32_t p1 = br_read(&br, 8);
                uint32_t p2 = br_read(&br, 8);
                uint32_t p3 = br_read(&br, 8);
                if (verbose && op_count < 100)
                    printf("  [blk=%4d op=%4d bit=%5d] RAW_2x2 dst=%u px=[%u,%u,%u,%u]\n",
                           blk, op_count, pos_total, dst_off, p0, p1, p2, p3);
                /* no blk advance */
                op_count++;
            } else if (emode >= 8 && emode <= 11) {
                /* Hypothesis: 4x4 affine with transposed symmetry (no align) */
                if (!br_has_bits(&br, 21)) break;
                uint32_t scale = br_read(&br, 7);
                uint32_t src = br_read(&br, 14);
                if (verbose && op_count < 100)
                    printf("  [blk=%4d op=%4d bit=%5d] EXT4x4_%u scale=%3u src=%5u%s\n",
                           blk, op_count, pos_total, emode, scale, src,
                           src >= (uint32_t)max_src ? " **OOB**" : "");
                if (src >= (uint32_t)max_src) break;
                blk++;
                op_count++;
            } else {
                if (verbose)
                    printf("  ABORT: unknown emode %u at bit %d\n", emode, pos_total);
                break;
            }
        }
    }

    if (out_blk) *out_blk = blk;
    return op_count;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: ftcprobe <input.ftc>\n");
        return 1;
    }

    long file_size = 0;
    uint8_t *file_data = read_file(argv[1], &file_size);
    if (!file_data) { perror("read"); return 1; }

    FTCHeader hdr;
    FTCSubHeader sub;
    memset(&sub, 0, sizeof(sub));
    memcpy(&hdr, file_data, sizeof(FTCHeader));
    if (hdr.hdr_size >= sizeof(FTCHeader) + sizeof(FTCSubHeader))
        memcpy(&sub, file_data + sizeof(FTCHeader), sizeof(FTCSubHeader));

    int W = hdr.width;
    int H = hdr.height;
    int CS = sub.chroma_subsample ? sub.chroma_subsample : 2;
    int total_blocks = W * H * 3 / 32;

    printf("FTC: %ux%u, data_size=%u, total_blocks=%d\n", W, H, hdr.data_size, total_blocks);

    const uint8_t *data = file_data + hdr.hdr_size;
    size_t data_size = file_size - hdr.hdr_size;

    /* Print first 20 bytes */
    printf("Data: ");
    for (int i = 0; i < 20 && i < (int)data_size; i++)
        printf("%02x ", data[i]);
    printf("\n\n");

    /* Try different skip amounts and buffer sizes */
    printf("=== Scanning: skip bytes x buffer sizes ===\n");
    int buf_configs[][2] = {
        /* {W_internal, H_internal} */
        {W, H},           /* actual dimensions */
        {320, 200},        /* FVF standard */
        {320, 208},        /* FVF + 8 rows */
        {256, 192},        /* power of 2 width */
        {256, 256},        /* square power of 2 */
        {224, 208},        /* padded dimensions */
    };
    int n_configs = sizeof(buf_configs) / sizeof(buf_configs[0]);

    int best_ops = 0, best_skip = 0, best_config = 0;

    for (int skip = 0; skip <= 16; skip++) {
        for (int c = 0; c < n_configs; c++) {
            int bw = buf_configs[c][0];
            int bh = buf_configs[c][1];
            int src_max = (bw / 2) * (bh / 2);
            int blk_out = 0;
            int ops = parse_ops(data, data_size, skip * 8, src_max, total_blocks, &blk_out, 0);
            if (ops > best_ops) {
                best_ops = ops;
                best_skip = skip;
                best_config = c;
            }
            if (ops >= 50)
                printf("  skip=%2d buf=%dx%d (src_max=%5d): %4d ops, %4d blocks\n",
                       skip, bw, bh, src_max, ops, blk_out);
        }
    }

    printf("\nBest: skip=%d, buf=%dx%d, ops=%d\n",
           best_skip, buf_configs[best_config][0], buf_configs[best_config][1], best_ops);

    /* Also try with very lenient max_src */
    printf("\n=== Lenient scan (max_src=16384) ===\n");
    int best_lenient_ops = 0, best_lenient_skip = 0;
    for (int skip = 0; skip <= 16; skip++) {
        int blk_out = 0;
        int ops = parse_ops(data, data_size, skip * 8, 16384, total_blocks, &blk_out, 0);
        if (ops > best_lenient_ops) {
            best_lenient_ops = ops;
            best_lenient_skip = skip;
        }
        if (ops >= 20)
            printf("  skip=%2d: %4d ops, %4d blocks\n", skip, ops, blk_out);
    }

    /* Now show verbose trace for the best lenient result */
    printf("\n=== Verbose trace: skip=%d, max_src=16384 ===\n", best_lenient_skip);
    int blk_out = 0;
    parse_ops(data, data_size, best_lenient_skip * 8, 16384, total_blocks, &blk_out, 1);
    printf("Blocks covered: %d / %d\n", blk_out, total_blocks);

    /* Also try bit offsets 0-7 within first byte */
    printf("\n=== Bit offset scan (max_src=16384) ===\n");
    for (int bit = 0; bit < 64; bit++) {
        int blk_out2 = 0;
        int ops = parse_ops(data, data_size, bit, 16384, total_blocks, &blk_out2, 0);
        if (ops >= 20)
            printf("  bit=%2d: %4d ops, %4d blocks\n", bit, ops, blk_out2);
    }

    free(file_data);
    return 0;
}
