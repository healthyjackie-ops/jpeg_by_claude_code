#include "decoder.h"
#include "bitstream.h"
#include "header_parser.h"
#include "huffman.h"
#include "dequant.h"
#include "idct.h"
#include "chroma.h"
#include <stdio.h>
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

/* Phase 13: 12-bit helpers ------------------------------------------------- */

static void dequant_block_i32(const int16_t coef[64], const uint16_t q[64],
                              int32_t out[64]) {
    for (int i = 0; i < 64; i++) {
        out[i] = (int32_t)coef[i] * (int32_t)q[i];
    }
}

static void copy_block_8x8_u16(const uint16_t *src, uint16_t *dst, size_t dst_stride) {
    for (size_t r = 0; r < 8; r++) {
        memcpy(dst + r * dst_stride, src + r * 8, 8 * sizeof(uint16_t));
    }
}

static void copy_block_16x16_y_u16(const uint16_t y_blk[4][64],
                                   uint16_t *dst, size_t dst_stride) {
    copy_block_8x8_u16(y_blk[0], dst + (size_t)0 * dst_stride + 0, dst_stride);
    copy_block_8x8_u16(y_blk[1], dst + (size_t)0 * dst_stride + 8, dst_stride);
    copy_block_8x8_u16(y_blk[2], dst + (size_t)8 * dst_stride + 0, dst_stride);
    copy_block_8x8_u16(y_blk[3], dst + (size_t)8 * dst_stride + 8, dst_stride);
}

static int decode_p12(bitstream_t *bs, jpeg_info_t *info, jpeg_decoded_t *out);
static int decode_progressive(bitstream_t *bs, jpeg_info_t *info,
                              jpeg_decoded_t *out);
static int decode_lossless(bitstream_t *bs, jpeg_info_t *info,
                           jpeg_decoded_t *out);

/* Phase 16b: progressive SOF2 first-scan dispatch state. Set from info before
 * entering the MCU loop; consulted by dec_blk(). Not thread-safe — jpeg_decode
 * is a single call path. */
static int     g_dc_only = 0;
static uint8_t g_al      = 0;

/* Phase 16b: wrapper around huff_decode_block. For SOF2 single DC-only scans
 * (Ss=Se=0, Ah=0) we decode only the DC coefficient and store it shifted by
 * Al; all AC coefficients remain zero. Baseline SOF0/SOF1 paths are untouched.
 */
