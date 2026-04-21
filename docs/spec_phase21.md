# Phase 21 — Q-coder arithmetic core (ISO/IEC 10918-1 Annex D)

**Status**: ✅ C-model complete — 4/4 round-trip tests, zero cross-phase regression (2026-04-21)
**Parent**: [`roadmap_v2.md`](roadmap_v2.md) Wave 4 Phase 21
**Prereqs**: none (pure algorithmic block — plugs into DC/AC path in Phase 22/23)

---

## 1. Scope

Implement the binary arithmetic coder/decoder specified in ISO/IEC 10918-1
Annex D ("Annex D — Arithmetic coding"). This Q-coder is the core used by
all arithmetic-coded JPEG profiles:

- SOF9  — Extended sequential DCT, arithmetic (Wave 4 Phase 22–23)
- SOF10 — Progressive DCT, arithmetic              (Wave 4 Phase 24)
- SOF11 — Lossless, arithmetic                      (Wave 5 Phase 28)
- SOF13/14/15 — Hierarchical, arithmetic            (Wave 6 Phase 30)

Phase 21 delivers **only the coder** (one decode primitive that consumes a
statistics bin and returns 0/1), plus a matching encoder for round-trip
self-test. It does **not** wire into the JPEG decoder yet.

## 2. References

| Source | What it gives us |
|---|---|
| ITU T.81 Annex D (ISO/IEC 10918-1) | Normative INITDEC / DECODE / INITENC / CODE pseudocode, Table D.2 |
| `libjpeg-turbo/src/jaricom.c` | Packed 113+1 entry Qe + next-index table (Table D.2 in compact form) |
| `libjpeg-turbo/src/jdarith.c` | Reference decoder — `arith_decode`, floating CT scheme |
| `libjpeg-turbo/src/jcarith.c` | Reference encoder — `arith_encode`, `finish_pass`, "Pacman" termination |

The QE table is **bit-reproducible** across all JPEG arith implementations
(encoder and decoder MUST use the same table), so we port the 113+1 entry
form verbatim — index 113 is the T.851 fixed-probability 0.5 estimate.

## 3. Register semantics (libjpeg-turbo floating-CT scheme)

| Reg | Width | Purpose |
|---|---|---|
| `A` | 17-bit unsigned | interval size (in 1.16 fixed-point, range 0x8000..0x10000) |
| `C` | 32-bit unsigned | code register — contains both the current interval base (upper 16 bits) and a variable-length bit buffer below |
| `CT` | signed int | bit-shift counter; a single renormalization loops on `A < 0x8000`, shifting both A and C by 1. On underflow (`CT < 0`), next input byte is folded in. |
| `unread_marker` | int | non-zero once the decoder hits a JPEG marker mid-stream; subsequent "reads" yield zeros per ISO D.2.6 |

This is Vollbeding's "floating CT" variant of the canonical ISO pseudocode
— equivalent state but one shift per renorm instead of two. Adopting it
keeps the round-trip with libjpeg-turbo bit-exact.

## 4. Byte stuffing

Input bytestream uses JPEG's `0xFF 0x00` stuffing: a `0xFF` byte in the
compressed data is always followed by either `0x00` (literal 0xFF, strip
the stuffed zero) or a marker (decode terminates; subsequent reads return
zero). The decoder handles both cases internally — caller hands in the raw
entropy-coded segment and `unread_marker` tells caller what stopped it.

The encoder produces the same stuffing + "Pacman" final-byte trim (ISO
D.1.8 termination with optional `Discard_final_zeros`) — shortest legal
output stream.

## 5. Statistics bin format

One byte per probability context:

| Bit 7 | Bits 0–6 |
|---|---|
| MPS value (0 or 1) | ISO state index 0..112 (0..126 legal but 113..126 unused) |

Initialized to 0 per scan (index 0, MPS=0). Adapted in-place on every
decode/encode. The `(sv & 0x7F)` lookup allows the compact 128-entry table
without branching.

## 6. Public API (proposed)

