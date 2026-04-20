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

/* Phase 17a: progressive SOF2 DC-only first scan but writes only coef[0],
 * preserving coef[1..63] (kept for cases where DC scan runs over a coef_buf
 * already initialised — which is always 0 initially so the two forms match).
 * Kept as a separate entry point so future refinement scans can treat DC
 * symmetrically with the AC path below. */
int huff_decode_dc_progressive(bitstream_t *bs,
                               const htable_t *dc_tab,
                               int16_t *dc_pred,
                               int16_t coef[64],
                               uint8_t al_shift);

/* Phase 17a: progressive SOF2 AC first scan (Ah=0) for a single block.
 *   - ss, se: spectral band (1 ≤ ss ≤ se ≤ 63)
 *   - al_shift: point transform — coefficient values left-shifted before store
 *   - eob_run: in/out running EOB counter, maintained across blocks within a
 *              single scan. Caller should init to 0 before the first block of
 *              each scan; helper decrements on EOBn matches.
 *   - coef is written in natural order at positions ZZ[ss..se]; other indexes
 *     untouched (DC from earlier scan preserved).
 * Returns 0 on success, -1 on bitstream error. */
int huff_decode_ac_progressive(bitstream_t *bs,
                               const htable_t *ac_tab,
                               int16_t coef[64],
                               uint8_t ss, uint8_t se,
                               uint8_t al_shift,
                               uint32_t *eob_run);

#endif
