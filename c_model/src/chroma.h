#ifndef JPEG_CHROMA_H
#define JPEG_CHROMA_H

#include "jpeg_types.h"

void chroma_upsample_nn(const uint8_t *c420, uint8_t *c444,
                        uint16_t width, uint16_t height);

/* Phase 13: uint16 variant for P=12 planes. */
void chroma_upsample_nn_u16(const uint16_t *c420, uint16_t *c444,
                            uint16_t width, uint16_t height);

#endif
