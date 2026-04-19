#ifndef JPEG_CHROMA_H
#define JPEG_CHROMA_H

#include "jpeg_types.h"

void chroma_upsample_nn(const uint8_t *c420, uint8_t *c444,
                        uint16_t width, uint16_t height);

#endif
