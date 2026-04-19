#ifndef JPEG_DEQUANT_H
#define JPEG_DEQUANT_H

#include "jpeg_types.h"

void dequant_block(int16_t coef[64], const uint16_t qt[64]);

#endif
