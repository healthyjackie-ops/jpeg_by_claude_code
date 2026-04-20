#ifndef JPEG_HEADER_PARSER_H
#define JPEG_HEADER_PARSER_H

#include "jpeg_types.h"
#include "bitstream.h"

int jpeg_parse_headers(bitstream_t *bs, jpeg_info_t *info, uint32_t *err);

void jpeg_build_huffman_tables(htable_t *h);

/* Phase 17a: multi-scan progressive support.
 * Called after an entropy-coded scan has ended (bitstream is byte-aligned or
 * carrying a pending marker in bs->last_marker).
 *
 * Returns:
 *   +1 — EOI marker consumed; decoder should drain.
 *    0 — SOS marker + header consumed (info->scan_* updated); decoder should
 *        start next scan.
 *   -1 — error; *err is updated.
 */
int jpeg_parse_between_scans(bitstream_t *bs, jpeg_info_t *info, uint32_t *err);

#endif
