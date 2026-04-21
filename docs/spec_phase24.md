# Phase 24a/24c — SOF10 progressive-arithmetic decode (ISO/IEC 10918-1 Annex F.1.4.4.2)

**Status**: ✅ **COMPLETE**
- 24a: 33/33 SOF10 vectors bit-exact (gray + 4:4:4 + 4:2:0)
- 24c: 162/162 SOF9/SOF10 non-420 chroma vectors bit-exact (4:2:2 + 4:4:0 + 4:1:1)
- 1363/1363 full regression (2026-04-21).

**Parent**: [`roadmap_v2.md`](roadmap_v2.md) Wave 4 Phase 24
**Prereqs**:
- [Phase 21](spec_phase21.md) (Q-coder core)
- [Phase 22/23](spec_phase22.md) (DC-diff helper, AC-block helper, SOF9
  end-to-end)
- [Phase 17/18](spec_phase17.md) (SOF2 progressive Huffman — coef_buf
  layout + drain path reused verbatim)

---

## 1. Scope

Deliver the **progressive arithmetic (SOF10)** C-model decode path. The
SOF10 frame shares its component / block / MCU layout with SOF2
(progressive Huffman), but each scan's entropy coding is the Q-coder
from Phase 21 instead of canonical Huffman. Four scan types per ISO
Annex F.1.4.4:

| Scan type | Params | Per-block work |
|---|---|---|
| **DC-first**  | `Ss=0, Se=0, Ah=0` | interleaved (Ns=num_comps). One `arith_dec_dc_diff` per block; coef[0] = `last_dc << Al`. |
| **DC-refine** | `Ss=0, Se=0, Ah>0` | interleaved. One `fixed_bin` bit per block; if 1, OR `(1<<Al)` into coef[0]. |
| **AC-first**  | `Ss>0, Ah=0`       | non-interleaved (Ns=1). Decode band k∈[Ss..Se] via `arith_dec_ac_block` with the SOS `Al` shift. |
| **AC-refine** | `Ss>0, Ah>0`       | non-interleaved. In-place refinement via new `arith_dec_ac_refine` helper. |

cjpeg's default progressive script emits **6 scans per component**:
DC-first (Al=1) → AC band 1..5 (Al=2) → AC band 6..63 (Al=2) →
AC-refine 1..63 (Ah=2 Al=1) → DC-refine (Ah=1 Al=0) →
AC-refine 1..63 (Ah=1 Al=0). So each vector exercises all four scan
types.

## 2. Deliverables

