# Phase 27 — Lossless 2–16-bit precision (SOF3, P ∈ {2..16})

**Status**: ✅ C-model complete — 334/334 bit-exact (2026-04-21)
**Parent**: [`spec_phase25.md`](spec_phase25.md) (baseline lossless scope)
**Roadmap entry**: [`roadmap_v2.md`](roadmap_v2.md) Wave 5 Phase 27

---

## 1. Scope

Extends Phase 25a/b/c (SOF3 P=8 gray + RGB + DRI) to the full lossless
precision range allowed by ISO/IEC 10918-1 Annex H.1.2.1:

| Parameter | Value / range |
|---|---|
| SOF marker | `0xFFC3` (SOF3 Lossless, Huffman) |
| `P` (precision) | **2..16** (8 values new vs Phase 25) |
| `Nf` | 1 (gray) or 3 (RGB interleaved, Hi=Vi=1) |
| `Ps` | 1..7 (all seven predictor formulae, ISO Table H.1) |
| `Pt` | 0..15, with `Pt < P` |
| `Ss`, `Se` | `Ss = Ps`, `Se = 0` (Ah=0 since no successive approximation) |
| DRI | 0 or any multiple of MCUs-per-row |
| `Hi × Vi` | 1 × 1 for every component |

Out of scope (future phases):

- `Nf ∈ {2, 4}` (2-component, CMYK lossless) — extension if needed.
- SOF7 / SOF11 / SOF15 (hierarchical or arithmetic lossless) —
  Waves 4 & 6.
- Non-row-aligned DRI (ISO allows it; libjpeg-turbo does not, so our C
  model keeps the libjpeg constraint).

## 2. ISO Annex H recap — sample-domain arithmetic

The lossless predictor operates in the **sample domain** of bit-width
`P - Pt`. For every sample:

```
    sample = (Px + diff) mod 2^(P-Pt)   (wraps into unsigned P-Pt bits)
    output = sample << Pt               (left-shifted to P bits)
```

where `Px` is one of seven predictors formed from already-decoded
neighbours `Ra` (left), `Rb` (above), `Rc` (above-left) — see
`spec_phase25.md` §3 for the formula table. The **first row of every
restart interval** (and the first row of the scan) uses the
INITIAL_PREDICTOR1 / horizontal-only rule per libjpeg-turbo, regardless
of `Ps`, as described in `spec_phase25.md` §5.2.

## 3. Bit-width handling

| Path | Storage |
|---|---|
| Predictor-domain scratch (internal) | `uint16_t[W*H]` per component (always — covers P up to 16) |
| Output for `P ≤ 8` | `uint8_t` planes (`y_plane`, `cb_plane` = G, `cr_plane` = B) |
| Output for `P > 8` | `uint16_t` planes (`y_plane16`, `cb_plane16`, `cr_plane16`), same fields as Phase 13 DCT P=12 |

The decoder dispatch in `jpeg_decode`:

1. `sof_type == 3` → `decode_lossless` (this phase).
2. Else if `precision == 12` → `decode_p12` (Phase 13 DCT).
3. Else if `sof_type == 2` → `decode_progressive`.
4. Else baseline.

**Critical fix during Phase 27**: the original dispatch had the `P=12`
DCT check *before* the SOF3 check, so SOF3 P=12 streams were
incorrectly routed into the P=12 DCT decoder. Fix (decoder.c) reorders
so SOF3 is matched first.

## 4. Huffman extension for SSSS > 8

`huff_decode_lossless_diff` already handled SSSS ∈ [0, 16] per ISO
H.1.2.2 (Phase 25a). For SSSS = 16 the decoder returns the special
value `+32768` with no extra bits — only legal at P = 16. The existing
`bs_get_bits` handles `n ≤ 15` via `bs_extend`; size 16 is short-circuited.

## 5. Output-side scaling + mask

Output is produced with the one-line rule:

```c
    out_mask = (P == 16) ? 0xFFFF : ((1u << P) - 1u);
    out[i]   = ((int32_t)sample[i] << Pt) & out_mask;
```

The `& out_mask` both (a) clips when `sample << Pt` overflows a 16-bit
word at `P = 16, Pt > 0`, and (b) keeps the representation identical
between the internal predictor domain and the output for Pt = 0.

## 6. Golden reference (libjpeg-turbo API triad)

`golden_compare.c::libjpeg_decode_lossless` dispatches by precision:

| P range | libjpeg-turbo API | Sample type |
|---|---|---|
| 2..8  | `jpeg_read_scanlines`   | `JSAMPLE` (uint8)  |
| 9..12 | `jpeg12_read_scanlines` | `J12SAMPLE` (short) |
| 13..16 | `jpeg16_read_scanlines` | `J16SAMPLE` (uint16) |

