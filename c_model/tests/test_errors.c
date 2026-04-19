#include "decoder.h"
#include "jpeg_types.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_empty(void) {
    uint8_t data[] = { 0xFF, 0xD8 };
    jpeg_decoded_t d;
    int rc = jpeg_decode(data, sizeof(data), &d);
    uint32_t err = d.err;
    assert(rc != 0);
    assert(err != 0);
    jpeg_free(&d);
    printf("  test_empty PASS (err=0x%X)\n", err);
}

static void test_bad_magic(void) {
    uint8_t data[] = { 0x00, 0x00, 0x00, 0x00 };
    jpeg_decoded_t d;
    int rc = jpeg_decode(data, sizeof(data), &d);
    uint32_t err = d.err;
    assert(rc != 0);
    assert(err & JPEG_ERR_BAD_MARKER);
    jpeg_free(&d);
    printf("  test_bad_magic PASS (err=0x%X)\n", err);
}

static void test_truncated(void) {
    uint8_t data[] = { 0xFF, 0xD8, 0xFF, 0xC0 };
    jpeg_decoded_t d;
    int rc = jpeg_decode(data, sizeof(data), &d);
    uint32_t err = d.err;
    assert(rc != 0);
    assert(err & JPEG_ERR_STREAM_TRUNC);
    jpeg_free(&d);
    printf("  test_truncated PASS (err=0x%X)\n", err);
}

int main(void) {
    printf("== test_errors ==\n");
    test_empty();
    test_bad_magic();
    test_truncated();
    return 0;
}
