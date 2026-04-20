# Phase 10 — 4:2:2 chroma (horizontal-only subsampling)

## Goal
支持 **4:2:2** 采样 (H_Y=2, V_Y=1; H_C=1, V_C=1)：chroma 横向半分辨率、纵向全分辨率。

## Format
- 每个 MCU = **16 × 8 像素** = 4 blocks
  - Y_left  (rows 0-7, cols 0-7 of Y 16×8)
  - Y_right (rows 0-7, cols 8-15)
  - Cb (8×8)
  - Cr (8×8)
- mcu_cols = (W + 15) / 16, mcu_rows = (H + 7) / 8
- chroma_mode = **3**

## C model
- `header_parser.c::parse_sof0` 判断 comp0.H=2 & V=1 → `CHROMA_422`
- `decoder.c`: 新增 4:2:2 MCU 循环 (2 Y + 1 Cb + 1 Cr)
  - Y/C 各 upsample 前扩到 Wp=mcu_cols*16, Hp=mcu_rows*8
  - 水平 chroma upsample nearest (与 4:2:0 共享逻辑即可，但无垂直)
- `golden_compare.c`: libjpeg 读 raw_data (Y 8 rows, Cb/Cr 8 rows at W/2)

## RTL changes
- `header_parser.v`: comp0 H=0x21 → chroma_mode_o=3
- `block_sequencer.v`: is_422 wire, last_blk=3, blk_idx→cur_blk_type:
  0→0 (Y_left), 1→1 (Y_right), 2→4 (Cb), 3→5 (Cr)
  - img_height 上限：mcu_rows = (H+7)/8 (与 GRAY/444 共享)
- `mcu_line_copy.v`: Y 扫描 8 行 × 16 列；chroma 8×8
  - y_col_base = mx*16 (与 4:2:0 共享)
  - c_col_base = mx*8 (半横向分辨率)
- `pixel_out.v`: is_422 → y_row_max=7 (8 行), c_row=y_row[2:0], c_col=x_col[11:1]
- `jpeg_axi_top.v`: 连接 is_422_w = (chroma_mode_w == 2'd3)

## Verification
- `tools/gen_phase10.py`: cjpeg `-sample 2x1,1x1,1x1` → 15 vectors
- 覆盖: gradient/checker/noise; 16×8, 32×16, 100×75, 321×241, DRI>0
- 回归: phase06/07/08/09 必须全部通过

## Acceptance
- 15/15 Phase 10 bit-exact vs libjpeg
- 70/70 Phase 6/7/8/9 regression pass
