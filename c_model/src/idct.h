#ifndef JPEG_IDCT_H
#define JPEG_IDCT_H

#include "jpeg_types.h"

void idct_islow(const int16_t coef[64], uint8_t out[64]);

/* Phase 13: 12-bit slow-int IDCT.
 * - coef: dequantized int32 coefficients (can exceed int16 for P=12 +Pq=1)
 * - out:  0..4095 samples (level shift +2048) */
void idct_islow_p12(const int32_t coef[64], uint16_t out[64]);

#endif
