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

/* Reset the decoder's entropy state (a/c/ct/unread_marker) without
 * rewinding src/pos — used at RSTn boundaries where the current source
 * cursor must persist but the arithmetic coder must re-prime. The
 * caller is responsible for re-initialising its own statistics area.
 * Matches libjpeg-turbo's process_restart reset for arith streams. */
void arith_dec_reset(arith_decoder_t *d);

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

/* ------------------------------------------------------------------ */
/* Phase 22: DC-difference symbol decoder (ISO F.1.4.4.1 / Figure F.19..F.24).
 *
 * Decodes one DC-difference symbol against a per-table statistics area.
 * The statistics layout (ISO Table F.4):
 *   offsets  0..19  : S0/SS/SP/SN entry bins (one 4-bin triple per
 *                     dc_context ∈ {0,4,8,12,16}, selected by the caller).
 *   offsets 20..33 : X1..X14 magnitude-category continuation bins.
 *   offsets 34..47 : M-bit bins (one per magnitude category).
 * Allocate at least DC_STAT_BINS (48) bytes per DC table; the libjpeg
 * convention is 64 to match AC_STAT_BINS boundary but only 48 are live.
 *
 *   d           : Q-coder decoder state, already primed via arith_dec_init
 *   dc_stats    : pointer to the 48+ byte statistics area for this table
 *   dc_context  : in/out — last-scan DC category for this component
 *                  (0, 4, 8, 12, 16 per ISO F.1.4.4.1.2)
 *   L, U        : arith conditioning bounds from the DAC marker
 *                  (defaults: L=0, U=1)
 *   out_diff    : decoded signed DC difference (caller adds to
 *                  last_dc_val, mask to 16-bit per ISO)
 *
 * Returns 0 on success, -1 on magnitude overflow (corrupt stream —
 * matches libjpeg's JWRN_ARITH_BAD_CODE path).
 */
#define JPEG_DC_STAT_BINS 64

int arith_dec_dc_diff(arith_decoder_t *d,
                      uint8_t *dc_stats,
                      int *dc_context,
                      int L, int U,
                      int *out_diff);

/* ------------------------------------------------------------------ */
/* Phase 23a: AC block decoder (ISO F.1.4.4.2 / Figure F.20..F.24).
 *
 * Decodes the 63 AC coefficients of one 8x8 block (k ∈ [1..63]).
 *
 * AC statistics layout (ISO Table F.4, 256 bytes per AC table):
 *   offset  3*(k-1)  (k=1..63) : EOB + EZR bins for zigzag position k
 *                                (bin 0 = EOB, bin 1 = zero-run bit)
 *   offset  3*(k-1)+2          : magnitude category (first M1 bin)
 *   offset  189                : X-loop bins for low-freq   (k ≤ Kx)
 *   offset  217                : X-loop bins for high-freq  (k >  Kx)
 *   offset  203 / 231          : shared M-bins for low/high  (these are
 *                                X-bin + 14, matching the "st+=14" jump
 *                                in both encoder and decoder).
 *
 *   d          : Q-coder decoder state (already primed)
 *   ac_stats   : 256-byte statistics area for this AC table
 *   fixed_bin  : 1-byte shared sign-bin (persists across the scan —
 *                caller holds one per scan, zero-initialised)
 *   Kx         : arith_ac_K[tbl] from the DAC marker (default 5)
 *   block      : 64 int16_t coefs in NATURAL (not zigzag) order.
 *                Caller must zero-init before calling — we only write
 *                non-zero coefficients.
 *   Ss, Se     : spectral-selection range (1..63 for sequential SOF9).
 *                Pass (1, 63) for sequential scans; caller supplies the
 *                subset for progressive AC-first scans.
 *
 * Returns 0 on success, -1 on corrupt stream (EOB out of range or
 * magnitude overflow). */
int arith_dec_ac_block(arith_decoder_t *d,
                       uint8_t *ac_stats,
                       uint8_t *fixed_bin,
                       int Kx,
                       int Ss, int Se,
                       int16_t *block);

#endif
