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

/* Phase 16b: progressive SOF2 DC-only first scan (Ah=0).
 *   - Decodes a single DC coefficient
 *   - Updates *dc_pred by DIFF
 *   - Writes coef[0] = (*dc_pred) << al_shift; coef[1..63] = 0
 * Returns 0 on success, -1 on bitstream error. */
int huff_decode_block_dc_only(bitstream_t *bs,
                              const htable_t *dc_tab,
                              int16_t *dc_pred,
                              int16_t coef[64],
                              uint8_t al_shift);

#endif
