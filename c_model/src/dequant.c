#include "dequant.h"

void dequant_block(int16_t coef[64], const uint16_t qt[64]) {
    for (int i = 0; i < 64; i++) {
        coef[i] = (int16_t)((int32_t)coef[i] * (int32_t)qt[i]);
    }
}
