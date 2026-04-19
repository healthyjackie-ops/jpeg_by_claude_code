#ifndef JPEG_IDCT_H
#define JPEG_IDCT_H

#include "jpeg_types.h"

void idct_islow(const int16_t coef[64], uint8_t out[64]);

#endif
