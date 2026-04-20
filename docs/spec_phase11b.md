# Phase 11b — 4:1:1 chroma (horizontal 4× subsampling)

## Goal
支持 **4:1:1** 采样 (H_Y=4, V_Y=1; H_C=1, V_C=1)：chroma 横向四分之一分辨率、纵向全分辨率。
Phase 11 通用 H×V 支持的又一个增量子集。引入 H=4 支持，这是需要首次面对 32 像素宽 MCU。

## Format
- 每个 MCU = **32 × 8 像素** = 6 blocks
  - Y0 (rows 0-7, cols 0-7)
  - Y1 (rows 0-7, cols 8-15)
  - Y2 (rows 0-7, cols 16-23)
  - Y3 (rows 0-7, cols 24-31)
  - Cb (8×8, 覆盖全 32 Y 列 via 横向 4× 上采样)
  - Cr (8×8)
- mcu_cols = (W + 31) / 32, mcu_rows = (H + 7) / 8
- chroma_mode = **5** (3 位已足够容纳)

## 模式比较
| 模式 | MCU W | MCU H | Y blocks | Chroma blocks | chroma_mode |
|------|-------|-------|----------|---------------|-------------|
| GRAY | 8     | 8     | 1        | 0             | 0           |
| 420  | 16    | 16    | 4        | 1             | 1           |
| 444  | 8     | 8     | 1        | 1             | 2           |
| 422  | 16    | 8     | 2        | 1             | 3           |
| 440  | 8     | 16    | 2        | 1             | 4           |
| 411  | 32    | 8     | 4        | 1             | 5           |

## C model
- `jpeg_types.h`: 新增 `CHROMA_411 = 5`
- `header_parser.c::parse_sof0`: 判断 `comp0.H=4 && comp0.V=1` → `CHROMA_411`
  - `mcu_cols = (W+31)/32`, `mcu_rows = (H+7)/8`
- `decoder.c`: 新 4:1:1 MCU 循环 (Y0..Y3 + Cb + Cr = 6 blocks)
  - `mcu_w = 32`, `mcu_h = 8`
  - padded buf: `Wp = mcu_cols*32`, `Hp = mcu_rows*8`
  - Chroma buffer `CWp_sub = Wp >> 2`, `CHp_sub = Hp` (横向 1/4)
  - 复制 Y: `copy_block_8x8(y_blk[i], y_dst + i*8, Wp)` — 4 个 Y block 横向堆叠
  - 上采样: chroma 横向最近邻 — 每 chroma 列映射到 4 个输出列
- `decoder.h`: 新增 `cb_plane_411` 和 `cr_plane_411` 字段 (W/4 × H) 存 chroma subsampled
- `golden_compare.c`: libjpeg `h_samp_factor=4, v_samp_factor=1` → 4:1:1
  - raw_data: Y 8 rows × Wp, Cb 8 rows × (Wp/4), Cr 8 rows × (Wp/4)
  - tag " [411]", chroma_mode=5

## RTL changes
- **`chroma_mode` 仍为 3b (足够存 0..5)**
- `header_parser.v`: SOF0 中 comp0 HV=0x41 → `chroma_mode_o = 3'd5`
  - `mcu_cols = (W+31) / 32` (列除以 32)
- `block_sequencer.v`:
  - `is_411 = (chroma_mode == 3'd5)` wire
  - MCU 宽 32: Y 占 4 块 — 需引入 `mcu_w32` 逻辑
  - `last_blk = is_gray ? 0 : is_444 ? 2 : (is_422 | is_440) ? 3 : 5`
    → 4:1:1 也是 5 (0..3=Y, 4=Cb, 5=Cr)，与 4:2:0 相同！
  - blk_idx → cur_blk_type 映射 for 4:1:1: 0→0, 1→1, 2→2, 3→3, 4→4, 5→5
    → **与 4:2:0 完全相同**；区分只在 mcu_buffer 内 Y 块布局
- `mcu_buffer.v`: **扩容 ybuf 0..255 → 0..255 但重新解读地址**
  - 保持 256 byte (32×8 刚好 = 256)
  - Y 写地址 for 4:1:1: `{abs_y_row[2:0], {blk_type[1:0], 3'd0} + col}` — 4 块横向
  - Y 写地址 for 4:2:0: `{abs_y_row[3:0], y_c_off[3] + col[3:0]}`  — 2×2 block
  - 为同时支持两种模式，需要 mode-aware 地址解码
  - **简化方案**: 扩展 ybuf 到 32×16 = 512 byte，全局用 `{row[3:0], col[4:0]}` 地址
    - 4:2:0 用 row 0..15, col 0..15
    - 4:1:1 用 row 0..7, col 0..31
    - 所有其他模式用 row 0..15 col 0..15 子集
- `mcu_line_copy.v`: 新增 `is_411` 输入
  - Y 扫描 8 行 × 32 列: `y_cnt_max = 7`, `col_cnt_max = 31`
  - 需要 5 位 `col_cnt` (原为 4 位)
  - Y 列基地址: `y_col_base = mcu_col_idx*32` for 4:1:1
  - Chroma 列基地址: `c_col_base = mcu_col_idx*8` (chroma 在 line_buffer 中按 chroma 列号存)
- `pixel_out.v`: 新增 `is_411` 输入
  - MCU 高度: 4:1:1 为 8 行 (同 gray/444/422)，`mcu_8x8 = is_gray | is_444 | is_422 | is_411`
  - Y 扫描宽: `x_col` 跟 `img_width`, 与所有模式相同 (line_buffer 按 raster 展开)
  - Chroma 读地址 for 4:1:1: `c_row = y_row[2:0]`, `c_col = x_col[11:2]` (横向 /4)
- `jpeg_axi_top.v`: 派生 `is_411_w = (chroma_mode_w == 3'd5)`; 连到 u_lc / u_po / u_seq / u_mb

## Verification
- `tools/gen_phase11b.py`: cjpeg `-sample 4x1,1x1,1x1` → 15 vectors
- 覆盖: gradient/checker/noise; 32×8, 64×16, 32×64, 100×75, 321×241, 非对齐宽, DRI>0
- 回归: phase06/07/08/09/10/11a 全部必须仍然通过 (100/100)

## Acceptance
- 15/15 Phase 11b bit-exact vs libjpeg
- 115/115 Phase 6/7/8/9/10/11a/11b 回归通过
