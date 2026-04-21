/*
 * Phase 21: round-trip test for the Q-coder decoder in c_model/src/arith.[ch].
 *
 * Embeds a self-contained port of libjpeg-turbo's arithmetic *encoder*
 * (jcarith.c arith_encode + finish_pass) writing to an in-memory buffer,
 * then feeds the produced bytestream into our decoder and asserts bit-
 * exact match across four stress patterns.
 *
 * Encoder stays test-scoped; product code only needs the decoder.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "arith.h"

/* --------------------- Embedded encoder (port of jcarith.c) ---------- */

typedef struct {
    uint32_t a;          /* interval register */
    uint32_t c;          /* code register (upper 19 bits = base) */
    int      ct;         /* bit-shift counter */
    int      buffer;     /* pending output byte or -1 (empty) */
    int      sc;         /* stacked 0xFF count */
    int      zc;         /* stacked 0x00 count */
    uint8_t *out;        /* output buffer */
    size_t   cap;        /* buffer capacity */
    size_t   pos;        /* bytes written so far */
} arith_encoder_t;

static void enc_init(arith_encoder_t *e, uint8_t *buf, size_t cap)
{
    e->a = 0x10000UL;
    e->c = 0;
    e->ct = 11;
    e->buffer = -1;
    e->sc = 0;
    e->zc = 0;
    e->out = buf;
    e->cap = cap;
    e->pos = 0;
}

static void emit_byte(arith_encoder_t *e, int val)
{
    if (e->pos >= e->cap) {
        fprintf(stderr, "FATAL: encoder output buffer overflow (cap=%zu)\n",
                e->cap);
        exit(2);
    }
    e->out[e->pos++] = (uint8_t)val;
}

static void arith_encode(arith_encoder_t *e, uint8_t *stat, int val)
{
    uint8_t  nl, nm;
    uint32_t qe, temp;
    int      sv;

    sv = *stat;
    qe = jpeg_aritab[sv & 0x7F];
    nl = (uint8_t)(qe & 0xFF);
    qe >>= 8;
    nm = (uint8_t)(qe & 0xFF);
    qe >>= 8;

    e->a -= qe;
    if (val != (sv >> 7)) {
        /* Encode LPS (ISO D.1.4). */
        if (e->a >= qe) {
            e->c += e->a;
            e->a = qe;
        }
        *stat = (uint8_t)((sv & 0x80) ^ nl);
    } else {
        /* Encode MPS (ISO D.1.5). */
        if (e->a >= 0x8000UL)
            return;              /* no renormalization needed */
        if (e->a < qe) {
            e->c += e->a;
            e->a = qe;
        }
        *stat = (uint8_t)((sv & 0x80) ^ nm);
    }

    /* Renormalization + byte output (ISO D.1.6) with Vollbeding's
     * "Pacman" carry propagation: 0xFF stack + 0x00 stack + pending
     * buffer byte. Verbatim port of jcarith.c. */
    do {
        e->a <<= 1;
        e->c <<= 1;
        if (--e->ct == 0) {
            uint32_t temp32 = e->c >> 19;
            if (temp32 > 0xFF) {
                /* Overflow — propagate carry through all stacked 0xFFs. */
                if (e->buffer >= 0) {
                    if (e->zc) {
                        do emit_byte(e, 0x00);
                        while (--e->zc);
                    }
                    emit_byte(e, e->buffer + 1);
                    if (e->buffer + 1 == 0xFF)
                        emit_byte(e, 0x00);
                }
                e->zc += e->sc;     /* stacked 0xFFs become 0x00 (with +1 carry) */
                e->sc = 0;
                e->buffer = (int)(temp32 & 0xFF);
            } else if (temp32 == 0xFF) {
                ++e->sc;            /* stack; might still overflow later */
            } else {
                /* Flush any pending 0xFFs + buffer. */
                if (e->buffer == 0) {
                    ++e->zc;
                } else if (e->buffer >= 0) {
                    if (e->zc) {
                        do emit_byte(e, 0x00);
                        while (--e->zc);
                    }
                    emit_byte(e, e->buffer);
                }
                if (e->sc) {
                    if (e->zc) {
                        do emit_byte(e, 0x00);
                        while (--e->zc);
                    }
                    do {
                        emit_byte(e, 0xFF);
                        emit_byte(e, 0x00);
                    } while (--e->sc);
                }
                e->buffer = (int)(temp32 & 0xFF);
            }
            e->c &= 0x7FFFFUL;
            e->ct += 8;
        }
        (void)nl; (void)nm; (void)temp;
    } while (e->a < 0x8000UL);
}