static int dec_blk(bitstream_t *bs, const htable_t *dc, const htable_t *ac,
                   int16_t *dc_pred, int16_t coef[64]) {
    if (g_dc_only) return huff_decode_block_dc_only(bs, dc, dc_pred, coef, g_al);
    return huff_decode_block(bs, dc, ac, dc_pred, coef);
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

    out->precision = info.precision;

    /* Phase 13a.3: P=12 branches to a dedicated 16-bit decode path supporting
       grayscale / 4:4:4 / 4:2:0. Other chroma modes with P=12 (CMYK, 4:2:2,
       4:4:0, 4:1:1) are out of scope for Phase 13. */
    if (info.precision == 12) {
        return decode_p12(&bs, &info, out);
    }

    /* Phase 17a/17c: SOF2 handled via a dedicated multi-scan decoder that
     * supports DC + spectral-selection AC scans (Ah=0) and refinement scans
     * (Ah>0, Phase 18a). DRI>0 is accepted (Phase 17c/18c) and enforced inside
     * each scan's MCU/block loop. The legacy single-scan DC-only path (Phase
     * 16b) is a degenerate sub-case and remains covered by the same routine. */
    g_dc_only = 0;
    g_al      = 0;
    if (info.sof_type == 2) {
        return decode_progressive(&bs, &info, out);
    }
    /* Phase 25a: SOF3 lossless. Scope = Nf=1 gray, P=8, Pt 0..15, Ps 1..7,
     * DRI=0 (multi-component + DRI is Phase 25b/c). */
    if (info.sof_type == 3) {
        return decode_lossless(&bs, &info, out);
    }

    out->width  = info.width;
    out->height = info.height;
    uint16_t W  = info.width;
    uint16_t H  = info.height;
    int is_gray = (info.chroma_mode == CHROMA_GRAY);
    int is_444  = (info.chroma_mode == CHROMA_444);
    int is_420  = (info.chroma_mode == CHROMA_420);
    int is_422  = (info.chroma_mode == CHROMA_422);
    int is_440  = (info.chroma_mode == CHROMA_440);
    int is_411  = (info.chroma_mode == CHROMA_411);
    int is_cmyk = (info.chroma_mode == CHROMA_CMYK);
    /* Phase 6: 内部用 MCU 对齐的 padded 尺寸解码，结束再 crop 到 W×H
       Phase 8: 灰度 MCU=8x8, 彩色 4:2:0 MCU=16x16
       Phase 9: 4:4:4 MCU=8x8
       Phase 10: 4:2:2 MCU=16x8 (Y 2x1, chroma horizontal-subsampled)
       Phase 11a: 4:4:0 MCU=8x16 (Y 1x2, chroma vertical-subsampled)
       Phase 11b: 4:1:1 MCU=32x8 (Y 4x1, chroma 4x horizontal-subsampled)
       Phase 12: CMYK Nf=4 all 1x1, MCU=8x8, 4 blocks */
    uint16_t mcu_w = is_411 ? 32 : ((is_420 || is_422) ? 16 : 8);
    uint16_t mcu_h = (is_420 || is_440) ? 16 : 8;
    uint16_t Wp = info.mcu_cols * mcu_w;
    uint16_t Hp = info.mcu_rows * mcu_h;
    /* Sub-sampled chroma pad dimensions
       420: half W, half H;  422: half W, full H;  440: full W, half H; 411: quarter W, full H */
    uint16_t CWp_sub = is_411 ? (Wp >> 2) : ((is_420 || is_422) ? (Wp >> 1) : Wp);
    uint16_t CHp_sub = (is_420 || is_440) ? (Hp >> 1) : Hp;

    int is_chroma_sub = (is_420 || is_422 || is_440 || is_411);
    uint8_t *y_pad       = (uint8_t*)calloc((size_t)Wp  * Hp,  1);
    uint8_t *cb_pad_sub  = is_chroma_sub ? (uint8_t*)calloc((size_t)CWp_sub * CHp_sub, 1) : NULL;
    uint8_t *cr_pad_sub  = is_chroma_sub ? (uint8_t*)calloc((size_t)CWp_sub * CHp_sub, 1) : NULL;
    uint8_t *cb_pad      = (is_gray || is_cmyk) ? NULL : (uint8_t*)calloc((size_t)Wp  * Hp,  1);
    uint8_t *cr_pad      = (is_gray || is_cmyk) ? NULL : (uint8_t*)calloc((size_t)Wp  * Hp,  1);
    /* Phase 12: CMYK needs 3 extra padded buffers for M, Y (yellow-comp), K;
       y_pad above holds the C component when is_cmyk. */
    uint8_t *m_pad       = is_cmyk ? (uint8_t*)calloc((size_t)Wp * Hp, 1) : NULL;
    uint8_t *yc_pad      = is_cmyk ? (uint8_t*)calloc((size_t)Wp * Hp, 1) : NULL;
    uint8_t *k_pad       = is_cmyk ? (uint8_t*)calloc((size_t)Wp * Hp, 1) : NULL;
    if (is_cmyk) {
        out->c_plane        = (uint8_t*)calloc((size_t)W * H, 1);
        out->m_plane        = (uint8_t*)calloc((size_t)W * H, 1);
        out->y_plane_cmyk   = (uint8_t*)calloc((size_t)W * H, 1);
        out->k_plane        = (uint8_t*)calloc((size_t)W * H, 1);
    } else {
        out->y_plane       = (uint8_t*)calloc((size_t)W  * H,  1);
        if (!is_gray) {
            out->cb_plane      = (uint8_t*)calloc((size_t)W  * H,  1);
            out->cr_plane      = (uint8_t*)calloc((size_t)W  * H,  1);
            if (is_420) {
                out->cb_plane_420 = (uint8_t*)calloc((size_t)(W >> 1) * (H >> 1), 1);
                out->cr_plane_420 = (uint8_t*)calloc((size_t)(W >> 1) * (H >> 1), 1);
            }
            if (is_422) {
                out->cb_plane_422 = (uint8_t*)calloc((size_t)(W >> 1) * H, 1);
                out->cr_plane_422 = (uint8_t*)calloc((size_t)(W >> 1) * H, 1);
            }
            if (is_440) {
                out->cb_plane_440 = (uint8_t*)calloc((size_t)W * (H >> 1), 1);
                out->cr_plane_440 = (uint8_t*)calloc((size_t)W * (H >> 1), 1);
            }
            if (is_411) {
                out->cb_plane_411 = (uint8_t*)calloc((size_t)(W >> 2) * H, 1);
                out->cr_plane_411 = (uint8_t*)calloc((size_t)(W >> 2) * H, 1);
            }
        }
    }
    int alloc_ok = y_pad &&
                   (is_cmyk
                    ? (m_pad && yc_pad && k_pad &&
                       out->c_plane && out->m_plane && out->y_plane_cmyk && out->k_plane)
                    : (out->y_plane &&
                       (is_gray ||
                        (cb_pad && cr_pad &&
                         out->cb_plane && out->cr_plane &&
                         (!is_420 ||
                          (cb_pad_sub && cr_pad_sub &&
                           out->cb_plane_420 && out->cr_plane_420)) &&
                         (!is_422 ||
                          (cb_pad_sub && cr_pad_sub &&
                           out->cb_plane_422 && out->cr_plane_422)) &&
                         (!is_440 ||
                          (cb_pad_sub && cr_pad_sub &&
                           out->cb_plane_440 && out->cr_plane_440)) &&
                         (!is_411 ||
                          (cb_pad_sub && cr_pad_sub &&
                           out->cb_plane_411 && out->cr_plane_411))))));
    if (!alloc_ok) {
        free(y_pad); free(cb_pad_sub); free(cr_pad_sub); free(cb_pad); free(cr_pad);
        free(m_pad); free(yc_pad); free(k_pad);
        out->err = (uint32_t)JPEG_ERR_INTERNAL;
        return -1;
    }
    (void)is_444;

    info.components[0].dc_pred = 0;
    if (!is_gray) {
        info.components[1].dc_pred = 0;
        info.components[2].dc_pred = 0;
    }
    if (is_cmyk) {
        info.components[3].dc_pred = 0;
    }

    const htable_t *y_dc  = &info.htables_dc[info.components[0].td];
    const htable_t *y_ac  = &info.htables_ac[info.components[0].ta];
    const htable_t *cb_dc = is_gray ? NULL : &info.htables_dc[info.components[1].td];
    const htable_t *cb_ac = is_gray ? NULL : &info.htables_ac[info.components[1].ta];
    const htable_t *cr_dc = is_gray ? NULL : &info.htables_dc[info.components[2].td];
    const htable_t *cr_ac = is_gray ? NULL : &info.htables_ac[info.components[2].ta];
    const uint16_t *y_qt  = info.qtables[info.components[0].qt_id].q;
    const uint16_t *cb_qt = is_gray ? NULL : info.qtables[info.components[1].qt_id].q;
    const uint16_t *cr_qt = is_gray ? NULL : info.qtables[info.components[2].qt_id].q;
    /* Phase 12: 4th component tables for CMYK K */
    const htable_t *k_dc  = is_cmyk ? &info.htables_dc[info.components[3].td] : NULL;
    const htable_t *k_ac  = is_cmyk ? &info.htables_ac[info.components[3].ta] : NULL;
    const uint16_t *k_qt  = is_cmyk ? info.qtables[info.components[3].qt_id].q : NULL;

    int16_t coef[64];
    uint8_t y_blk[4][64];
    uint8_t cb_blk[64];
    uint8_t cr_blk[64];
    uint8_t k_blk[64];

    /* Phase 7: DRI restart 计数器 */
    uint16_t restart_cnt = 0;

    for (uint16_t my = 0; my < info.mcu_rows; my++) {
        for (uint16_t mx = 0; mx < info.mcu_cols; mx++) {
            if (is_gray) {
                /* Phase 8: 单 Y block, 8x8 MCU */
                if (dec_blk(&bs, y_dc, y_ac,
                                      &info.components[0].dc_pred, coef)) {
                    out->err = JPEG_ERR_BAD_HUFFMAN;
                    return -1;
                }
                dequant_block(coef, y_qt);
                idct_islow(coef, y_blk[0]);
                uint8_t *y_dst = y_pad + (size_t)(my * 8) * Wp + (mx * 8);
                copy_block_8x8(y_blk[0], y_dst, Wp);
            } else if (is_420) {
                for (int i = 0; i < 4; i++) {
                    if (dec_blk(&bs, y_dc, y_ac,
                                          &info.components[0].dc_pred, coef)) {
                        out->err = JPEG_ERR_BAD_HUFFMAN;
                        return -1;
                    }
                    dequant_block(coef, y_qt);
                    idct_islow(coef, y_blk[i]);
                }
                if (dec_blk(&bs, cb_dc, cb_ac,
                                      &info.components[1].dc_pred, coef)) {
                    out->err = JPEG_ERR_BAD_HUFFMAN;
                    return -1;
                }
                dequant_block(coef, cb_qt);
                idct_islow(coef, cb_blk);

                if (dec_blk(&bs, cr_dc, cr_ac,
                                      &info.components[2].dc_pred, coef)) {
                    out->err = JPEG_ERR_BAD_HUFFMAN;
                    return -1;
                }
                dequant_block(coef, cr_qt);
                idct_islow(coef, cr_blk);

                uint8_t *y_dst = y_pad + (size_t)(my * 16) * Wp + (mx * 16);
                copy_block_16x16_y(y_blk, y_dst, Wp);

                uint8_t *cb_dst = cb_pad_sub + (size_t)(my * 8) * CWp_sub + (mx * 8);
                uint8_t *cr_dst = cr_pad_sub + (size_t)(my * 8) * CWp_sub + (mx * 8);
                copy_block_8x8(cb_blk, cb_dst, CWp_sub);
                copy_block_8x8(cr_blk, cr_dst, CWp_sub);
            } else if (is_422) {
                /* Phase 10: 4:2:2 — 4 blocks/MCU: Y_left, Y_right, Cb, Cr.
                   Y is 2x1 subsampled, chroma is 1x1 at half horizontal res. */
                for (int i = 0; i < 2; i++) {
                    if (dec_blk(&bs, y_dc, y_ac,
                                          &info.components[0].dc_pred, coef)) {
                        out->err = JPEG_ERR_BAD_HUFFMAN;
                        return -1;
                    }
                    dequant_block(coef, y_qt);
                    idct_islow(coef, y_blk[i]);
                }
                if (dec_blk(&bs, cb_dc, cb_ac,
                                      &info.components[1].dc_pred, coef)) {
                    out->err = JPEG_ERR_BAD_HUFFMAN;
                    return -1;
                }
                dequant_block(coef, cb_qt);
                idct_islow(coef, cb_blk);

                if (dec_blk(&bs, cr_dc, cr_ac,
                                      &info.components[2].dc_pred, coef)) {
                    out->err = JPEG_ERR_BAD_HUFFMAN;
                    return -1;
                }
                dequant_block(coef, cr_qt);
                idct_islow(coef, cr_blk);

                uint8_t *y_dst = y_pad + (size_t)(my * 8) * Wp + (mx * 16);
                copy_block_8x8(y_blk[0], y_dst + 0, Wp);
                copy_block_8x8(y_blk[1], y_dst + 8, Wp);

                uint8_t *cb_dst = cb_pad_sub + (size_t)(my * 8) * CWp_sub + (mx * 8);
                uint8_t *cr_dst = cr_pad_sub + (size_t)(my * 8) * CWp_sub + (mx * 8);
                copy_block_8x8(cb_blk, cb_dst, CWp_sub);
                copy_block_8x8(cr_blk, cr_dst, CWp_sub);
            } else if (is_411) {
                /* Phase 11b: 4:1:1 — 6 blocks/MCU: Y0,Y1,Y2,Y3, Cb, Cr.
                   Y is 4x1 subsampled (horizontal), chroma is 1x1 at quarter horizontal res. */
                for (int i = 0; i < 4; i++) {
                    if (dec_blk(&bs, y_dc, y_ac,
                                          &info.components[0].dc_pred, coef)) {
                        out->err = JPEG_ERR_BAD_HUFFMAN;
                        return -1;
                    }
                    dequant_block(coef, y_qt);
                    idct_islow(coef, y_blk[i]);
                }
                if (dec_blk(&bs, cb_dc, cb_ac,
                                      &info.components[1].dc_pred, coef)) {
                    out->err = JPEG_ERR_BAD_HUFFMAN;
                    return -1;
                }
                dequant_block(coef, cb_qt);
                idct_islow(coef, cb_blk);

                if (dec_blk(&bs, cr_dc, cr_ac,
                                      &info.components[2].dc_pred, coef)) {
                    out->err = JPEG_ERR_BAD_HUFFMAN;
                    return -1;
                }
                dequant_block(coef, cr_qt);
                idct_islow(coef, cr_blk);

                uint8_t *y_dst = y_pad + (size_t)(my * 8) * Wp + (mx * 32);
                copy_block_8x8(y_blk[0], y_dst +  0, Wp);
                copy_block_8x8(y_blk[1], y_dst +  8, Wp);
                copy_block_8x8(y_blk[2], y_dst + 16, Wp);
                copy_block_8x8(y_blk[3], y_dst + 24, Wp);

                uint8_t *cb_dst = cb_pad_sub + (size_t)(my * 8) * CWp_sub + (mx * 8);
                uint8_t *cr_dst = cr_pad_sub + (size_t)(my * 8) * CWp_sub + (mx * 8);
                copy_block_8x8(cb_blk, cb_dst, CWp_sub);
                copy_block_8x8(cr_blk, cr_dst, CWp_sub);
            } else if (is_440) {
                /* Phase 11a: 4:4:0 — 4 blocks/MCU: Y_top, Y_bot, Cb, Cr.
                   Y is 1x2 subsampled (vertical), chroma is 1x1 at half vertical res. */
                for (int i = 0; i < 2; i++) {
                    if (dec_blk(&bs, y_dc, y_ac,
                                          &info.components[0].dc_pred, coef)) {
                        out->err = JPEG_ERR_BAD_HUFFMAN;
                        return -1;
                    }
                    dequant_block(coef, y_qt);
                    idct_islow(coef, y_blk[i]);
                }
                if (dec_blk(&bs, cb_dc, cb_ac,
                                      &info.components[1].dc_pred, coef)) {
                    out->err = JPEG_ERR_BAD_HUFFMAN;
                    return -1;
                }
                dequant_block(coef, cb_qt);
                idct_islow(coef, cb_blk);

                if (dec_blk(&bs, cr_dc, cr_ac,
                                      &info.components[2].dc_pred, coef)) {
                    out->err = JPEG_ERR_BAD_HUFFMAN;
                    return -1;
                }
                dequant_block(coef, cr_qt);
                idct_islow(coef, cr_blk);

                uint8_t *y_dst = y_pad + (size_t)(my * 16) * Wp + (mx * 8);
                copy_block_8x8(y_blk[0], y_dst + (size_t)0 * Wp, Wp);
                copy_block_8x8(y_blk[1], y_dst + (size_t)8 * Wp, Wp);

                uint8_t *cb_dst = cb_pad_sub + (size_t)(my * 8) * CWp_sub + (mx * 8);
                uint8_t *cr_dst = cr_pad_sub + (size_t)(my * 8) * CWp_sub + (mx * 8);
                copy_block_8x8(cb_blk, cb_dst, CWp_sub);
                copy_block_8x8(cr_blk, cr_dst, CWp_sub);
            } else if (is_cmyk) {
                /* Phase 12: CMYK — 4 blocks/MCU, 8x8 each, all 1x1.
                   Component order in stream matches SOF/SOS ordering: C, M, Y, K.
                   Store into y_pad (C), m_pad (M), yc_pad (Y-comp), k_pad (K). */
                /* Block 0: C */
                if (dec_blk(&bs, y_dc, y_ac,
                                      &info.components[0].dc_pred, coef)) {
                    out->err = JPEG_ERR_BAD_HUFFMAN;
                    return -1;
                }
                dequant_block(coef, y_qt);
                idct_islow(coef, y_blk[0]);

                /* Block 1: M */
                if (dec_blk(&bs, cb_dc, cb_ac,
                                      &info.components[1].dc_pred, coef)) {
                    out->err = JPEG_ERR_BAD_HUFFMAN;
                    return -1;
                }
                dequant_block(coef, cb_qt);
                idct_islow(coef, cb_blk);

                /* Block 2: Y (yellow) */
                if (dec_blk(&bs, cr_dc, cr_ac,
                                      &info.components[2].dc_pred, coef)) {
                    out->err = JPEG_ERR_BAD_HUFFMAN;
                    return -1;
                }
                dequant_block(coef, cr_qt);
                idct_islow(coef, cr_blk);

                /* Block 3: K */
                if (dec_blk(&bs, k_dc, k_ac,
                                      &info.components[3].dc_pred, coef)) {
                    out->err = JPEG_ERR_BAD_HUFFMAN;
                    return -1;
                }
                dequant_block(coef, k_qt);
                idct_islow(coef, k_blk);

                uint8_t *c_dst  = y_pad  + (size_t)(my * 8) * Wp + (mx * 8);
                uint8_t *m_dst  = m_pad  + (size_t)(my * 8) * Wp + (mx * 8);
                uint8_t *yc_dst = yc_pad + (size_t)(my * 8) * Wp + (mx * 8);
                uint8_t *k_dst  = k_pad  + (size_t)(my * 8) * Wp + (mx * 8);
                copy_block_8x8(y_blk[0], c_dst,  Wp);
                copy_block_8x8(cb_blk,   m_dst,  Wp);
                copy_block_8x8(cr_blk,   yc_dst, Wp);
                copy_block_8x8(k_blk,    k_dst,  Wp);
            } else {
                /* Phase 9: 4:4:4 — 3 blocks/MCU, 8x8 each, chroma full-res */
                if (dec_blk(&bs, y_dc, y_ac,
                                      &info.components[0].dc_pred, coef)) {
                    out->err = JPEG_ERR_BAD_HUFFMAN;
                    return -1;
                }
                dequant_block(coef, y_qt);
                idct_islow(coef, y_blk[0]);

                if (dec_blk(&bs, cb_dc, cb_ac,
                                      &info.components[1].dc_pred, coef)) {
                    out->err = JPEG_ERR_BAD_HUFFMAN;
                    return -1;
                }
                dequant_block(coef, cb_qt);
                idct_islow(coef, cb_blk);

                if (dec_blk(&bs, cr_dc, cr_ac,
                                      &info.components[2].dc_pred, coef)) {
                    out->err = JPEG_ERR_BAD_HUFFMAN;
                    return -1;
                }
                dequant_block(coef, cr_qt);
                idct_islow(coef, cr_blk);

                uint8_t *y_dst  = y_pad  + (size_t)(my * 8) * Wp + (mx * 8);
                uint8_t *cb_dst = cb_pad + (size_t)(my * 8) * Wp + (mx * 8);
                uint8_t *cr_dst = cr_pad + (size_t)(my * 8) * Wp + (mx * 8);
                copy_block_8x8(y_blk[0], y_dst,  Wp);
                copy_block_8x8(cb_blk,   cb_dst, Wp);
                copy_block_8x8(cr_blk,   cr_dst, Wp);
            }

            /* Phase 7: DRI 边界 — 每 N 个 MCU 一个 RSTn。
               最后一个 MCU 之后不再要求 RST（紧跟 EOI）。 */
            if (info.dri != 0) {
                restart_cnt++;
                int is_last = (my == info.mcu_rows - 1) && (mx == info.mcu_cols - 1);
                if (restart_cnt == info.dri && !is_last) {
                    /* 消费 RSTn marker。fetch_entropy_byte 可能已经把它记录
                       到 bs->last_marker（marker_pending=1），也可能还没读到。 */
                    bs_align_to_byte(&bs);
                    if (!bs.marker_pending) {
                        uint8_t b;
                        if (bs_read_byte(&bs, &b) || b != 0xFF) {
                            out->err = JPEG_ERR_BAD_MARKER;
                            return -1;
                        }
                        while (bs_read_byte(&bs, &b) == 0 && b == 0xFF) {}
                        if (b < MARKER_RST0 || b > MARKER_RST7) {
                            out->err = JPEG_ERR_BAD_MARKER;
                            return -1;
                        }
                    } else {
                        if (bs.last_marker < MARKER_RST0 ||
                            bs.last_marker > MARKER_RST7) {
                            out->err = JPEG_ERR_BAD_MARKER;
                            return -1;
                        }
                        bs.marker_pending = 0;
                        bs.last_marker    = 0;
                    }
                    /* 重置 DC 预测器 */
                    info.components[0].dc_pred = 0;
                    if (!is_gray) {
                        info.components[1].dc_pred = 0;
                        info.components[2].dc_pred = 0;
                    }
                    if (is_cmyk) {
                        info.components[3].dc_pred = 0;
                    }
                    restart_cnt = 0;
                }
            }
        }
    }

    if (is_420) {
        chroma_upsample_nn(cb_pad_sub, cb_pad, Wp, Hp);
        chroma_upsample_nn(cr_pad_sub, cr_pad, Wp, Hp);
    } else if (is_422) {
        /* Phase 10: 4:2:2 — horizontal-only nearest-neighbor upsample.
           src: CWp_sub × Hp  →  dst: Wp × Hp   (each src col maps to 2 dst cols) */
        for (uint16_t r = 0; r < Hp; r++) {
            const uint8_t *cb_src = cb_pad_sub + (size_t)r * CWp_sub;
            const uint8_t *cr_src = cr_pad_sub + (size_t)r * CWp_sub;
            uint8_t *cb_dst = cb_pad + (size_t)r * Wp;
            uint8_t *cr_dst = cr_pad + (size_t)r * Wp;
            for (uint16_t c = 0; c < CWp_sub; c++) {
                cb_dst[2*c    ] = cb_src[c];
                cb_dst[2*c + 1] = cb_src[c];
                cr_dst[2*c    ] = cr_src[c];
                cr_dst[2*c + 1] = cr_src[c];
            }
        }
    } else if (is_411) {
        /* Phase 11b: 4:1:1 — horizontal 4x nearest-neighbor upsample.
           src: CWp_sub × Hp  →  dst: Wp × Hp   (each src col maps to 4 dst cols) */
        for (uint16_t r = 0; r < Hp; r++) {
            const uint8_t *cb_src = cb_pad_sub + (size_t)r * CWp_sub;
            const uint8_t *cr_src = cr_pad_sub + (size_t)r * CWp_sub;
            uint8_t *cb_dst = cb_pad + (size_t)r * Wp;
            uint8_t *cr_dst = cr_pad + (size_t)r * Wp;
            for (uint16_t c = 0; c < CWp_sub; c++) {
                cb_dst[4*c    ] = cb_src[c];
                cb_dst[4*c + 1] = cb_src[c];
                cb_dst[4*c + 2] = cb_src[c];
                cb_dst[4*c + 3] = cb_src[c];
                cr_dst[4*c    ] = cr_src[c];
                cr_dst[4*c + 1] = cr_src[c];
                cr_dst[4*c + 2] = cr_src[c];
                cr_dst[4*c + 3] = cr_src[c];
            }
        }
    } else if (is_440) {
        /* Phase 11a: 4:4:0 — vertical-only nearest-neighbor upsample.
           src: Wp × CHp_sub  →  dst: Wp × Hp   (each src row maps to 2 dst rows) */
        for (uint16_t r = 0; r < CHp_sub; r++) {
            const uint8_t *cb_src = cb_pad_sub + (size_t)r * CWp_sub;
            const uint8_t *cr_src = cr_pad_sub + (size_t)r * CWp_sub;
            uint8_t *cb_dst0 = cb_pad + (size_t)(2*r    ) * Wp;
            uint8_t *cb_dst1 = cb_pad + (size_t)(2*r + 1) * Wp;
            uint8_t *cr_dst0 = cr_pad + (size_t)(2*r    ) * Wp;
            uint8_t *cr_dst1 = cr_pad + (size_t)(2*r + 1) * Wp;
            memcpy(cb_dst0, cb_src, Wp);
            memcpy(cb_dst1, cb_src, Wp);
            memcpy(cr_dst0, cr_src, Wp);
            memcpy(cr_dst1, cr_src, Wp);
        }
    }
    /* Phase 9: 4:4:4 → cb_pad/cr_pad 已写为全分辨率，无需 upsample */

    /* Phase 6: crop padded planes → actual W×H output planes */
    if (is_cmyk) {
        /* Phase 12: CMYK — 4 planes, all 1×1 */
        for (uint16_t r = 0; r < H; r++) {
            memcpy(out->c_plane      + (size_t)r * W, y_pad  + (size_t)r * Wp, W);
            memcpy(out->m_plane      + (size_t)r * W, m_pad  + (size_t)r * Wp, W);
            memcpy(out->y_plane_cmyk + (size_t)r * W, yc_pad + (size_t)r * Wp, W);
            memcpy(out->k_plane      + (size_t)r * W, k_pad  + (size_t)r * Wp, W);
        }
    } else {
        for (uint16_t r = 0; r < H; r++) {
            memcpy(out->y_plane + (size_t)r * W, y_pad + (size_t)r * Wp, W);
            if (!is_gray) {
                memcpy(out->cb_plane + (size_t)r * W, cb_pad + (size_t)r * Wp, W);
                memcpy(out->cr_plane + (size_t)r * W, cr_pad + (size_t)r * Wp, W);
            }
        }
    }
    if (is_420) {
        /* 4:2:0 chroma planes: only fill the top-left (W/2)×(H/2) portion; caller
           may inspect these when needed. MCU-aligned W/H were previously assumed,
           so we pack conservatively for non-aligned: copy (W>>1) cols from padded. */
        for (uint16_t r = 0; r < (H >> 1); r++) {
            memcpy(out->cb_plane_420 + (size_t)r * (W >> 1),
                   cb_pad_sub + (size_t)r * CWp_sub, (W >> 1));
            memcpy(out->cr_plane_420 + (size_t)r * (W >> 1),
                   cr_pad_sub + (size_t)r * CWp_sub, (W >> 1));
        }
    }
    if (is_422) {
        /* 4:2:2 sub-res chroma planes: (W/2) × H for libjpeg raw comparison. */
        for (uint16_t r = 0; r < H; r++) {
            memcpy(out->cb_plane_422 + (size_t)r * (W >> 1),
                   cb_pad_sub + (size_t)r * CWp_sub, (W >> 1));
            memcpy(out->cr_plane_422 + (size_t)r * (W >> 1),
                   cr_pad_sub + (size_t)r * CWp_sub, (W >> 1));
        }
    }
    if (is_440) {
        /* 4:4:0 sub-res chroma planes: W × (H/2) for libjpeg raw comparison. */
        for (uint16_t r = 0; r < (H >> 1); r++) {
            memcpy(out->cb_plane_440 + (size_t)r * W,
                   cb_pad_sub + (size_t)r * CWp_sub, W);
            memcpy(out->cr_plane_440 + (size_t)r * W,
                   cr_pad_sub + (size_t)r * CWp_sub, W);
        }
    }
    if (is_411) {
        /* 4:1:1 sub-res chroma planes: (W/4) × H for libjpeg raw comparison. */
        for (uint16_t r = 0; r < H; r++) {
            memcpy(out->cb_plane_411 + (size_t)r * (W >> 2),
                   cb_pad_sub + (size_t)r * CWp_sub, (W >> 2));
            memcpy(out->cr_plane_411 + (size_t)r * (W >> 2),
                   cr_pad_sub + (size_t)r * CWp_sub, (W >> 2));
        }
    }
    free(y_pad); free(cb_pad_sub); free(cr_pad_sub); free(cb_pad); free(cr_pad);
    free(m_pad); free(yc_pad); free(k_pad);

    /* Phase 16b: SOF2 single DC-only scan must be followed directly by EOI
     * (possibly after padding/byte-stuffing). A different marker in-between
     * (SOS, DHT, ...) means the file is multi-scan progressive which we do
     * not yet support. Catch the pending-marker case first so it's not
     * silently swallowed by the EOI scanner below. */
    if (info.sof_type == 2 && bs.marker_pending &&
        bs.last_marker != MARKER_EOI) {
        out->err = JPEG_ERR_UNSUP_SOF;
        return -1;
    }

    bs_align_to_byte(&bs);
    uint8_t b;
    int saw_eoi = 0;
    while (bs_read_byte(&bs, &b) == 0) {
        if (b == 0xFF) {
            while (bs_read_byte(&bs, &b) == 0 && b == 0xFF) {}
            if (b == MARKER_EOI) { saw_eoi = 1; break; }
            /* Phase 16b: anything else past the DC-only scan is unsupported. */
            if (info.sof_type == 2 && b != 0x00) {
                out->err = JPEG_ERR_UNSUP_SOF;
                return -1;
            }
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

static int decode_p12(bitstream_t *bs, jpeg_info_t *info, jpeg_decoded_t *out) {
    out->width  = info->width;
    out->height = info->height;
    uint16_t W  = info->width;
    uint16_t H  = info->height;

    int is_gray = (info->chroma_mode == CHROMA_GRAY);
    int is_444  = (info->chroma_mode == CHROMA_444);
    int is_420  = (info->chroma_mode == CHROMA_420);
    if (!is_gray && !is_444 && !is_420) {
        /* Phase 13 spec defers P=12 + {4:2:2, 4:4:0, 4:1:1, CMYK} to later. */
        out->err = JPEG_ERR_UNSUP_CHROMA;
        return -1;
    }

    uint16_t mcu_w = is_420 ? 16 : 8;
    uint16_t mcu_h = is_420 ? 16 : 8;
    uint16_t Wp = info->mcu_cols * mcu_w;
    uint16_t Hp = info->mcu_rows * mcu_h;
    uint16_t CWp_sub = is_420 ? (Wp >> 1) : Wp;
    uint16_t CHp_sub = is_420 ? (Hp >> 1) : Hp;

    size_t y_samples  = (size_t)Wp * Hp;
    size_t cb_samples = is_gray ? 0 : (is_420 ? (size_t)CWp_sub * CHp_sub : y_samples);

    uint16_t *y_pad      = (uint16_t*)calloc(y_samples,  sizeof(uint16_t));
    uint16_t *cb_pad_sub = (is_420)   ? (uint16_t*)calloc(cb_samples, sizeof(uint16_t)) : NULL;
    uint16_t *cr_pad_sub = (is_420)   ? (uint16_t*)calloc(cb_samples, sizeof(uint16_t)) : NULL;
    uint16_t *cb_pad     = (is_gray)  ? NULL : (uint16_t*)calloc(y_samples, sizeof(uint16_t));
    uint16_t *cr_pad     = (is_gray)  ? NULL : (uint16_t*)calloc(y_samples, sizeof(uint16_t));

    out->y_plane16 = (uint16_t*)calloc((size_t)W * H, sizeof(uint16_t));
    if (!is_gray) {
        out->cb_plane16 = (uint16_t*)calloc((size_t)W * H, sizeof(uint16_t));
        out->cr_plane16 = (uint16_t*)calloc((size_t)W * H, sizeof(uint16_t));
        if (is_420) {
            out->cb_plane16_420 = (uint16_t*)calloc((size_t)(W >> 1) * (H >> 1), sizeof(uint16_t));
            out->cr_plane16_420 = (uint16_t*)calloc((size_t)(W >> 1) * (H >> 1), sizeof(uint16_t));
        }
    }
    int alloc_ok = y_pad && out->y_plane16 &&
                   (is_gray ||
                    (cb_pad && cr_pad && out->cb_plane16 && out->cr_plane16 &&
                     (!is_420 || (cb_pad_sub && cr_pad_sub &&
                                  out->cb_plane16_420 && out->cr_plane16_420))));
    if (!alloc_ok) {
        free(y_pad); free(cb_pad_sub); free(cr_pad_sub); free(cb_pad); free(cr_pad);
        out->err = (uint32_t)JPEG_ERR_INTERNAL;
        return -1;
    }

    info->components[0].dc_pred = 0;
    if (!is_gray) {
        info->components[1].dc_pred = 0;
        info->components[2].dc_pred = 0;
    }

    const htable_t *y_dc  = &info->htables_dc[info->components[0].td];
    const htable_t *y_ac  = &info->htables_ac[info->components[0].ta];
    const htable_t *cb_dc = is_gray ? NULL : &info->htables_dc[info->components[1].td];
    const htable_t *cb_ac = is_gray ? NULL : &info->htables_ac[info->components[1].ta];
    const htable_t *cr_dc = is_gray ? NULL : &info->htables_dc[info->components[2].td];
    const htable_t *cr_ac = is_gray ? NULL : &info->htables_ac[info->components[2].ta];
    const uint16_t *y_qt  = info->qtables[info->components[0].qt_id].q;
    const uint16_t *cb_qt = is_gray ? NULL : info->qtables[info->components[1].qt_id].q;
    const uint16_t *cr_qt = is_gray ? NULL : info->qtables[info->components[2].qt_id].q;

    int16_t coef[64];
    int32_t dq[64];
    uint16_t y_blk[4][64];
    uint16_t cb_blk[64];
    uint16_t cr_blk[64];

    uint16_t restart_cnt = 0;

    for (uint16_t my = 0; my < info->mcu_rows; my++) {
        for (uint16_t mx = 0; mx < info->mcu_cols; mx++) {
            if (is_gray) {
                if (huff_decode_block(bs, y_dc, y_ac,
                                      &info->components[0].dc_pred, coef)) {
                    out->err = JPEG_ERR_BAD_HUFFMAN;
                    goto fail;
                }
                dequant_block_i32(coef, y_qt, dq);
                idct_islow_p12(dq, y_blk[0]);
                uint16_t *y_dst = y_pad + (size_t)(my * 8) * Wp + (mx * 8);
                copy_block_8x8_u16(y_blk[0], y_dst, Wp);
            } else if (is_444) {
                if (huff_decode_block(bs, y_dc, y_ac,
                                      &info->components[0].dc_pred, coef)) {
                    out->err = JPEG_ERR_BAD_HUFFMAN; goto fail;
                }
                dequant_block_i32(coef, y_qt, dq);
                idct_islow_p12(dq, y_blk[0]);

                if (huff_decode_block(bs, cb_dc, cb_ac,
                                      &info->components[1].dc_pred, coef)) {
                    out->err = JPEG_ERR_BAD_HUFFMAN; goto fail;
                }
                dequant_block_i32(coef, cb_qt, dq);
                idct_islow_p12(dq, cb_blk);

                if (huff_decode_block(bs, cr_dc, cr_ac,
                                      &info->components[2].dc_pred, coef)) {
                    out->err = JPEG_ERR_BAD_HUFFMAN; goto fail;
                }
                dequant_block_i32(coef, cr_qt, dq);
                idct_islow_p12(dq, cr_blk);

                uint16_t *y_dst  = y_pad  + (size_t)(my * 8) * Wp + (mx * 8);
                uint16_t *cb_dst = cb_pad + (size_t)(my * 8) * Wp + (mx * 8);
                uint16_t *cr_dst = cr_pad + (size_t)(my * 8) * Wp + (mx * 8);
                copy_block_8x8_u16(y_blk[0], y_dst,  Wp);
                copy_block_8x8_u16(cb_blk,   cb_dst, Wp);
                copy_block_8x8_u16(cr_blk,   cr_dst, Wp);
            } else {  /* is_420 */
                for (int i = 0; i < 4; i++) {
                    if (huff_decode_block(bs, y_dc, y_ac,
                                          &info->components[0].dc_pred, coef)) {
                        out->err = JPEG_ERR_BAD_HUFFMAN; goto fail;
                    }
                    dequant_block_i32(coef, y_qt, dq);
                    idct_islow_p12(dq, y_blk[i]);
                }
                if (huff_decode_block(bs, cb_dc, cb_ac,
                                      &info->components[1].dc_pred, coef)) {
                    out->err = JPEG_ERR_BAD_HUFFMAN; goto fail;
                }
                dequant_block_i32(coef, cb_qt, dq);
                idct_islow_p12(dq, cb_blk);

                if (huff_decode_block(bs, cr_dc, cr_ac,
                                      &info->components[2].dc_pred, coef)) {
                    out->err = JPEG_ERR_BAD_HUFFMAN; goto fail;
                }
                dequant_block_i32(coef, cr_qt, dq);
                idct_islow_p12(dq, cr_blk);

                uint16_t *y_dst = y_pad + (size_t)(my * 16) * Wp + (mx * 16);
                copy_block_16x16_y_u16(y_blk, y_dst, Wp);

                uint16_t *cb_dst = cb_pad_sub + (size_t)(my * 8) * CWp_sub + (mx * 8);
                uint16_t *cr_dst = cr_pad_sub + (size_t)(my * 8) * CWp_sub + (mx * 8);
                copy_block_8x8_u16(cb_blk, cb_dst, CWp_sub);
                copy_block_8x8_u16(cr_blk, cr_dst, CWp_sub);
            }

            if (info->dri != 0) {
                restart_cnt++;
                int is_last = (my == info->mcu_rows - 1) && (mx == info->mcu_cols - 1);
                if (restart_cnt == info->dri && !is_last) {
                    bs_align_to_byte(bs);
                    if (!bs->marker_pending) {
                        uint8_t bb;
                        if (bs_read_byte(bs, &bb) || bb != 0xFF) {
                            out->err = JPEG_ERR_BAD_MARKER; goto fail;
                        }
                        while (bs_read_byte(bs, &bb) == 0 && bb == 0xFF) {}
                        if (bb < MARKER_RST0 || bb > MARKER_RST7) {
                            out->err = JPEG_ERR_BAD_MARKER; goto fail;
                        }
                    } else {
                        if (bs->last_marker < MARKER_RST0 ||
                            bs->last_marker > MARKER_RST7) {
                            out->err = JPEG_ERR_BAD_MARKER; goto fail;
                        }
                        bs->marker_pending = 0;
                        bs->last_marker    = 0;
                    }
                    info->components[0].dc_pred = 0;
                    if (!is_gray) {
                        info->components[1].dc_pred = 0;
                        info->components[2].dc_pred = 0;
                    }
                    restart_cnt = 0;
                }
            }
        }
    }

    if (is_420) {
        chroma_upsample_nn_u16(cb_pad_sub, cb_pad, Wp, Hp);
        chroma_upsample_nn_u16(cr_pad_sub, cr_pad, Wp, Hp);
    }

    /* Crop to actual W × H (uint16_t) */
    for (uint16_t r = 0; r < H; r++) {
        memcpy(out->y_plane16 + (size_t)r * W,
               y_pad + (size_t)r * Wp, (size_t)W * sizeof(uint16_t));
        if (!is_gray) {
            memcpy(out->cb_plane16 + (size_t)r * W,
                   cb_pad + (size_t)r * Wp, (size_t)W * sizeof(uint16_t));
            memcpy(out->cr_plane16 + (size_t)r * W,
                   cr_pad + (size_t)r * Wp, (size_t)W * sizeof(uint16_t));
        }
    }
    if (is_420) {
        for (uint16_t r = 0; r < (H >> 1); r++) {
            memcpy(out->cb_plane16_420 + (size_t)r * (W >> 1),
                   cb_pad_sub + (size_t)r * CWp_sub, (size_t)(W >> 1) * sizeof(uint16_t));
            memcpy(out->cr_plane16_420 + (size_t)r * (W >> 1),
                   cr_pad_sub + (size_t)r * CWp_sub, (size_t)(W >> 1) * sizeof(uint16_t));
        }
    }
    free(y_pad); free(cb_pad_sub); free(cr_pad_sub); free(cb_pad); free(cr_pad);

    /* Consume EOI */
    bs_align_to_byte(bs);
    uint8_t b;
    int saw_eoi = 0;
    while (bs_read_byte(bs, &b) == 0) {
        if (b == 0xFF) {
            while (bs_read_byte(bs, &b) == 0 && b == 0xFF) {}
            if (b == MARKER_EOI) { saw_eoi = 1; break; }
        }
    }
    if (!saw_eoi && bs->marker_pending && bs->last_marker == MARKER_EOI) {
        saw_eoi = 1;
    }
    if (!saw_eoi) {
        out->err = JPEG_ERR_STREAM_TRUNC;
        return -1;
    }

    out->err = 0;
    return 0;

fail:
    free(y_pad); free(cb_pad_sub); free(cr_pad_sub); free(cb_pad); free(cr_pad);
    return -1;
}

/* ========================================================================= */
/* Phase 17a: progressive SOF2 decoder (gray / 4:4:4 / 4:2:0, Ah=0 only)     */
/* ========================================================================= */

/* Per-component block-grid dimensions (in blocks).
 *
 * blk_rows/blk_cols are the MCU-padded extents used as the stride for
 * coef_buf indexing. Interleaved DC scans walk MCU-major and fill every
 * MCU block (including those beyond natural extent).
 *
 * nat_rows/nat_cols are the "natural" component block extents per ISO
 * 10918-1 A.2.3: ceil(Xi/8) × ceil(Yi/8). Non-interleaved AC scans traverse
 * only the natural extent; the edge MCU-pad blocks (DC-only) keep zero AC.
 */
typedef struct {
    uint32_t blk_rows;
    uint32_t blk_cols;
    uint32_t nat_rows;
    uint32_t nat_cols;
    uint32_t base;
} comp_grid_t;

static int pdbg_enabled(void) {
    static int v = -1;
    if (v < 0) v = (getenv("PROG_DBG") != NULL) ? 1 : 0;
    return v;
}

/* Phase 17c/18c: consume a RSTn marker at a progressive-scan restart boundary.
 *
 * Mirrors the baseline (SOF0/SOF1) restart sequence at decoder.c lines 469–508
 * and the P12 path at lines 811–838. Byte-align the bitstream, accept either a
 * still-in-the-stream 0xFF..RSTn pair or a marker the bitstream layer already
 * latched into bs->last_marker, then reset the scan's prediction state:
 *   • DC preds (both first-scan and refine run 1 bit/block → no cross-block
 *     state, but libjpeg resets them anyway; we follow.)
 *   • EOB run (AC scans only — EOBn spans blocks, must zero at every restart).
 * Returns 0 on success, -1 on malformed marker.
 */
static int prog_handle_restart(bitstream_t *bs, jpeg_info_t *info,
                               int num_comps, uint32_t *eob_run) {
    bs_align_to_byte(bs);
    if (!bs->marker_pending) {
        uint8_t b;
        if (bs_read_byte(bs, &b) || b != 0xFF) return -1;
        while (bs_read_byte(bs, &b) == 0 && b == 0xFF) {}
        if (b < MARKER_RST0 || b > MARKER_RST7) return -1;
    } else {
        if (bs->last_marker < MARKER_RST0 ||
            bs->last_marker > MARKER_RST7) return -1;
        bs->marker_pending = 0;
        bs->last_marker    = 0;
    }
    for (int c = 0; c < num_comps; c++) info->components[c].dc_pred = 0;
    if (eob_run) *eob_run = 0;
    return 0;
}

static int decode_progressive(bitstream_t *bs, jpeg_info_t *info,
                              jpeg_decoded_t *out) {
    int dbg = pdbg_enabled();
    int is_gray = (info->chroma_mode == CHROMA_GRAY);
    int is_444  = (info->chroma_mode == CHROMA_444);
    int is_420  = (info->chroma_mode == CHROMA_420);
    if (!is_gray && !is_444 && !is_420) {
        /* Phase 17a: scope limited to gray / 4:4:4 / 4:2:0 */
        out->err = JPEG_ERR_UNSUP_CHROMA;
        return -1;
    }
    if (info->precision != 8) {
        /* SOF2 + P=12 out of scope */
        out->err = JPEG_ERR_UNSUP_PREC;
        return -1;
    }

    uint16_t W  = info->width;
    uint16_t H  = info->height;
    uint16_t mcu_w = is_420 ? 16 : 8;
    uint16_t mcu_h = is_420 ? 16 : 8;
    uint16_t Wp = info->mcu_cols * mcu_w;
    uint16_t Hp = info->mcu_rows * mcu_h;
    uint16_t CWp_sub = is_420 ? (Wp >> 1) : Wp;
    uint16_t CHp_sub = is_420 ? (Hp >> 1) : Hp;

    out->width  = W;
    out->height = H;
    out->precision = info->precision;

    int num_comps = is_gray ? 1 : 3;

    /* Block grid per component.
     *
     * MCU-padded dims (blk_rows/blk_cols) serve as coef_buf stride. Natural
     * dims (nat_rows/nat_cols) per ISO 10918-1 A.2.3 drive non-interleaved
     * AC scan traversal:
     *   x_i = ceil(X * Hi / Hmax), y_i = ceil(Y * Vi / Vmax)
     *   natural blocks = ceil(x_i/8) × ceil(y_i/8)
     * For gray and 4:4:4 natural == MCU-padded. For 4:2:0 the Y component
     * can differ when image width/height is not a multiple of 16.
     */
    uint32_t yw_nat = (uint32_t)((W + 7u) / 8u);
    uint32_t yh_nat = (uint32_t)((H + 7u) / 8u);
    comp_grid_t cg[3] = {0};
    uint32_t total_blocks = 0;
    if (is_gray) {
        cg[0].blk_rows = info->mcu_rows;
        cg[0].blk_cols = info->mcu_cols;
        cg[0].nat_rows = yh_nat;
        cg[0].nat_cols = yw_nat;
    } else if (is_444) {
        for (int c = 0; c < 3; c++) {
            cg[c].blk_rows = info->mcu_rows;
            cg[c].blk_cols = info->mcu_cols;
            cg[c].nat_rows = yh_nat;
            cg[c].nat_cols = yw_nat;
        }
    } else { /* 4:2:0 */
        uint32_t cw = (uint32_t)((W + 1u) / 2u);
        uint32_t ch = (uint32_t)((H + 1u) / 2u);
        uint32_t cw_nat = (cw + 7u) / 8u;
        uint32_t ch_nat = (ch + 7u) / 8u;
        cg[0].blk_rows = info->mcu_rows * 2u;
        cg[0].blk_cols = info->mcu_cols * 2u;
        cg[0].nat_rows = yh_nat;
        cg[0].nat_cols = yw_nat;
        for (int c = 1; c < 3; c++) {
            cg[c].blk_rows = info->mcu_rows;
            cg[c].blk_cols = info->mcu_cols;
            cg[c].nat_rows = ch_nat;
            cg[c].nat_cols = cw_nat;
        }
    }
    for (int c = 0; c < num_comps; c++) {
        cg[c].base = total_blocks;
        total_blocks += cg[c].blk_rows * cg[c].blk_cols;
    }

    int16_t (*coef_buf)[64] = (int16_t(*)[64])calloc(total_blocks, 64 * sizeof(int16_t));
    if (!coef_buf) { out->err = (uint32_t)JPEG_ERR_INTERNAL; return -1; }

    /* Zero DC predictors for first DC scan. */
    for (int c = 0; c < num_comps; c++) info->components[c].dc_pred = 0;

    /* -------------------------------------------------------------------- */
    /* Scan loop                                                            */
    /* -------------------------------------------------------------------- */

    int saw_eoi = 0;
    int scan_no = 0;
    for (;;) {
        int is_dc = (info->scan_ss == 0 && info->scan_se == 0);
        int is_ac = (info->scan_ss >= 1 && info->scan_se >= info->scan_ss &&
                     info->scan_se <= 63);
        if (!is_dc && !is_ac) {
            out->err = JPEG_ERR_UNSUP_SOF;
            goto fail;
        }
        uint8_t al = info->scan_al;
        int is_refine = (info->scan_ah != 0);
        if (dbg) fprintf(stderr,
            "[prog] scan=%d Ss=%u Se=%u Ah=%u Al=%u Ns=%u bytepos=%zu bitcnt=%d\n",
            scan_no, info->scan_ss, info->scan_se, info->scan_ah,
            info->scan_al, info->scan_num_comps, bs->byte_pos, bs->bit_cnt);
        scan_no++;

        if (is_dc) {
            /* Interleaved DC scan (Ns = num_comps). Walk MCU-major.
             * First scan: DC preds reset, coef[0] = dc<<Al.
             * Refinement (Ah>0): per block, 1 bit OR'd into coef[0] at pos Al.
             *
             * Phase 17c/18c: DRI — every info->dri MCUs insert a RSTn marker,
             * reset DC preds. No RST after the final MCU (EOI follows). */
            if (!is_refine) {
                for (int c = 0; c < num_comps; c++) info->components[c].dc_pred = 0;
            }
            uint32_t restart_cnt = 0;
            for (uint32_t my = 0; my < info->mcu_rows; my++) {
                for (uint32_t mx = 0; mx < info->mcu_cols; mx++) {
                    if (is_gray) {
                        uint32_t blk = cg[0].base + my * cg[0].blk_cols + mx;
                        if (is_refine) {
                            if (huff_decode_dc_refine(bs, coef_buf[blk], al)) {
                                out->err = JPEG_ERR_BAD_HUFFMAN; goto fail;
                            }
                        } else {
                            const htable_t *dc_tab =
                                &info->htables_dc[info->components[0].td];
                            if (huff_decode_dc_progressive(
                                    bs, dc_tab,
                                    &info->components[0].dc_pred,
                                    coef_buf[blk], al)) {
                                out->err = JPEG_ERR_BAD_HUFFMAN; goto fail;
                            }
                        }
                    } else if (is_444) {
                        for (int c = 0; c < 3; c++) {
                            uint32_t blk = cg[c].base + my * cg[c].blk_cols + mx;
                            if (is_refine) {
                                if (huff_decode_dc_refine(bs, coef_buf[blk], al)) {
                                    out->err = JPEG_ERR_BAD_HUFFMAN; goto fail;
                                }
                            } else {
                                const htable_t *dc_tab =
                                    &info->htables_dc[info->components[c].td];
                                if (huff_decode_dc_progressive(
                                        bs, dc_tab,
                                        &info->components[c].dc_pred,
                                        coef_buf[blk], al)) {
                                    out->err = JPEG_ERR_BAD_HUFFMAN; goto fail;
                                }
                            }
                        }
                    } else { /* 4:2:0 */
                        uint32_t y_by0 = my * 2u, y_bx0 = mx * 2u;
                        for (int iy = 0; iy < 2; iy++) {
                            for (int ix = 0; ix < 2; ix++) {
                                uint32_t blk = cg[0].base +
                                    (y_by0 + (uint32_t)iy) * cg[0].blk_cols +
                                    (y_bx0 + (uint32_t)ix);
                                if (is_refine) {
                                    if (huff_decode_dc_refine(bs, coef_buf[blk], al)) {
                                        out->err = JPEG_ERR_BAD_HUFFMAN; goto fail;
                                    }
                                } else {
                                    const htable_t *dc_tab =
                                        &info->htables_dc[info->components[0].td];
                                    if (huff_decode_dc_progressive(
                                            bs, dc_tab,
                                            &info->components[0].dc_pred,
                                            coef_buf[blk], al)) {
                                        out->err = JPEG_ERR_BAD_HUFFMAN; goto fail;
                                    }
                                }
                            }
                        }
                        for (int c = 1; c < 3; c++) {
                            uint32_t blk = cg[c].base + my * cg[c].blk_cols + mx;
                            if (is_refine) {
                                if (huff_decode_dc_refine(bs, coef_buf[blk], al)) {
                                    out->err = JPEG_ERR_BAD_HUFFMAN; goto fail;
                                }
                            } else {
                                const htable_t *dc_tab =
                                    &info->htables_dc[info->components[c].td];
                                if (huff_decode_dc_progressive(
                                        bs, dc_tab,
                                        &info->components[c].dc_pred,
                                        coef_buf[blk], al)) {
                                    out->err = JPEG_ERR_BAD_HUFFMAN; goto fail;
                                }
                            }
                        }
                    }
                    if (info->dri != 0) {
                        restart_cnt++;
                        int is_last = (my == info->mcu_rows - 1u) &&
                                      (mx == info->mcu_cols - 1u);
                        if (restart_cnt == info->dri && !is_last) {
                            if (prog_handle_restart(bs, info, num_comps, NULL)) {
                                out->err = JPEG_ERR_BAD_MARKER; goto fail;
                            }
                            restart_cnt = 0;
                        }
                    }
                }
            }
        } else {
            /* AC scan: must be non-interleaved (Ns=1). The SOS parser stored
             * the scan's tables into components[?].ta — we need to find which
             * component this scan addresses.  ISO G.1.1.1 requires Ns=1 for
             * AC scans; the current parse_sos still enforces Ns == num_components.
             * So for Phase 17a, libjpeg-turbo's default progressive script
             * actually starts with an interleaved DC scan and then single-comp
             * AC scans whose SOS has Ns=1.  Loosen parse_sos to permit Ns=1
             * for SOF2 is a prerequisite — see header_parser patch.
             *
             * We identify the component by the one whose td/ta was updated
             * during parse_sos: parse_sos writes td=tdta>>4, ta=tdta&0xF for
             * the component(s) listed in this scan's SOS.  The AC table index
             * "ta" carries the per-scan setting, while the other components'
             * ta still reflects their previous scan's value — so we cannot
             * disambiguate reliably that way.  Simplest solution: extend
             * parse_sos to record the scan's component list in info. */
            if (info->scan_num_comps != 1) {
                out->err = JPEG_ERR_UNSUP_SOF; goto fail;
            }
            int scan_comp = info->scan_comp_idx[0];
            const htable_t *ac_tab =
                &info->htables_ac[info->components[scan_comp].ta];
            uint32_t eob_run = 0;
            uint32_t nat_rows = cg[scan_comp].nat_rows;
            uint32_t nat_cols = cg[scan_comp].nat_cols;
            uint32_t stride   = cg[scan_comp].blk_cols;
            uint32_t n_blk    = nat_rows * nat_cols;
            uint32_t b = 0;
            /* Phase 17c/18c: for non-interleaved scans the ISO "MCU" is a
             * single data unit (one 8×8 block), so restart interval applies
             * per block. No RST after the final block. */
            uint32_t restart_cnt = 0;
            for (uint32_t by = 0; by < nat_rows; by++) {
                for (uint32_t bx = 0; bx < nat_cols; bx++) {
                    uint32_t blk = cg[scan_comp].base + by * stride + bx;
                    int rc;
                    if (is_refine) {
                        rc = huff_decode_ac_refine(
                                bs, ac_tab, coef_buf[blk],
                                info->scan_ss, info->scan_se,
                                al, &eob_run);
                    } else {
                        rc = huff_decode_ac_progressive(
                                bs, ac_tab, coef_buf[blk],
                                info->scan_ss, info->scan_se,
                                al, &eob_run);
                    }
                    if (rc) {
                        if (dbg) fprintf(stderr,
                            "[prog] AC%s FAIL comp=%d b=%u/%u (by=%u bx=%u) eob_run=%u bytepos=%zu bitcnt=%d\n",
                            is_refine ? "_refine" : "",
                            scan_comp, b, n_blk, by, bx,
                            eob_run, bs->byte_pos, bs->bit_cnt);
                        out->err = JPEG_ERR_BAD_HUFFMAN; goto fail;
                    }
                    b++;
                    if (info->dri != 0) {
                        restart_cnt++;
                        int is_last_blk = (by == nat_rows - 1u) &&
                                          (bx == nat_cols - 1u);
                        if (restart_cnt == info->dri && !is_last_blk) {
                            if (prog_handle_restart(bs, info, num_comps, &eob_run)) {
                                out->err = JPEG_ERR_BAD_MARKER; goto fail;
                            }
                            restart_cnt = 0;
                        }
                    }
                }
            }
            if (dbg) fprintf(stderr,
                "[prog] AC%s OK comp=%d n_blk=%u final_eob_run=%u bytepos=%zu bitcnt=%d\n",
                is_refine ? "_refine" : "",
                scan_comp, n_blk, eob_run, bs->byte_pos, bs->bit_cnt);
        }

        /* End of scan. Peek next marker. */
        uint32_t e = 0;
        int r = jpeg_parse_between_scans(bs, info, &e);
        if (r < 0) { out->err = e ? e : (uint32_t)JPEG_ERR_BAD_MARKER; goto fail; }
        if (r == 1) { saw_eoi = 1; break; }
        /* r == 0: another SOS, loop */
    }

    if (!saw_eoi) { out->err = JPEG_ERR_STREAM_TRUNC; goto fail; }

    /* -------------------------------------------------------------------- */
    /* Drain: dequant + IDCT per block, place into pad planes              */
    /* -------------------------------------------------------------------- */

    uint8_t *y_pad      = (uint8_t*)calloc((size_t)Wp * Hp, 1);
    uint8_t *cb_pad_sub = is_420 ? (uint8_t*)calloc((size_t)CWp_sub * CHp_sub, 1) : NULL;
    uint8_t *cr_pad_sub = is_420 ? (uint8_t*)calloc((size_t)CWp_sub * CHp_sub, 1) : NULL;
    uint8_t *cb_pad     = is_gray ? NULL : (uint8_t*)calloc((size_t)Wp * Hp, 1);
    uint8_t *cr_pad     = is_gray ? NULL : (uint8_t*)calloc((size_t)Wp * Hp, 1);

    out->y_plane = (uint8_t*)calloc((size_t)W * H, 1);
    if (!is_gray) {
        out->cb_plane = (uint8_t*)calloc((size_t)W * H, 1);
        out->cr_plane = (uint8_t*)calloc((size_t)W * H, 1);
        if (is_420) {
            out->cb_plane_420 = (uint8_t*)calloc((size_t)(W >> 1) * (H >> 1), 1);
            out->cr_plane_420 = (uint8_t*)calloc((size_t)(W >> 1) * (H >> 1), 1);
        }
    }
    int alloc_ok = y_pad && out->y_plane &&
                   (is_gray ||
                    (cb_pad && cr_pad && out->cb_plane && out->cr_plane &&
                     (!is_420 || (cb_pad_sub && cr_pad_sub &&
                                  out->cb_plane_420 && out->cr_plane_420))));
    if (!alloc_ok) {
        free(y_pad); free(cb_pad_sub); free(cr_pad_sub); free(cb_pad); free(cr_pad);
        out->err = (uint32_t)JPEG_ERR_INTERNAL; goto fail;
    }

    uint8_t blk_out[64];
    for (int c = 0; c < num_comps; c++) {
        const uint16_t *qt = info->qtables[info->components[c].qt_id].q;
        uint32_t blk_rows = cg[c].blk_rows;
        uint32_t blk_cols = cg[c].blk_cols;
        uint32_t base = cg[c].base;
        uint8_t *pad;
        uint16_t pad_stride;
        if (c == 0) {
            pad = y_pad;
            pad_stride = Wp;
        } else if (is_420) {
            pad = (c == 1) ? cb_pad_sub : cr_pad_sub;
            pad_stride = CWp_sub;
        } else { /* 4:4:4 */
            pad = (c == 1) ? cb_pad : cr_pad;
            pad_stride = Wp;
        }

        for (uint32_t by = 0; by < blk_rows; by++) {
            for (uint32_t bx = 0; bx < blk_cols; bx++) {
                uint32_t blk = base + by * blk_cols + bx;
                dequant_block(coef_buf[blk], qt);
                idct_islow(coef_buf[blk], blk_out);
                uint8_t *dst = pad + ((size_t)by * 8u) * pad_stride + (bx * 8u);
                copy_block_8x8(blk_out, dst, pad_stride);
            }
        }
    }

    free(coef_buf);

    /* Chroma upsample (4:2:0 NN, same as baseline) */
    if (is_420) {
        chroma_upsample_nn(cb_pad_sub, cb_pad, Wp, Hp);
        chroma_upsample_nn(cr_pad_sub, cr_pad, Wp, Hp);
    }

    /* Crop padded → output planes */
    for (uint16_t r = 0; r < H; r++) {
        memcpy(out->y_plane + (size_t)r * W, y_pad + (size_t)r * Wp, W);
        if (!is_gray) {
            memcpy(out->cb_plane + (size_t)r * W, cb_pad + (size_t)r * Wp, W);
            memcpy(out->cr_plane + (size_t)r * W, cr_pad + (size_t)r * Wp, W);
        }
    }
    if (is_420) {
        for (uint16_t r = 0; r < (H >> 1); r++) {
            memcpy(out->cb_plane_420 + (size_t)r * (W >> 1),
                   cb_pad_sub + (size_t)r * CWp_sub, (W >> 1));
            memcpy(out->cr_plane_420 + (size_t)r * (W >> 1),
                   cr_pad_sub + (size_t)r * CWp_sub, (W >> 1));
        }
    }

    free(y_pad); free(cb_pad_sub); free(cr_pad_sub); free(cb_pad); free(cr_pad);

    out->err = 0;
    return 0;

fail:
    free(coef_buf);
    return -1;
}

/* ========================================================================== */
/* Phase 25a/25b: Lossless (SOF3) decode — predictor + Huffman, no DCT        */
/* ========================================================================== */
/*
 * Scope (Phase 25a + 25b):
 *   - Nf ∈ {1, 3}   (Phase 25a: gray; Phase 25b: Nf=3 interleaved RGB)
 *   - P=8            (Phase 27 extends to 2..16)
 *   - Pt ∈ {0..15}   (point transform: output = reconstructed << Pt)
 *   - Ps ∈ {1..7}    (predictors P1..P7 per ISO H.1.2.1)
 *   - All components Hi = Vi = 1
 *   - Ns = num_components (fully interleaved) or Ns = 1 (non-interleaved only
 *     makes sense for gray — for 3-component, we reject non-interleaved).
 *   - DRI = 0        (Phase 25c adds RSTn)
 *
 * Prediction rules (ISO H.1.2.1). Each component tracks its own predictor
 * state (y_plane/cb_plane/cr_plane for R/G/B):
 *   - First sample of the scan: Px = 2^(P-Pt-1)
 *   - First sample of a new row (x=0, y>0): Px = Rb
 *   - First row, subsequent samples (y=0, x>0): Px = Ra
 *   - Otherwise: Ps-indexed predictor
 *       Ps=1: Ra           Ps=2: Rb           Ps=3: Rc
 *       Ps=4: Ra+Rb-Rc
 *       Ps=5: Ra + (Rb-Rc)>>1
 *       Ps=6: Rb + (Ra-Rc)>>1
 *       Ps=7: (Ra+Rb)>>1
 *
 * Reconstruction (per component):
 *   diff = huff_decode_lossless_diff(dc_tab[td])
 *   sample_p = (Px + diff) & ((1 << P) - 1)       (modulo 2^P wraparound)
 *   output   = (uint8_t)(sample_p << Pt)
 *
 * Scan raster ordering (interleaved, ISO A.2.3 + H.1.2):
 *   for y:
 *     for x:
 *       for c = 0..Ns-1: decode one sample of component scan_comp_idx[c]
 *
 * Output:
 *   Nf=1 gray → out->y_plane
 *   Nf=3 RGB  → out->y_plane, cb_plane, cr_plane (R, G, B respectively),
 *               is_rgb_lossless = 1 to let consumers skip YCbCr conversion.
 */
static int decode_lossless(bitstream_t *bs, jpeg_info_t *info,
                           jpeg_decoded_t *out) {
    /* ---- precision / DRI / scope guards ---- */
    if (info->precision != 8) {
        /* Phase 27 extends to 2..16. */
        out->err = JPEG_ERR_UNSUP_PREC;
        return -1;
    }
    if (info->dri != 0) {
        /* Phase 25c will add RSTn handling. */
        out->err = JPEG_ERR_UNSUP_SOF;
        return -1;
    }

    /* Nf=3 RGB must use the 4:4:4-shaped component layout. */
    int is_gray = (info->num_components == 1);
    int is_rgb3 = (info->num_components == 3 &&
                   info->chroma_mode == CHROMA_444);
    if (!is_gray && !is_rgb3) {
        /* Phase 25b scope: Nf∈{1,3} with all Hi=Vi=1. Nf=2/4 → future. */
        out->err = JPEG_ERR_UNSUP_CHROMA;
        return -1;
    }

    uint16_t W = info->width;
    uint16_t H = info->height;
    uint8_t  Ps = info->scan_ss;         /* predictor selection */
    uint8_t  Pt = info->scan_al;         /* point transform */
    uint8_t  P  = info->precision;       /* 8 */

    if (Ps < 1 || Ps > 7) {
        out->err = JPEG_ERR_UNSUP_SOF;
        return -1;
    }
    if (Pt > 15) {
        out->err = JPEG_ERR_UNSUP_SOF;
        return -1;
    }

    /* For Nf=3 we require a fully interleaved scan (all 3 comps). Nf=1
     * naturally has Ns=1. Non-interleaved multi-component (one scan per
     * component) is legal ISO but not produced by cjpeg and deferred. */
    int Nf = info->num_components;
    int Ns = info->scan_num_comps;
    if (Nf == 3 && Ns != 3) {
        out->err = JPEG_ERR_UNSUP_SOF;
        return -1;
    }
    if (Nf == 1 && Ns != 1) {
        out->err = JPEG_ERR_UNSUP_SOF;
        return -1;
    }

    out->width     = W;
    out->height    = H;
    out->precision = P;
    out->is_rgb_lossless = (Nf == 3) ? 1 : 0;

    /* Output planes (W×H uint8 per component). For 3-comp: R/G/B. */
    size_t npix = (size_t)W * H;
    uint8_t *out_planes[3] = { NULL, NULL, NULL };
    out_planes[0] = (uint8_t*)calloc(npix, 1);
    if (!out_planes[0]) { out->err = (uint32_t)JPEG_ERR_INTERNAL; return -1; }
    out->y_plane = out_planes[0];
    if (Nf == 3) {
        out_planes[1] = (uint8_t*)calloc(npix, 1);
        out_planes[2] = (uint8_t*)calloc(npix, 1);
        if (!out_planes[1] || !out_planes[2]) {
            free(out_planes[1]); free(out_planes[2]);
            out->err = (uint32_t)JPEG_ERR_INTERNAL;
            return -1;
        }
        out->cb_plane = out_planes[1];  /* G */
        out->cr_plane = out_planes[2];  /* B */
    }

    /* Phase 25b: internal predictor-domain buffer(s). Predictor neighbors
     * Ra/Rb/Rc are compared in the domain of the decoded samples pre-Pt-shift
     * (ISO H.1.2). For Pt==0 internal domain == output domain; for Pt>0 the
     * internal domain is P-Pt bits wide so ≤ 8 bits for P=8. We can reuse the
     * out_planes buffer for Pt==0 and allocate separate uint8 scratch only
     * when Pt>0. */
    uint8_t *int_planes[3];
    uint8_t *int_scratch[3] = { NULL, NULL, NULL };
    if (Pt == 0) {
        for (int c = 0; c < Nf; c++) int_planes[c] = out_planes[c];
    } else {
        for (int c = 0; c < Nf; c++) {
            int_scratch[c] = (uint8_t*)calloc(npix, 1);
            if (!int_scratch[c]) {
                for (int k = 0; k < Nf; k++) free(int_scratch[k]);
                out->err = (uint32_t)JPEG_ERR_INTERNAL;
                return -1;
            }
            int_planes[c] = int_scratch[c];
        }
    }

    /* DC Huffman tables — each component has its own Td index (set by
     * parse_sos). */
    const htable_t *dc_tabs[3] = { NULL, NULL, NULL };
    for (int c = 0; c < Nf; c++) {
        uint8_t idx = info->components[c].td;
        dc_tabs[c] = &info->htables_dc[idx];
    }

    /* Wrap mask and initial predictor live in the predictor domain (P-Pt bits).
     * For Pt=0 this collapses to the sample domain. */
    int32_t mask = (int32_t)(((uint32_t)1u << (P - Pt)) - 1u);   /* 0xFF for P=8/Pt=0 */
    int32_t initial_pred = (int32_t)1 << (P - Pt - 1);

    for (uint16_t y = 0; y < H; y++) {
        uint8_t *row_c[3];
        const uint8_t *prev_c[3];
        for (int c = 0; c < Nf; c++) {
            row_c[c]  = int_planes[c] + (size_t)y * W;
            prev_c[c] = (y > 0) ? (int_planes[c] + (size_t)(y - 1) * W) : NULL;
        }
        for (uint16_t x = 0; x < W; x++) {
            /* For each (y, x) decode one sample per component, in scan-comp
             * order. For Nf=3 interleaved scan, that's R, G, B sequentially. */
            for (int si = 0; si < Ns; si++) {
                int c = info->scan_comp_idx[si];
                uint8_t *cur = row_c[c];
                const uint8_t *prev = prev_c[c];

                int32_t Px;
                if (y == 0 && x == 0) {
                    Px = initial_pred;
                } else if (y == 0) {
                    Px = (int32_t)cur[x - 1];              /* Ra */
                } else if (x == 0) {
                    Px = (int32_t)prev[0];                 /* Rb */
                } else {
                    int32_t Ra = (int32_t)cur[x - 1];
                    int32_t Rb = (int32_t)prev[x];
                    int32_t Rc = (int32_t)prev[x - 1];
                    switch (Ps) {
                        case 1: Px = Ra;                      break;
                        case 2: Px = Rb;                      break;
                        case 3: Px = Rc;                      break;
                        case 4: Px = Ra + Rb - Rc;            break;
                        case 5: Px = Ra + ((Rb - Rc) >> 1);   break;
                        case 6: Px = Rb + ((Ra - Rc) >> 1);   break;
                        case 7: Px = (Ra + Rb) >> 1;          break;
                        default: Px = 0;                      break;  /* unreachable */
                    }
                }

                int32_t diff = 0;
                if (huff_decode_lossless_diff(bs, dc_tabs[c], &diff)) {
                    out->err = JPEG_ERR_BAD_HUFFMAN;
                    for (int k = 0; k < Nf; k++) free(int_scratch[k]);
                    return -1;
                }

                /* Predictor-domain sample (P-Pt bits, wrapped). */
                int32_t sample = (Px + diff) & mask;
                cur[x] = (uint8_t)sample;
            }
        }
    }

    /* Post-process: shift-left by Pt to produce output samples. For Pt=0
     * int_planes == out_planes already, skip. */
    if (Pt > 0) {
        for (int c = 0; c < Nf; c++) {
            uint8_t *src = int_planes[c];
            uint8_t *dst = out_planes[c];
            for (size_t i = 0; i < npix; i++) {
                int32_t v = (int32_t)src[i] << Pt;
                if (v > 255) v = 255;
                dst[i] = (uint8_t)v;
            }
            free(int_scratch[c]);
        }
    }

    /* Consume any pending marker up to EOI. */
    bs_align_to_byte(bs);
    uint8_t b;
    int saw_eoi = 0;
    if (bs->marker_pending && bs->last_marker == MARKER_EOI) {
        saw_eoi = 1;
    } else {
        while (bs_read_byte(bs, &b) == 0) {
            if (b == 0xFF) {
                while (bs_read_byte(bs, &b) == 0 && b == 0xFF) {}
                if (b == MARKER_EOI) { saw_eoi = 1; break; }
            }
        }
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
    free(out->cb_plane_422);
    free(out->cr_plane_422);
    free(out->cb_plane_440);
    free(out->cr_plane_440);
    free(out->cb_plane_411);
    free(out->cr_plane_411);
    free(out->c_plane);
    free(out->m_plane);
    free(out->y_plane_cmyk);
    free(out->k_plane);
    free(out->y_plane16);
    free(out->cb_plane16);
    free(out->cr_plane16);
    free(out->cb_plane16_420);
    free(out->cr_plane16_420);
    memset(out, 0, sizeof(*out));
}
