#include "huffman.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static int hdbg_enabled(void) {
    static int v = -1;
    if (v < 0) v = (getenv("HUF_DBG") != NULL) ? 1 : 0;
    return v;
}
static int g_blk_idx = 0;

static const uint8_t ZIGZAG[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

int huff_decode_symbol(bitstream_t *bs, const htable_t *h, uint8_t *symbol) {
    int32_t code = 0;
    for (int l = 1; l <= 16; l++) {
        uint32_t bit;
        if (bs_get_bits_u(bs, 1, &bit)) return -1;
        code = (code << 1) | (int32_t)bit;
        if (code <= h->maxcode[l]) {
            int j = h->valptr[l] + (code - (int32_t)h->mincode[l]);
            if (j < 0 || j >= 256) return -1;
            *symbol = h->huffval[j];
            return 0;
        }
    }
    return -1;
}

int huff_decode_block(bitstream_t *bs,
                      const htable_t *dc_tab,
                      const htable_t *ac_tab,
                      int16_t *dc_pred,
                      int16_t coef[64])
{
    memset(coef, 0, 64 * sizeof(int16_t));
    int dbg = hdbg_enabled();
    int bidx = g_blk_idx++;

    uint8_t size;
    if (huff_decode_symbol(bs, dc_tab, &size)) return -1;
    /* Phase 13: DC category ≤ 15 for P=12 (was ≤ 11 for P=8); s=15 extend
       yields ±32767 which still fits in int16 for DC pred accumulation on
       real-world streams. */
    if (size > 15) return -1;

    int32_t diff = 0;
    if (size > 0) {
        if (bs_get_bits(bs, size, &diff)) return -1;
    }
    *dc_pred += (int16_t)diff;
    coef[0] = *dc_pred;

    if (dbg) fprintf(stderr,
        "[C blk=%d] bytepos=%zu bit_cnt=%d DC size=%u diff=%d dc=%d\n",
        bidx, bs->byte_pos, bs->bit_cnt, size, diff, *dc_pred);
    size_t pre_byte = bs->byte_pos;
    int pre_bc = bs->bit_cnt;

    int k = 1;
    while (k < 64) {
        uint8_t rs;
        if (huff_decode_symbol(bs, ac_tab, &rs)) return -1;
        int r = rs >> 4;
        int s = rs & 0xF;

        if (s == 0) {
            if (r == 15) {
                if (dbg) fprintf(stderr, "[C blk=%d] ZRL k %d -> %d\n", bidx, k, k+16);
                k += 16;
                continue;
            } else {
                if (dbg) fprintf(stderr, "[C blk=%d] EOB at k=%d\n", bidx, k);
                break;
            }
        }

        k += r;
        if (k >= 64) return -1;

        int32_t amp;
        if (bs_get_bits(bs, s, &amp)) return -1;

        if (dbg) fprintf(stderr,
            "[C blk=%d] AC rs=0x%02X r=%d s=%d amp=%d k=%d\n",
            bidx, rs, r, s, amp, k);
        coef[ZIGZAG[k]] = (int16_t)amp;
        k++;
    }

    if (dbg) fprintf(stderr,
        "[C blk=%d] POST_BLK bytepos=%zu bit_cnt=%d (pre=%zu,%d)\n",
        bidx, bs->byte_pos, bs->bit_cnt, pre_byte, pre_bc);
    return 0;
}

int huff_decode_block_dc_only(bitstream_t *bs,
                              const htable_t *dc_tab,
                              int16_t *dc_pred,
                              int16_t coef[64],
                              uint8_t al_shift) {
    memset(coef, 0, 64 * sizeof(int16_t));

    uint8_t size;
    if (huff_decode_symbol(bs, dc_tab, &size)) return -1;
    if (size > 15) return -1;

    int32_t diff = 0;
    if (size > 0) {
        if (bs_get_bits(bs, size, &diff)) return -1;
    }
    *dc_pred += (int16_t)diff;

    /* Point transform: store predictor left-shifted by Al. For Al=0 this is a
     * plain int16 assignment. ISO 10918-1 F.1.4.4.1.2 keeps the predictor in
     * its unshifted form and applies the shift only to the stored coefficient.
     */
    coef[0] = (int16_t)((int32_t)(*dc_pred) << al_shift);
    return 0;
}
