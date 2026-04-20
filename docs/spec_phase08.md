# Phase 8 — 灰度 1-component (Nf=1)

**Wave**: 1 (JFIF-full)
**Depends on**: Phase 6, 7
**Blocks**: Phase 9/10（多组件 H×V 参数化的前置铺垫）

## 规格 delta

| 项 | Phase 7 (旧) | Phase 8 (新) |
|---|---|---|
| 合法 `Nf` | 必须 3 (`ERR_UNSUP_CHROMA`) | **1 或 3** |
| MCU 尺寸（像素） | 恒 16×16 | Nf=1 → **8×8**；Nf=3 → 16×16（4:2:0） |
| MCU 内 block 数 | 恒 6（4 Y + Cb + Cr） | Nf=1 → **1**（只 Y0）；Nf=3 → 6 |
| Q 表 / H 表（chroma） | 必 `y+cb+cr` 三套 | Nf=1 时只要 Y 的 DC+AC 两套 Huffman，1 套 Q |
| 输出 AXI-Stream `tdata[23:0]` | `{Y, Cb, Cr}` | Nf=1 时 `{Y, 8'h80, 8'h80}`（中灰占位） |
| 输出 plane（软件视角） | y + cb + cr + cb_420 + cr_420 | Nf=1 时只填 `y_plane`，其他为 NULL 或 0 |

仅支持 `H_samp = V_samp = 1` 的灰度（ISO baseline 下的"常见"灰度图）。更灵活的 H×V 参数化留到 Phase 11。

## 算法（C 模型）

[c_model/src/decoder.c](../c_model/src/decoder.c) 的主循环按 `num_components` 分支：

```c
if (num_components == 1) {
    /* MCU = 1 Y block = 8x8 */
    mcu_cols = (W + 7) / 8;
    mcu_rows = (H + 7) / 8;
    for my, mx:
        huff_decode_block(Y DC/AC tables, &dc_pred_Y, coef);
        dequant(coef, y_qt);
        idct(coef, y_blk);
        copy_block_8x8(y_blk, y_pad + my*8*Wp + mx*8, Wp);
        /* DRI 分支同 Phase 7 */
    crop y_pad → out->y_plane (W×H);
} else {
    /* Nf=3: 原 Phase 7 4:2:0 路径 */
}
```

不输出 `cb_plane` / `cr_plane`（置 NULL）；`golden_compare` 对灰度也只比 Y。

## RTL 改动

| 文件 | 改动 |
|---|---|
| [`rtl/src/header_parser.v`](../rtl/src/header_parser.v) | 接受 Nf∈{1,3}；导出 `num_components_o`（2 位够） |
| [`rtl/src/block_sequencer.v`](../rtl/src/block_sequencer.v) | 新增 `num_components` 输入；Nf=1 时 `blk_idx` 只走 0，MCU 尺寸 8×8，`mcu_cols=(W+7)/8`, `mcu_rows=(H+7)/8` |
| [`rtl/src/mcu_line_copy.v`](../rtl/src/mcu_line_copy.v) | Nf=1 时只拷 Y，跳过 Cb/Cr |
| [`rtl/src/pixel_out.v`](../rtl/src/pixel_out.v) | 新增 `is_grayscale` 输入；Nf=1 时 MCU-row 高度=8（非 16），Cb/Cr 恒 0x80 |
| [`rtl/src/jpeg_axi_top.v`](../rtl/src/jpeg_axi_top.v) | 新增 `num_components_w` 导线 |

**不变**：`huffman_decoder`、`dequant_izz`、`idct_2d`、`line_buffer`、`mcu_buffer`、`dc_predictor`。`htable_ram` 已支持 4 独立 DC/AC 表，无需改。

## 验收

1. **Phase 6 / 7 回归**：20 + 20 + 12 全绿。
2. **新向量**：[`verification/vectors/phase08/`](../verification/vectors/phase08/)，15 张灰度 JPEG；含 8 对齐 (8×8、16×16、64×32) 与非对齐 (17×13、100×75、321×241) 与 DRI >0 组合，bit-exact vs libjpeg。

## 已知限制

- 仅 Nf=1 且 H=V=1；H/V=2/4 或其他灰度样例报 `ERR_UNSUP_CHROMA`。
- 彩色输出格式仍是 24-bit `{Y,Cb,Cr}`；灰度下 Cb/Cr 填充 0x80 便于下游统一处理。

## 交付

- RTL：`header_parser.v`、`block_sequencer.v`、`mcu_line_copy.v`、`pixel_out.v`、`jpeg_axi_top.v`
- C 模型：`decoder.c`、`header_parser.c`
- 测试：`tools/gen_phase08.py`、15 张灰度向量
- 文档：本文件
