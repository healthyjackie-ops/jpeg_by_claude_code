# JPEG Baseline Decoder — 微架构 (uArch)

**Revision**: 0.1  
**Date**: 2026-04-18

---

## 1. 数据流总览

```
  AXI-S bitstream (32b)                                    AXI-S pixel (24b)
         │                                                        ▲
         ▼                                                        │
  ┌───────────┐    ┌────────────┐    ┌──────────┐    ┌────────┐    ┌──────────┐
  │ Bitstream │───▶│  Header    │    │  QTable  │    │ DC Pred│    │          │
  │  FIFO +   │    │  Parser    │───▶│   RAM    │    │  (Y,   │    │          │
  │ Unstuff   │    │  (FSM)     │    │ (4×64×16)│    │ Cb,Cr) │    │          │
  └─────┬─────┘    └─────┬──────┘    └────┬─────┘    └────┬───┘    │          │
        │                │                 │                │      │          │
        │ bits           │ program         ▼                │      │          │
        ▼                ▼            ┌─────────┐           │      │          │
  ┌───────────┐    ┌────────────┐    │ Dequant │           │      │          │
  │ Huffman   │───▶│ RLE Expand │───▶│  + IZZ  │──┐        │      │          │
  │ Decoder   │    │ (run,size, │    │ (×Q tab)│  │        │      │          │
  │ (VLD)     │    │  amp→coef) │    └─────────┘  │        │      │          │
  └─────┬─────┘    └────────────┘                 │        │      │          │
        │                                          ▼        │      │          │
  ┌─────▼──────┐                            ┌───────────┐   │      │          │
  │  HTable    │                            │  2D IDCT  │   │      │          │
  │    RAM     │                            │ (Loeffler)│   │      │          │
  │ (4 tables) │                            └─────┬─────┘   │      │          │
  └────────────┘                                  │         │      │          │
                                                  ▼         │      │          │
                                            ┌───────────┐   │      │          │
                                            │   MCU     │◀──┘      │          │
                                            │  Buffer   │──────────┘      │          │
                                            │ (6 blks)  │◀─── Line Buffer (Y 16×4096) │
                                            └─────┬─────┘     Cb/Cr (8×2048 ×2)        │
                                                  │                                    │
                                                  ▼                                    │
                                            ┌───────────┐                              │
                                            │  Chroma   │                              │
                                            │ Upsample  │                              │
                                            │ 4:2:0→444 │                              │
                                            └─────┬─────┘                              │
                                                  ▼                                    │
                                            ┌───────────┐                              │
                                            │ Pixel Out │──────────────────────────────┘
                                            │  (raster) │
                                            └───────────┘
```

---

## 2. 模块详设

### M1. `jpeg_axi_top` — 顶层与 AXI 封装
- AXI4-Lite slave interpreter（标准模板）
- 寄存器堆（见 regmap.md）
- 中断控制
- AXI-Stream 输入 FIFO（深度 256，32-bit），输出 FIFO（深度 256，24-bit）
- Reset 同步器

**关键时序**：AXI 读写延迟 ≤ 2 cycles；FIFO 内部无组合反馈。

---

### M2. `header_parser` — JPEG 头解析
**FSM 状态**：
```
IDLE → SCAN_MARKER → MARKER_TYPE → (根据 marker 分发)
       │
       ├── APP0-F    → SKIP_SEGMENT (读 Ls 字节跳过)
       ├── DQT       → LOAD_QTABLE  (写 QTable RAM)
       ├── DHT       → LOAD_HTABLE  (写 HTable RAM)
       ├── SOF0      → PARSE_SOF    (提取 W/H/components)
       ├── SOS       → PARSE_SOS    (提取 Td/Ta 映射，进入数据阶段)
       ├── EOI       → DONE
       └── (其他)    → ERROR
```

- 码流输入 **每周期最多消耗 1 byte**（头部分吞吐不关键）
- `DATA` 阶段（SOS 之后）把 byte 流转交给 Bitstream Unpacker

**输出**：
- W, H, QT/HT 映射关系寄存器
- 控制信号：start_entropy_decode, end_of_frame
- 错误上报：bit mask → `ERROR_CODE`

---

### M3. `bitstream_unpack` — 码流解压与 barrel shift
**内部结构**：
```
  byte stream (8b/cyc, from header_parser in DATA state)
         │
         ▼
  ┌──────────────┐    0xFF 跟随 0x00 → 丢弃 0x00
  │  Unstuff FSM │    0xFF 跟随 非 0x00 → marker（RSTn/EOI）
  └──────┬───────┘
         ▼
  ┌──────────────┐    64-bit shift register
  │  Bit Buffer  │    bit count: 0~63
  │  (64 bits)   │
  └──────┬───────┘
         ▼
  ┌──────────────┐    consume: 1~16 bits/cyc
  │Barrel Shifter│    output: leading 16 bits + bits valid
  └──────────────┘
         │
         ▼ 16b window + consume signal
     to Huffman Decoder
```

