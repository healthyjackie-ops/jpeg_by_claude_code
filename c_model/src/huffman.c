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

/* Phase 17a: DC-only first scan, but does NOT clear coef[1..63]. Used inside
 * decode_progressive() where coef_buf is pre-cleared and subsequent AC scans
 * fill in band-by-band. */
int huff_decode_dc_progressive(bitstream_t *bs,
                               const htable_t *dc_tab,
                               int16_t *dc_pred,
                               int16_t coef[64],
                               uint8_t al_shift) {
    uint8_t size;
    if (huff_decode_symbol(bs, dc_tab, &size)) return -1;
    if (size > 15) return -1;

    int32_t diff = 0;
    if (size > 0) {
        if (bs_get_bits(bs, size, &diff)) return -1;
    }
    *dc_pred += (int16_t)diff;
    coef[0] = (int16_t)((int32_t)(*dc_pred) << al_shift);
    return 0;
}

/* Phase 17a: AC spectral-selection first scan (Ah=0).
 * ISO/IEC 10918-1 G.1.2.2 (Decode_AC_coefficients).
 *
 * eob_run counts how many additional blocks must be skipped before decoding
 * the next symbol. Caller resets to 0 before each scan.
 */
int huff_decode_ac_progressive(bitstream_t *bs,
                               const htable_t *ac_tab,
                               int16_t coef[64],
                               uint8_t ss, uint8_t se,
                               uint8_t al_shift,
                               uint32_t *eob_run) {
    static int trace_enabled = -1;
    if (trace_enabled < 0) trace_enabled = (getenv("AC_TRACE") != NULL) ? 1 : 0;
    if (ss == 0 || ss > se || se > 63) return -1;

    if (*eob_run > 0) {
        (*eob_run)--;
        return 0;
    }

    int k = (int)ss;
    while (k <= (int)se) {
        uint8_t rs;
        if (huff_decode_symbol(bs, ac_tab, &rs)) return -1;
        int r = rs >> 4;
        int s = rs & 0xF;
        if (trace_enabled) fprintf(stderr, "   [ac] k=%d rs=0x%02X r=%d s=%d bp=%zu bc=%d\n",
                                   k, rs, r, s, bs->byte_pos, bs->bit_cnt);

        if (s == 0) {
            if (r < 15) {
                /* EOBn: EOB run of (2^r + extra) blocks starting from this one.
                 * Extra bits are read as unsigned — bs_get_bits would sign-
                 * extend and produce garbage for r>0. */
                uint32_t extra = 0;
                if (r > 0) {
                    if (bs_get_bits_u(bs, r, &extra)) return -1;
                }
                *eob_run = ((uint32_t)1u << r) + extra - 1u;
                if (trace_enabled) fprintf(stderr, "   [ac] EOB r=%d extra=%u eob_run=%u\n",
                                           r, extra, *eob_run);
                break;
            } else {
                /* ZRL: skip 16 zero coefficients */
                k += 16;
                continue;
            }
        }

        k += r;
        if (k > (int)se) return -1;

        int32_t amp;
        if (bs_get_bits(bs, (uint8_t)s, &amp)) return -1;
        coef[ZIGZAG[k]] = (int16_t)((int32_t)amp << al_shift);
        if (trace_enabled) fprintf(stderr, "   [ac] amp=%d store coef[%d]=%d\n",
                                   amp, ZIGZAG[k], coef[ZIGZAG[k]]);
        k++;
    }
    return 0;
}

/* Phase 18a: DC refinement scan (ISO 10918-1 G.1.2.1.1).
 * Each block contributes one bit at position Al appended to coef[0].
 */
int huff_decode_dc_refine(bitstream_t *bs, int16_t coef[64], uint8_t al) {
    uint32_t bit;
    if (bs_get_bits_u(bs, 1, &bit)) return -1;
    if (bit) {
        int16_t mask = (int16_t)((int32_t)1 << al);
        coef[0] = (int16_t)(coef[0] | mask);
    }
    return 0;
}

/* Apply one correction bit to a non-zero refinement-scan coefficient.
 * Matches libjpeg-turbo: the `& p1 == 0` guard is a safety net; for valid
 * progressive streams coef values at scan entry are aligned multiples of
 * (1<<Ah) and thus always pass it.
 */