static void arith_encode_finish(arith_encoder_t *e)
{
    /* ISO D.1.8 termination with Pacman-style trimming (verbatim from
     * jcarith.c finish_pass). */
    uint32_t temp;

    if ((temp = (e->a - 1 + e->c) & 0xFFFF0000UL) < e->c)
        e->c = temp + 0x8000UL;
    else
        e->c = temp;
    e->c <<= e->ct;
    if (e->c & 0xF8000000UL) {
        if (e->buffer >= 0) {
            if (e->zc) {
                do emit_byte(e, 0x00);
                while (--e->zc);
            }
            emit_byte(e, e->buffer + 1);
            if (e->buffer + 1 == 0xFF)
                emit_byte(e, 0x00);
        }
        e->zc += e->sc;
        e->sc = 0;
    } else {
        if (e->buffer == 0) {
            ++e->zc;
        } else if (e->buffer >= 0) {
            if (e->zc) {
                do emit_byte(e, 0x00);
                while (--e->zc);
            }
            emit_byte(e, e->buffer);
        }
        if (e->sc) {
            if (e->zc) {
                do emit_byte(e, 0x00);
                while (--e->zc);
            }
            do {
                emit_byte(e, 0xFF);
                emit_byte(e, 0x00);
            } while (--e->sc);
        }
    }
    if (e->c & 0x7FFF800UL) {
        if (e->zc) {
            do emit_byte(e, 0x00);
            while (--e->zc);
        }
        emit_byte(e, (int)((e->c >> 19) & 0xFF));
        if (((e->c >> 19) & 0xFF) == 0xFF)
            emit_byte(e, 0x00);
        if (e->c & 0x7F800UL) {
            emit_byte(e, (int)((e->c >> 11) & 0xFF));
            if (((e->c >> 11) & 0xFF) == 0xFF)
                emit_byte(e, 0x00);
        }
    }
}

/* --------------------- Test helpers ---------------------------------- */

/* 64-bit xorshift — portable, deterministic, no libc rand() variance. */
static uint64_t xorshift64(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return (*s = x);
}

/* ---- Test 1: single bin, random bits (adaptive) --------------------- */

static int test_single_bin_adaptive(void)
{
    const int N = 10000;
    uint8_t enc_buf[32768];
    uint8_t in_bits[10000];
    uint8_t stat_e = 0;
    uint8_t stat_d = 0;
    uint64_t rs = 0x1234567890abcdefULL;

    for (int i = 0; i < N; i++) in_bits[i] = (uint8_t)(xorshift64(&rs) & 1);

    arith_encoder_t e;
    enc_init(&e, enc_buf, sizeof(enc_buf));
    for (int i = 0; i < N; i++) arith_encode(&e, &stat_e, in_bits[i]);
    arith_encode_finish(&e);

    arith_decoder_t d;
    arith_dec_init(&d, enc_buf, e.pos);
    for (int i = 0; i < N; i++) {
        int b = arith_dec_decode(&d, &stat_d);
        if (b != in_bits[i]) {
            fprintf(stderr,
                    "[test_single_bin_adaptive] mismatch at bit %d: "
                    "expected %d, got %d (stat_e=0x%02x stat_d=0x%02x "
                    "enc_bytes=%zu)\n",
                    i, in_bits[i], b, stat_e, stat_d, e.pos);
            return 1;
        }
    }
    printf("[PASS] single_bin_adaptive  N=%d encoded=%zu bytes\n", N, e.pos);
    return 0;
}

/* ---- Test 2: 8 independent bins round-trip (each random) ------------ */

static int test_multi_bin_adaptive(void)
{
    const int N = 10000;
    const int NB = 8;
    uint8_t enc_buf[32768];
    uint8_t in_bits[10000];
    uint8_t ctx[10000];
    uint8_t stat_e[8] = {0};
    uint8_t stat_d[8] = {0};
    uint64_t rs = 0xdeadbeefcafebabeULL;

    for (int i = 0; i < N; i++) {
        uint64_t r = xorshift64(&rs);
        in_bits[i] = (uint8_t)(r & 1);
        ctx[i] = (uint8_t)((r >> 1) & 7);
    }

    arith_encoder_t e;
    enc_init(&e, enc_buf, sizeof(enc_buf));
    for (int i = 0; i < N; i++)
        arith_encode(&e, &stat_e[ctx[i]], in_bits[i]);
    arith_encode_finish(&e);

    arith_decoder_t d;
    arith_dec_init(&d, enc_buf, e.pos);
    for (int i = 0; i < N; i++) {
        int b = arith_dec_decode(&d, &stat_d[ctx[i]]);
        if (b != in_bits[i]) {
            fprintf(stderr,
                    "[test_multi_bin_adaptive] mismatch at bit %d "
                    "(ctx=%u): expected %d, got %d\n",
                    i, ctx[i], in_bits[i], b);
            return 1;
        }
    }
    /* Confirm both encoder & decoder statistics converged identically. */
    for (int i = 0; i < NB; i++) {
        if (stat_e[i] != stat_d[i]) {
            fprintf(stderr,
                    "[test_multi_bin_adaptive] final stats diverge on ctx %d: "
                    "enc=0x%02x dec=0x%02x\n",
                    i, stat_e[i], stat_d[i]);
            return 1;
        }
    }
    printf("[PASS] multi_bin_adaptive   N=%d encoded=%zu bytes "
           "(8 bins converged identically)\n", N, e.pos);
    return 0;
}

