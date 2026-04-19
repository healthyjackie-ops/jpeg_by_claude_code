# Phase 6 — 非 16 对齐尺寸裁剪

**Wave**: 1 (JFIF-full)
**Depends on**: v1.0 baseline (Phase 0..4)
**Blocks**: Phase 7..12 的尺寸独立性

## 规格 delta

| 项 | v1.0 (旧) | Phase 6 (新) |
|---|---|---|
| 合法 W/H 范围 | 16..4096，且必须 16 的倍数 | **1..4096，任意值** |
| `ERR_SIZE_OOR` 触发 | W/H=0、>4096、不是 16 倍数 | W/H=0 或 >4096（**不再**对齐检查） |
| `block_sequencer.mcu_cols` | `W >> 4`（截断） | `(W + 15) >> 4`（向上取整） |
| `block_sequencer.mcu_rows` | `H >> 4`（截断） | `(H + 15) >> 4` |
| `pixel_out` 输出行数 | 每 MCU-row 固定 16 行 | 最后 MCU-row 只输出 `((H-1) mod 16) + 1` 行 |
| `pixel_out` 输出列数 | 已按 W 截断 | 不变 |

## RTL 改动

| 文件 | 改动 |
|---|---|
| [`rtl/src/header_parser.v`](../rtl/src/header_parser.v) | 去掉 `img_width[3:0] != 0 \|\| img_height[3:0] != 0` 的检查 |
| [`rtl/src/block_sequencer.v`](../rtl/src/block_sequencer.v) | `mcu_cols/rows` 用向上取整 |
| [`rtl/src/pixel_out.v`](../rtl/src/pixel_out.v) | 加 `img_height` 输入；算 `y_row_max`，替换硬编 15 |
| [`rtl/src/jpeg_axi_top.v`](../rtl/src/jpeg_axi_top.v) | 把 `img_h_w` 接到 `pixel_out.img_height` |

**不变**：`mcu_line_copy`、`line_buffer`、`dequant_izz`、`idct_2d` —— 这些仍按 16-对齐块操作，最后 MCU 的越界写都是 DRAM pad，不读就不影响。

## 验收

1. **Smoke 回归**：原 12 张（16..64 对齐）仍 bit-exact。
2. **新向量**：`verification/vectors/phase06/` 生成 20 张 4:2:0 JPEG，W/H ∈ {1, 7, 17, 100, 123, 127, 320, 321, 1919, 3840} 组合，bit-exact vs libjpeg。
3. **错误码**：W=4097 / H=0 / W=0 仍正确触发 `ERR_SIZE_OOR`。

## 已知限制

- Phase 6 仅处理 4:2:0 非对齐；其他采样形状由 Phase 9/10/11 覆盖。
- 输出像素数组精确为 W×H，`tlast` 在第 W 像素后拉高，`row_done` 在最后有效行后拉高。

## 交付

- RTL diff（4 文件）
- 20 张 phase06 测试向量
- `verification` bit-exact PASS 记录
- 本文档
