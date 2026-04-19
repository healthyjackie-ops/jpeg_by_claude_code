#include "huffman.h"
#include "header_parser.h"
#include "bitstream.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t STD_DC_LUM_BITS[17] = {
    0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0
};
static const uint8_t STD_DC_LUM_VAL[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
};

static void test_symbol_decode(void) {
    htable_t h;
    memset(&h, 0, sizeof(h));
    memcpy(h.bits, STD_DC_LUM_BITS, sizeof(STD_DC_LUM_BITS));
    memcpy(h.huffval, STD_DC_LUM_VAL, sizeof(STD_DC_LUM_VAL));
    jpeg_build_huffman_tables(&h);

    uint8_t stream[] = { 0x00, 0xFF, 0x00 };
    bitstream_t bs; bs_init(&bs, stream, sizeof(stream));
    uint8_t sym;
    assert(huff_decode_symbol(&bs, &h, &sym) == 0);
    assert(sym == 0);
    printf("  test_symbol_decode PASS (sym=%u)\n", sym);
}

static void test_full_block_all_zero_ac(void) {
    htable_t dc, ac;
    memset(&dc, 0, sizeof(dc));
    memset(&ac, 0, sizeof(ac));
    memcpy(dc.bits, STD_DC_LUM_BITS, sizeof(STD_DC_LUM_BITS));
    memcpy(dc.huffval, STD_DC_LUM_VAL, sizeof(STD_DC_LUM_VAL));
    jpeg_build_huffman_tables(&dc);

    uint8_t ac_bits[17] = { 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t ac_val[] = { 0x00 };
    memcpy(ac.bits, ac_bits, sizeof(ac_bits));
    memcpy(ac.huffval, ac_val, sizeof(ac_val));
    jpeg_build_huffman_tables(&ac);

    uint8_t stream[] = { 0x04, 0xFF, 0x00 };
    bitstream_t bs; bs_init(&bs, stream, sizeof(stream));
    int16_t dc_pred = 0;
    int16_t coef[64];
    int rc = huff_decode_block(&bs, &dc, &ac, &dc_pred, coef);
    assert(rc == 0);
    assert(dc_pred == 0);
    assert(coef[0] == 0);
    for (int i = 1; i < 64; i++) assert(coef[i] == 0);
    printf("  test_full_block_all_zero_ac PASS\n");
}

int main(void) {
    printf("== test_huffman ==\n");
    test_symbol_decode();
    test_full_block_all_zero_ac();
    return 0;
}