/* ---- Test 3: skewed Bernoulli(p=0.02) — compression sanity ---------- */

static int test_skewed_bernoulli(void)
{
    const int N = 10000;
    uint8_t enc_buf[8192];
    uint8_t in_bits[10000];
    uint8_t stat_e = 0;
    uint8_t stat_d = 0;
    uint64_t rs = 0xfeedfacecafe1234ULL;

    /* p = 0.02 * 2^32 ≈ 85899346.  Keep xorshift strictly positive below. */
    for (int i = 0; i < N; i++) {
        uint64_t r = xorshift64(&rs) & 0xFFFFFFFFULL;
        in_bits[i] = (uint8_t)(r < 85899346ULL);
    }

    arith_encoder_t e;
    enc_init(&e, enc_buf, sizeof(enc_buf));
    for (int i = 0; i < N; i++) arith_encode(&e, &stat_e, in_bits[i]);
    arith_encode_finish(&e);

    if (e.pos >= 300) {  /* loose upper bound; theoretical H(0.02) ≈ 0.14 bit/sym */
        fprintf(stderr,
                "[test_skewed_bernoulli] compression worse than expected: "
                "%zu bytes for %d symbols (Bernoulli(p=0.02))\n", e.pos, N);
        return 1;
    }

    arith_decoder_t d;
    arith_dec_init(&d, enc_buf, e.pos);
    for (int i = 0; i < N; i++) {
        int b = arith_dec_decode(&d, &stat_d);
        if (b != in_bits[i]) {
            fprintf(stderr,
                    "[test_skewed_bernoulli] mismatch at bit %d: "
                    "expected %d, got %d\n", i, in_bits[i], b);
            return 1;
        }
    }
    printf("[PASS] skewed_bernoulli     N=%d encoded=%zu bytes "
           "(compression ratio %.1f:1)\n",
           N, e.pos, (double)N / 8.0 / (double)e.pos);
    return 0;
}

/* ---- Test 4: edge cases -------------------------------------------- */