The RGB path uses `JCS_RGB` (libjpeg preserves the Adobe-tagged RGB
stream untouched) and then de-interleaves into R/G/B planes.

For P > 8 the golden populates `libjpeg_ycc_t::y16 / cb16 / cr16`; the
existing P=12 DCT plane-compare loop was generalised from
`precision == 12` to `precision > 8`, letting it cover both Phase 13
DCT and Phase 27 lossless uint16 paths without further branching.

## 7. Test matrix (334 vectors)

`tools/gen_phase27.py` invokes `cjpeg -lossless Ps[,Pt] -precision P`
over 9 grids. Grids A-E cover the high-precision range P ∈ {9..16};
grids F-I extend down to the minimum ISO precision P=2.

| Grid | Combinations | # |
|---|---|---|
| A — P ∈ {9..16} × every Ps × 32×32 gray+RGB gradient | 8 × 7 × 2 | 112 |
| B — P ∈ {9..16} × Ps ∈ {1,4,7} × 32×32 noise | 8 × 3 × 2 | 48 |
| C — P ∈ {9,12,16} × Ps ∈ {1,7} × 48×32 gray check + RGB grad | 3 × 2 × 2 | 12 |
| D — P ∈ {9,12,16} × Ps ∈ {1,7} × Pt ∈ {1,2} × gray+RGB gradient | 3 × 2 × 2 × 2 | 24 |
| E — P ∈ {12,16} × Ps ∈ {1,4,7} × 2-row DRI × gray+RGB gradient | 2 × 3 × 2 | 12 |
| F — P ∈ {2..7} × every Ps × 32×32 gray+RGB gradient | 6 × 7 × 2 | 84 |
| G — P ∈ {2,4,7} × Ps ∈ {1,4,7} × 32×32 gray+RGB noise | 3 × 3 × 2 | 18 |
| H — P ∈ {4,7} × Ps ∈ {1,7} × Pt ∈ {1,2} × gray+RGB gradient | 2 × 2 × 2 × 2 | 16 |
| I — P ∈ {4,7} × Ps ∈ {1,7} × 2-row DRI × gray+RGB gradient | 2 × 2 × 2 | 8 |
| **Total** | | **334** |

Vectors are wrapped in 16-bit big-endian PGM/PPM with `maxval = 2^P - 1`
so cjpeg preserves the input sample values without rescaling (verified
via a one-off roundtrip before bulk generation). For `P ≤ 8` the
writer falls through to 8-bit binary PNM.

Low-precision sub-range (grids F-I) was added in a follow-up sweep
after the initial 208-vector matrix — cjpeg was verified to accept
every `-precision` value down to 2, and djpeg round-trips each.

## 8. Regression summary (2026-04-21)

```
phase06 … phase13        :  150 / 150    (waves 1-2, unchanged)
phase14 … phase18 + prog_dri :   75 / 75    (wave 3, unchanged)
phase25 / 25b / 25c      :  215 / 215    (lossless P=8, unchanged)
phase27                  :  334 / 334    (P ∈ {2..7, 9..16}; grids A..I)
smoke                    :   12 / 12
full                     : 1150 / 1150
TOTAL                    : 1936 / 1936    bit-exact
```

All pre-existing suites remain worst_diff Y=0 C=0 after the refactor of
`parse_sof_common` (narrow → `(p_min, p_max)` signature),
`decode_lossless` (uint8 internal → uint16 internal), and the
dispatch-reorder fix.

The low-P sweep (+126 vectors in grids F-I) passed bit-exact on the
first run — no code changes needed because `decode_lossless` already
handles P ∈ {2..16} with uint16 predictor-domain storage and P-bit
output masking, and the libjpeg-turbo golden picks the 8-bit API path
for P ≤ 8 automatically.

## 9. Debugging notes

**P=12 failure mass (38/208)** discovered mid-Phase-27: all SOF3 P=12
streams hit error 0x08 (BAD_HUFFMAN) or diff=2048 = 2^11 vs the golden.
Root cause was the `P == 12 → decode_p12` branch firing for SOF3
streams. Moving the `sof_type == 3 → decode_lossless` dispatch ahead of
the `P == 12 → decode_p12` dispatch fixed all 38 in one go.

**Initial predictor value** for `P=16, Pt=0` is `2^15 = 32768`. This is
exactly the same magnitude as the special SSSS=16 encoded diff, and
both are naturally handled by `uint16` predictor-domain storage with
`(Px + diff) & 0xFFFF`.

## 10. RTL hand-off (Phase 26 / 27 RTL)

Phase 27 is a pure C-model extension; no RTL changes. Phase 26 (Lossless
Huffman 1-16 sizes — RTL) and a future Phase 27-RTL will need to
extend the sample bus width to 16 bits per component and support the
predictor-domain mask + Pt post-shift shown in §5.