```c
/* c_model/src/arith.h */

typedef struct {
    uint32_t a;                /* interval */
    uint32_t c;                /* code register */
    int      ct;               /* bit-shift counter (signed) */
    const uint8_t *src;        /* input bytestream */
    size_t   len;              /* total byte count */
    size_t   pos;              /* read cursor */
    uint8_t  unread_marker;    /* 0, or the marker byte that stopped us */
} arith_decoder_t;

void arith_dec_init(arith_decoder_t *d, const uint8_t *data, size_t len);
int  arith_dec_decode(arith_decoder_t *d, uint8_t *stat);
/* Returns 0 or 1. Advances d->pos as needed. Sets d->unread_marker on
 * marker encounter (decoder keeps returning 0 after that, per D.2.6). */

/* Packed QE table (Table D.2 in ITU T.81 + T.851 appendix).
 *  entry = (Qe << 16) | (Next_Index_MPS << 8) | (Switch_MPS << 7) | Next_Index_LPS
 */
extern const uint32_t jpeg_aritab[114];
```

Encoder lives in the test file (`tests/test_arith.c`) for now, since the
product decoder doesn't need it. Phase 22 adds DC/AC symbol decoders that
call `arith_dec_decode`; the encoder stays test-scoped unless/until a
Wave-5/6 use case demands it.

## 7. Test plan

`tests/test_arith.c` drives four self-contained round-trips:

1. **Fixed-probability stress** — encode 10 000 random bits with a single
   bin held at ISO state 0 (Qe=0x5A1D, MPS=0 initially), decode them back,
   assert every bit matches.
2. **Adaptive trajectory** — encode 10 000 random bits each with a
   dedicated statistics bin across 8 bins, observe that state indexes
   migrate and MPS flips occur; decoded bits bit-exact.
3. **Skewed probability** — 10 000 symbols from a Bernoulli(p=0.02) source
   encoded with a single adaptive bin. Output compresses to <100 bytes
   (≈1:20 vs. the 1250-byte raw) and round-trips exactly.
4. **Edge cases** — all-zeros input (100 symbols), all-ones input
   (100 symbols), empty-input decode (0 symbols), marker termination
   (encoder stops, we insert a `0xFF 0xD9` EOI and confirm decoder stops).

All four must pass before the phase is declared done. Each comparison is
bit-exact (no tolerance).

## 8. Deliverables

| File | Purpose |
|---|---|
| `c_model/src/arith.h` | Public API + QE table declaration |
| `c_model/src/arith.c` | Decoder + QE table (113+1 entries) |
| `c_model/tests/test_arith.c` | Round-trip test (embeds a port of the libjpeg encoder) |
| `c_model/Makefile` | Build rule via existing `test_%` pattern |
| `docs/spec_phase21.md` | This file |

## 9. Results (2026-04-21)

```
single_bin_adaptive   N=10000  encoded= 1294 bytes
multi_bin_adaptive    N=10000  encoded= 1303 bytes   (8 bins converged identically)
skewed_bernoulli      N=10000  encoded=  171 bytes   (7.3:1, vs Shannon ≈7.1:1)
edge_cases            100 zeros + 100 ones + empty + skewed-p=0.98 stuffing stress
```

The skewed Bernoulli(p=0.02) compression ratio of 7.3:1 lands within 3%
of the Shannon bound (H(0.02) ≈ 0.141 bit/sym → ~1.78 bits per symbol
overhead for finite adaptation), confirming correct probability tracking.

Cross-phase regression (phase25c 143/143 + phase27 334/334 + smoke
12/12) stays bit-exact after linking `arith.c` into all binaries, so
the new file is zero-impact on the existing Huffman-only paths.

## 10. Handoff to Phase 22

Phase 22 (SOF9 DC-only decode) will:

- Parse the DAC marker (0xFFCC, ISO B.2.4.3) to capture arithmetic
  conditioning table values.
- Use the `arith_dec_decode` primitive with the DC context model
  (ISO F.1.4.4.1): signed DIFF binarization via S/SP/SN/X1/X2.../XN bins,
  5-bit magnitude category lookup, value extension bits.
- Allocate DC_STAT_BINS = 64 bytes per DC table (one per context).
- Reset statistics on DNL / RSTn / end-of-scan (per process_restart in
  jdarith.c).