static int test_edge_cases(void)
{
    uint8_t enc_buf[4096];

    /* 4a: 100 zeros. */
    {
        const int N = 100;
        uint8_t stat_e = 0, stat_d = 0;
        arith_encoder_t e; enc_init(&e, enc_buf, sizeof(enc_buf));
        for (int i = 0; i < N; i++) arith_encode(&e, &stat_e, 0);
        arith_encode_finish(&e);
        arith_decoder_t d; arith_dec_init(&d, enc_buf, e.pos);
        for (int i = 0; i < N; i++) {
            if (arith_dec_decode(&d, &stat_d) != 0) {
                fprintf(stderr, "[test_edge_cases] 4a mismatch at %d\n", i);
                return 1;
            }
        }
    }

    /* 4b: 100 ones. */
    {
        const int N = 100;
        uint8_t stat_e = 0, stat_d = 0;
        arith_encoder_t e; enc_init(&e, enc_buf, sizeof(enc_buf));
        for (int i = 0; i < N; i++) arith_encode(&e, &stat_e, 1);
        arith_encode_finish(&e);
        arith_decoder_t d; arith_dec_init(&d, enc_buf, e.pos);
        for (int i = 0; i < N; i++) {
            if (arith_dec_decode(&d, &stat_d) != 1) {
                fprintf(stderr, "[test_edge_cases] 4b mismatch at %d\n", i);
                return 1;
            }
        }
    }

    /* 4c: empty encode — finish only. Decoder on empty buffer must not
     * crash on a few decode calls (they'll pull zeros and return MPS bits). */
    {
        arith_encoder_t e; enc_init(&e, enc_buf, sizeof(enc_buf));
        arith_encode_finish(&e);
        arith_decoder_t d; arith_dec_init(&d, enc_buf, e.pos);
        uint8_t st = 0;
        /* Pull a few bits; just require that it terminates without UB. */
        volatile int acc = 0;
        for (int i = 0; i < 32; i++) acc += arith_dec_decode(&d, &st);
        (void)acc;
    }

    /* 4d: encoder hits a simulated 0xFF run in its output — stress the
     * stuffing path by feeding it a skewed stream biased the other way
     * (Bernoulli(p=0.98) MPS-drift produces long 0xFF byte chains). */
    {
        const int N = 10000;
        uint8_t stat_e = 0, stat_d = 0;
        uint64_t rs = 0xabcdef0123456789ULL;
        uint8_t bits[10000];
        for (int i = 0; i < N; i++) {
            uint64_t r = xorshift64(&rs) & 0xFFFFFFFFULL;
            bits[i] = (uint8_t)(r > 85899346ULL);   /* p(1) = 0.98 */
        }
        arith_encoder_t e; enc_init(&e, enc_buf, sizeof(enc_buf));
        for (int i = 0; i < N; i++) arith_encode(&e, &stat_e, bits[i]);
        arith_encode_finish(&e);
        /* Count 0xFF 0x00 stuffing pairs to confirm they occur. */
        int ff_zero_pairs = 0;
        for (size_t j = 0; j + 1 < e.pos; j++) {
            if (enc_buf[j] == 0xFF && enc_buf[j + 1] == 0x00) ff_zero_pairs++;
        }
        arith_decoder_t d; arith_dec_init(&d, enc_buf, e.pos);
        for (int i = 0; i < N; i++) {
            int b = arith_dec_decode(&d, &stat_d);
            if (b != bits[i]) {
                fprintf(stderr,
                        "[test_edge_cases] 4d mismatch at %d: exp=%d got=%d "
                        "(ff_zero_pairs=%d)\n", i, bits[i], b, ff_zero_pairs);
                return 1;
            }
        }
        /* ff_zero_pairs may legitimately be 0 depending on adaptation;
         * just log and don't assert. */
        printf("[PASS] edge_cases           zeros+ones+empty+stuffing "
               "(stuffing pairs=%d in MPS-drift stream)\n", ff_zero_pairs);
    }

    return 0;
}

/* ---- Test 5: DC-diff symbol round-trip (ISO F.1.4.4.1) -------------- */

/* Matching encoder helper — port of jcarith.c:encode_mcu_DC_first inner
 * loop (minus last_dc_val bookkeeping, which the test driver manages).
 * Test-scope only; product decoder does not need a symmetric encoder. */
static void arith_enc_dc_diff(arith_encoder_t *e,
                              uint8_t *dc_stats,
                              int *dc_context,
                              int L, int U,
                              int diff)
{
    uint8_t *st = dc_stats + *dc_context;
    int v, v2, m;

    if (diff == 0) {
        arith_encode(e, st, 0);
        *dc_context = 0;
        return;
    }

    arith_encode(e, st, 1);
    if (diff > 0) {
        v    = diff;
        arith_encode(e, st + 1, 0);
        st += 2;                               /* SP */
        *dc_context = 4;                       /* small positive (provisional) */
    } else {
        v    = -diff;
        arith_encode(e, st + 1, 1);
        st += 3;                               /* SN */
        *dc_context = 8;                       /* small negative (provisional) */
    }

    /* Figure F.8: magnitude category (unary on X1..X14). */
    m = 0;
    v -= 1;
    if (v != 0) {
        arith_encode(e, st, 1);
        m  = 1;
        v2 = v;
        st = dc_stats + 20;                    /* X1 */
        while (v2 >>= 1) {
            arith_encode(e, st, 1);
            m <<= 1;
            st += 1;
        }
    }
    arith_encode(e, st, 0);                     /* terminator (always emitted) */

    /* ISO F.1.4.4.1.2: update dc_context for next block. */
    if (m < ((1 << L) >> 1)) {
        *dc_context = 0;
    } else if (m > ((1 << U) >> 1)) {
        *dc_context += 8;                       /* small -> large (12 or 16) */
    }

    /* Figure F.9: magnitude bit pattern at shared M-bin (st+14). */
    st += 14;
    while (m >>= 1) {
        arith_encode(e, st, (m & v) ? 1 : 0);
    }
}

/* 5a: exhaustive coverage of the critical diff magnitudes 0..±32
 *     (hits zero-path, ±1 m=0 path, every X-loop depth up to 5). */
