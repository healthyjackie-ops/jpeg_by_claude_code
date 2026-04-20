# Phase 13 — SOF1 + 12-bit sample precision

## Goal
扩展 v1.2 baseline（SOF0 + 8-bit）至支持 **SOF1 (0xFFC1, extended sequential baseline) 与 12-bit 采样精度**。
完成后解码器覆盖 ISO/IEC 10918-1 中 **Baseline + Extended Sequential** 的 P ∈ {8, 12} 两档，Q-table 支持 Pq ∈ {0, 1}。

## Format

### SOF1 帧头
- 结构同 SOF0：`{len, P, Y, X, Nf, (Ci, Hi|Vi, Tqi)×Nf}`
- `P = 12`（SOF1 时必须）或 `P = 8`（SOF0 语义子集；少见）
- Huffman 编码层（DHT/SOS/zigzag/RLE）结构 **不变**，但数值范围扩大

### DQT (Pq=1)
- 8-bit Q-table: `Pq=0`, 64 × 8b = 64 B
- 16-bit Q-table: `Pq=1`, 64 × 16b = 128 B（每值 MSB byte first）
- Pq 必须 ≤ (P==8 ? 0 : 1)

### 系数数值范围
| 采样深度 | DC 系数范围 | DC category | AC category (RLE ssss) |
|---:|---|---|---|
| 8-bit  | ±2^10 = ±1024 | 0..11 | 0..10 |
| 12-bit | ±2^14 = ±16384 | 0..15 | 0..14 |

- DC/AC Huffman 表的 `HUFFVAL[]` 可能包含更大的 category 值（DC 12b 最多 15，AC `(run<<4)|size` 中 size ≤ 14 → 最大 `0xFE`）
- 现有 htable 结构（`bits[17]`, `huffval[256]`）容量充足，**无需扩容**

### 像素范围
- 8-bit: 0..255，level shift +128
- 12-bit: 0..4095，level shift +2048

### MCU / 采样比
- 与 P=8 完全一致（H×V ∈ 1..4×1..4，chroma_mode 枚举不变）
- MCU 内每 sample 存储 **int16**（足够 ±16384）
- Line buffer 每 sample 12b，实际存储 16b（多 4b 零填充）

## 输出总线方案（**breaking change**）

v1.2 `m_px_tdata[31:0]`（32b） 对 12-bit 三或四通道无法承载（3×12=36b / 4×12=48b）。

**方案 A（被采用，最小破坏）**：扩至 **48b**，8-bit 模式下高位保留 0。

| P | Nf | [47:36] | [35:24] | [23:12] | [11:0] |
|:-:|:-:|---|---|---|---|
| 8  | 1 (GRAY) | `{Y[7:0], 4'd0}` | `{0x80,4'd0}` | `{0x80,4'd0}` | `{0x00,4'd0}` |
| 8  | 3 (YCbCr)| `{Y,4'd0}` | `{Cb,4'd0}` | `{Cr,4'd0}` | `{0,4'd0}` |
| 8  | 4 (CMYK) | `{C,4'd0}` | `{M,4'd0}` | `{Y,4'd0}` | `{K,4'd0}` |
| 12 | 1 (GRAY) | `Y[11:0]` | `12'h800` | `12'h800` | `12'h000` |
| 12 | 3 (YCbCr)| `Y[11:0]` | `Cb[11:0]` | `Cr[11:0]` | `12'h000` |
| 12 | 4 (CMYK) | `C[11:0]` | `M[11:0]` | `Y[11:0]` | `K[11:0]` |

所有样本左对齐到 12b 槽，8-bit 模式末尾 4b 恒 0（利于下游简化）。CSR 新增 `PIX_FMT` 字段表达 `{P, Nf}`。

**方案 B（未采用）**：保持 32b，P=12 时分两拍输出高/低 4b —— 打破 1 px/cyc 原则，否决。

## C model

