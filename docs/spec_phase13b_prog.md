# Phase 13b-prog — P=12 in SOF2 progressive Huffman

**Status**: ✅ **COMPLETE** — 20/20 phase13b_prog bit-exact, 1515/1515 aggregate (2026-04-21).
**Parent specs**: [`spec_phase13.md`](spec_phase13.md), [`spec_phase17.md`](spec_phase17.md), [`spec_phase12c.md`](spec_phase12c.md).
**Scope**: C model only. RTL deferred alongside the rest of the P=12 rollout (Phase 13b/c roadmap rows).

## 0. 目标

Phase 13a 把 P=12 落到 SOF0 + SOF1 (`decode_p12`) 并通过 20/20 bit-exact 验收。残留的 P=12 空缺在 entropy-coded DCT 的三条 `decode_*` 上：SOF2 progressive Huffman / SOF9 sequential arith / SOF10 progressive arith。Phase 13b-prog 收掉其中第一条 — **SOF2 + P=12 for gray / 4:4:4 / 4:2:0**。

与 Phase 13a 一致，CMYK + P=12 与 chroma 扩集 (4:2:2 / 4:4:0 / 4:1:1) + P=12 继续延后，保持 "先窄深再宽扩" 的节奏。

## 1. 为什么这次改动是极小面积

libjpeg-turbo 的 `JCOEF` 对 P=8 和 P=12 都是 `int16`，所以 Phase 17 系列扫描期累积到 `coef_buf` 的数据类型对两档精度共用同一份代码路径。`huff_decode_dc_progressive` / `huff_decode_ac_progressive` / `huff_decode_dc_refine` / `huff_decode_ac_refine` / MCU 交错调度 / SOS 解析 / restart interval reset 全部与精度无关 — 改动被精确夹在两端：

- **入口**：SOF2 解析 + 精度 gate 接受 P=12
- **出口**：drain (dequant → IDCT → pad → chroma upsample → crop) 派生一份 uint16 并行通路

scan-time (entropy + RLE + zigzag + coef accumulation) 原封保留，规避一切对 17d/12c 已验证通路的回归。

## 2. C model 改动

### 2.1 `c_model/src/header_parser.c`

`parse_sof2` 从 `parse_sof_common(..., 8, 8)` 放宽到 `parse_sof_common(..., 8, 12)`。SOF2 + P=12 的 frame header 与 P=8 完全同构 (Pq=1 的 DQT 已经在 Phase 13a 支持)，放宽 precision 验收门即可。

### 2.2 `c_model/src/decoder.c`

三处编辑：

#### 2.2.1 精度 gate (`decode_progressive` 头部)

```c
if (info->precision != 8 && info->precision != 12) {
    out->err = JPEG_ERR_UNSUP_PREC;
    return -1;
}
if (info->precision == 12 && !(is_gray || is_444 || is_420)) {
    out->err = JPEG_ERR_UNSUP_PREC;
    return -1;
}
```

P=12 暂只开 gray/444/420 三种 chroma，与 Phase 13a 的覆盖面完全对齐。CMYK + P=12 和 4:2:2/4:4:0/4:1:1 + P=12 走 future work。

#### 2.2.2 P=12 drain 分支 (`decode_progressive` 尾部，EOI 校验之后、P=8 drain 之前)

约 95 行的早退分支，保留 P=8 drain 完全不被触及：

```c
if (info->precision == 12) {
    out->precision = 12;
    /* 1. 分配 uint16 pad buffers (Y 全分辨率; Cb/Cr 按 chroma 粒度) */
    /* 2. 分配 out->y_plane16 + (非 gray ? cb_plane16/cr_plane16) + (420 ? cb_plane16_420/cr_plane16_420) */
    /* 3. 逐 component 逐 block: dequant_block_i32 → idct_islow_p12 → copy_block_8x8_u16 到对应 pad (步幅 = MCU-padded blk_cols*8) */
    /* 4. 4:2:0 时对 cb_pad16_sub/cr_pad16_sub 做 2×2 最近邻 uint16 upsample 到 cb_pad16/cr_pad16 */
    /* 5. Crop: 按每行 W*sizeof(uint16_t) 从 pad 拷贝到 *_plane16 (420 分支额外填 *_plane16_420) */
    free(coef_buf); free(pads); return 0;
}
```

关键细节：
- **复用 Phase 13a 的构件**：`dequant_block_i32` (int32 coef + uint16 QT)、`idct_islow_p12` (`PASS1_BITS_P12=1`, int64 中间、uint16 输出)、`copy_block_8x8_u16`
- **pad 步幅**：沿用 `cg[c].blk_cols * 8`，即 MCU-padded 步幅，与 Phase 17 系列保持一致
- **nat-dim crop**：用 `cg[c].nat_cols / nat_rows`，对应 ISO A.2.3 non-interleaved AC scan 定义的自然维度
- **output plane 二元布局**：gray 只填 `y_plane16`；4:4:4 / 4:2:0 同时填 `y_plane16 + cb_plane16 + cr_plane16`；4:2:0 额外填 `cb_plane16_420 / cr_plane16_420` (sub-resolution 半分辨率平面)

#### 2.2.3 `jpeg_decode` dispatch 排序修正 (**关键**)

**Before**:

```c
if (info.precision == 12) return decode_p12(...);   /* SOF0/SOF1 only */
if (sof_type == 2)        return decode_progressive(...);
```

P=12 的 SOF2 被前一条先截住，进入 `decode_p12` 又在里面碰到 SOF2 → BAD_HUFFMAN。