**接口**：
- `fetch_n_bits[4:0]` ∈ [1,16]，下一周期消耗
- `peek_window[15:0]`：当前最高 16 bits
- `bits_valid`：≥ 请求的位数时拉高
- `marker_detected`: 遇到非 0x00 的 0xFF 后续字节时置位（v1.0 遇到非 EOI 即报 ERR_BAD_MARKER）

**注意**：每周期最多补充 1 byte（8 bits）进入 buffer，Huffman 平均消耗 ~9 bits（DC 差分 + AC run/size + amp），因此需要 **两个字节并行补充**以维持 ≥1 symbol/cyc → buffer 输入侧升级为 2 byte/cyc（从输入 FIFO 读 2 字节）。

---

### M4. `huffman_decoder` — 变长码解码（**关键模块**）

**算法**：两级查表
- **Level 1**：用 peek_window 的高 8 bits 查 256-entry LUT
  - 若码长 ≤ 8：立即返回 (symbol, length)，1 cyc 完成
  - 若码长 > 8：进入 Level 2 逐位比较
- **Level 2**：使用标准 JPEG `MINCODE[L]/MAXCODE[L]/VALPTR[L]` 表（L=9..16），每周期检查一层 → 最坏 8 cycles

**覆盖率**：标准图像 ~99% symbol 走 L1 fast path；L2 仅 ~1%，允许吞吐局部下降。

**HTable RAM**:
- 4 tables × (256 L1 entries + 16 (MINCODE/MAXCODE/VALPTR) × 3 + 256 HUFFVAL) × 16 bits
- ~4 × 800 × 16b = 51200 bits ≈ 6.4 KB SRAM（或用 flops 若小）

**输出**：
- DC path: `symbol = size (0..11)`，下一步从 bitstream 再取 `size` bits 作为 amplitude
- AC path: `symbol = {run[7:4], size[3:0]}`，特殊：`0x00 = EOB`，`0xF0 = ZRL (run=16)`

**符号扩展**（amplitude）：
```
if amp_raw MSB == 1: amp = amp_raw
else: amp = amp_raw - ((1 << size) - 1)
```

**控制**：块内计数器 k=0..63，遇 EOB 填零直到 k=63。

---

### M5. `dc_predictor`
- 3 组 DC 前值寄存器（Y, Cb, Cr），每次解出 `DC_diff` → `DC = DC_prev + DC_diff`
- SOS 起始时清零
- 新一帧 start 时也清零

---

### M6. `dequant_izz` — 反量化 + 反 Zigzag
- 输入：(k, coef) 流，k=0..63
- ZZ ROM 64-entry：`zz[k]` → 8×8 natural order 索引
- 乘法：`coef[natural_idx] = coef_raw × qt[k]`（符号扩展后 × 无符号 Q）
- **写入 block RAM**（64×16-bit，natural order）
- 乘法器：16b × 8b → 16b + 饱和，1 个即可（1 coef/cyc）

---

### M7. `idct_2d` — 8×8 IDCT（**关键模块**）

**算法**：Loeffler/Chen-Wang 8-point 1D IDCT × 2（行 / 列）
- 11 mult + 29 add per 1D
- **乘法用移位-加法分解**（乘法常数是固定值），不用通用乘法器

**流水线**：
```
  [input: 1 coef/cyc]
         │
  行 IDCT (8-pt pipelined, 8 cyc latency)
         │
  8×8 转置 buffer（双端口 SRAM / 64×16 flops）
         │
  列 IDCT (8-pt pipelined, 8 cyc latency)
         │
  舍入 + 偏移 +128 + 饱和 [0,255]
         │
  [output: 1 pixel/cyc, 8-bit]
```

**延迟**：输入第一个系数到输出第一个像素 ≈ 24 cycles。
**吞吐**：稳态 1 sample/cyc 输入，1 pixel/cyc 输出。

**乘法常数**（Loeffler）：
```
c1 = cos(π/16),  c2 = cos(2π/16), ..., c7 = cos(7π/16)
m1=0.5411, m2=0.7071, m3=0.3827, m5=1.3066, m6=0.5412 ...
```
定点化：16-bit 小数精度。每个常数分解为 2-4 项移位加法。

---

### M8. `mcu_buffer` — MCU 组装
**MCU 结构（YUV420）**：每个 MCU = 16×16 Y + 8×8 Cb + 8×8 Cr
- 包含 **4 个 Y 8×8 block（顺序：Y00, Y01, Y10, Y11）+ 1 Cb + 1 Cr**
- 按此顺序送入 IDCT

**存储**：
- IDCT 输出后写入 **MCU Buffer**（ping-pong，2×（4+1+1）×64×8-bit ≈ 6 KB）
- 下游 raster 扫描时从 Buffer 读出

**Line Buffer**（光栅化）：
- Y: 16 行 × 4096 列 × 8b = 64 KB（用 2 块 32 KB SRAM）
- Cb/Cr: 8 行 × 2048 列 × 8b × 2 = 32 KB
- **生产侧**：MCU 完成后，把 16×16 Y 写入 Y line buffer 对应行范围；8×8 Cb/Cr 写入 chroma line buffer
- **消费侧**：pixel_out 按光栅扫描从 line buffer 读

