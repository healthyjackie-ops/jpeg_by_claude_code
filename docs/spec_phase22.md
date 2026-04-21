# Phase 22 — SOF9 DC-arithmetic decode (ISO/IEC 10918-1 Annex F.1.4.4)

**Status**: 🟡 a+b landed — header parse + Q-coder DC-diff helper with
self-test (2026-04-21). Wave-4 DC-only end-to-end vs libjpeg-turbo
(Phase 22c) is the next step.

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
| **22c** | Wire DC decode into a real SOF9-path (`decode_sof9_dc_first`), pull libjpeg-turbo-compressed DC-only vectors via `golden_compare --arith`, land ≥10 bit-exact images (gray + 4:4:4 + 4:2:0). | new `phase22/` vectors |

22c is deferred to a later session so 22a/22b can push as a green
foundation.

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

## 7. Handoff to Phase 22c

Open items for 22c:

1. New file `c_model/src/decode_arith.c` with `decode_sof9(...)` that:
   - allocates `dc_stats[NUM_ARITH_TBLS][64]` (initialised to zero),
   - tracks `dc_context[Nf]` and `last_dc_val[Nf]` per scan,
   - drives `arith_dec_init` on the entropy-coded segment,
   - loops MCUs invoking `arith_dec_dc_diff` per component block,
   - resets on RSTn (clear stats tables + dc_context + last_dc_val).
2. Vector generator (`tools/gen_phase22.py`) calling
   `cjpeg -arithmetic -sample 1x1,1x1,1x1 …` for DC-only SOF9 test
   inputs (note: libjpeg-turbo's `cjpeg -arithmetic` produces full DCT,
   so DC-only isolation uses a progressive-style scan with `Ss=Se=0`,
   or we re-encode with custom scan scripts).
3. `golden_compare` arith branch: add libjpeg-turbo DC-only decode via
   `jpeg_start_decompress`/`read_coefficients` and compare DC planes.
4. Target: ≥10 bit-exact images across gray / 4:4:4 / 4:2:0 before
   landing Phase 22c.
