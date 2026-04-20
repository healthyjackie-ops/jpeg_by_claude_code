# Phase 12 — Nf ∈ {2, 4} (CMYK & 2-comp)

## Goal
引入多组件支持：除了已有的 Nf=1 (灰度) 与 Nf=3 (YCbCr)，进一步支持 **Nf=4 (CMYK)** 以及很少见的 **Nf=2 (Y + 单 chroma)**。
完成后解码器兼容 ISO/IEC 10918-1 Baseline 中的全部 Nf ∈ {1,2,3,4}。

## Format

### Nf=4 CMYK
- 4 个组件 id = 1..4（libjpeg 习惯）或其他唯一整数
- 典型采样 `cjpeg -colorspace CMYK -sample 1x1,1x1,1x1,1x1` → 全 1×1（等价于 4:4:4:4 CMYK）
- 较少见：非 1×1 CMYK，与 YCbCr 共享同套 H×V 子采样逻辑
- libjpeg raw 路径：4 个 `JSAMPARRAY` plane；每组件按自己 `v_samp_factor` 提供多行

### Nf=2
- 2 个组件，通常 Y + 单通道（Motion JPEG 残余或 Adobe JPEG-Tagged）
- MCU 块结构：`blk_count = sum(Hi·Vi)` （与 Nf=3 相同的通用公式）
- 输出：本 spec 用 `{comp0, comp1, 8'h00, 8'h00}` 方式占 32b

## 输出总线方案（**breaking change**）
当前 `m_px_tdata[23:0] = {Y, Cb, Cr}` 对 CMYK 4 组件无法承载。方案 A（被采用）：

> **将 `m_px_tdata` 扩至 32 位**；根据 Nf 打包如下：
>
> | Nf | tdata[31:24] | tdata[23:16] | tdata[15:8] | tdata[7:0] |
> |---:|---|---|---|---|
> | 1 (GRAY) | `Y` | `0x80` | `0x80` | `0x00` |
> | 2        | `comp0` | `comp1` | `0x80` | `0x00` |
> | 3 (YCbCr)| `Y` | `Cb`    | `Cr`    | `0x00` |
> | 4 (CMYK) | `C` | `M`     | `Y`     | `K`    |
>
> 并新增 CSR `OUTPUT_FMT`（2 bit，= Nf−1 自动反映，软件只读）用以消费端解读 tdata 语义。

方案 B（待讨论）：保留 24b，在 CMYK 模式下每像素分两拍输出 `{C,M,Y}` + `{K,?,?}` — 时序复杂且打破单拍 1 像素原则，**否决**。

方案 A 对 YCbCr 消费者的影响：tdata 从 24b 变 32b，高 8 位保留 0。SV 顶层需 bump major 版本。

## C model
- `jpeg_types.h`：新增枚举 `CHROMA_MODE` 值或改用独立 `num_components` 字段承担 Nf。已有 `chroma_mode` 字段保留表达 YCbCr 子采样比；新增 `is_cmyk` bool 或扩展枚举 `CHROMA_CMYK = 6`.
- `header_parser.c`：SOF0 接受 Nf=4；校验 4 个组件的 H/V ∈ 1..4；依据 H/V 组合派生 chroma_mode（CMYK 时通常 1×1）.
- `decoder.c`：
  - `mcu_block_order[]` 通用化为 `sum(Hi*Vi)` 长度，按组件 id 填充
  - `y_plane / cb_plane / cr_plane / k_plane` 新增 K 平面
  - 输出 tdata 32b 打包
- `golden_compare.c`：libjpeg `JCS_CMYK` + `raw_data_out`；比对 4 plane。

## RTL changes

### 新增/加宽接口
- `m_px_tdata[31:0]` （from 23:0）
- CSR `OUTPUT_FMT` 读取（Nf−1）

### 模块
- `header_parser.v`：SOF0 FSM 支持 Nf ∈ {1,2,3,4}；存 `num_components_o` ∈ {1,2,3,4}；导出 `comp3_qt/td/ta/hv` 新端口。
- `block_sequencer.v`：
  - blk_idx 上限从 2..5 改为 **0..15**（需要 4b 计数器）支持 sum(Hi*Vi)；典型 CMYK 1×1×4 = 4 块
  - `last_blk = sum(Hi*Vi) - 1`
  - blk_idx → comp_sel 查表：按组件内块顺序给出 qt_sel / dc_sel / ac_sel / dcp_sel
  - dcp_sel 扩至 2b → 3b 支持 4 组件（或继续用 2b {Y,Cb,Cr,K}）
- `dc_predictor.v`：4 slot（原 3 slot）
- `mcu_buffer.v`：加 K buffer 8×8 (64B)；blk_type = 6 → Cr，7 → K（或重编码）
- `mcu_line_copy.v`：chroma phase 分拆 → "extra comp" phase 循环遍历 comp 1..Nf-1
- `pixel_out.v`：tdata 32b；按 num_components 选择打包路径
- `jpeg_axi_top.v`：tdata 端口 32b；top FIFO 也加宽
- `axi_stream_fifo`：参数化 DW 已支持，只需实例化 DW=32

## Verification

### 测试生成
- `tools/gen_phase12.py`
  - Nf=4 CMYK：`cjpeg -colorspace CMYK -sample 1x1,1x1,1x1,1x1 -quality {Q}`（需 libjpeg-turbo 支持 CMYK JPEG 产出；备选 ImageMagick `convert`）
  - Nf=2 Y+alpha：现成样本罕见，可先 skip 或在 Phase 12b 补
- 15 张 CMYK 矩阵：checker / grad / noise × 对齐 + 非对齐 × {DRI=0, DRI=4}

### 交叉验证
- libjpeg `JCS_CMYK + raw_data_out` 4 plane 逐字节比较
- 差分 harness：扩 `PixelSink` 从 24b 到 32b；按 Nf 解包并与 golden 对比

### 回归
- phase06..phase11b 全部 115/115 继续通过（tdata 高 8 位 = 0 对消费端无影响）
- phase12/ 新语料 ≥ 10/10 bit-exact

## Acceptance
- 15/15 Phase 12 bit-exact vs libjpeg (CMYK)
- 130/130 (+115 旧 + 15 新) 累积回归通过
- `m_px_tdata` 32b，高位兼容 YCbCr 语料

## 拆分建议
可进一步分：
- **Phase 12a**: Nf=4 CMYK（本 spec 的主干）
- **Phase 12b**: Nf=2（少数 Y+α 语料，可独立 1 天落地）

建议先做 12a（现实价值高），12b 视语料可获得性而定。

## 风险 / 开放问题
1. **tdata 加宽破坏向后兼容**：上游 SoC 需同步更新。评估是否在配置 `num_components=3` 时仍保持 24b（引入 gen param `PX_BW` 默认 24）以避免破坏。
2. **CMYK JPEG 的语料来源**：`cjpeg` Ubuntu 默认 libjpeg-turbo 支持 `-colorspace CMYK`，但需要输入 CMYK TIFF/PGM。实际生成需 ImageMagick：
   ```
   convert input.png -colorspace CMYK -sampling-factor 1x1 cmyk.jpg
   ```
   必要时本 spec 接受使用 ImageMagick 作为测试依赖。
3. **4 组件 DC 预测器**：dc_predictor 需从 3-slot 扩到 4-slot；接口位宽影响若干模块。可把 `comp_sel` 从 2b → 3b 并在 `num_components=1/2/3` 时兼容。
