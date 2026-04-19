#ifndef JPEG_HUFFMAN_H
#define JPEG_HUFFMAN_H

#include "jpeg_types.h"
#include "bitstream.h"

int huff_decode_symbol(bitstream_t *bs, const htable_t *h, uint8_t *symbol);

int huff_decode_block(bitstream_t *bs,
                      const htable_t *dc_tab,
                      const htable_t *ac_tab,
                      int16_t *dc_pred,
                      int16_t coef[64]);

#endif