| File | Purpose |
|---|---|
| `c_model/src/arith.h` | `arith_dec_ac_block` signature extended with `int Al`; new `arith_dec_ac_refine` prototype |
| `c_model/src/arith.c` | `arith_dec_ac_block` applies `v << Al` on coef write; new `arith_dec_ac_refine` (port of libjpeg-turbo `decode_mcu_AC_refine`) |
| `c_model/src/header_parser.c` | `parse_sos` accepts SOF10 progressive SOS params (same branch as SOF2); `jpeg_parse_between_scans` handles DAC markers |
| `c_model/src/decoder.c` | `decode_sof10` — full scan loop + coef_buf + drain path (reuses SOF2's IDCT/chroma/crop pipeline) |
| `c_model/tests/test_arith.c` | 3 call-sites pass `Al=0` for backward-compat |
| `tools/gen_phase24.py` | Generates 18 `cjpeg -progressive -arithmetic` vectors (gray + 4:4:4 + 4:2:0 × grad/check/noise × 6 sizes × DRI 0/4) |
| `verification/vectors/phase24/*.jpg` | Generated vector set (18 files) |
| `docs/spec_phase24.md` | This file |
| `docs/roadmap_v2.md` | Phase 24a row marked ✅ |

## 3. AC-refine coding (ISO F.1.4.4.2 refinement path recap)

Per scan, p1 = `1<<Al`, m1 = `-(1<<Al)`. `kex` = largest k ∈ [1..Se]
whose coef is nonzero (0 if block all-zero before this scan).

```
for k = Ss .. Se:
    st = ac_stats + 3*(k-1)
    if k > kex:
        if decode(st) == 1: break      # EOB
    loop:
        if coef[k] != 0:                # previously nonzero
            if decode(st+2) == 1:
                coef[k] += (coef[k] > 0) ? p1 : m1
            break
        if decode(st+1) == 1:           # newly-nonzero
            coef[k] = decode(fixed_bin) ? m1 : p1
            break
        st += 3; k++                    # still zero — advance
        if k > Se: return -1            # spectral overflow
```

This is a faithful port of `libjpeg-turbo/src/jdarith.c:decode_mcu_AC_refine`.
The new helper in `c_model/src/arith.c` is exactly 60 lines and reuses
the zigzag table already defined for `arith_dec_ac_block`.

## 4. Scan-level stats lifecycle (mirrors libjpeg-turbo `start_pass`)

| Event | fixed_bin[0] | dc_stats | ac_stats | dc_context | last_dc |
|---|---|---|---|---|---|
| `jinit` (first time) | ←113 | zero | zero | zero | zero |
| SOS DC-first (Ss=0, Ah=0) | untouched | zero for scan tables | untouched | zero for scan comps | zero for scan comps |
| SOS DC-refine (Ss=0, Ah>0) | untouched | untouched | untouched | untouched | untouched |
| SOS AC-first / AC-refine (Ss>0) | untouched | untouched | zero for scan tables | untouched | untouched |
| RSTn inside DC-first scan | untouched | zero for scan tables | untouched | zero for scan comps | zero for scan comps |
| RSTn inside AC scan | untouched | untouched | zero for scan tables | untouched | untouched |

**Key**: the `fixed_bin[0] = 113` seed is a T.851 self-transitioning
state — its Qe never changes. libjpeg-turbo still seeds it once at
jinit and never resets it thereafter, and we match that byte-for-byte.

Our C-model `decode_sof10` implements this via two helpers:
- `sof10_reset_dc_scan` — zero dc_stats[tbl] for every table in SOS,
  zero dc_context[ci] and last_dc[ci] for every scan component.
- `sof10_reset_ac_scan` — zero ac_stats[tbl] for every table in SOS.

Both are called unconditionally at scan start (DC vs AC branches
clearly) AND at RSTn boundaries (via `sof9_consume_rst` + `arith_dec_reset`).

## 5. Byte-source ↔ bitstream sync

Each scan re-primes arith against the remaining byte stream:
```c
arith_decoder_t ad;
arith_dec_init(&ad, bs->data + bs->byte_pos, bs->size - bs->byte_pos);
```
At scan end, `sof10_scan_end_sync` pulls the terminating marker out of
`ad.unread_marker` (or scans ahead if arith stopped exactly on a byte
boundary), then:
```c
bs->byte_pos += ad.pos;
bs->marker_pending = 1;
bs->last_marker    = end_marker;
int r = jpeg_parse_between_scans(bs, info, &e);
```

**Between-scan DAC**: cjpeg re-emits DAC before every progressive-arith
scan to restate conditioning (even when it doesn't change). Phase 24a
adds a `case MARKER_DAC:` to `jpeg_parse_between_scans` that forwards
to `parse_dac`; this was the **only** change needed to the between-scan
loop beyond what Phase 17a already supported.

## 6. Arith core extension — `Al` parameter on AC-first

SOF9 sequential arith writes coefs directly (`block[...] = v`). SOF10
AC-first writes `v << Al` per ISO F.1.4.4 successive-approximation
encoding. The Phase 24a landing extends `arith_dec_ac_block`:

```c
int arith_dec_ac_block(arith_decoder_t *d,
                       uint8_t *ac_stats,
                       uint8_t *fixed_bin,
                       int Kx,
                       int Ss, int Se,
                       int Al,                 /* << NEW */
                       int16_t *block);
```

All previous callers (sequential SOF9 + unit tests) pass `Al=0` and are
byte-for-byte identical to the pre-extension version. The 10/10 arith
unit tests and 726/726 phase22 + smoke regression confirmed no
behavioural drift at the `Al=0` path.

## 7. Results — Phase 24a end-to-end

33 libjpeg-turbo vectors generated by `tools/gen_phase24.py`:

- **11 gray** (Nf=1): grad/check/noise × sizes {8×8, 16×16, 64×32, 17×13,
  100×75, 128×96, 160×120, 200×150, 256×256, 320×200}
  × DRI ∈ {0, 1, 4, 8, 16} × quality ∈ {30, 50, 60, 75, 80, 85, 90}
- **11 4:4:4** (Nf=3): same pattern
- **11 4:2:0** (Nf=3): same pattern, starting at 16×16

Each file uses cjpeg's default 6-scan progressive script, so every
vector exercises all 4 scan types (DC-first, AC-first, AC-refine,
DC-refine) against our decoder.

**33/33 pixel-exact** vs libjpeg-turbo's reference decode.

The initial 18-vector set (q 50-80, DRI 0/4, ≤128×96) landed first.
The hardening pass added 15 aggressive vectors — larger dimensions
(256×256, 320×200), DRI=1 (restart every MCU — stresses arith re-prime
per block), DRI=16, and extreme quality (q30 → many-EOB, q90 → long
coef runs) — to harden the arith state-machine + bs-sync contract.

Full regression:
- Phase 6-18 + phase_prog_dri + smoke: 1150/1150 ✅
- Phase 22 (SOF9 sequential arith): 18/18 ✅
- Phase 24 (SOF10 progressive arith): 33/33 ✅
- Combined: **1201/1201 bit-exact**, Worst diff Y=0 C=0.

## 8. Debug trail (first-case bring-up)

First SOF10 test (8×8 gray) failed with `err=0x01` (UNSUP_SOF) because
`parse_sos` at `header_parser.c:380` had a blanket `else if (sof_type != 2)`
guard that rejected SOF10's `Ss=0 Se=0 Ah_al=01` progressive-DC params.
Fixed by extending both Ns-validation and spectral-validation to treat
SOF10 the same as SOF2 via
`is_progressive = (sof_type == 2 || sof_type == 10)`.

Second failure `err=0x10` (BAD_MARKER) — traced via PROG_DBG stderr
prints in `decode_sof10`:
```
[sof10] scan#0 end: ad.pos=5 ad.unread_marker=0xcc
```
0xCC = DAC. `jpeg_parse_between_scans`'s switch had no case for DAC, so
it hit `default → BAD_MARKER`. Added `case MARKER_DAC: parse_dac(...)`.
After this, the 8×8 gray case passed with all 6 scans clean.

Then 18/18 on first try — no more bring-up issues for 4:4:4 / 4:2:0 /
non-MCU-aligned / DRI variants.

## 9. Handoff to Phase 24b (RTL) / Phase 28 (SOF11 lossless+arith)

**Phase 24b (RTL integration)** will layer the progressive coef buffer
(already wired in Phase 15 + 18) with the arith entropy DC/AC path
(Phase 23c). The C-model's `decode_sof10` defines the per-scan stats
lifecycle and bs-sync contract; the RTL follows the same handshake:
- arith unit re-primed per SOS + RSTn
- stats SRAM region per (DC/AC × table × scan-type) cleared by
  `start_pass`-equivalent state transitions
- coef_buf read-modify-write for the refine paths (matches SOF2 refine)

**Phase 28 (SOF11 lossless + arith)** is currently **blocked**:
libjpeg-turbo 3.1.3 does not support lossless + arith in either
direction. `cjpeg -lossless -arithmetic` fails with "Requested feature
was omitted at compile time", and the decoder's `jdmaster.c` emits
`JERR_ARITH_NOTIMPL` when arith coding is combined with lossless mode.

Options to unblock Phase 28:
1. **Port libjpeg-9 lossless-arith path** — the canonical IJG reference
   in `jdlossls.c`/`jdarith.c`. Requires merging that path into a local
   patched libjpeg-turbo build.
2. **Self-encode + self-decode** round-trip test — use our validated
   `arith_dec_dc_diff` helper (Phase 22b, 7/7 tests) with a new encoder
   mirror + SOF3-derived lossless predictor. Validates internal
   consistency but not third-party bit-exact.
3. **Skip Phase 28** — SOF11 has near-zero deployed corpus; the IP can
   ship without it and pick it up in a future wave if customer need
   emerges.

**Related deferred work** — these are decoder capabilities the arith
C-model could extend but are not yet wired:
- **SOF2 P=12** (progressive Huffman 12-bit) — `decode_progressive`
  currently asserts P=8.
- **SOF9 P=12** (sequential arith 12-bit) — gated by explicit check in
  `decode_sof9`.
- **SOF10 P=12** (progressive arith 12-bit) — gated by explicit check
  in `decode_sof10`.
- **4:2:2 / 4:4:0 / 4:1:1** in any SOF2/9/10 path — gated to
  gray/4:4:4/4:2:0 only (matches Phase 17a scope).

All of these have libjpeg-turbo reference support, so they are
unblockable future extensions (not blocked like SOF11).

## 10. Phase 24c — SOF9/SOF10 extended chroma (4:2:2 / 4:4:0 / 4:1:1)

Phases 22c/23b (SOF9) and 24a (SOF10) landed gray / 4:4:4 / 4:2:0 only.
Phase 24c folds in the three remaining 3-comp YCbCr chroma layouts that
the baseline Huffman path has supported since Phase 10/11. cjpeg
supports all three with both `-arithmetic` and `-progressive -arithmetic`.

| Mode | Sampling | MCU | Y blocks | Cb/Cr blocks |
|---|---|---|---|---|
| 4:2:2 | Y 2×1, chroma 1×1 | 16×8  | 2 (horizontal) | 1 each |
| 4:4:0 | Y 1×2, chroma 1×1 | 8×16  | 2 (vertical)   | 1 each |
| 4:1:1 | Y 4×1, chroma 1×1 | 32×8  | 4 (horizontal) | 1 each |

### Changes to `decode_sof9`

1. **Accept gate** — extend from `gray/444/420` to also accept
   `422/440/411`.
2. **MCU footprint** — match baseline:
   ```
   mcu_w = is_411 ? 32 : ((is_420 || is_422) ? 16 : 8);
   mcu_h = (is_420 || is_440) ? 16 : 8;
   ```
3. **Sub-sampled chroma pad dims** — match baseline:
   ```
   CWp_sub = is_411 ? Wp>>2 : ((is_420 || is_422) ? Wp>>1 : Wp);
   CHp_sub = (is_420 || is_440) ? Hp>>1 : Hp;
   ```
4. **MCU decode loop** — three new branches (`is_422`, `is_440`, `is_411`)
   mirroring the baseline decode layout but calling `sof9_decode_block`
   with the scan's arith tables. Same per-component td/ta/qt pointers
   and same idct_islow → copy_block_8x8 sequence.
5. **Chroma upsample** — three new cases: horizontal-only (4:2:2),
   horizontal-4x (4:1:1), vertical-only (4:4:0).
6. **Sub-res plane copy** — three new cases emitting
   `out->cb_plane_422 / _440 / _411` (and `cr_` equivalents) so the
   golden_compare can diff against libjpeg raw component output.

### Changes to `decode_sof10`

Same five changes, plus:

7. **`cg[c]` natural block grid setup** — per-sampling layout. E.g. for
   4:2:2: `cg[0].blk_rows = mcu_rows, blk_cols = mcu_cols*2` (Y is 2×1
   per MCU); Cb/Cr use `cw_nat × yh_nat`. The generic AC scan loop then
   walks each component's nat_rows × nat_cols grid without further
   per-mode code.
8. **DC-first MCU loop** — three new per-sampling branches, each
   emitting `sof10_dc_first_block` calls in the ISO-mandated interleave
   order (Y blocks first in natural scan, then Cb, then Cr). RSTn and
   refine paths reuse the existing 420 template.
9. **Drain pad/stride** — route `c ≥ 1` through `cb_pad_sub / cr_pad_sub`
   whenever `is_chroma_sub` instead of only when `is_420`.

### Vectors

`tools/gen_phase24c.py` generates 162 vectors:
- 3 chroma modes × 3 patterns (grad/check/noise) × 9 size/quality/DRI
  combinations × {SOF9, SOF10}
- Sizes span 17×16 (non-MCU-aligned) → 320×200
- DRI ∈ {0, 1, 4, 8} to exercise arith re-prime per MCU-count
- Quality ∈ {30, 60, 70, 75, 80, 85, 90}

**162/162 pixel-exact vs libjpeg-turbo** on first try, no bring-up issues
beyond the straightforward branch replication. The coef_buf + cg[c]
abstraction from Phase 24a already generalized the AC-scan path —
only the DC-first MCU interleave and drain upsample/copy needed new
code.

### Regression

- `make test`: ALL TESTS PASSED
- `full` (phase06-18 legacy): 1150/1150 Y=0 C=0
- `phase22` (SOF9 arith 420/444/gray): 18/18
- `phase24` (SOF10 arith 420/444/gray): 33/33
- `phase24c` (SOF9/SOF10 arith 422/440/411): 162/162
- `phase_prog_dri`: 20/20
- **Combined 1363/1363 bit-exact**

### Remaining gaps

After Phase 24c, the arith C-model covers **all 5 common 8-bit 3-comp
chroma layouts** for both sequential (SOF9) and progressive (SOF10).
Still deferred (unblockable future work):
- **CMYK (Nf=4)** in any SOF9/10 path
- **P=12** in any SOF2/9/10 path (1050-line decode_progressive
  refactor)

SOF11 lossless+arith remains **blocked** by libjpeg-turbo tooling.
