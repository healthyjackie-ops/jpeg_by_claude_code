#include "bitstream.h"
#include <string.h>

void bs_init(bitstream_t *bs, const uint8_t *data, size_t size) {
    memset(bs, 0, sizeof(*bs));
    bs->data = data;
    bs->size = size;
}

int bs_read_byte(bitstream_t *bs, uint8_t *out) {
    if (bs->byte_pos >= bs->size) return -1;
    *out = bs->data[bs->byte_pos++];
    return 0;
}

int bs_read_u16(bitstream_t *bs, uint16_t *out) {
    uint8_t hi, lo;
    if (bs_read_byte(bs, &hi)) return -1;
    if (bs_read_byte(bs, &lo)) return -1;
    *out = (uint16_t)(((uint16_t)hi << 8) | lo);
    return 0;
}

void bs_skip(bitstream_t *bs, size_t n) {
    bs->byte_pos += n;
    if (bs->byte_pos > bs->size) bs->byte_pos = bs->size;
}

void bs_align_to_byte(bitstream_t *bs) {
    bs->bit_buf = 0;
    bs->bit_cnt = 0;
}

static int fetch_entropy_byte(bitstream_t *bs, uint8_t *out) {
    if (bs->marker_pending) return -1;
    if (bs->byte_pos >= bs->size) return -1;

    uint8_t b = bs->data[bs->byte_pos++];
    if (b == 0xFF) {
        if (bs->byte_pos >= bs->size) return -1;
        uint8_t next = bs->data[bs->byte_pos++];
        if (next == 0x00) {
            *out = 0xFF;
            return 0;
        }
        bs->marker_pending = 1;
        bs->last_marker = next;
        return -1;
    }
    *out = b;
    return 0;
}

int bs_fill_bits(bitstream_t *bs, int need) {
    while (bs->bit_cnt < need) {
        uint8_t b;
        if (fetch_entropy_byte(bs, &b)) {
            return (bs->bit_cnt >= need) ? 0 : -1;
        }
        bs->bit_buf = (bs->bit_buf << 8) | b;
        bs->bit_cnt += 8;
    }
    return 0;
}

uint32_t bs_peek_bits(bitstream_t *bs, int n) {
    if (bs->bit_cnt < n) {
        bs_fill_bits(bs, n);
    }
    if (bs->bit_cnt < n) {
        uint32_t v = bs->bit_buf << (n - bs->bit_cnt);
        return v & ((1u << n) - 1);
    }
    return (bs->bit_buf >> (bs->bit_cnt - n)) & ((1u << n) - 1);
}

int bs_get_bits_u(bitstream_t *bs, int n, uint32_t *out) {
    if (n == 0) { *out = 0; return 0; }
    if (bs_fill_bits(bs, n)) return -1;
    *out = (bs->bit_buf >> (bs->bit_cnt - n)) & ((1u << n) - 1);
    bs->bit_cnt -= n;
    bs->bit_buf &= (bs->bit_cnt == 0) ? 0 : ((1u << bs->bit_cnt) - 1);
    return 0;
}

int bs_get_bits(bitstream_t *bs, int n, int32_t *out) {
    uint32_t u;
    if (bs_get_bits_u(bs, n, &u)) return -1;
    *out = bs_extend((int32_t)u, n);
    return 0;
}

int32_t bs_extend(int32_t v, int n) {
    if (n == 0) return 0;
    int32_t vt = 1 << (n - 1);
    if (v < vt) {
        v += (-1 << n) + 1;
    }
    return v;
}