static inline int rfn_apply_bit(bitstream_t *bs, int16_t *cp,
                                int16_t p1, int16_t m1) {
    uint32_t cb;
    if (bs_get_bits_u(bs, 1, &cb)) return -1;
    if (cb && ((*cp & p1) == 0)) {
        if (*cp >= 0) *cp = (int16_t)(*cp + p1);
        else          *cp = (int16_t)(*cp + m1);
    }
    return 0;
}

/* Phase 18a: AC refinement scan (ISO/IEC 10918-1 G.1.2.3).
 *
 * Mirrors libjpeg-turbo's decode_mcu_AC_refine (jdphuff.c). Rules:
 *   - SSSS must be 0 or 1; refinement amplitude is ±1 only.
 *   - RRRR counts skipped *zero* positions. Non-zero coefs traversed along
 *     the way each receive one correction bit.
 *   - EOBn sets eob_run to 2^RRRR + extra, then the current block still
 *     finishes with correction bits on any remaining non-zeros in [k..se].
 *   - Subsequent in-EOB blocks (eob_run>0 at entry) just apply correction
 *     bits to non-zeros in [ss..se] and decrement eob_run.
 */
int huff_decode_ac_refine(bitstream_t *bs,
                          const htable_t *ac_tab,
                          int16_t coef[64],
                          uint8_t ss, uint8_t se, uint8_t al,
                          uint32_t *eob_run) {
    if (ss == 0 || ss > se || se > 63) return -1;

    int16_t p1 = (int16_t)((int32_t)1 << al);       /* +1 at bit position Al */
    int16_t m1 = (int16_t)(-((int32_t)1 << al));    /* -1 at bit position Al */

    int k = (int)ss;

    /* If entering inside an EOB run, skip straight to correction-bit loop. */
    if (*eob_run > 0) {
        while (k <= (int)se) {
            int16_t *cp = &coef[ZIGZAG[k]];
            if (*cp != 0) {
                if (rfn_apply_bit(bs, cp, p1, m1)) return -1;
            }
            k++;
        }
        (*eob_run)--;
        return 0;
    }

    while (k <= (int)se) {
        uint8_t rs;
        if (huff_decode_symbol(bs, ac_tab, &rs)) return -1;
        int r = rs >> 4;
        int s = rs & 0xF;
        int sval = 0;      /* placed value (0 = ZRL / no new placement) */

        if (s == 0) {
            if (r != 15) {
                uint32_t extra = 0;
                if (r > 0) {
                    if (bs_get_bits_u(bs, (uint8_t)r, &extra)) return -1;
                }
                *eob_run = ((uint32_t)1u << r) + extra;
                /* Finish this block's non-zero refines, then eat one eob_run. */
                while (k <= (int)se) {
                    int16_t *cp = &coef[ZIGZAG[k]];
                    if (*cp != 0) {
                        if (rfn_apply_bit(bs, cp, p1, m1)) return -1;
                    }
                    k++;
                }
                (*eob_run)--;
                return 0;
            }
            /* ZRL: r stays at 15, sval stays 0. Walk loop below will
             * pre-decrement until r<0 at the 16th zero and break there. */
        } else {
            if (s != 1) return -1;           /* refinement: only ±1 allowed */
            uint32_t bit;
            if (bs_get_bits_u(bs, 1, &bit)) return -1;
            sval = bit ? (int)p1 : (int)m1;
            /* r remains the number of zero positions to skip before placement. */
        }

        /* Walk: refine non-zeros, count-down r on zeros. Break when r<0 so k
         * lands at the target position for placement (or 16th zero for ZRL).
         * For sval!=0 with r=0, the first zero encountered becomes the target.
         */
        int placed = 0;
        while (k <= (int)se) {
            int16_t *cp = &coef[ZIGZAG[k]];
            if (*cp != 0) {
                if (rfn_apply_bit(bs, cp, p1, m1)) return -1;
                k++;
            } else {
                if (r == 0) {
                    /* Target zero — place sval if any, then advance. */
                    if (sval != 0) {
                        coef[ZIGZAG[k]] = (int16_t)sval;
                        placed = 1;
                    }
                    k++;
                    break;
                }
                r--;
                k++;
            }
        }

        if (sval != 0 && !placed) {
            /* Ran off band without a target zero — malformed stream. */
            return -1;
        }
        /* ZRL (sval==0, r should now be 0 if exactly 16 zeros consumed; if k>se
         * with r>0 the stream is malformed but we accept and continue.) */
    }
    return 0;
}
