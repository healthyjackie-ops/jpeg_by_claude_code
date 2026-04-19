#include "decoder.h"
#include "bitstream.h"
#include "header_parser.h"
#include "huffman.h"
#include "dequant.h"
#include "idct.h"
#include "chroma.h"
#include <stdlib.h>
#include <string.h>

static void copy_block_8x8(const uint8_t *src, uint8_t *dst, size_t dst_stride) {
    for (size_t r = 0; r < 8; r++) {
        memcpy(dst + r * dst_stride, src + r * 8, 8);
    }
}

static void copy_block_16x16_y(const uint8_t y_blk[4][64],
                               uint8_t *dst, size_t dst_stride) {
    copy_block_8x8(y_blk[0], dst + (size_t)0 * dst_stride + 0, dst_stride);
    copy_block_8x8(y_blk[1], dst + (size_t)0 * dst_stride + 8, dst_stride);
    copy_block_8x8(y_blk[2], dst + (size_t)8 * dst_stride + 0, dst_stride);
    copy_block_8x8(y_blk[3], dst + (size_t)8 * dst_stride + 8, dst_stride);
}

int jpeg_decode(const uint8_t *data, size_t size, jpeg_decoded_t *out) {
    memset(out, 0, sizeof(*out));

    bitstream_t bs;
    bs_init(&bs, data, size);

    jpeg_info_t info;
    uint32_t err = 0;
    if (jpeg_parse_headers(&bs, &info, &err)) {
        out->err = err;
        return -1;
    }

    out->width  = info.width;
    out->height = info.height;
    uint16_t W = info.width;
    uint16_t H = info.height;
    uint16_t CW = W >> 1;
    uint16_t CH = H >> 1;

    out->y_plane       = (uint8_t*)calloc((size_t)W  * H,  1);
    out->cb_plane_420  = (uint8_t*)calloc((size_t)CW * CH, 1);
    out->cr_plane_420  = (uint8_t*)calloc((size_t)CW * CH, 1);
    out->cb_plane      = (uint8_t*)calloc((size_t)W  * H,  1);
    out->cr_plane      = (uint8_t*)calloc((size_t)W  * H,  1);
    if (!out->y_plane || !out->cb_plane_420 || !out->cr_plane_420 ||
        !out->cb_plane || !out->cr_plane) {
        out->err = (uint32_t)JPEG_ERR_INTERNAL;
        return -1;
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
    const uint16_t *y_qt  = info.qtables[info.components[0].qt_id].q;
    const uint16_t *cb_qt = info.qtables[info.components[1].qt_id].q;
    const uint16_t *cr_qt = info.qtables[info.components[2].qt_id].q;

    int16_t coef[64];
    uint8_t y_blk[4][64];
    uint8_t cb_blk[64];
    uint8_t cr_blk[64];

    for (uint16_t my = 0; my < info.mcu_rows; my++) {
        for (uint16_t mx = 0; mx < info.mcu_cols; mx++) {
            for (int i = 0; i < 4; i++) {
                if (huff_decode_block(&bs, y_dc, y_ac,
                                      &info.components[0].dc_pred, coef)) {
                    out->err = JPEG_ERR_BAD_HUFFMAN;
                    return -1;
                }
                dequant_block(coef, y_qt);
                idct_islow(coef, y_blk[i]);
            }
            if (huff_decode_block(&bs, cb_dc, cb_ac,
                                  &info.components[1].dc_pred, coef)) {
                out->err = JPEG_ERR_BAD_HUFFMAN;
                return -1;
            }
            dequant_block(coef, cb_qt);
            idct_islow(coef, cb_blk);

            if (huff_decode_block(&bs, cr_dc, cr_ac,
                                  &info.components[2].dc_pred, coef)) {
                out->err = JPEG_ERR_BAD_HUFFMAN;
                return -1;
            }
            dequant_block(coef, cr_qt);
            idct_islow(coef, cr_blk);

            uint8_t *y_dst = out->y_plane + (my * 16) * W + (mx * 16);
            copy_block_16x16_y(y_blk, y_dst, W);

            uint8_t *cb_dst = out->cb_plane_420 + (my * 8) * CW + (mx * 8);
            uint8_t *cr_dst = out->cr_plane_420 + (my * 8) * CW + (mx * 8);
            copy_block_8x8(cb_blk, cb_dst, CW);
            copy_block_8x8(cr_blk, cr_dst, CW);
        }
    }

    chroma_upsample_nn(out->cb_plane_420, out->cb_plane, W, H);
    chroma_upsample_nn(out->cr_plane_420, out->cr_plane, W, H);

    bs_align_to_byte(&bs);
    uint8_t b;
    int saw_eoi = 0;
    while (bs_read_byte(&bs, &b) == 0) {
        if (b == 0xFF) {
            while (bs_read_byte(&bs, &b) == 0 && b == 0xFF) {}
            if (b == MARKER_EOI) { saw_eoi = 1; break; }
        }
    }
    if (!saw_eoi && bs.marker_pending && bs.last_marker == MARKER_EOI) {
        saw_eoi = 1;
    }
    if (!saw_eoi) {
        out->err = JPEG_ERR_STREAM_TRUNC;
        return -1;
    }

    out->err = 0;
    return 0;
}

void jpeg_free(jpeg_decoded_t *out) {
    free(out->y_plane);
    free(out->cb_plane);
    free(out->cr_plane);
    free(out->cb_plane_420);
    free(out->cr_plane_420);
    memset(out, 0, sizeof(*out));
}
