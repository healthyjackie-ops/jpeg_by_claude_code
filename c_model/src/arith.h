#ifndef JPEG_ARITH_H
#define JPEG_ARITH_H
/*
 * Phase 21: Q-coder binary arithmetic decoder (ISO/IEC 10918-1 Annex D).
 *
 * Pure algorithmic block — returns one bit per call given a statistics bin.
 * The DC/AC symbol decoders (Phase 22/23) layer on top of this primitive.
 *
 * Implementation follows the compact "floating CT" variant used in
 * libjpeg-turbo/src/jdarith.c, which is bit-equivalent to the ISO
 * INITDEC / DECODE procedures but with one shift per renormalization.
 *
 * Statistics-bin encoding (1 byte per context):
 *   bit 7   = MPS value (0 or 1)
 *   bits 0..6 = ISO state index S ∈ {0..112}, index 113 = fixed 0.5.
 * Initialize bins to 0 at the start of each scan.
 *
 * Byte stuffing: the decoder eats 0xFF 0x00 → 0xFF internally and sets
 * d->unread_marker when a genuine marker is encountered mid-stream.
 * After that, the decoder returns zero bits per D.2.6.
 */

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t a;              /* interval register (upper 17 bits used) */
    uint32_t c;              /* code register (32-bit, floating cut-point) */
    int      ct;             /* bit-shift counter (signed; <0 demands bytes) */
    const uint8_t *src;      /* input bytestream (entropy-coded segment) */
    size_t   len;            /* total bytes in src */
    size_t   pos;            /* read cursor */
    uint8_t  unread_marker;  /* 0, or the marker byte that stopped us */
} arith_decoder_t;

/* Initialize the decoder against an in-memory entropy-coded segment.
 * After init, the first arith_dec_decode call drives the initial
 * byte-priming per INITDEC. */
void arith_dec_init(arith_decoder_t *d, const uint8_t *data, size_t len);

/* Decode one binary symbol using the probability bin at *stat.
 * Updates *stat (state-index migration / MPS flip) per D.2.4-D.2.5.
 * Returns 0 or 1. Never returns a failure value — on byte exhaustion it
 * synthesizes zero bytes (per ISO D.2.6) and the caller uses scan
 * framing (not symbol count) to know when decoding is done. */
int arith_dec_decode(arith_decoder_t *d, uint8_t *stat);

/* Packed ISO/IEC 10918-1 Table D.2 (Annex D §D.3):
 *
 *   entry = (Qe_Value << 16) | (Next_Index_MPS << 8)
 *         | (Switch_MPS << 7) | Next_Index_LPS
 *
 * Index 0..112 = normative JPEG table; index 113 = T.851 fixed-0.5 bin.
 * This packing matches libjpeg-turbo so we can cross-check bit-for-bit.
 */
extern const uint32_t jpeg_aritab[114];

#endif
