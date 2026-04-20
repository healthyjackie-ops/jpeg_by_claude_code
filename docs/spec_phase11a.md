# Phase 11a — 4:4:0 chroma (vertical-only subsampling)

## Goal
支持 **4:4:0** 采样 (H_Y=1, V_Y=2; H_C=1, V_C=1)：chroma 纵向半分辨率、横向全分辨率。
这是 Phase 11 通用 H×V 支持的增量子集，仅引入一种新采样模式以保持变更面可控。

## Format
- 每个 MCU = **8 × 16 像素** = 4 blocks
  - Y_top (rows 0-7, cols 0-7)
  - Y_bot (rows 8-15, cols 0-7)
  - Cb (8×8, 覆盖全 8 列但仅 8 chroma 行，对应 16 Y 行)
  - Cr (8×8)
- mcu_cols = (W + 7) / 8, mcu_rows = (H + 15) / 16
- chroma_mode = **4** (新值，需将 chroma_mode 位宽从 2b → 3b)

## 模式比较
| 模式 | MCU W | MCU H | Y blocks | Chroma blocks | chroma_mode |
|------|-------|-------|----------|---------------|-------------|
| GRAY | 8     | 8     | 1        | 0             | 0           |
| 420  | 16    | 16    | 4        | 1             | 1           |
| 444  | 8     | 8     | 1        | 1             | 2           |
| 422  | 16    | 8     | 2        | 1             | 3           |
| 440  | 8     | 16    | 2        | 1             | 4           |

## C model
- `jpeg_types.h`: 新增 `CHROMA_440 = 4`
- `header_parser.c::parse_sof0`: 判断 `comp0.H=1 && comp0.V=2` → `CHROMA_440`
  - `mcu_cols = (W+7)/8`, `mcu_rows = (H+15)/16`
- `decoder.c`: 新 4:4:0 MCU 循环 (Y_top + Y_bot + Cb + Cr, 4 blocks)
  - Y 分别放到 `y_blk[0]` (Y_top) 和 `y_blk[2]` (Y_bot) 对应 mcu_buffer 的 0/2 位
    其实 C 侧不需要 "位" 概念，直接存 2 个 Y block 即可。使用索引 0 和 1，
    然后 copy 到 y_pad 时 y_blk[0] 放 rows 0-7，y_blk[1] 放 rows 8-15
  - `mcu_w = 8`, `mcu_h = 16`, `CWp_sub = Wp` (横向全分辨率), `CHp_sub = Hp >> 1` (纵向半分)
  - 复用 `cb_pad_sub` / `cr_pad_sub` buffer 但尺寸为 `Wp × (Hp/2)`
  - 上采样: 纵向最近邻 (每 chroma 行映射到 2 行输出)
- `decoder.h`: 新增 `cb_plane_440` 和 `cr_plane_440` 字段 (W × H/2)
- `golden_compare.c`: libjpeg `h_samp_factor=1, v_samp_factor=2` → 4:4:0
  - raw_data: Y 16 rows × W, Cb 8 rows × W, Cr 8 rows × W (chroma 纵向半分)
  - tag " [440]", chroma_mode=4

## RTL changes
- **所有接口 `chroma_mode` 位宽: 2b → 3b**
  - `jpeg_axi_top.v`: `chroma_mode_w [2:0]`
  - `header_parser.v`: `chroma_mode_o [2:0]`
  - `block_sequencer.v`: `chroma_mode [2:0]`
- `header_parser.v`: SOF0 中 comp0 HV=0x12 → `chroma_mode_o = 3'd4`
- `block_sequencer.v`:
  - `is_440 = (chroma_mode == 3'd4)` wire
  - MCU 宽 8: `mcu_w8 = is_gray | is_444 | is_440`
  - MCU 高 16: `mcu_h16 = is_420 | is_440` (换个命名更清晰); `mcu_h8 = !mcu_h16`
  - `last_blk = is_gray ? 0 : is_444 ? 2 : (is_422 | is_440) ? 3 : 5`
  - blk_idx → cur_blk_type 映射 for 4:4:0: 0→0 (Y_top), 1→2 (Y_bot), 2→4 (Cb), 3→5 (Cr)
- `mcu_line_copy.v`: 新增 `is_440` 输入
  - Y 扫描 16 行 × 8 列: `mcu_y_tall = is_420 | is_440`, `mcu_y_wide = is_420 | is_422`
  - Y 基址: `y_col_base = mcu_y_wide ? mx*16 : mx*8`
  - Chroma 基址: `c_col_base = mcu_c_wide ? mx*8 (is_420|is_422) : mx*8 (is_444|is_440)`
    → 实际都是 `mx*8` 无论是否下采样，因为 chroma 在 line_buffer 内本来就按 chroma 列号存
    但对 4:4:4/4:4:0 chroma 是全分辨率，存到 line_buffer 用 abs col (0..W-1)
    参考 Phase 9 的处理: 4:4:4 → `c_col_base = mx*8` (chroma 跟 Y 同步)
  - 修正: 4:4:0 chroma col 跟 4:4:4 一样 (mx*8)，因为横向全分辨率
- `pixel_out.v`: 新增 `is_440` 输入
  - MCU 高度判断: `mcu_16row = is_420 | is_440`; `y_row_max` 用 15 (或裁剪后的值)
  - Chroma 读地址 for 4:4:0: `c_row = y_row[3:1]` (纵向上采样 by 2, 同 4:2:0), `c_col = x_col` (横向全分辨率, 同 4:4:4)
- `jpeg_axi_top.v`: 派生 `is_440_w = (chroma_mode_w == 3'd4)`; 连到 u_seq / u_lc / u_po

## Verification
- `tools/gen_phase11a.py`: cjpeg `-sample 1x2,1x1,1x1` → 15 vectors
- 覆盖: gradient/checker/noise; 8×16, 16×32, 32×16, 100×75, 321×241, DRI>0
- 回归: phase06/07/08/09/10 全部必须仍然通过 (85/85)

## Acceptance
- 15/15 Phase 11a bit-exact vs libjpeg
- 100/100 Phase 6/7/8/9/10/11a 回归通过