**After**:

```c
if (sof_type == 2)        return decode_progressive(...);   /* 自己处理 P∈{8,12} */
if (info.precision == 12) return decode_p12(...);
```

`decode_progressive` 现在自带 P=12 drain，dispatch 层把 SOF2 优先路由给它是正确抽象层。注释同步更新说明该职责分配。

## 3. 验证向量 (`tools/gen_phase13b_prog.py`)

`cjpeg -precision 12 -progressive -quality Q -optimize` 的默认 progressive script (不带 `-scans`) 会发出全套 scan 类型 (DC-first / AC-first / AC-refine / DC-refine)，单次 encode 就把 SOF2 四种 scan 路径全部踩一遍 — 沿用 Phase 17d 策略。

输入用 16-bit Netpbm (P5 PGM / P6 PPM，maxval 65535，big-endian)，`cjpeg` 会按需下采样到 12-bit 样本。

### 3.1 用例矩阵 (20 张)

```
gray (P12):       8×8 grad q75
                  16×16 check q80
                  23×17 grad q50
                  96×96 check q75 r=4
                  241×321 noise q60
                  45×33 grad q70 (non-aligned)
4:4:4 (P12):      8×8 grad q75
                  16×16 check q85
                  32×32 grad q60
                  64×64 noise q85 r=1
                  199×257 grad q55
                  17×13 grad q80 (non-aligned)
4:2:0 (P12):      16×16 grad q75
                  32×32 check q80
                  64×64 grad q70 r=1
                  128×64 grad q50
                  321×241 noise q60 r=4
                  57×39 grad q50
                  23×19 check q75 (non-aligned)
                  128×128 grad q90 r=16
```

覆盖 DRI ∈ {0, 1, 4, 16} MCUs，quality 50–90，图案 grad/check/noise。编码后每张用 `verify_sof2_p12` 扫 0xFFC2 marker 确认 SOF2 + P=12。

## 4. 验收

- **20/20** Phase 13b-prog 向量 bit-exact vs libjpeg-turbo 3.1.3 (`[P12 GRAY / P12 444 / P12 420]` 三种标签)
- **Aggregate regression 1515/1515** (Y=0 C=0 worst-diff)：
  - full (baseline 回归包): 1150
  - phase22 (SOF9 gray/444/420): 18
  - phase24 (SOF10 gray/444/420): 33
  - phase24c (SOF9/SOF10 422/440/411): 162
  - phase17d (SOF2 422/440/411): 81
  - phase12c_prog / phase12c_sof9 / phase12c_sof10 (CMYK): 17 × 3 = 51
  - **phase13b_prog (SOF2 + P=12 gray/444/420)**: 20
- Unit tests (`make test`): ALL TESTS PASSED
- Build clean under `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`

## 5. 交付

- `c_model/src/decoder.c` — `decode_progressive` 接受 P=12 + uint16 drain；`jpeg_decode` dispatch 重排
- `c_model/src/header_parser.c` — `parse_sof2` 放宽到 P∈{8,12}
- `tools/gen_phase13b_prog.py` — 20 个用例覆盖 gray/444/420 × grad/check/noise × DRI ∈ {0,1,4,16}
- `verification/vectors/phase13b_prog/*.jpg` × 20

## 6. 完工后剩余 P=12 / CMYK 空缺

Phase 13b-prog 落完后的 P=12 地图：

- ✅ SOF0/SOF1 (Phase 13a, `decode_p12`)
- ✅ **SOF2 gray/444/420** (Phase 13b-prog, `decode_progressive`)
- ⏳ SOF2 + 4:2:2 / 4:4:0 / 4:1:1 + P=12 — 只需把 Phase 17d 的 chroma 分支扩到 drain，与本 Phase 同构
- ⏳ SOF9 + P=12 (`decode_sof9`) — arith entropy 本身 P-agnostic，改动集中在 drain
- ⏳ SOF10 + P=12 (`decode_sof10`) — 同 SOF2，drain 早退分支
- ⏳ CMYK + P=12 (任何 SOF) — 一起收掉时成本最低
- ⏳ SOF5/6/7 (DCT hierarchical)、SOF13/14/15 (arith hierarchical) — 与 P=12 正交
- 🔒 SOF11 — libjpeg-turbo 3.1.3 参考端仍阻塞，等 Wave 5 切换到 libjpeg-9 参考

按 roadmap_v2.md 节奏继续推。

## 7. 实现备注

- **最重要的 1 行修复**：`jpeg_decode` 的 dispatch 顺序。P=12 精度先于 SOF-type 判定会把 SOF2 P=12 错误路由到 `decode_p12`。修复后在 32×32 smoke 上立即 PASS，20 张正式向量第一轮就 20/20。
- scan-time 代码一行没改 — 证实 Phase 17 系列 `cg[c]` 抽象 + 17d 的 chroma 泛化 + 12c 的 Nf 泛化已经把 progressive Huffman 打磨到 "只剩一个精度轴"，本期在 drain 层增加早退支路即可关掉。
- drain 早退把 P=8 / P=12 路径做硬物理隔离，零 P=8 回归风险。aggregate 1515/1515 印证这一设计选择。
- P=12 drain 对 4:2:0 的 chroma 上采样选最近邻 2×2，这是为了与 `decode_p12` (Phase 13a) 保持一致 — 与 libjpeg-turbo 默认 `jsample_smooth = FALSE` 的 box upsample 对齐。
