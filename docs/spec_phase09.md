# Phase 9 — 4:4:4 chroma (H=V=1 全分量)

**Wave**: 1 (JFIF-full)
**Depends on**: Phase 8
**Blocks**: Phase 10（4:2:2 水平子采样）

## 规格 delta

| 项 | Phase 8 (旧) | Phase 9 (新) |
|---|---|---|
| 合法 chroma 模式 | grayscale (Nf=1) + 4:2:0 (Nf=3, H0=V0=2) | **+ 4:4:4 (Nf=3, H0=V0=1)** |
| MCU 尺寸（像素） | Nf=1 → 8×8；4:2:0 → 16×16 | **+ 4:4:4 → 8×8（1 Y + 1 Cb + 1 Cr）** |
| MCU 内 block 数 | Nf=1 → 1；4:2:0 → 6 | **+ 4:4:4 → 3** |
| Chroma 分辨率 | Nf=1 → none；4:2:0 → (W/2)×(H/2) | **+ 4:4:4 → W×H（与 Y 同分辨率）** |
| Chroma upsample | 4:2:0 → nearest-neighbor 2× | **+ 4:4:4 → bypass（已全分辨率）** |
| 输出 plane | y + (optional) cb/cr (full) + cb/cr_420 | **+ 4:4:4 不填 cb_plane_420 / cr_plane_420** |

约束：`H_Cb = V_Cb = H_Cr = V_Cr = 1` 且 `H_Y = V_Y = 1`。不支持异常 H/V 组合（如 H_Y=2,H_Cb=1,H_Cr=1 类 4:2:2）本期不纳入。

## `jpeg_info_t.chroma_mode` 枚举

新增两位字段，便于 C + RTL 共享：

```
enum {
  CHROMA_GRAY = 0,  // Nf=1, H=V=1 → MCU=8×8, 1 block
  CHROMA_420  = 1,  // Nf=3, H0=V0=2, H1=V1=H2=V2=1 → MCU=16×16, 6 block
  CHROMA_444  = 2,  // Nf=3, H0=V0=H1=V1=H2=V2=1   → MCU= 8× 8, 3 block
  CHROMA_422  = 3,  // 保留给 Phase 10
};
```

`header_parser` 对 SOF0 的 H/V 参数做判别：

```c
if (nf == 1 && H0==1 && V0==1)               → CHROMA_GRAY
else if (nf==3 && H0==2 && V0==2 && chroma_H=V=1) → CHROMA_420
else if (nf==3 && H0==1 && V0==1 && chroma_H=V=1) → CHROMA_444
else                                          → ERR_UNSUP_CHROMA
```

## 算法（C 模型）

[`c_model/src/decoder.c`](../c_model/src/decoder.c) 主循环按 `chroma_mode` 三分支：

```c
if (chroma_mode == CHROMA_GRAY) {
    /* 同 Phase 8 */
} else if (chroma_mode == CHROMA_444) {
    mcu_dim = 8;
    /* 每 MCU 3 block：Y + Cb + Cr，全部 8×8 */
    for my, mx:
        huff(Y);  dequant(y_qt); idct → y_blk[0];
        huff(Cb); dequant(cb_qt); idct → cb_blk;
        huff(Cr); dequant(cr_qt); idct → cr_blk;
        copy_block_8x8(y_blk[0], y_pad + my*8*Wp + mx*8, Wp);
        copy_block_8x8(cb_blk,   cb_pad + my*8*Wp + mx*8, Wp);
        copy_block_8x8(cr_blk,   cr_pad + my*8*Wp + mx*8, Wp);
    /* 无需 chroma_upsample_nn */
} else { /* CHROMA_420, 同 Phase 7 */ }
```

## RTL 改动