static int test_dc_diff_small_range(void)
{
    uint8_t enc_buf[16384];
    uint8_t stats_e[64] = {0};
    uint8_t stats_d[64] = {0};
    int ctx_e = 0, ctx_d = 0;
    int N = 0;
    int diffs[200];

    for (int d = -32; d <= 32; d++) diffs[N++] = d;

    /* Record per-symbol encoder contexts so we can cross-check against the
     * decoder without having to replay encoding in sync. */
    int ctx_e_log[200];
    arith_encoder_t e; enc_init(&e, enc_buf, sizeof(enc_buf));
    for (int i = 0; i < N; i++) {
        arith_enc_dc_diff(&e, stats_e, &ctx_e, 0, 1, diffs[i]);
        ctx_e_log[i] = ctx_e;
    }
    arith_encode_finish(&e);

    arith_decoder_t dd; arith_dec_init(&dd, enc_buf, e.pos);
    for (int i = 0; i < N; i++) {
        int got, rc;
        rc = arith_dec_dc_diff(&dd, stats_d, &ctx_d, 0, 1, &got);
        if (rc != 0) {
            fprintf(stderr, "[test_dc_diff_small_range] rc=%d at i=%d\n", rc, i);
            return 1;
        }
        if (got != diffs[i]) {
            fprintf(stderr,
                    "[test_dc_diff_small_range] mismatch at %d: "
                    "exp=%d got=%d (ctx_e=%d ctx_d=%d)\n",
                    i, diffs[i], got, ctx_e_log[i], ctx_d);
            return 1;
        }
        if (ctx_e_log[i] != ctx_d) {
            fprintf(stderr,
                    "[test_dc_diff_small_range] dc_context diverged at %d: "
                    "diff=%d enc=%d dec=%d\n",
                    i, diffs[i], ctx_e_log[i], ctx_d);
            return 1;
        }
    }
    /* Encoder/decoder stats must also agree byte-for-byte. */
    if (memcmp(stats_e, stats_d, 64) != 0) {
        fprintf(stderr, "[test_dc_diff_small_range] stats table diverged\n");
        return 1;
    }
    printf("[PASS] dc_diff_small_range  diffs=-32..+32  N=%d encoded=%zu bytes\n",
           N, e.pos);
    return 0;
}

/* 5b: random 12-bit signed diffs (full JPEG DC range for P=12 lossy).
 *     Exercises deep X-loop iterations (m up to 0x800 / 2048). */
static int test_dc_diff_random_12bit(void)
{
    const int N = 4000;
    uint8_t enc_buf[32768];
    int16_t diffs[4000];
    uint8_t stats_e[64] = {0};
    uint8_t stats_d[64] = {0};
    int ctx_e = 0, ctx_d = 0;
    uint64_t rs = 0x0f1e2d3c4b5a6978ULL;

    /* Generate diffs in [-2047, +2047]. Most 12-bit DC deltas fall
     * within this range per ISO F.1.4.1. */
    for (int i = 0; i < N; i++) {
        uint64_t r = xorshift64(&rs);
        int mag = (int)(r & 0x7FF);             /* 0..2047 */
        int sign = (r >> 11) & 1;
        diffs[i] = (int16_t)(sign ? -mag : mag);
    }

    arith_encoder_t e; enc_init(&e, enc_buf, sizeof(enc_buf));
    for (int i = 0; i < N; i++)
        arith_enc_dc_diff(&e, stats_e, &ctx_e, 0, 1, diffs[i]);
    arith_encode_finish(&e);

    arith_decoder_t dd; arith_dec_init(&dd, enc_buf, e.pos);
    for (int i = 0; i < N; i++) {
        int got, rc;
        rc = arith_dec_dc_diff(&dd, stats_d, &ctx_d, 0, 1, &got);
        if (rc != 0) {
            fprintf(stderr, "[test_dc_diff_random_12bit] rc=%d at i=%d\n", rc, i);
            return 1;
        }
        if (got != diffs[i]) {
            fprintf(stderr,
                    "[test_dc_diff_random_12bit] mismatch at %d: "
                    "exp=%d got=%d\n", i, diffs[i], got);
            return 1;
        }
    }
    if (ctx_e != ctx_d) {
        fprintf(stderr, "[test_dc_diff_random_12bit] final ctx diverged e=%d d=%d\n",
                ctx_e, ctx_d);
        return 1;
    }
    printf("[PASS] dc_diff_random_12bit N=%d encoded=%zu bytes "
           "(avg=%.2f bits/symbol)\n",
           N, e.pos, (double)e.pos * 8.0 / (double)N);
    return 0;
}

/* 5c: non-default conditioning — sweep L ∈ {0..3}, U ∈ {L..5} and
 *     confirm each (L,U) pair round-trips.  Independent stats tables
 *     per sub-test so context classification stays coherent. */
