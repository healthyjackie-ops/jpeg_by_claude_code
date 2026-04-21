/*
 * Phase 21: Q-coder binary arithmetic decoder (ISO/IEC 10918-1 Annex D).
 *
 * Ported from libjpeg-turbo/src/jdarith.c (Vollbeding, IJG/libjpeg-turbo).
 * The decoder core is byte-for-byte equivalent; only the byte source has
 * been adapted to read from an in-memory buffer instead of a libjpeg
 * source_mgr callback. See spec_phase21.md §3 for register semantics.
 */

#include "arith.h"

/*
 * Table D.2 (ISO/IEC 10918-1) — probability estimation state machine.
 *
 *   entry = (Qe_Value << 16) | (Next_Index_MPS << 8)
 *         | (Switch_MPS << 7) | Next_Index_LPS
 *
 * Index 113 is the T.851 fixed 0.5 estimate; never reached by state
 * transitions from indexes 0..112.
 */
#define V(a, b, c, d) \
    (((uint32_t)(a) << 16) | ((uint32_t)(c) << 8) \
     | ((uint32_t)(d) << 7) | (uint32_t)(b))

const uint32_t jpeg_aritab[114] = {
    V(0x5a1d,   1,   1, 1),  V(0x2586,  14,   2, 0),
    V(0x1114,  16,   3, 0),  V(0x080b,  18,   4, 0),
    V(0x03d8,  20,   5, 0),  V(0x01da,  23,   6, 0),
    V(0x00e5,  25,   7, 0),  V(0x006f,  28,   8, 0),
    V(0x0036,  30,   9, 0),  V(0x001a,  33,  10, 0),
    V(0x000d,  35,  11, 0),  V(0x0006,   9,  12, 0),
    V(0x0003,  10,  13, 0),  V(0x0001,  12,  13, 0),
    V(0x5a7f,  15,  15, 1),  V(0x3f25,  36,  16, 0),
    V(0x2cf2,  38,  17, 0),  V(0x207c,  39,  18, 0),
    V(0x17b9,  40,  19, 0),  V(0x1182,  42,  20, 0),
    V(0x0cef,  43,  21, 0),  V(0x09a1,  45,  22, 0),
    V(0x072f,  46,  23, 0),  V(0x055c,  48,  24, 0),
    V(0x0406,  49,  25, 0),  V(0x0303,  51,  26, 0),
    V(0x0240,  52,  27, 0),  V(0x01b1,  54,  28, 0),
    V(0x0144,  56,  29, 0),  V(0x00f5,  57,  30, 0),
    V(0x00b7,  59,  31, 0),  V(0x008a,  60,  32, 0),
    V(0x0068,  62,  33, 0),  V(0x004e,  63,  34, 0),
    V(0x003b,  32,  35, 0),  V(0x002c,  33,   9, 0),
    V(0x5ae1,  37,  37, 1),  V(0x484c,  64,  38, 0),
    V(0x3a0d,  65,  39, 0),  V(0x2ef1,  67,  40, 0),
    V(0x261f,  68,  41, 0),  V(0x1f33,  69,  42, 0),
    V(0x19a8,  70,  43, 0),  V(0x1518,  72,  44, 0),
    V(0x1177,  73,  45, 0),  V(0x0e74,  74,  46, 0),
    V(0x0bfb,  75,  47, 0),  V(0x09f8,  77,  48, 0),
    V(0x0861,  78,  49, 0),  V(0x0706,  79,  50, 0),
    V(0x05cd,  48,  51, 0),  V(0x04de,  50,  52, 0),
    V(0x040f,  50,  53, 0),  V(0x0363,  51,  54, 0),
    V(0x02d4,  52,  55, 0),  V(0x025c,  53,  56, 0),
    V(0x01f8,  54,  57, 0),  V(0x01a4,  55,  58, 0),
    V(0x0160,  56,  59, 0),  V(0x0125,  57,  60, 0),
    V(0x00f6,  58,  61, 0),  V(0x00cb,  59,  62, 0),
    V(0x00ab,  61,  63, 0),  V(0x008f,  61,  32, 0),
    V(0x5b12,  65,  65, 1),  V(0x4d04,  80,  66, 0),
    V(0x412c,  81,  67, 0),  V(0x37d8,  82,  68, 0),
    V(0x2fe8,  83,  69, 0),  V(0x293c,  84,  70, 0),
    V(0x2379,  86,  71, 0),  V(0x1edf,  87,  72, 0),
    V(0x1aa9,  87,  73, 0),  V(0x174e,  72,  74, 0),
    V(0x1424,  72,  75, 0),  V(0x119c,  74,  76, 0),
    V(0x0f6b,  74,  77, 0),  V(0x0d51,  75,  78, 0),
    V(0x0bb6,  77,  79, 0),  V(0x0a40,  77,  48, 0),
    V(0x5832,  80,  81, 1),  V(0x4d1c,  88,  82, 0),
    V(0x438e,  89,  83, 0),  V(0x3bdd,  90,  84, 0),
    V(0x34ee,  91,  85, 0),  V(0x2eae,  92,  86, 0),
    V(0x299a,  93,  87, 0),  V(0x2516,  86,  71, 0),
    V(0x5570,  88,  89, 1),  V(0x4ca9,  95,  90, 0),
    V(0x44d9,  96,  91, 0),  V(0x3e22,  97,  92, 0),
    V(0x3824,  99,  93, 0),  V(0x32b4,  99,  94, 0),
    V(0x2e17,  93,  86, 0),  V(0x56a8,  95,  96, 1),
    V(0x4f46, 101,  97, 0),  V(0x47e5, 102,  98, 0),
    V(0x41cf, 103,  99, 0),  V(0x3c3d, 104, 100, 0),
    V(0x375e,  99,  93, 0),  V(0x5231, 105, 102, 0),
    V(0x4c0f, 106, 103, 0),  V(0x4639, 107, 104, 0),
    V(0x415e, 103,  99, 0),  V(0x5627, 105, 106, 1),
    V(0x50e7, 108, 107, 0),  V(0x4b85, 109, 103, 0),
    V(0x5597, 110, 109, 0),  V(0x504f, 111, 107, 0),
    V(0x5a10, 110, 111, 1),  V(0x5522, 112, 109, 0),
    V(0x59eb, 112, 111, 1),
    /* Index 113 — T.851 fixed 0.5 probability estimate. */
    V(0x5a1d, 113, 113, 0),
};