### jpeg_types.h
```c
#define JPEG_MAX_SAMPLE (4095)  /* P=12 上界 */
typedef struct { uint16_t q[64]; int loaded; } qtable_t;  /* 已经 16b，无需改 */
typedef struct {
    ...
    uint8_t  precision;    /* 8 or 12 */
    ...
} jpeg_info_t;
typedef struct {
    uint16_t width, height;
    uint8_t precision;
    uint16_t *y_plane, *cb_plane, *cr_plane, *k_plane;  /* P=12 用 16b plane */
    /* 保留 uint8_t* 变体供 P=8 fast path */
    uint8_t  *y_plane8, *cb_plane8, *cr_plane8, *k_plane8;
    ...
} jpeg_decoded_t;
```
新增 `precision` 字段；planes 从 `uint8_t*` 升为 `uint16_t*`（左对齐 12b，高 4b 为 0）。

### header_parser.c
- `parse_sof0/sof1`：去除 `p != 8` 硬禁；接受 `p ∈ {8,12}`。SOF1 允许 P=8 作为子集。
- `parse_dqt`：`Pq=0` 读 8b，`Pq=1` 读 16b；保存时均升至 16b。

### decoder.c
- 像素存储类型 `uint8_t → uint16_t`
- IDCT 输出 clamp 至 `(1<<P)-1`，level shift 用 `1<<(P-1)`
- `copy_block_*` 按 P 切换字节宽度
- chroma upsample 对 12b 值做同样 nearest-neighbor 拷贝

### idct.c
- 现有定点 `CONST_BITS + PASS1_BITS = 13+2 = 15` 足以应对 P=12（coef ±16384 × q ≤ 16k → 乘加 ≤ 28b，int32 够）
- 替换 `clamp_byte` 为 `clamp_sample(P)`：8b 0..255，12b 0..4095

### golden_compare.c
- libjpeg 12-bit 走 `J12_` 前缀 API（libjpeg-turbo 3.x）— 或编译 `libjpeg12` 变体
- macOS Homebrew `jpeg-turbo` 已含 J12 支持（`libjpeg.h` 定义 `J12SAMPLE = uint16_t`）
- 若缺失：fallback 到用 `djpeg -precision 12` 解到 PPM/pfm 对比

## RTL changes

### 新增 top-level 参数
```verilog
parameter P_MAX = 12,           // 支持的最大样本精度
parameter SAMPLE_W = 16         // 内部 sample word 宽度（>=P_MAX）
```
- 现有 8b 路径作为 `P=8` 子集；`P=12` 走全 16b 路径
- Line buffer / mcu buffer 内存从 8b 升到 16b **代价**：

| 模块 | 8-bit 体积 | 16-bit 体积 | 增量 |
|---|---:|---:|---:|
| `mcu_buffer.ybuf` | 512 B  | 1024 B | +512 B |
| `mcu_buffer.cbbuf/crbuf/kbuf` | 3×64 B | 3×128 B | +192 B |
| `line_buffer.ybuf` | 16×4096 B | 32 KB | +32 KB |
| `line_buffer.cbbuf/crbuf/kbuf` | 3×8×4096 B | 3×64 KB | +96 KB |
| **合计** | 200 KB | ~328 KB | **+128 KB** |

（推理为 ~16 SRAM tile @ 8KB 粒度）

### 模块级改动
- **header_parser.v**：接受 marker 0xFFC1；P ∈ {8,12}；Pq 变长；新增 `precision_o[3:0]`。
- **qtable_ram.v**：entries 从 8b 升到 16b（与 C 模型对齐）— 若已经 16b，无改动。
- **huffman_decoder.v**：算法不变；DC category 上限从 11 升到 15，`l` 仍 5b 足以。
- **bitstream_unpack.v**：`extend()` 输入 size 可达 15 或 14（不再 ≤ 11）；内部 `code_sz_max`/shreg 位宽检查。
- **dequant_izz.v**：coef 从 `int12` 升到 `int16`；乘法器 16×16。
- **idct_1d.v / idct_2d.v**：
  - 输入 coef 16b
  - 中间累加器位宽 +4b
  - 输出 clamp 参数化 `(1<<P)-1`，level shift `1<<(P-1)`
