# Phase 22 — SOF9 sequential-arithmetic decode (ISO/IEC 10918-1 Annex F.1.4.4)

**Status**: ✅ **COMPLETE** — 22a/b header parse + DC-diff helper,
23a AC-block helper, and **22c/23b** end-to-end SOF9 decode all land
with 18/18 phase22 vectors bit-exact vs libjpeg-turbo (2026-04-21).

**Parent**: [`roadmap_v2.md`](roadmap_v2.md) Wave 4 Phase 22
**Prereqs**: [Phase 21](spec_phase21.md) (Q-coder core, 4/4 round-trip)

---

## 1. Scope

Deliver the **sequential arithmetic (SOF9)** DC-only decode path in the
C model. Phase 22 is split into three micro-phases so each lands with
its own regression:

| Sub | What lands | Verification |
|---|---|---|
| **22a** ✅ | Marker recognition: `SOF9/10/11 = 0xFFC9/CA/CB`, `DAC = 0xFFCC`. Header parser accepts them and stores DAC conditioning (`arith_dc_L/U/K`). Decoder still returns `JPEG_ERR_UNSUP_SOF` on SOF9/10/11 so arith payloads fail fast. | phase27 + smoke regression green |
| **22b** ✅ | `arith_dec_dc_diff` — DC-difference symbol decoder per ISO F.1.4.4.1 (Figures F.19–F.24). Matching test-only encoder `arith_enc_dc_diff` + three new round-trip tests (small-range, random 12-bit, L/U conditioning sweep). | `tests/test_arith.c` 7/7 passes |
| **23a** ✅ | `arith_dec_ac_block` — full 63-coef AC block decoder per ISO F.1.4.4.2. Shared `fixed_bin` sign estimator (ISO T.851 index 113). 3 round-trip tests (varied blocks, edge blocks, Kx conditioning sweep). | `tests/test_arith.c` 10/10 passes |
| **22c / 23b** ✅ | `decode_sof9` end-to-end path. `arith_dec_reset` at RSTn. Supports Nf=1 gray, Nf=3 4:4:4, Nf=3 4:2:0, DRI=0 and DRI>0, non-MCU-aligned dims. 18 libjpeg-turbo vectors generated via `cjpeg -arithmetic`. | 18/18 phase22 vectors bit-exact |

## 2. Deliverables (22a/22b)

| File | Purpose |
|---|---|
| `c_model/src/jpeg_types.h` | Added marker defs + `arith_dc_L/U/K[4]` on `jpeg_info_t` |
| `c_model/src/header_parser.c` | `parse_sof9/10/11` + `parse_dac`, wired into the marker dispatcher |
| `c_model/src/decoder.c` | UNSUP_SOF stub for SOF9/10/11 (prevents decode attempts until 22c) |
| `c_model/src/arith.h` | `arith_dec_dc_diff` prototype + `JPEG_DC_STAT_BINS` |
| `c_model/src/arith.c` | `arith_dec_dc_diff` implementation (port of libjpeg `decode_mcu_DC_first` inner block) |
| `c_model/tests/test_arith.c` | Matching `arith_enc_dc_diff` helper + three new round-trip tests |
| `docs/spec_phase22.md` | This file |

## 3. DC-difference coding (ISO F.1.4.4.1 recap)

### Statistics layout per DC table

`dc_stats[64]` (only 48 bytes actually accessed):

| Offset | Bins | Meaning |
|---|---|---|
| 0..3   | S0/SS/SP/SN | entry for `dc_context = 0`  (zero or undef) |
| 4..7   | S0/SS/SP/SN | entry for `dc_context = 4`  (small positive) |
| 8..11  | S0/SS/SP/SN | entry for `dc_context = 8`  (small negative) |
| 12..15 | S0/SS/SP/SN | entry for `dc_context = 12` (large positive) |
| 16..19 | S0/SS/SP/SN | entry for `dc_context = 16` (large negative) |
| 20..33 | X1..X14     | magnitude-category unary continuation bins |
| 34..47 | M2..M15     | one M-bin per magnitude category (all bits of the lower pattern share the same bin) |

### Decode flow (F.19–F.24)

