# Phase 24a — SOF10 progressive-arithmetic decode (ISO/IEC 10918-1 Annex F.1.4.4.2)

**Status**: ✅ **COMPLETE** — 18/18 phase24 vectors bit-exact vs
libjpeg-turbo, 1186/1186 全回归零退步 (2026-04-21).

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

18 libjpeg-turbo vectors generated by `tools/gen_phase24.py`:

- 6 gray (Nf=1): grad/check/noise × {8×8, 16×16, 64×32, 17×13, 100×75,
  128×96+DRI=4}
- 6 4:4:4 (Nf=3): grad/check/noise × {8×8, 16×16, 32×32, 17×13, 100×75,
  128×96+DRI=4}
- 6 4:2:0 (Nf=3): grad/check/noise × {16×16, 32×32, 64×64, 17×13,
  100×75, 128×96+DRI=4}

Each file uses cjpeg's default 6-scan progressive script, so every
vector exercises all 4 scan types (DC-first, AC-first, AC-refine,
DC-refine) against our decoder.

**18/18 pixel-exact** vs libjpeg-turbo's reference decode.

Full regression:
- Phase 6-18 + phase_prog_dri + smoke: 1150/1150 ✅
- Phase 22 (SOF9 sequential arith): 18/18 ✅
- Phase 24 (SOF10 progressive arith): 18/18 ✅
- Combined: **1186/1186 bit-exact**, Worst diff Y=0 C=0.

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

**Phase 28 (SOF11 lossless + arith)** shares the arith core but uses
the lossless predictor pipeline from Phase 25a+. No AC coding path
(SOF11 is 1 coef per pixel); only the DC-diff helper is needed.
