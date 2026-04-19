#include "bitstream.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_basic_read(void) {
    uint8_t data[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    bitstream_t bs;
    bs_init(&bs, data, sizeof(data));
    uint8_t b; uint16_t s;
    assert(bs_read_byte(&bs, &b) == 0 && b == 0xDE);
    assert(bs_read_u16(&bs, &s) == 0 && s == 0xADBE);
    assert(bs_read_byte(&bs, &b) == 0 && b == 0xEF);
    assert(bs_read_byte(&bs, &b) == -1);
    printf("  test_basic_read PASS\n");
}

static void test_byte_stuff(void) {
    uint8_t data[] = { 0x12, 0xFF, 0x00, 0x34 };
    bitstream_t bs;
    bs_init(&bs, data, sizeof(data));
    uint32_t v;
    assert(bs_get_bits_u(&bs, 8, &v) == 0 && v == 0x12);
    assert(bs_get_bits_u(&bs, 8, &v) == 0 && v == 0xFF);
    assert(bs_get_bits_u(&bs, 8, &v) == 0 && v == 0x34);
    printf("  test_byte_stuff PASS\n");
}

static void test_marker_detect(void) {
    uint8_t data[] = { 0x12, 0xFF, 0xD9 };
    bitstream_t bs;
    bs_init(&bs, data, sizeof(data));
    uint32_t v;
    assert(bs_get_bits_u(&bs, 8, &v) == 0 && v == 0x12);
    int rc = bs_get_bits_u(&bs, 8, &v);
    assert(rc == -1 && bs.marker_pending == 1 && bs.last_marker == 0xD9);
    printf("  test_marker_detect PASS\n");
}

static void test_variable_bits(void) {
    uint8_t data[] = { 0xB4, 0xCA };
    bitstream_t bs;
    bs_init(&bs, data, sizeof(data));
    uint32_t v;
    assert(bs_get_bits_u(&bs, 3, &v) == 0 && v == 0x5);
    assert(bs_get_bits_u(&bs, 5, &v) == 0 && v == 0x14);
    assert(bs_get_bits_u(&bs, 8, &v) == 0 && v == 0xCA);
    printf("  test_variable_bits PASS\n");
}

static void test_extend(void) {
    assert(bs_extend(0, 0) == 0);
    assert(bs_extend(1, 1) == 1);
    assert(bs_extend(0, 1) == -1);
    assert(bs_extend(5, 3) == 5);
    assert(bs_extend(2, 3) == -5);
    assert(bs_extend(0x3FF, 10) == 0x3FF);
    assert(bs_extend(0, 10) == -0x3FF);
    printf("  test_extend PASS\n");
}

int main(void) {
    printf("== test_bitstream ==\n");
    test_basic_read();
    test_byte_stuff();
    test_marker_detect();
    test_variable_bits();
    test_extend();
    return 0;
}
