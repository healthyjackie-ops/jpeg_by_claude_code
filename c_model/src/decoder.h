#ifndef JPEG_DECODER_H
#define JPEG_DECODER_H

#include "jpeg_types.h"

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t *y_plane;
    uint8_t *cb_plane;
    uint8_t *cr_plane;
    uint8_t *cb_plane_420;
    uint8_t *cr_plane_420;
    uint8_t *cb_plane_422;   /* Phase 10: sub-res chroma (W/2) × H */
    uint8_t *cr_plane_422;
    uint8_t *cb_plane_440;   /* Phase 11a: sub-res chroma W × (H/2) */
    uint8_t *cr_plane_440;
    uint8_t *cb_plane_411;   /* Phase 11b: sub-res chroma (W/4) × H */
    uint8_t *cr_plane_411;
    uint8_t *c_plane;        /* Phase 12: CMYK planes (all W × H, 1x1) */
    uint8_t *m_plane;
    uint8_t *y_plane_cmyk;
    uint8_t *k_plane;
    uint32_t err;
} jpeg_decoded_t;

int jpeg_decode(const uint8_t *data, size_t size, jpeg_decoded_t *out);

void jpeg_free(jpeg_decoded_t *out);

#endif
