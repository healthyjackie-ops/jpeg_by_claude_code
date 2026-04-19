#ifndef JPEG_BITSTREAM_H
#define JPEG_BITSTREAM_H

#include "jpeg_types.h"

void bs_init(bitstream_t *bs, const uint8_t *data, size_t size);

int bs_read_byte(bitstream_t *bs, uint8_t *out);

int bs_read_u16(bitstream_t *bs, uint16_t *out);

void bs_skip(bitstream_t *bs, size_t n);

void bs_align_to_byte(bitstream_t *bs);

int bs_fill_bits(bitstream_t *bs, int need);

uint32_t bs_peek_bits(bitstream_t *bs, int n);

int bs_get_bits(bitstream_t *bs, int n, int32_t *out);

int bs_get_bits_u(bitstream_t *bs, int n, uint32_t *out);

int32_t bs_extend(int32_t v, int n);

#endif
