#ifndef JPEG_TYPES_H
#define JPEG_TYPES_H

#include <stdint.h>
#include <stddef.h>

#define JPEG_MAX_WIDTH   4096
#define JPEG_MAX_HEIGHT  4096
#define JPEG_MAX_COMPONENTS 3
#define JPEG_BLOCK_SIZE  64
#define JPEG_MCU_WIDTH   16
#define JPEG_MCU_HEIGHT  16

#define MARKER_PREFIX    0xFF
#define MARKER_SOI       0xD8
#define MARKER_EOI       0xD9
#define MARKER_SOF0      0xC0
#define MARKER_SOF1      0xC1
#define MARKER_SOF2      0xC2
#define MARKER_SOF3      0xC3
#define MARKER_SOF5      0xC5
#define MARKER_SOF15     0xCF
#define MARKER_DHT       0xC4
#define MARKER_DQT       0xDB
#define MARKER_DRI       0xDD
#define MARKER_SOS       0xDA
#define MARKER_APP0      0xE0
#define MARKER_APP15     0xEF
#define MARKER_COM       0xFE
#define MARKER_RST0      0xD0
#define MARKER_RST7      0xD7

typedef enum {
    JPEG_OK = 0,
    JPEG_ERR_UNSUP_SOF       = 1 << 0,
    JPEG_ERR_UNSUP_PREC      = 1 << 1,
    JPEG_ERR_UNSUP_CHROMA    = 1 << 2,
    JPEG_ERR_BAD_HUFFMAN     = 1 << 3,
    JPEG_ERR_BAD_MARKER      = 1 << 4,
    JPEG_ERR_DRI_NONZERO     = 1 << 5,
    JPEG_ERR_SIZE_OOR        = 1 << 6,
    JPEG_ERR_STREAM_TRUNC    = 1 << 7,
    JPEG_ERR_INTERNAL        = 1 << 31,
} jpeg_err_t;

typedef struct {
    uint16_t q[64];
    int loaded;
} qtable_t;

typedef struct {
    uint8_t  bits[17];
    uint8_t  huffval[256];
    uint16_t mincode[17];
    int32_t  maxcode[18];
    uint8_t  valptr[17];
    int loaded;
} htable_t;

typedef struct {
    uint8_t  id;
    uint8_t  h_samp;
    uint8_t  v_samp;
    uint8_t  qt_id;
    uint8_t  td;
    uint8_t  ta;
    int16_t  dc_pred;
} component_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t  precision;
    uint8_t  num_components;
    uint16_t dri;
    component_t components[JPEG_MAX_COMPONENTS];
    qtable_t qtables[4];
    htable_t htables_dc[4];
    htable_t htables_ac[4];
    uint16_t mcu_cols;
    uint16_t mcu_rows;
} jpeg_info_t;

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t byte_pos;
    uint32_t bit_buf;
    int      bit_cnt;
    int      marker_pending;
    uint8_t  last_marker;
} bitstream_t;

typedef struct {
    uint8_t *y;
    uint8_t *cb;
    uint8_t *cr;
    uint16_t width;
    uint16_t height;
    size_t   stride;
} image_t;

#endif