#undef V

/* ---- byte source + decoder core ------------------------------------ */

static int get_byte(arith_decoder_t *d)
{
    /* ISO D.2.6: on byte exhaustion (or after seeing a marker), return 0.
     * That value is folded into the C register and the decoder stays
     * consistent — upper-layer framing detects completion. */
    if (d->unread_marker) return 0;
    if (d->pos >= d->len) return 0;
    return d->src[d->pos++];
}

void arith_dec_init(arith_decoder_t *d, const uint8_t *data, size_t len)
{
    d->src = data;
    d->len = len;
    d->pos = 0;
    d->unread_marker = 0;

    /* Per jdarith.c process_restart init (and the very first scan):
     *   c = 0, a = 0, ct = -16  → loop demands 2 initial bytes,
     *                              sets a = 0x10000 after priming. */
    d->a = 0;
    d->c = 0;
    d->ct = -16;
}

void arith_dec_reset(arith_decoder_t *d)
{
    /* Phase 22c: RSTn re-prime. Preserve src/len/pos so decoding
     * resumes at the byte right after the marker — the caller has
     * already consumed the RSTn bytes (either via unread_marker clear
     * or via a manual scan in the framing layer). */
    d->a = 0;
    d->c = 0;
    d->ct = -16;
    d->unread_marker = 0;
}

int arith_dec_decode(arith_decoder_t *d, uint8_t *stat)
{
    uint8_t  nl, nm;
    uint32_t qe, temp;
    int      sv;
    int      data;

    /* Renormalization + byte input per ISO D.2.6 / libjpeg's floating-CT
     * scheme: shift A & C until A >= 0x8000, fetching new bytes as
     * needed through the stuffing-aware byte source. */
    while (d->a < 0x8000UL) {
        if (--d->ct < 0) {
            if (d->unread_marker) {
                data = 0;                /* synthesize zero after marker */
            } else {
                data = get_byte(d);
                if (data == 0xFF) {
                    /* 0xFF followed by:
                     *   0x00       → literal 0xFF (stuffed zero discarded)
                     *   run of 0xFF→ swallow (0xFFFF is an alias for 0xFF 0xFF 0x00 collapse)
                     *   any other  → genuine marker, latch in unread_marker,
                     *                resume with zeros per D.2.6.
                     */
                    do {
                        data = get_byte(d);
                    } while (data == 0xFF);
                    if (data == 0) {
                        data = 0xFF;
                    } else {
                        d->unread_marker = (uint8_t)data;
                        data = 0;
                    }
                }
            }
            d->c = (d->c << 8) | (uint32_t)data;
            if ((d->ct += 8) < 0) {
                if (++d->ct == 0) {
                    /* Two initial bytes absorbed → prime A and exit. */
                    d->a = 0x8000UL;
                }
            }
        }
        d->a <<= 1;
    }

    /* State lookup — compact packed form matches libjpeg-turbo. */
    sv = *stat;
    qe = jpeg_aritab[sv & 0x7F];
    nl = (uint8_t)(qe & 0xFF);       /* Next_Index_LPS (bit 7 = Switch_MPS) */
    qe >>= 8;
    nm = (uint8_t)(qe & 0xFF);       /* Next_Index_MPS */
    qe >>= 8;                        /* Qe_Value */

    /* Decode + estimation per ISO D.2.4 / D.2.5. */
    temp = d->a - qe;
    d->a = temp;
    temp <<= d->ct;
    if (d->c >= temp) {
        d->c -= temp;
        /* Conditional LPS exchange (Section D.2.4.1). */
        if (d->a < qe) {
            d->a = qe;
            *stat = (uint8_t)((sv & 0x80) ^ nm);       /* Estimate_after_MPS */
        } else {
            d->a = qe;
            *stat = (uint8_t)((sv & 0x80) ^ nl);       /* Estimate_after_LPS */
            sv ^= 0x80;                                 /* LPS/MPS exchange */
        }
    } else if (d->a < 0x8000UL) {
        /* Conditional MPS exchange (Section D.2.4.2). */
        if (d->a < qe) {
            *stat = (uint8_t)((sv & 0x80) ^ nl);       /* Estimate_after_LPS */
            sv ^= 0x80;
        } else {
            *stat = (uint8_t)((sv & 0x80) ^ nm);       /* Estimate_after_MPS */
        }
    }

    return sv >> 7;
}

