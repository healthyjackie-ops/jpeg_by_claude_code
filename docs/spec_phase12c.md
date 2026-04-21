# Phase 12c — CMYK (Nf=4) across SOF2 / SOF9 / SOF10

**Status**: ✅ **COMPLETE** — 51/51 phase12c bit-exact, 1495/1495 aggregate (2026-04-21).
**Parent specs**: [`spec_phase12.md`](spec_phase12.md), [`spec_phase17.md`](spec_phase17.md), [`spec_phase22.md`](spec_phase22.md), [`spec_phase24.md`](spec_phase24.md).
**Scope**: C model only. RTL deferred until the rest of the CMYK rollout reaches parity.

## 0. 目标

Phase 12 baseline 已覆盖 **SOF0 + CMYK (Nf=4, 全 1×1)**。随后 Phase 17a..d 把 SOF2 progressive Huffman 的 chroma 覆盖推到 gray / 4:4:4 / 4:2:0 / 4:2:2 / 4:4:0 / 4:1:1，Phase 22c/23b 和 Phase 24a/c 在 arith 通路做了对称覆盖 (SOF9/SOF10)。**但三条 entropy-coded DCT 通路 (SOF2/9/10) 都还没接 Nf=4**。

Phase 12c 把 CMYK 补到 **SOF2 + SOF9 + SOF10** 三条通路：

| Path | SOF | Entropy | Frame structure |
|---|---|---|---|
| 1 | SOF2  | Huffman    | progressive multi-scan |
| 2 | SOF9  | Arithmetic | sequential |
| 3 | SOF10 | Arithmetic | progressive multi-scan |

验收：与 libjpeg-turbo 3.1.3 对 CMYK SOF2/9/10 图像的 4-plane 解码 **bit-exact**，且 Phase 6..27 + Phase 12 + Phase 17d + Phase 24c 零退步。

## 1. 格式约束

CMYK 在三条通路里的 MCU / 扫描几何都与 Phase 12 SOF0 一致：
- `Nf = 4`, 组件 id = 1..4
- 全部 `Hi = Vi = 1` (唯一生产级 CMYK 布局；非 1×1 CMYK 超出本期)
- MCU 8×8, 每 MCU 含 4 blocks (按 C → M → Y → K 扫描顺序)
- Adobe APP14 color_transform = 0 ⇒ libjpeg-turbo 识别为 `JCS_CMYK`

SOF2 可带任意多扫描 (DC-first / AC-first / AC-refine / DC-refine)；SOF10 同样；SOF9 是单 scan sequential。所有通路都允许 `DRI>0` 的 restart markers。

## 2. C model 改动 (`c_model/src/decoder.c`)

### 2.1 `decode_progressive` (SOF2)

已在前期被扩展为 `{gray, 444, 420, 422, 440, 411}` 多分支结构。本期补 `is_cmyk` 第 7 条支路：

1. **Accept gate** 追加 `is_cmyk`；`num_comps = is_gray ? 1 : (is_cmyk ? 4 : 3)`；`cg[3] → cg[4]`。
2. **`cg[c]` 自然块网格** CMYK 分支：4 组件全部用 `mcu_rows × mcu_cols` (blk_rows/cols) 加 `yh_nat / yw_nat` (nat_rows/cols)，与 Phase 17d 的 4:4:4 分支同构。
3. **DC-first MCU 交错** 新增 CMYK 分支，循环 4 组件按 id 顺序调 `huff_decode_dc_progressive` / `huff_decode_dc_refine`。
4. **Drain**：pad buffer 沿用 Phase 12 baseline 的惯例 (y_pad ← C，另建 m_pad / yc_pad / k_pad)；输出 plane 映射 c_plane / m_plane / y_plane_cmyk / k_plane；`alloc_ok` 追加 `is_cmyk` 嵌套三元；AC 非交错扫描循环无需改动 (已经按 `cg[c]` 遍历)。
5. **Crop** 分 `is_cmyk ? (4-plane memcpy) : (1-or-3 plane memcpy)`，再补 4 次 free。

### 2.2 `decode_sof9` (SOF9)

SOF9 是 sequential，`td[] / ta[] / qt[]` 由 `[3] → [4]`，新增 `k_blk[64]` scratch。新 CMYK MCU 分支把 4 block 依次送入 `sof9_decode_block` 后 IDCT：
- block 0 → C (y_blk[0] 复用槽位)
- block 1 → M (cb_blk)
- block 2 → Y (cr_blk)
- block 3 → K (k_blk)

每 block IDCT 后 `copy_block_8x8` 到对应 pad (y_pad / m_pad / yc_pad / k_pad)。DRI restart 沿用原有 `arith_dec_flush + arith_dec_init` + 4-slot `last_dc[] = 0` 复位。

### 2.3 `decode_sof10` (SOF10)

SOF10 与 SOF2 同构 (多 scan + coef_buf)，差异在 DC-first 调 `sof10_dc_first_block` + `dc_stats`，AC scan 调 `sof10_ac_scan_block`。CMYK 分支在所有 5 步上与 SOF2 一致：accept gate、`cg[4]`、DC-first 4 组件循环、drain (pads + planes + alloc_ok + free)、crop。