| 文件 | 改动 |
|---|---|
| [`rtl/src/header_parser.v`](../rtl/src/header_parser.v) | 导出 `chroma_mode_o [1:0]`；SOF0 H/V 判别 420 vs 444 |
| [`rtl/src/block_sequencer.v`](../rtl/src/block_sequencer.v) | 新增 `chroma_mode` 输入；444 → MCU=8×8, `last_blk=3'd2`（Y/Cb/Cr），blk_idx→component 映射 |
| [`rtl/src/mcu_buffer.v`](../rtl/src/mcu_buffer.v) | 若已按 8×8 chroma 存储则无需改；Y 在 444 下只占第 0 个 8×8 子块 |
| [`rtl/src/mcu_line_copy.v`](../rtl/src/mcu_line_copy.v) | 444 → 只拷 8×8 Y 和 8×8 Cb/Cr，Cb/Cr 地址改用 `mx*8`（与 Y 同步） |
| [`rtl/src/pixel_out.v`](../rtl/src/pixel_out.v) | 444 → MCU-row 高度=8；Cb/Cr 扫描不再 /2（直接 col 映射） |
| [`rtl/src/jpeg_axi_top.v`](../rtl/src/jpeg_axi_top.v) | 连 `chroma_mode_w` |

### block_sequencer 的 blk_idx → component 映射

| mode | last_blk | blk 0 | blk 1 | blk 2 | blk 3 | blk 4 | blk 5 |
|---|---|---|---|---|---|---|---|
| GRAY | 0 | Y | - | - | - | - | - |
| 4:4:4 | 2 | Y | Cb | Cr | - | - | - |
| 4:2:0 | 5 | Y0 | Y1 | Y2 | Y3 | Cb | Cr |

`block_sequencer` 的 `cur_blk_type` 给 `mcu_buffer` 用来选择写入哪块子 buffer — 4:4:4 下 Y 只用 blk_type=0，Cb=blk_type=4，Cr=blk_type=5（复用 4:2:0 的路径，mcu_buffer 无需改）。

### mcu_line_copy 改动要点

444 下 `y_col_base = mx*8`（与 grayscale 同）；`c_col_base = mx*8` 而非 `mx*8`（相同！因 chroma 不下采样）。新增 `is_444` 输入让 chroma phase 扫 8 行而非复用 8×8 路径（Phase 8 的 chroma phase 正好是 8×8，可直接复用，关键是**基地址**与 **line_buffer 的 c_col 范围**）。

注意：`line_buffer.v` 原本 chroma plane 是 8 行 × (width/2) 列（4:2:0 下），444 下要 8 行 × width 列。如果 line_buffer 接口参数不足，需要扩宽 `lb_c_col` 到 12 位（或直接复用 `lb_y_col` 的 12 位）。

### pixel_out 改动要点

444 下 chroma 扫描：
- `rd_c_row = y_row[2:0]`（而不是 `y_row[3:1]`）
- `rd_c_col = x_col[11:0]`（而不是 `x_col[11:1]`）

用 `is_444` 区分（与 `is_grayscale` 同级控制信号）。

## 验收

1. **回归**：Phase 6 / 7 / 8 全绿（20 + 20 + 15 = 55）。
2. **新向量**：[`verification/vectors/phase09/`](../verification/vectors/phase09/)，15 张 4:4:4 JPEG；cjpeg `-sample 1x1,1x1,1x1`；含 8 对齐 与 非对齐 与 DRI>0 组合，bit-exact vs libjpeg。

## 已知限制

- 仅支持 H_Y=V_Y=1 的 4:4:4；H_Y=2 且 H_Cb=V_Cb=1 的非规范变体不在本期。
- 4:2:2（H=2,V=1）留 Phase 10。

## 交付

- RTL：`header_parser.v`、`block_sequencer.v`、`mcu_line_copy.v`、`pixel_out.v`、`line_buffer.v`（仅可能扩位）、`jpeg_axi_top.v`
- C 模型：`decoder.c`、`header_parser.c`（引入 `chroma_mode` 字段）
- 测试：`tools/gen_phase09.py`、15 张 4:4:4 向量
- 文档：本文件
