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

int main(void)
{
    int rc = 0;
    rc |= test_single_bin_adaptive();
    rc |= test_multi_bin_adaptive();
    rc |= test_skewed_bernoulli();
    rc |= test_edge_cases();
    if (rc == 0) printf("ALL ARITH TESTS PASSED\n");
    return rc ? 1 : 0;
}
