#include "idct.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_dc_only(void) {
    int16_t coef[64] = {0};
    coef[0] = 800;
    uint8_t out[64];
    idct_islow(coef, out);
    for (int i = 0; i < 64; i++) {
        assert(out[i] == 228 || out[i] == 227 || out[i] == 229);
    }
    printf("  test_dc_only PASS (dc=%u)\n", out[0]);
}

static void test_all_zero(void) {
    int16_t coef[64] = {0};
    uint8_t out[64];
    idct_islow(coef, out);
    for (int i = 0; i < 64; i++) assert(out[i] == 128);
    printf("  test_all_zero PASS\n");
}

static void test_dc_negative(void) {
    int16_t coef[64] = {0};
    coef[0] = -800;
    uint8_t out[64];
    idct_islow(coef, out);
    for (int i = 0; i < 64; i++) {
        assert(out[i] <= 29);
    }
    printf("  test_dc_negative PASS (dc=%u)\n", out[0]);
}

static void test_inverse_forward(void) {
    int16_t coef[64] = {0};
    coef[0] = 1024;
    coef[1] = 100;
    coef[8] = 50;
    coef[9] = 25;
    uint8_t out[64];
    idct_islow(coef, out);
    int min = 255, max = 0;
    for (int i = 0; i < 64; i++) {
        if (out[i] < min) min = out[i];
        if (out[i] > max) max = out[i];
    }
    assert(max > min);
    printf("  test_inverse_forward PASS (min=%d max=%d)\n", min, max);
}

static void test_saturation(void) {
    int16_t coef[64] = {0};
    coef[0] = 8000;
    uint8_t out[64];
    idct_islow(coef, out);
    for (int i = 0; i < 64; i++) assert(out[i] == 255);

    coef[0] = -8000;
    idct_islow(coef, out);
    for (int i = 0; i < 64; i++) assert(out[i] == 0);
    printf("  test_saturation PASS\n");
}

int main(void) {
    printf("== test_idct ==\n");
    test_all_zero();
    test_dc_only();
    test_dc_negative();
    test_inverse_forward();
    test_saturation();
    return 0;
}
