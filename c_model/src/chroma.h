#ifndef JPEG_CHROMA_H
#define JPEG_CHROMA_H

#include "jpeg_types.h"

void chroma_upsample_nn(const uint8_t *c420, uint8_t *c444,
                        uint16_t width, uint16_t height);

/* Phase 13: uint16 variant for P=12 planes. */
void chroma_upsample_nn_u16(const uint16_t *c420, uint16_t *c444,
                            uint16_t width, uint16_t height);

/* Directional nearest-neighbor upsamplers for the extended chroma modes.
 * width/height are the full (MCU-padded) luma dims; the sub-res source
 * stride is derived inside: h2 = width/2 (4:2:2), h4 = width/4 (4:1:1),
 * v2 = width with height/2 source rows (4:4:0). */
void chroma_upsample_h2(const uint8_t *c_sub, uint8_t *c_full,
                        uint16_t width, uint16_t height);
void chroma_upsample_h4(const uint8_t *c_sub, uint8_t *c_full,
                        uint16_t width, uint16_t height);
void chroma_upsample_v2(const uint8_t *c_sub, uint8_t *c_full,
                        uint16_t width, uint16_t height);

void chroma_upsample_h2_u16(const uint16_t *c_sub, uint16_t *c_full,
                            uint16_t width, uint16_t height);
void chroma_upsample_h4_u16(const uint16_t *c_sub, uint16_t *c_full,
                            uint16_t width, uint16_t height);
void chroma_upsample_v2_u16(const uint16_t *c_sub, uint16_t *c_full,
                            uint16_t width, uint16_t height);

#endif