---

### M9. `chroma_upsample` — 4:2:0 → 4:4:4
**算法 v1.0**：**最近邻（box filter）** — 每个 Cb/Cr 值复制 2×2
- 与 libjpeg `fancy_upsampling=FALSE` 默认行为一致，便于 C 模型对拍
- 实现：chroma line buffer 每读一个值，输出 2 个像素；每行重复读 2 次

**未来可选**：双线性或 JPEG spec 的 cosited 插值，需要额外滤波器（v1.0 不做）

---

### M10. `pixel_out`
- 从 Y / Cb / Cr line buffer 按光栅顺序读
- 组合成 `{Y, Cb, Cr}` 24-bit
- 驱动 AXI-Stream：`TVALID, TDATA[23:0], TUSER (SOF), TLAST (EOL)`
- 背压：若 `TREADY=0`，整个下游停顿，向上游（MCU Buffer 读）施加背压

---

## 3. 吞吐与延迟分析

### 3.1 稳态吞吐
一个 MCU：
- Bitstream → Huffman → Dequant：变长，平均 ~384 cycles（6 blocks × 64 coefs @ 1 coef/cyc 稳态）
- IDCT：6 blocks × 64 cyc = 384 cycles（全流水，1 sample/cyc）
- **MCU 周期 = max(entropy, IDCT) ≈ 384 cycles → 输出 256 Y pixels**
- Y 像素吞吐 = 256/384 ≈ **0.67 Y-pix/cyc**

**注**：峰值 1 Y-pix/cyc 仅在输出侧（line buffer → pixel_out）达到；整个流水线瓶颈在 IDCT，约 0.67 Y-pix/cyc。600MHz × 0.67 = 402 Mpix/s > 4K@60 需求 498 Mpix/s？

**⚠️ 差一点**：498 / 600 = 0.83 pix/cyc，高于 0.67。需要优化：
- **选项 A**：IDCT 用 2 parallel pipelines（Y 专用 + C 专用），Y 拿独立管线 → Y 吞吐 1 pix/cyc
- **选项 B**：IDCT 用 2 coef/cyc 输入（双端口），1 block 32 cyc，MCU 192 cyc → 256/192 = 1.33 Y-pix/cyc ✅
- **选项 C**：降低目标为 4K@30fps（Y 需求 249 Mpix/s）

**v1.0 选 B**（单 IDCT，2 coef/cyc），实现成本适中：
- 双口 transpose buffer
- 2 sample/cyc 输入的 1D IDCT（11 mult × 2 = 22 个乘法器硬件，面积翻倍但 7nm 可接受）

### 3.2 首像素延迟
- Header 解析：~200 cycles（典型 JPEG header ≤ 600 bytes）
- 第 1 个 MCU：~192 cycles（entropy + IDCT 流水，192 cyc）
- 首像素：从 header 结束起 ≈ 200 + 24 (IDCT latency) ≈ 224 cycles
- **总首像素延迟 ≈ 400~500 cycles @ 600MHz ≈ 0.8 µs**

---

## 4. SRAM 清单

| Block | 大小 | 端口 | 模块 |
|---|---|---|---|
| HTable | 6.4 KB | 1R1W | M4 Huffman |
| QTable | 0.5 KB（4×64×16b） | 1R1W | M6 Dequant |
| Transpose Buffer | 128 B（64×16b）×2 ping-pong | 2R2W | M7 IDCT |
| MCU Buffer | 6 KB（2 ping-pong） | 1R1W | M8 |
| Y Line Buffer | 64 KB（2×32 KB） | 1R1W | M8 |
| C Line Buffer | 32 KB（2×16 KB） | 1R1W | M8 |
| Input FIFO | 1 KB | FIFO | M1 |
| Output FIFO | 0.75 KB | FIFO | M1 |
| **合计** | **~111 KB** | | |

---

## 5. 时钟 / 复位
- 单时钟域 `clk`
- `rst_n` 异步复位，寄存器通过 2-flop 同步器对齐到 clk
- 所有 SRAM 复位通过 soft reset FSM 清零（避免 power-on X）

---

## 6. 综合目标与时序预算（7nm ASAP7）

| 模块 | 关键路径预估 | 风险 |
|---|---|---|
| Huffman L1 LUT | 0.5 ns | 低 |
| Huffman L2 compare | 0.9 ns | 中 |
| IDCT row（最长加法树） | 1.1 ns | **高** — 可能需 retiming |
| IDCT col（同上） | 1.1 ns | 高 |
| AXI-Lite 读回 mux | 0.4 ns | 低 |
| SRAM access time | 0.5 ns | 低（foundry SRAM ~0.3 ns） |
| **最坏路径目标** | **< 1.2 ns** | |

**若时序不收敛**：
- IDCT 插入 pipeline stage（延迟 +1~2 cycle，流水线深度允许）
- Huffman L2 路径分成两级比较