## 3. 向量生成

PIL 的 `img.save(..., arithmetic=True)` 在 Pillow 的 libjpeg 绑定里被静默忽略 (实测产出仍是 SOF0)。因此 SOF9/SOF10 走 jpegtran 转码：

```
PIL  →  baseline CMYK (SOF0, Huffman)
jpegtran -arithmetic                  →  SOF9  (sequential arith)
jpegtran -arithmetic -progressive     →  SOF10 (progressive arith)
[可选] -restart N                     →  添加 DRI
```

SOF2 则用 PIL 的 `progressive=True` 直接产出，再视需要 `jpegtran -restart N` post-process。

### 3.1 生成脚本

| 脚本 | 目标 | 数量 |
|---|---|---|
| `tools/gen_phase12c_prog.py`  | SOF2 progressive Huffman CMYK | 17 |
| `tools/gen_phase12c_arith.py` | SOF9 + SOF10 CMYK (同批源图双转码) | 17 + 17 |

合计 **51 张**，输出到 `verification/vectors/phase12c_{prog,sof9,sof10}/`。每张经 `djpeg` 往返校验。

### 3.2 用例矩阵 (17 size × pattern × q × DRI)

```
MCU-aligned:       8×8, 16×16, 32×32, 64×64, 128×64
non-aligned:       9×9, 23×17, 57×39, 97×113, 241×321
DRI sweep:         64×64 r=1, 96×96 r=4, 128×128 r=16, 241×321 r=32
aspect-ratio sanity: 199×257, 48×200 r=2, 200×48 r=8
```

质量覆盖 50–85，图案 `grad / check / noise`。DRI ∈ {0, 1, 2, 4, 8, 16, 32} MCUs。

## 4. 验收

- **51/51** Phase 12c 向量 bit-exact vs libjpeg-turbo 3.1.3：
  - `phase12c_prog`  (SOF2 progressive Huffman CMYK): 17/17
  - `phase12c_sof9`  (SOF9 sequential arith CMYK):   17/17
  - `phase12c_sof10` (SOF10 progressive arith CMYK): 17/17
- **Aggregate regression 1495/1495** (Y=0 C=0 worst-diff)：
  - full (baseline 回归包): 1150
  - phase22 (SOF9 gray/444/420): 18
  - phase24 (SOF10 gray/444/420): 33
  - phase24c (SOF9/SOF10 422/440/411): 162
  - phase17d (SOF2 422/440/411): 81
  - phase12c_prog: 17
  - phase12c_sof9: 17
  - phase12c_sof10: 17
- Unit tests (`make test`): ALL TESTS PASSED
- Build clean under `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`

## 5. 交付

- `c_model/src/decoder.c` — SOF2 / SOF9 / SOF10 三函数 CMYK 分支 (+350 / −141 行)
- `tools/gen_phase12c_prog.py` — PIL 直接产 SOF2 + jpegtran 可选 DRI
- `tools/gen_phase12c_arith.py` — PIL 产 SOF0 + jpegtran -arithmetic [-progressive] 可选 DRI
- `verification/vectors/phase12c_prog/*.jpg` × 17
- `verification/vectors/phase12c_sof9/*.jpg` × 17
- `verification/vectors/phase12c_sof10/*.jpg` × 17

## 6. 完工后剩余 CMYK / 高精度空缺

Phase 12c 落完后，CMYK Nf=4 已经覆盖 SOF0/1/2/3/9/10 六条通路（SOF1 复用 SOF0 accept path；SOF3 lossless 天然按 Nf 通用）。尚未覆盖：

- **P=12** 任何 entropy-coded DCT 通路 (SOF1/2/9/10 都 assert P=8；SOF3 lossless 已经 P∈{2..16})
- **SOF5/6/7** DCT hierarchical
- **SOF13/14/15** arith hierarchical
- **SOF11** lossless + arith (被 libjpeg-turbo 3.1.3 参考端 block；Wave 5 待迁至 libjpeg-9 参考)

这些都不阻塞 Wave-1..4 完成路径，按 roadmap_v2.md 节奏继续推进。

## 7. 实现备注

- 复用了 Phase 24c/17d 的 `cg[c]` natural-block-grid 抽象 — 只需把 `[3] → [4]`，AC 非交错扫描循环完全 mode-agnostic。
- SOF9 没有 `cg[]`(它是 sequential，不依赖全局 block-base 表)，只需把 `td/ta/qt` 数组加到 4，再复用 Phase 12 baseline 的 pad/plane 映射惯例。
- 三条路径的 drain 都沿用 Phase 12 baseline 定义的 pad 命名 (`y_pad` 存 C，其余 m/yc/k)，避免重新设计 output-plane 布局。
- 首轮就 51/51 bit-exact，无 bring-up 问题 — 说明前期 (Phase 17d + 24c) 在几何泛化上投入的结构性重构在 Nf=4 上自然成立。
