#ifndef JPEG_HEADER_PARSER_H
#define JPEG_HEADER_PARSER_H

#include "jpeg_types.h"
#include "bitstream.h"

int jpeg_parse_headers(bitstream_t *bs, jpeg_info_t *info, uint32_t *err);

void jpeg_build_huffman_tables(htable_t *h);

#endif