static int test_dc_diff_conditioning(void)
{
    uint8_t enc_buf[16384];
    uint64_t rs = 0xfacefeed12345678ULL;
    const int N = 500;
    int total_ok = 0;

    for (int L = 0; L <= 3; L++) {
        for (int U = L; U <= 5; U++) {
            uint8_t stats_e[64] = {0};
            uint8_t stats_d[64] = {0};
            int ctx_e = 0, ctx_d = 0;
            int16_t diffs[500];

            for (int i = 0; i < N; i++) {
                uint64_t r = xorshift64(&rs);
                /* Mix small + large magnitudes so we touch both
                 * classifier branches. */
                int mag = (int)((r & 0x3FF) + 1);  /* 1..1024 */
                int sign = (r >> 10) & 1;
                int zero = ((r >> 11) & 7) == 0;    /* ~12% zero-diffs */
                diffs[i] = (int16_t)(zero ? 0 : (sign ? -mag : mag));
            }

            arith_encoder_t e; enc_init(&e, enc_buf, sizeof(enc_buf));
            for (int i = 0; i < N; i++)
                arith_enc_dc_diff(&e, stats_e, &ctx_e, L, U, diffs[i]);
            arith_encode_finish(&e);

            arith_decoder_t dd; arith_dec_init(&dd, enc_buf, e.pos);
            for (int i = 0; i < N; i++) {
                int got, rc;
                rc = arith_dec_dc_diff(&dd, stats_d, &ctx_d, L, U, &got);
                if (rc != 0) {
                    fprintf(stderr,
                            "[test_dc_diff_conditioning] rc=%d L=%d U=%d i=%d\n",
                            rc, L, U, i);
                    return 1;
                }
                if (got != diffs[i]) {
                    fprintf(stderr,
                            "[test_dc_diff_conditioning] mismatch L=%d U=%d "
                            "at %d: exp=%d got=%d\n",
                            L, U, i, diffs[i], got);
                    return 1;
                }
            }
            if (ctx_e != ctx_d || memcmp(stats_e, stats_d, 64) != 0) {
                fprintf(stderr,
                        "[test_dc_diff_conditioning] state diverged at "
                        "L=%d U=%d (ctx_e=%d ctx_d=%d)\n",
                        L, U, ctx_e, ctx_d);
                return 1;
            }
            total_ok++;
        }
    }
    printf("[PASS] dc_diff_conditioning pairs=%d  (L∈0..3, U∈L..5, N=%d per pair)\n",
           total_ok, N);
    return 0;
}

/* ---- Test 6: AC-block round-trip (ISO F.1.4.4.2) -------------------- */

/* Local zigzag table — duplicate of JPEG_ZIGZAG in arith.c. Keeps the
 * test independent of arith.c's file-scope table. */