- **mcu_buffer.v**：每 sample 16b（高 4b 为 0 在 P=8）。
- **mcu_line_copy.v / line_buffer.v**：同步 16b。
- **pixel_out.v**：打包至 48b（见上表）。
- **jpeg_axi_top.v**：`m_px_tdata` 48b；FIFO `DW=48`；CSR 新增 `PIX_FMT`。

## Verification

### 测试向量生成（`tools/gen_phase13.py`）
- 使用 `cjpeg -precision 12 -quality Q -sample H0xV0 -restart N input.ppm`
- 输入 8-bit PPM（需缩放至 12b）或 16-bit PPM（libjpeg 直接用）
- 覆盖组合：
  - 尺寸：8×8, 16×8, 17×13, 32×32, 64×64, 123×45, 321×241
  - 采样：4:4:4 (通用起点，最简)，4:2:0 (主流)，grayscale
  - Quality：50/75/90
  - DRI：0 / 4 / 16
- 目标 20 张

**注意**：CMYK + 12b 组合极少见（JPEG printing 专业设备外），初版不覆盖。

### 差分链路
- C 模型 → libjpeg12 golden：两者均以 16b plane 输出，逐 sample 比对
- RTL → C 模型：testbench `PixelSink` 扩到 48b tdata，按 `precision` 拆分 4 × 12b

### Bit-exact 目标
- 20/20 ΔY=0 ΔC=0 （P=12 整数 IDCT 应与 libjpeg-turbo 一致）
- 保留 Phase 6..12 全部回归（P=8 路径不得回归）

## Acceptance
1. `docs/spec_phase13.md` 合入
2. `c_model/src/*` 升级支持 P=12；`build/decoder_test` 独立跑通 12-bit golden
3. `verification/vectors/phase13/` ≥ 20 张
4. RTL 全链路 P=12 跑通，`make diff DIFF_DIR=../vectors/phase13` 20/20
5. 回归 phase06..phase12 130/130 不变
6. `reports/` 暂不必新增（Wave 2 结束再统一）

## 拆分建议
考虑复杂度，可拆：
- **Phase 13a** — C model + vectors + SOF1 parsing（不改 RTL）
- **Phase 13b** — RTL 16b datapath + 48b tdata
- **Phase 13c** — 12-bit 流水线 bit-exact + 回归

每阶段独立 commit。

## 风险 / 开放问题

1. **libjpeg-turbo J12 API**：Homebrew 默认是否编译了 `--with-12bit`？若未，需要从源码编译或回退到 `libjpeg8d` 配合 djpeg CLI pipe。
2. **IDCT overflow**：整数 IDCT 的中间累加器在 P=12 下 +4b 是否仍 ≤ 32b？手动推算：
   - coef ±16384 (15b)，q 16b → 乘积 31b
   - 8-tap 累加 3b → 34b —— **需要 int64 或 saturation**
   - 或：保持 CONST_BITS 不变，先 coefficient clamp，同 libjpeg-turbo 做法
3. **48b tdata 的下游兼容**：FPGA AXI4-Stream slave 端 TDATA 位宽一般 8×N —— 48b 合法但少见。可同时支持 CSR 模式切换 8b/12b 输出（12b 模式则 tdata=48b、8b 模式 tdata=32b）。

## 依赖

需确认本机 libjpeg-turbo 是否开 J12：
```
$ cjpeg -help 2>&1 | grep precision
  -precision N   Create JPEG file with N-bit data precision
                 (N=2..16; default is 8; if N is not 8 or 12, then -lossless
                 must also be specified)
```
若 `-precision 12` 能直接跑 → J12 已编译。

---

**下一步**：本 spec 冻结后开始 Phase 13a（C model + vectors），不触 RTL。