/* ------------------------------------------------------------------ */
/* Phase 22: DC-difference symbol decoder (ISO F.1.4.4.1).
 *
 * Faithful port of libjpeg-turbo/src/jdarith.c:decode_mcu_DC_first
 * (inner loop over one block). Statistics layout documented in
 * arith.h. The helper does not touch last_dc_val — the caller maintains
 * the 16-bit accumulator (per Table F.4, DC value wraps mod 2^16). */
int arith_dec_dc_diff(arith_decoder_t *d,
                      uint8_t *dc_stats,
                      int *dc_context,
                      int L, int U,
                      int *out_diff)
{
    uint8_t *st = dc_stats + *dc_context;
    int sign;
    int v, m;

    /* Figure F.19: zero vs nonzero diff. */
    if (arith_dec_decode(d, st) == 0) {
        *dc_context = 0;
        *out_diff   = 0;
        return 0;
    }

    /* Figure F.22: sign bit. */
    sign = arith_dec_decode(d, st + 1);
    st += 2 + sign;                      /* SP or SN entry bin */

    /* Figure F.23: magnitude category (unary on X1..X14). */
    m = arith_dec_decode(d, st);
    if (m != 0) {
        st = dc_stats + 20;              /* X1 */
        while (arith_dec_decode(d, st)) {
            m <<= 1;
            if (m == 0x8000) {
                /* ISO F.1.4.4.1.4: magnitude overflow → corrupt stream. */
                return -1;
            }
            st += 1;
        }
    }

    /* Section F.1.4.4.1.2: derive next-block dc_context. Boundaries use
     * L (zero-category threshold) and U (small/large threshold) from the
     * DAC marker. Defaults (L=0, U=1) land m ∈ {0,1} in the "small"
     * bucket and m ≥ 2 in the "large" bucket — matching the usual
     * libjpeg behaviour for the common case. */
    if (m < ((1 << L) >> 1)) {
        *dc_context = 0;                          /* zero-diff category */
    } else if (m > ((1 << U) >> 1)) {
        *dc_context = 12 + (sign * 4);            /* large diff */
    } else {
        *dc_context = 4 + (sign * 4);             /* small diff */
    }

    /* Figure F.24: magnitude bit pattern. All bits share the M-bin at
     * st+14 for this magnitude category (st currently points at the
     * X-bin whose '0' terminated the category loop, so +14 lands in the
     * 14-bin M region at offset 34..47 of dc_stats). */
    v = m;
    st += 14;
    while (m >>= 1) {
        if (arith_dec_decode(d, st)) v |= m;
    }
    v += 1;
    if (sign) v = -v;

    *out_diff = v;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Phase 23a: AC-coefficient block decoder (ISO F.1.4.4.2).
 *
 * Faithful port of libjpeg-turbo/src/jdarith.c:decode_mcu AC loop. Writes
 * non-zero coefs into the natural-order `block[64]` via the ZIGZAG
 * table; caller zero-initialises the block. */
static const uint8_t JPEG_ZIGZAG[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

int arith_dec_ac_block(arith_decoder_t *d,
                       uint8_t *ac_stats,
                       uint8_t *fixed_bin,
                       int Kx,
                       int Ss, int Se,
                       int Al,
                       int16_t *block)
{
    int k, sign, v, m;
    uint8_t *st;

    for (k = Ss; k <= Se; k++) {
        st = ac_stats + 3 * (k - 1);
        /* Figure F.20: EOB flag — 1 ⇒ no more coefs in this block. */
        if (arith_dec_decode(d, st)) break;

        /* Zero-run: advance k while "coef is zero" bit is 0. */
        while (arith_dec_decode(d, st + 1) == 0) {
            st += 3;
            k++;
            if (k > Se) {
                /* Spectral overflow — corrupt stream. */
                return -1;
            }
        }

        /* Sign: single shared fixed_bin across all AC sign decisions. */
        sign = arith_dec_decode(d, fixed_bin);
        st += 2;

        /* Figure F.23: magnitude category. */
        m = arith_dec_decode(d, st);
        if (m != 0) {
            if (arith_dec_decode(d, st)) {
                m <<= 1;
                /* Low-freq (k ≤ Kx) and high-freq (k > Kx) get
                 * independent X-bin regions. */
                st = ac_stats + (k <= Kx ? 189 : 217);
                while (arith_dec_decode(d, st)) {
                    m <<= 1;
                    if (m == 0x8000) {
                        return -1;          /* magnitude overflow */
                    }
                    st += 1;
                }
            }
        }

        /* Magnitude bits share one M-bin at st+14 (same as DC). */
        v = m;
        st += 14;
        while (m >>= 1) {
            if (arith_dec_decode(d, st)) v |= m;
        }
        v += 1;
        if (sign) v = -v;

        /* Phase 24a: progressive AC-first scans apply the SOS point
         * transform here (<< Al). Sequential SOF9 passes Al=0. */
        block[JPEG_ZIGZAG[k]] = (int16_t)((uint32_t)v << Al);
    }

    return 0;
}

/* -------------------------------------------------------------------- */
/* Phase 24a: AC refinement (ISO F.1.4.4.2 refinement path).
 *
 * Port of libjpeg-turbo/src/jdarith.c:decode_mcu_AC_refine (lines
 * 436..497). p1 = 1<<Al, m1 = -1<<Al. For each k ∈ [Ss..Se]:
 *   - EOB check at ac_stats[3*(k-1)]   (only if k > kex)
 *   - inner loop: if coef nonzero → st+2 decides ±p1 refinement
 *                 else if st+1 → newly-nonzero, sign from fixed_bin
 *                 else advance (st+=3, k++) past the zero slot
 * kex = index of last previously non-zero coef (0 if block all-zero).
 * This function reads AND writes block — caller passes the coef
 * buffer from the last scan and we mutate it in place. */
int arith_dec_ac_refine(arith_decoder_t *d,
                        uint8_t *ac_stats,
                        uint8_t *fixed_bin,
                        int Ss, int Se, int Al,
                        int16_t *block)
{
    int16_t p1 = (int16_t)((uint32_t)1 << Al);
    int16_t m1 = (int16_t)(-((int32_t)1 << Al));   /* -p1, precomputed */
    int kex;

    /* kex = largest k in [1..Se] whose coef is nonzero, or 0. */
    for (kex = Se; kex > 0; kex--) {
        if (block[JPEG_ZIGZAG[kex]] != 0) break;
    }

    int k = Ss;
    while (k <= Se) {
        uint8_t *st = ac_stats + 3 * (k - 1);
        if (k > kex) {
            /* EOB decision only applies past the last known non-zero. */
            if (arith_dec_decode(d, st)) {
                /* EOB — terminate the scan band for this block. */
                break;
            }
        }
        /* Inner loop: walk until we decide on k (refine or new or skip). */
        for (;;) {
            int16_t *thiscoef = &block[JPEG_ZIGZAG[k]];
            if (*thiscoef != 0) {
                /* Previously nonzero — maybe add a refinement bit. */
                if (arith_dec_decode(d, st + 2)) {
                    if (*thiscoef < 0) {
                        *thiscoef = (int16_t)(*thiscoef + m1);
                    } else {
                        *thiscoef = (int16_t)(*thiscoef + p1);
                    }
                }
                break;
            }
            /* Zero slot: st+1 decides newly-nonzero vs skip. */
            if (arith_dec_decode(d, st + 1)) {
                /* Newly-nonzero — sign from fixed_bin. */
                if (arith_dec_decode(d, fixed_bin)) {
                    *thiscoef = m1;
                } else {
                    *thiscoef = p1;
                }
                break;
            }
            /* Still zero — advance k. */
            st += 3;
            k++;
            if (k > Se) {
                /* Ran off the end without finding EOB nor a nonzero —
                 * corrupt stream per ISO. */
                return -1;
            }
        }
        k++;   /* outer advance past the coef we just decided. */
    }

    return 0;
}
