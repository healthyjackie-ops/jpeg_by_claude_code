#include "chroma.h"

#include <string.h>

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

void chroma_upsample_h2(const uint8_t *c_sub, uint8_t *c_full,
                        uint16_t width, uint16_t height)
{
    uint16_t cw = width >> 1;
    for (uint16_t y = 0; y < height; y++) {
        const uint8_t *src = c_sub  + (size_t)y * cw;
        uint8_t       *dst = c_full + (size_t)y * width;
        for (uint16_t x = 0; x < width; x++) {
            dst[x] = src[x >> 1];
        }
    }
}

void chroma_upsample_h4(const uint8_t *c_sub, uint8_t *c_full,
                        uint16_t width, uint16_t height)
{
    uint16_t cw = width >> 2;
    for (uint16_t y = 0; y < height; y++) {
        const uint8_t *src = c_sub  + (size_t)y * cw;
        uint8_t       *dst = c_full + (size_t)y * width;
        for (uint16_t x = 0; x < width; x++) {
            dst[x] = src[x >> 2];
        }
    }
}

void chroma_upsample_v2(const uint8_t *c_sub, uint8_t *c_full,
                        uint16_t width, uint16_t height)
{
    for (uint16_t y = 0; y < height; y++) {
        memcpy(c_full + (size_t)y * width,
               c_sub  + (size_t)(y >> 1) * width, width);
    }
}

void chroma_upsample_h2_u16(const uint16_t *c_sub, uint16_t *c_full,
                            uint16_t width, uint16_t height)
{
    uint16_t cw = width >> 1;
    for (uint16_t y = 0; y < height; y++) {
        const uint16_t *src = c_sub  + (size_t)y * cw;
        uint16_t       *dst = c_full + (size_t)y * width;
        for (uint16_t x = 0; x < width; x++) {
            dst[x] = src[x >> 1];
        }
    }
}

void chroma_upsample_h4_u16(const uint16_t *c_sub, uint16_t *c_full,
                            uint16_t width, uint16_t height)
{
    uint16_t cw = width >> 2;
    for (uint16_t y = 0; y < height; y++) {
        const uint16_t *src = c_sub  + (size_t)y * cw;
        uint16_t       *dst = c_full + (size_t)y * width;
        for (uint16_t x = 0; x < width; x++) {
            dst[x] = src[x >> 2];
        }
    }
}

void chroma_upsample_v2_u16(const uint16_t *c_sub, uint16_t *c_full,
                            uint16_t width, uint16_t height)
{
    for (uint16_t y = 0; y < height; y++) {
        memcpy(c_full + (size_t)y * width,
               c_sub  + (size_t)(y >> 1) * width,
               (size_t)width * sizeof(uint16_t));
    }
}
