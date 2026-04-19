// Dump pre-dequant zigzag coefficients per block for a JPEG file.
#include "bitstream.h"
#include "header_parser.h"
#include "huffman.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t*)malloc(n);
    fread(buf, 1, n, f);
    fclose(f);
    *size = n;
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <in.jpg>\n", argv[0]);
        return 1;
    }
    size_t size;
    uint8_t *data = read_file(argv[1], &size);
    if (!data) { perror("read"); return 1; }

    bitstream_t bs;
    bs_init(&bs, data, size);

    jpeg_info_t info;
    uint32_t err = 0;
    if (jpeg_parse_headers(&bs, &info, &err)) {
        fprintf(stderr, "header err 0x%x\n", err);
        return 2;
    }

    info.components[0].dc_pred = 0;
    info.components[1].dc_pred = 0;
    info.components[2].dc_pred = 0;

    const htable_t *y_dc  = &info.htables_dc[info.components[0].td];
    const htable_t *y_ac  = &info.htables_ac[info.components[0].ta];
    const htable_t *cb_dc = &info.htables_dc[info.components[1].td];
    const htable_t *cb_ac = &info.htables_ac[info.components[1].ta];
    const htable_t *cr_dc = &info.htables_dc[info.components[2].td];
    const htable_t *cr_ac = &info.htables_ac[info.components[2].ta];

    int16_t coef[64];
    int mcu_idx = 0;
    for (int my = 0; my < info.mcu_rows; my++) {
        for (int mx = 0; mx < info.mcu_cols; mx++) {
            for (int i = 0; i < 4; i++) {
                printf("MCU %d blk %d (Y%d) bit_pos=%zu.%d:\n",
                       mcu_idx, i, i,
                       bs.byte_pos, bs.bit_cnt);
                if (huff_decode_block(&bs, y_dc, y_ac,
                                      &info.components[0].dc_pred, coef)) {
                    fprintf(stderr, "  HUFFMAN ERR\n");
                    return 3;
                }
                for (int k = 0; k < 64; k++) {
                    if (coef[k]) printf("  zz=%d coef=%d\n", k, coef[k]);
                }
            }
            printf("MCU %d blk 4 (Cb) bit_pos=%zu.%d:\n",
                   mcu_idx, bs.byte_pos, bs.bit_cnt);
            if (huff_decode_block(&bs, cb_dc, cb_ac,
                                  &info.components[1].dc_pred, coef)) {
                fprintf(stderr, "  HUFFMAN ERR\n");
                return 3;
            }
            for (int k = 0; k < 64; k++) {
                if (coef[k]) printf("  zz=%d coef=%d\n", k, coef[k]);
            }
            printf("MCU %d blk 5 (Cr) bit_pos=%zu.%d:\n",
                   mcu_idx, bs.byte_pos, bs.bit_cnt);
            if (huff_decode_block(&bs, cr_dc, cr_ac,
                                  &info.components[2].dc_pred, coef)) {
                fprintf(stderr, "  HUFFMAN ERR\n");
                return 3;
            }
            for (int k = 0; k < 64; k++) {
                if (coef[k]) printf("  zz=%d coef=%d\n", k, coef[k]);
            }
            printf("MCU %d end bit_pos=%zu.%d\n", mcu_idx, bs.byte_pos, bs.bit_cnt);
            mcu_idx++;
        }
    }

    free(data);
    return 0;
}
