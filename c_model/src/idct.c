#include "idct.h"

#define CONST_BITS 13
#define PASS1_BITS 2

#define FIX_0_298631336  ((int32_t) 2446)
#define FIX_0_390180644  ((int32_t) 3196)
#define FIX_0_541196100  ((int32_t) 4433)
#define FIX_0_765366865  ((int32_t) 6270)
#define FIX_0_899976223  ((int32_t) 7373)
#define FIX_1_175875602  ((int32_t) 9633)
#define FIX_1_501321110  ((int32_t)12299)
#define FIX_1_847759065  ((int32_t)15137)
#define FIX_1_961570560  ((int32_t)16069)
#define FIX_2_053119869  ((int32_t)16819)
#define FIX_2_562915447  ((int32_t)20995)
#define FIX_3_072711026  ((int32_t)25172)

#define MULTIPLY(v, c) ((int32_t)(v) * (int32_t)(c))
#define DESCALE(x, n)  (((x) + (1 << ((n) - 1))) >> (n))

static inline uint8_t clamp_byte(int32_t v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static inline uint16_t clamp_12b(int64_t v) {
    if (v < 0) return 0;
    if (v > 4095) return 4095;
    return (uint16_t)v;
}

void idct_islow(const int16_t coef[64], uint8_t out[64]) {
    int32_t ws[64];
    int32_t tmp0, tmp1, tmp2, tmp3;
    int32_t tmp10, tmp11, tmp12, tmp13;
    int32_t z1, z2, z3, z4, z5;

    for (int col = 0; col < 8; col++) {
        const int16_t *in = coef + col;
        int32_t *wsc = ws + col;

        if (in[1*8] == 0 && in[2*8] == 0 && in[3*8] == 0 && in[4*8] == 0 &&
            in[5*8] == 0 && in[6*8] == 0 && in[7*8] == 0) {
            int32_t dcval = (int32_t)in[0] << PASS1_BITS;
            wsc[0*8] = wsc[1*8] = wsc[2*8] = wsc[3*8] = dcval;
            wsc[4*8] = wsc[5*8] = wsc[6*8] = wsc[7*8] = dcval;
            continue;
        }

        z2 = in[2*8];
        z3 = in[6*8];
        z1 = MULTIPLY(z2 + z3, FIX_0_541196100);
        tmp2 = z1 + MULTIPLY(z3, -FIX_1_847759065);
        tmp3 = z1 + MULTIPLY(z2,  FIX_0_765366865);

        z2 = in[0];
        z3 = in[4*8];
        tmp0 = (z2 + z3) << CONST_BITS;
        tmp1 = (z2 - z3) << CONST_BITS;

        tmp10 = tmp0 + tmp3;
        tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2;
        tmp12 = tmp1 - tmp2;

        tmp0 = in[7*8];
        tmp1 = in[5*8];
        tmp2 = in[3*8];
        tmp3 = in[1*8];

        z1 = tmp0 + tmp3;
        z2 = tmp1 + tmp2;
        z3 = tmp0 + tmp2;
        z4 = tmp1 + tmp3;
        z5 = MULTIPLY(z3 + z4, FIX_1_175875602);

        tmp0 = MULTIPLY(tmp0, FIX_0_298631336);
        tmp1 = MULTIPLY(tmp1, FIX_2_053119869);
        tmp2 = MULTIPLY(tmp2, FIX_3_072711026);
        tmp3 = MULTIPLY(tmp3, FIX_1_501321110);
        z1   = MULTIPLY(z1,  -FIX_0_899976223);
        z2   = MULTIPLY(z2,  -FIX_2_562915447);
        z3   = MULTIPLY(z3,  -FIX_1_961570560);
        z4   = MULTIPLY(z4,  -FIX_0_390180644);

        z3 += z5;
        z4 += z5;

        tmp0 += z1 + z3;
        tmp1 += z2 + z4;
        tmp2 += z2 + z3;
        tmp3 += z1 + z4;

        wsc[0*8] = DESCALE(tmp10 + tmp3, CONST_BITS - PASS1_BITS);
        wsc[7*8] = DESCALE(tmp10 - tmp3, CONST_BITS - PASS1_BITS);
        wsc[1*8] = DESCALE(tmp11 + tmp2, CONST_BITS - PASS1_BITS);
        wsc[6*8] = DESCALE(tmp11 - tmp2, CONST_BITS - PASS1_BITS);
        wsc[2*8] = DESCALE(tmp12 + tmp1, CONST_BITS - PASS1_BITS);
        wsc[5*8] = DESCALE(tmp12 - tmp1, CONST_BITS - PASS1_BITS);
        wsc[3*8] = DESCALE(tmp13 + tmp0, CONST_BITS - PASS1_BITS);
        wsc[4*8] = DESCALE(tmp13 - tmp0, CONST_BITS - PASS1_BITS);
    }

    for (int row = 0; row < 8; row++) {
        int32_t *wsr = ws + row * 8;
        uint8_t *outr = out + row * 8;

        if (wsr[1] == 0 && wsr[2] == 0 && wsr[3] == 0 && wsr[4] == 0 &&
            wsr[5] == 0 && wsr[6] == 0 && wsr[7] == 0) {
            uint8_t dcval = clamp_byte(DESCALE(wsr[0], PASS1_BITS + 3) + 128);
            for (int i = 0; i < 8; i++) outr[i] = dcval;
            continue;
        }

        z2 = wsr[2];
        z3 = wsr[6];
        z1 = MULTIPLY(z2 + z3, FIX_0_541196100);
        tmp2 = z1 + MULTIPLY(z3, -FIX_1_847759065);
        tmp3 = z1 + MULTIPLY(z2,  FIX_0_765366865);

        tmp0 = (wsr[0] + wsr[4]) << CONST_BITS;
        tmp1 = (wsr[0] - wsr[4]) << CONST_BITS;

        tmp10 = tmp0 + tmp3;
        tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2;
        tmp12 = tmp1 - tmp2;

        tmp0 = wsr[7];
        tmp1 = wsr[5];
        tmp2 = wsr[3];
        tmp3 = wsr[1];

        z1 = tmp0 + tmp3;
        z2 = tmp1 + tmp2;
        z3 = tmp0 + tmp2;
        z4 = tmp1 + tmp3;
        z5 = MULTIPLY(z3 + z4, FIX_1_175875602);

        tmp0 = MULTIPLY(tmp0, FIX_0_298631336);
        tmp1 = MULTIPLY(tmp1, FIX_2_053119869);
        tmp2 = MULTIPLY(tmp2, FIX_3_072711026);
        tmp3 = MULTIPLY(tmp3, FIX_1_501321110);
        z1   = MULTIPLY(z1,  -FIX_0_899976223);
        z2   = MULTIPLY(z2,  -FIX_2_562915447);
        z3   = MULTIPLY(z3,  -FIX_1_961570560);
        z4   = MULTIPLY(z4,  -FIX_0_390180644);

        z3 += z5;
        z4 += z5;

        tmp0 += z1 + z3;
        tmp1 += z2 + z4;
        tmp2 += z2 + z3;
        tmp3 += z1 + z4;

        outr[0] = clamp_byte(DESCALE(tmp10 + tmp3, CONST_BITS + PASS1_BITS + 3) + 128);
        outr[7] = clamp_byte(DESCALE(tmp10 - tmp3, CONST_BITS + PASS1_BITS + 3) + 128);
        outr[1] = clamp_byte(DESCALE(tmp11 + tmp2, CONST_BITS + PASS1_BITS + 3) + 128);
        outr[6] = clamp_byte(DESCALE(tmp11 - tmp2, CONST_BITS + PASS1_BITS + 3) + 128);
        outr[2] = clamp_byte(DESCALE(tmp12 + tmp1, CONST_BITS + PASS1_BITS + 3) + 128);
        outr[5] = clamp_byte(DESCALE(tmp12 - tmp1, CONST_BITS + PASS1_BITS + 3) + 128);
        outr[3] = clamp_byte(DESCALE(tmp13 + tmp0, CONST_BITS + PASS1_BITS + 3) + 128);
        outr[4] = clamp_byte(DESCALE(tmp13 - tmp0, CONST_BITS + PASS1_BITS + 3) + 128);
    }
}

/* ---- 12-bit slow-int IDCT (Phase 13) --------------------------------------
 * Same algorithm as idct_islow, but with int64 intermediates for safety and
 * PASS1_BITS=1 (vs 2 at P=8) to match libjpeg-turbo's j12idctint — losing 1b
 * of pass-1 precision avoids overflow/rounding divergence at 12-bit.
 * Level shift = +2048 (1 << 11). Clamp = 0..4095. */
#define PASS1_BITS_P12 1
#define MULTIPLY64(v, c) ((int64_t)(v) * (int64_t)(c))
#define DESCALE64(x, n)  (((x) + ((int64_t)1 << ((n) - 1))) >> (n))

void idct_islow_p12(const int32_t coef[64], uint16_t out[64]) {
    int64_t ws[64];
    int64_t tmp0, tmp1, tmp2, tmp3;
    int64_t tmp10, tmp11, tmp12, tmp13;
    int64_t z1, z2, z3, z4, z5;

    for (int col = 0; col < 8; col++) {
        const int32_t *in = coef + col;
        int64_t *wsc = ws + col;

        if (in[1*8] == 0 && in[2*8] == 0 && in[3*8] == 0 && in[4*8] == 0 &&
            in[5*8] == 0 && in[6*8] == 0 && in[7*8] == 0) {
            int64_t dcval = (int64_t)in[0] << PASS1_BITS_P12;
            wsc[0*8] = wsc[1*8] = wsc[2*8] = wsc[3*8] = dcval;
            wsc[4*8] = wsc[5*8] = wsc[6*8] = wsc[7*8] = dcval;
            continue;
        }

        z2 = in[2*8];
        z3 = in[6*8];
        z1 = MULTIPLY64(z2 + z3, FIX_0_541196100);
        tmp2 = z1 + MULTIPLY64(z3, -FIX_1_847759065);
        tmp3 = z1 + MULTIPLY64(z2,  FIX_0_765366865);

        z2 = in[0];
        z3 = in[4*8];
        tmp0 = (z2 + z3) * ((int64_t)1 << CONST_BITS);
        tmp1 = (z2 - z3) * ((int64_t)1 << CONST_BITS);

        tmp10 = tmp0 + tmp3;
        tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2;
        tmp12 = tmp1 - tmp2;

        tmp0 = in[7*8];
        tmp1 = in[5*8];
        tmp2 = in[3*8];
        tmp3 = in[1*8];

        z1 = tmp0 + tmp3;
        z2 = tmp1 + tmp2;
        z3 = tmp0 + tmp2;
        z4 = tmp1 + tmp3;
        z5 = MULTIPLY64(z3 + z4, FIX_1_175875602);

        tmp0 = MULTIPLY64(tmp0, FIX_0_298631336);
        tmp1 = MULTIPLY64(tmp1, FIX_2_053119869);
        tmp2 = MULTIPLY64(tmp2, FIX_3_072711026);
        tmp3 = MULTIPLY64(tmp3, FIX_1_501321110);
        z1   = MULTIPLY64(z1,  -FIX_0_899976223);
        z2   = MULTIPLY64(z2,  -FIX_2_562915447);
        z3   = MULTIPLY64(z3,  -FIX_1_961570560);
        z4   = MULTIPLY64(z4,  -FIX_0_390180644);

        z3 += z5;
        z4 += z5;

        tmp0 += z1 + z3;
        tmp1 += z2 + z4;
        tmp2 += z2 + z3;
        tmp3 += z1 + z4;

        wsc[0*8] = DESCALE64(tmp10 + tmp3, CONST_BITS - PASS1_BITS_P12);
        wsc[7*8] = DESCALE64(tmp10 - tmp3, CONST_BITS - PASS1_BITS_P12);
        wsc[1*8] = DESCALE64(tmp11 + tmp2, CONST_BITS - PASS1_BITS_P12);
        wsc[6*8] = DESCALE64(tmp11 - tmp2, CONST_BITS - PASS1_BITS_P12);
        wsc[2*8] = DESCALE64(tmp12 + tmp1, CONST_BITS - PASS1_BITS_P12);
        wsc[5*8] = DESCALE64(tmp12 - tmp1, CONST_BITS - PASS1_BITS_P12);
        wsc[3*8] = DESCALE64(tmp13 + tmp0, CONST_BITS - PASS1_BITS_P12);
        wsc[4*8] = DESCALE64(tmp13 - tmp0, CONST_BITS - PASS1_BITS_P12);
    }

    for (int row = 0; row < 8; row++) {
        int64_t *wsr = ws + row * 8;
        uint16_t *outr = out + row * 8;

        /* Row DC-only shortcut. For P=12 we use PASS1_BITS_P12=1 to match
           libjpeg-turbo's j12idctint — the pass-1 descale drops 1b less, so
           the row-pass shift is CONST_BITS + PASS1_BITS_P12 + 3. */
        if (wsr[1] == 0 && wsr[2] == 0 && wsr[3] == 0 && wsr[4] == 0 &&
            wsr[5] == 0 && wsr[6] == 0 && wsr[7] == 0) {
            uint16_t dcval = clamp_12b(DESCALE64(wsr[0], PASS1_BITS_P12 + 3) + 2048);
            for (int i = 0; i < 8; i++) outr[i] = dcval;
            continue;
        }

        z2 = wsr[2];
        z3 = wsr[6];
        z1 = MULTIPLY64(z2 + z3, FIX_0_541196100);
        tmp2 = z1 + MULTIPLY64(z3, -FIX_1_847759065);
        tmp3 = z1 + MULTIPLY64(z2,  FIX_0_765366865);

        tmp0 = (wsr[0] + wsr[4]) * ((int64_t)1 << CONST_BITS);
        tmp1 = (wsr[0] - wsr[4]) * ((int64_t)1 << CONST_BITS);

        tmp10 = tmp0 + tmp3;
        tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2;
        tmp12 = tmp1 - tmp2;

        tmp0 = wsr[7];
        tmp1 = wsr[5];
        tmp2 = wsr[3];
        tmp3 = wsr[1];

        z1 = tmp0 + tmp3;
        z2 = tmp1 + tmp2;
        z3 = tmp0 + tmp2;
        z4 = tmp1 + tmp3;
        z5 = MULTIPLY64(z3 + z4, FIX_1_175875602);

        tmp0 = MULTIPLY64(tmp0, FIX_0_298631336);
        tmp1 = MULTIPLY64(tmp1, FIX_2_053119869);
        tmp2 = MULTIPLY64(tmp2, FIX_3_072711026);
        tmp3 = MULTIPLY64(tmp3, FIX_1_501321110);
        z1   = MULTIPLY64(z1,  -FIX_0_899976223);
        z2   = MULTIPLY64(z2,  -FIX_2_562915447);
        z3   = MULTIPLY64(z3,  -FIX_1_961570560);
        z4   = MULTIPLY64(z4,  -FIX_0_390180644);

        z3 += z5;
        z4 += z5;

        tmp0 += z1 + z3;
        tmp1 += z2 + z4;
        tmp2 += z2 + z3;
        tmp3 += z1 + z4;

        outr[0] = clamp_12b(DESCALE64(tmp10 + tmp3, CONST_BITS + PASS1_BITS_P12 + 3) + 2048);
        outr[7] = clamp_12b(DESCALE64(tmp10 - tmp3, CONST_BITS + PASS1_BITS_P12 + 3) + 2048);
        outr[1] = clamp_12b(DESCALE64(tmp11 + tmp2, CONST_BITS + PASS1_BITS_P12 + 3) + 2048);
        outr[6] = clamp_12b(DESCALE64(tmp11 - tmp2, CONST_BITS + PASS1_BITS_P12 + 3) + 2048);
        outr[2] = clamp_12b(DESCALE64(tmp12 + tmp1, CONST_BITS + PASS1_BITS_P12 + 3) + 2048);
        outr[5] = clamp_12b(DESCALE64(tmp12 - tmp1, CONST_BITS + PASS1_BITS_P12 + 3) + 2048);
        outr[3] = clamp_12b(DESCALE64(tmp13 + tmp0, CONST_BITS + PASS1_BITS_P12 + 3) + 2048);
        outr[4] = clamp_12b(DESCALE64(tmp13 - tmp0, CONST_BITS + PASS1_BITS_P12 + 3) + 2048);
    }
}
