#include "chroma.h"

void chroma_upsample_nn(const uint8_t *c420, uint8_t *c444,
                        uint16_t width, uint16_t height)
{
    uint16_t cw = width  >> 1;
    for (uint16_t y = 0; y < height; y++) {
        const uint8_t *src = c420 + (y >> 1) * cw;
        uint8_t       *dst = c444 + y * width;
        for (uint16_t x = 0; x < width; x++) {
            dst[x] = src[x >> 1];
        }
    }
}

void chroma_upsample_nn_u16(const uint16_t *c420, uint16_t *c444,
                            uint16_t width, uint16_t height)
{
    uint16_t cw = width  >> 1;
    for (uint16_t y = 0; y < height; y++) {
        const uint16_t *src = c420 + (y >> 1) * cw;
        uint16_t       *dst = c444 + y * width;
        for (uint16_t x = 0; x < width; x++) {
            dst[x] = src[x >> 1];
        }
    }
}