static const uint8_t ZZ[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* Matching AC block encoder — port of jcarith.c:encode_mcu AC branch.
 * Test-scope only. */
static void arith_enc_ac_block(arith_encoder_t *e,
                               uint8_t *ac_stats,
                               uint8_t *fixed_bin,
                               int Kx,
                               int Ss, int Se,
                               const int16_t *block)
{
    int k, ke, v, v2, m;
    uint8_t *st;

    /* Find EOB position — highest non-zero zigzag index, within [Ss,Se]. */
    ke = Ss - 1;
    for (k = Se; k >= Ss; k--) {
        if (block[ZZ[k]] != 0) { ke = k; break; }
    }

    for (k = Ss; k <= ke; k++) {
        st = ac_stats + 3 * (k - 1);
        arith_encode(e, st, 0);                /* EOB=0 (more coefs) */
        /* Zero run: emit "zero at this k" bits. */
        while ((v = block[ZZ[k]]) == 0) {
            arith_encode(e, st + 1, 0);
            st += 3;
            k++;
        }
        arith_encode(e, st + 1, 1);            /* "coef is non-zero" */

        /* Sign on shared fixed_bin. */
        if (v > 0) {
            arith_encode(e, fixed_bin, 0);
        } else {
            v = -v;
            arith_encode(e, fixed_bin, 1);
        }
        st += 2;

        /* Magnitude category. */
        m = 0;
        v -= 1;
        if (v != 0) {
            arith_encode(e, st, 1);
            m = 1;
            v2 = v;
            if (v2 >>= 1) {
                arith_encode(e, st, 1);
                m <<= 1;
                st = ac_stats + (k <= Kx ? 189 : 217);
                while (v2 >>= 1) {
                    arith_encode(e, st, 1);
                    m <<= 1;
                    st += 1;
                }
            }
        }
        arith_encode(e, st, 0);                /* terminator */

        /* Magnitude bits at shared M-bin (st + 14). */
        st += 14;
        while (m >>= 1) {
            arith_encode(e, st, (m & v) ? 1 : 0);
        }
    }

    /* Emit trailing EOB=1 if we didn't run the full band. */
    if (ke < Se) {
        st = ac_stats + 3 * (ke);              /* k = ke+1 → 3*((ke+1)-1) = 3*ke */
        arith_encode(e, st, 1);
    }
}

/* Build a block with sparsity `sparsity` (fraction of zero coefs) and
 * magnitudes in [-max_mag, +max_mag]. DC position (index 0) is always 0
 * so the caller can reuse this for AC-only tests. */
static void fill_random_block(int16_t block[64], uint64_t *rs,
                              int max_mag, int sparsity_32)
{
    memset(block, 0, 64 * sizeof(int16_t));
    for (int k = 1; k < 64; k++) {
        uint64_t r = xorshift64(rs);
        if ((int)((r >> 16) & 0x1F) < sparsity_32) continue;  /* zero */
        int sign = (r & 1) ? -1 : 1;
        int mag = (int)((r >> 1) & 0x1FFF);
        mag = mag % max_mag + 1;
        block[ZZ[k]] = (int16_t)(sign * mag);
    }
}

/* 6a: 20 blocks with varying sparsity and magnitudes (q=baseline range). */
static int test_ac_block_varied(void)
{
    const int NB = 20;
    uint8_t enc_buf[65536];
    uint8_t stats_e[256] = {0};
    uint8_t stats_d[256] = {0};
    uint8_t fb_e = 113, fb_d = 113;  /* ISO T.851 fixed-0.5 bin */
    int16_t blocks[20][64];
    uint64_t rs = 0x5a5a5a5a12345678ULL;

    /* Mix sparsity: 0 = dense, 30 = ~94% zeros. */
    int sparsities[20];
    int max_mags[20];
    for (int i = 0; i < NB; i++) {
        sparsities[i] = (int)(xorshift64(&rs) & 0x1F);          /* 0..31 */
        max_mags[i]   = (int)((xorshift64(&rs) & 0x3FF) + 1);   /* 1..1024 */
        fill_random_block(blocks[i], &rs, max_mags[i], sparsities[i]);
    }

    arith_encoder_t e; enc_init(&e, enc_buf, sizeof(enc_buf));
    for (int i = 0; i < NB; i++)
        arith_enc_ac_block(&e, stats_e, &fb_e, 5, 1, 63, blocks[i]);
    arith_encode_finish(&e);

    arith_decoder_t dd; arith_dec_init(&dd, enc_buf, e.pos);
    int16_t out[64];
    for (int i = 0; i < NB; i++) {
        memset(out, 0, sizeof(out));
        int rc = arith_dec_ac_block(&dd, stats_d, &fb_d, 5, 1, 63, out);
        if (rc != 0) {
            fprintf(stderr, "[test_ac_block_varied] block %d rc=%d\n", i, rc);
            return 1;
        }
        for (int j = 1; j < 64; j++) {          /* AC only — skip DC */
            if (out[j] != blocks[i][j]) {
                fprintf(stderr,
                        "[test_ac_block_varied] block %d coef[%d]: "
                        "exp=%d got=%d (sparsity=%d max_mag=%d)\n",
                        i, j, blocks[i][j], out[j], sparsities[i], max_mags[i]);
                return 1;
            }
        }
    }
    if (memcmp(stats_e, stats_d, 256) != 0 || fb_e != fb_d) {
        fprintf(stderr, "[test_ac_block_varied] stats table / fixed_bin diverged\n");
        return 1;
    }
    printf("[PASS] ac_block_varied       %d blocks, encoded=%zu bytes\n",
           NB, e.pos);
    return 0;
}

/* 6b: edge blocks — all-zero AC, single coef at various positions,
 *     full dense band, long zero-run then late coef. */
static int test_ac_block_edge_cases(void)
{
    uint8_t enc_buf[32768];
    uint8_t stats_e[256] = {0};
    uint8_t stats_d[256] = {0};
    uint8_t fb_e = 113, fb_d = 113;  /* ISO T.851 fixed-0.5 bin */
    int16_t blocks[8][64] = {0};

    /* 0: all-zero AC (pure EOB at k=1). */
    /* 1: only block[ZZ[1]] = 7. */
    blocks[1][ZZ[1]] = 7;
    /* 2: only block[ZZ[63]] = -511 (last position, large magnitude). */
    blocks[2][ZZ[63]] = -511;
    /* 3: single coef at k=32 = 1024. */
    blocks[3][ZZ[32]] = 1024;
    /* 4: dense run k=1..8 with small magnitudes. */
    for (int k = 1; k <= 8; k++) blocks[4][ZZ[k]] = (int16_t)k;
    /* 5: sparse — zeros at k=1..40 then block[ZZ[41]] = -1. */
    blocks[5][ZZ[41]] = -1;
    /* 6: all coefs = 1 (max density, min magnitude). */
    for (int k = 1; k <= 63; k++) blocks[6][ZZ[k]] = 1;
    /* 7: alternating ±255 signs. */
    for (int k = 1; k <= 63; k++) blocks[7][ZZ[k]] = (k & 1) ? 255 : -255;

    arith_encoder_t e; enc_init(&e, enc_buf, sizeof(enc_buf));
    for (int i = 0; i < 8; i++)
        arith_enc_ac_block(&e, stats_e, &fb_e, 5, 1, 63, blocks[i]);
    arith_encode_finish(&e);

    arith_decoder_t dd; arith_dec_init(&dd, enc_buf, e.pos);
    int16_t out[64];
    for (int i = 0; i < 8; i++) {
        memset(out, 0, sizeof(out));
        int rc = arith_dec_ac_block(&dd, stats_d, &fb_d, 5, 1, 63, out);
        if (rc != 0) {
            fprintf(stderr, "[test_ac_block_edge_cases] block %d rc=%d\n", i, rc);
            return 1;
        }
        for (int j = 1; j < 64; j++) {
            if (out[j] != blocks[i][j]) {
                fprintf(stderr,
                        "[test_ac_block_edge_cases] block %d coef[%d]: "
                        "exp=%d got=%d\n", i, j, blocks[i][j], out[j]);
                return 1;
            }
        }
    }
    if (memcmp(stats_e, stats_d, 256) != 0 || fb_e != fb_d) {
        fprintf(stderr, "[test_ac_block_edge_cases] stats diverged\n");
        return 1;
    }
    printf("[PASS] ac_block_edge_cases   8 specialised blocks, encoded=%zu bytes\n",
           e.pos);
    return 0;
}

/* 6c: Kx conditioning sweep — 5 Kx values × 10 random blocks each,
 *     check round-trip + stats convergence. */
static int test_ac_block_conditioning(void)
{
    uint8_t enc_buf[32768];
    uint64_t rs = 0xbadf00dc0ffee123ULL;
    int total = 0;

    for (int Kx = 1; Kx <= 32; Kx += 8) {
        uint8_t stats_e[256] = {0};
        uint8_t stats_d[256] = {0};
        uint8_t fb_e = 113, fb_d = 113;  /* ISO T.851 fixed-0.5 bin */
        int16_t blocks[10][64];
        for (int i = 0; i < 10; i++)
            fill_random_block(blocks[i], &rs, 512, 10);

        arith_encoder_t e; enc_init(&e, enc_buf, sizeof(enc_buf));
        for (int i = 0; i < 10; i++)
            arith_enc_ac_block(&e, stats_e, &fb_e, Kx, 1, 63, blocks[i]);
        arith_encode_finish(&e);

        arith_decoder_t dd; arith_dec_init(&dd, enc_buf, e.pos);
        int16_t out[64];
        for (int i = 0; i < 10; i++) {
            memset(out, 0, sizeof(out));
            int rc = arith_dec_ac_block(&dd, stats_d, &fb_d, Kx, 1, 63, out);
            if (rc != 0) {
                fprintf(stderr, "[test_ac_block_conditioning] Kx=%d i=%d rc=%d\n",
                        Kx, i, rc);
                return 1;
            }
            for (int j = 1; j < 64; j++) {
                if (out[j] != blocks[i][j]) {
                    fprintf(stderr,
                            "[test_ac_block_conditioning] Kx=%d block %d coef[%d]: "
                            "exp=%d got=%d\n",
                            Kx, i, j, blocks[i][j], out[j]);
                    return 1;
                }
            }
        }
        if (memcmp(stats_e, stats_d, 256) != 0 || fb_e != fb_d) {
            fprintf(stderr, "[test_ac_block_conditioning] Kx=%d stats diverged\n", Kx);
            return 1;
        }
        total++;
    }
    printf("[PASS] ac_block_conditioning Kx sweeps=%d  (Kx ∈ {1,9,17,25}, N=10 per Kx)\n",
           total);
    return 0;
}

int main(void)
{
    int rc = 0;
    rc |= test_single_bin_adaptive();
    rc |= test_multi_bin_adaptive();
    rc |= test_skewed_bernoulli();
    rc |= test_edge_cases();
    rc |= test_dc_diff_small_range();
    rc |= test_dc_diff_random_12bit();
    rc |= test_dc_diff_conditioning();
    rc |= test_ac_block_varied();
    rc |= test_ac_block_edge_cases();
    rc |= test_ac_block_conditioning();
    if (rc == 0) printf("ALL ARITH TESTS PASSED\n");
    return rc ? 1 : 0;
}