1. Decode S0. If 0 → diff = 0, `dc_context := 0`; otherwise continue.
2. Decode SS → `sign` ∈ {0, 1}.
3. Decode SP or SN to get the first magnitude bit. If 0 → `m = 0` and
   `|diff| = 1`.
4. Otherwise walk `X1 → X14`, decoding one bit per category; `m` left-
   shifts with each "continue". Overflow at `m = 0x8000` = corrupt
   stream.
5. Derive the NEW `dc_context` from `m` vs `L / U`:
   - `m < 2^L / 2` → 0 (zero-diff category)
   - `m > 2^U / 2` → 12 + 4·sign (large diff)
   - else → 4 + 4·sign (small diff)
6. Read ⌊log₂ m⌋ M-bits at the shared bin `st + 14`, OR-ing into `v`.
7. `v := v + 1`; negate if `sign = 1`. Return `v`.

## 4. Public API (22b)

```c
/* c_model/src/arith.h */
#define JPEG_DC_STAT_BINS 64

int arith_dec_dc_diff(arith_decoder_t *d,
                      uint8_t  *dc_stats,
                      int      *dc_context,
                      int       L,
                      int       U,
                      int      *out_diff);
/* Returns 0 on success, -1 on magnitude overflow (corrupt stream). */
```

Caller is responsible for:

- allocating `dc_stats` (one 64-byte area per DC table, zero-initialised
  at scan start and at every RSTn),
- maintaining the per-component `dc_context` across blocks,
- accumulating `last_dc_val[ci] = (last_dc_val[ci] + diff) & 0xFFFF`
  and applying Al (point transform).

Phase 22c will drive this from a new `decode_mcu_sof9_dc_first` that
mirrors `decode_mcu_sof2_dc_first` but substitutes the arithmetic path.

## 5. Test matrix (22b)

`tests/test_arith.c` grows from 4 to 7 round-trip tests:

| # | Name | What it covers |
|---|---|---|
| 1 | `single_bin_adaptive`      | Phase 21 baseline — still passes (N=10 000) |
| 2 | `multi_bin_adaptive`       | Phase 21 (N=10 000 × 8 bins) |
| 3 | `skewed_bernoulli`         | Phase 21 (p=0.02, 7.3:1 compression) |
| 4 | `edge_cases`               | Phase 21 (zeros/ones/empty/stuffing) |
| 5 | `dc_diff_small_range`      | **Phase 22b** — exhaustive diffs −32..+32 (zero/±1/multi-X-loop), byte-wise stats + per-symbol context tracked |
| 6 | `dc_diff_random_12bit`     | **Phase 22b** — 4 000 random diffs in [−2047,+2047], avg bits/symbol logged |
| 7 | `dc_diff_conditioning`     | **Phase 22b** — 18 (L,U) pairs × 500 symbols, mixed zero + small + large diffs |

All seven pass on first green build.

## 6. Results (2026-04-21)

```
dc_diff_small_range  diffs=-32..+32   N=65    encoded=48  bytes
dc_diff_random_12bit N=4000           encoded=6268 bytes  (avg=12.54 bits/sym)
dc_diff_conditioning pairs=18         L∈0..3, U∈L..5, N=500 per pair
```

Encoder and decoder statistics tables match byte-for-byte after every
test, confirming symmetric adaptation. Compression for uniform 12-bit
random diffs sits at 12.54 bits/sample — close to the 12.0 theoretical
entropy for 12-bit uniform input plus ~0.5 bit finite-adaptation
overhead.

Cross-phase regression (smoke 12/12, phase25c 143/143, phase27
334/334) stays green after the header-parser and decoder-stub changes,
because arith markers were previously rejected by the catch-all SOF
fallback — now they parse but still return `UNSUP_SOF` downstream.

## 7. Results — 22c/23b end-to-end (2026-04-21)

### What landed

- `c_model/src/arith.h|c` — added `arith_dec_reset` (clears a/c/ct/
  unread_marker without rewinding the byte cursor; the caller wipes
  stats + dc_context + last_dc at RSTn boundaries).
- `c_model/src/decoder.c` — ~280 new lines:
  - `sof9_decode_block` — one-block DC diff + AC + dequant.
  - `sof9_consume_rst` — RSTn handshake (handles both "arith already
    latched the marker" and "marker not yet in the unread slot").
  - `sof9_find_eoi` — EOI framing at scan end.
  - `decode_sof9` — the glue: allocates DC/AC stats (4 tables each
    for `SOF9` per ISO), `fixed_bin` shared sign bin, runs the MCU
    loop for Nf∈{1,3} × sampling modes, dispatches RSTn every
    `Ri` MCUs, re-primes arith state + stats on each boundary.
- `c_model/tests/test_arith.c` — `fixed_bin` initialised to 113 in
  round-trip helpers so encoder/decoder stay symmetric *and*
  interoperable with libjpeg-turbo.
- `tools/gen_phase22.py` — drives `cjpeg -arithmetic` for 18 cases:
  6 gray + 6 4:4:4 + 6 4:2:0, sizes 8x8..128x96, q 50-80, DRI 0/4,
  including non-MCU-aligned (17x13). Round-trips each through djpeg
  as a libjpeg-turbo sanity gate.
- `verification/vectors/phase22/*.jpg` — 18 generated vectors.

### Parity diagnostic

First decode attempt against the 18 vectors yielded catastrophic
pixel divergence (~69 luma worst case). The cause was **not** in the
Q-coder, DC-diff, or AC-block math — all round-trip tests were
symmetric and passed. It was the **shared sign bin initial state**:

> libjpeg-turbo's `start_pass_huff_decoder` (despite the name, this
> arith entropy init) sets `entropy->fixed_bin[0] = 113;` — the
> ISO/T.851 **fixed-0.5** index from Table D.2 (Qe=0x5a1d,
> self-transitioning MPS/LPS). My initial `fixed_bin = 0` picked up
> the adaptive state-0 probability on the first sign bit, and from
> there the two streams diverge within the first MCU.

Fixing `fixed_bin = 113` at `decode_sof9` entry and at each
`arith_dec_reset` turned all 18 vectors bit-exact on the next run.
The round-trip tests also use 113 now — they still pass because they
are self-consistent, and they would have caught the parity bug
earlier had we seeded from a libjpeg-turbo-encoded reference block
instead of self-encoded data.

### Verification summary

```
c_model arith round-trips      : 10/10 PASS
phase22 golden_compare         : 18/18 bit-exact (Y=0 C=0)
full regression (p06..p27)     : 726/726 bit-exact, 0 skip
```

Coverage breakdown for phase22:

| Mode | Count | Sizes | Quality | DRI |
|---|---|---|---|---|
| gray    | 6 | 8x8, 16x16, 64x32, 17x13, 100x75, 128x96 | 50–80 | 0 or 4 |
| 4:4:4   | 6 | 8x8, 16x16, 32x32, 17x13, 100x75, 128x96 | 50–80 | 0 or 4 |
| 4:2:0   | 6 | 16x16, 32x32, 64x64, 17x13, 100x75, 128x96 | 50–80 | 0 or 4 |

## 8. Handoff to Phase 23c / 24

**23c (RTL SOF9)**: the C model gives a reference for the arith
entropy block. RTL needs a pipelined Q-coder (INITDEC + DECODE +
LPS/MPS migration state machine), DC-context classifier + statistics
SRAM (1 port, 48–64 bytes per table × 4 tables), AC decode FSM
(EOB / X-loop / magnitude), and a shared sign bin register. The MCU
loop reuses existing `block_sequencer` and connects to the same
coef buffer as baseline huffman.

**24 (SOF10, progressive + arithmetic)**: reuses `arith_dec_dc_diff`
and `arith_dec_ac_block` with progressive gating:
- DC-first scan: `Ah=0` → decode diff + shift by Al; `Ah>0` → 1-bit
  refinement at `fixed_bin`.
- AC-first scan: decode k ∈ [Ss..Se] per F.1.4.4.2 — `arith_dec_ac_block`
  already accepts `(Ss, Se)` bounds, so the only new code is EOBRUN
  handling at `fixed_bin` and the correlation-bin sharing that
  progressive uses.
- AC-refinement scan: F.1.4.4.2 step-by-step refinement with the
  low/high bands addressed the same way as sequential.
Plan: generator targets 4–6 libjpeg-turbo vectors per sub-mode,
gated on `golden_compare` coefficient-plane equality before pixel
equality.
