# Phase 2 — RTL Design Report

**Revision**: 0.1
**Date**: 2026-04-18
**Target**: JPEG Baseline Decoder IP (ASAP7 7nm, 600MHz, 1 px/cyc)
**Source**: [`rtl/src/*.v`](../../rtl/src/), [`rtl/include/jpeg_defs.vh`](../../rtl/include/jpeg_defs.vh)
**HDL**: Verilog-2001 (严格；无 SystemVerilog 构造)
**Lint**: Verilator 5.046 `--lint-only -Wall` **零警告** ✅

---

## 1. 阶段目标回顾

| 项 | 计划 | 状态 |
|---|---|---|
| 模块拆分与 C 模型一一对应 | 是 | ✅ |
| 所有模块 Verilog-2001 编译 | 是 | ✅ |
| Verilator `--lint-only -Wall` 零警告 | 是 | ✅ |
| 顶层 AXI4-Lite + AXI-Stream 封装 | 是 | ✅ |
| 功能仿真通过（C↔RTL 差分） | 推迟至 Phase 3 | ⏳ |

---

## 2. 模块清单（18 个）

| # | 模块 | 文件 | 代码行 | 对应 C 符号 |
|---|---|---|---:|---|
| 1 | `jpeg_defs.vh`       | [include/jpeg_defs.vh](../../rtl/include/jpeg_defs.vh)    |  45 | 宏 / FIX 常数 |
| 2 | `bitstream_unpack`   | [src/bitstream_unpack.v](../../rtl/src/bitstream_unpack.v) | 124 | `bs_fill_bits / bs_get_bits_u` |
| 3 | `header_parser`      | [src/header_parser.v](../../rtl/src/header_parser.v)       | 522 | `jpeg_parse_headers` + `parse_d*` |
| 4 | `qtable_ram`         | [src/qtable_ram.v](../../rtl/src/qtable_ram.v)             |  28 | `jpeg_ctx_t.qt[]` |
| 5 | `htable_ram`         | [src/htable_ram.v](../../rtl/src/htable_ram.v)             | 153 | `jpeg_build_huffman_tables` |
| 6 | `huffman_decoder`    | [src/huffman_decoder.v](../../rtl/src/huffman_decoder.v)   | 310 | `huff_decode_symbol` + `huff_decode_block` |
| 7 | `dc_predictor`       | [src/dc_predictor.v](../../rtl/src/dc_predictor.v)         |  40 | `component_t.dc_pred` |
| 8 | `dequant_izz`        | [src/dequant_izz.v](../../rtl/src/dequant_izz.v)           | 117 | `dequant_block` |
| 9 | `idct_1d`            | [src/idct_1d.v](../../rtl/src/idct_1d.v)                   |  82 | `idct_islow` 列核 |
| 10| `idct_2d`            | [src/idct_2d.v](../../rtl/src/idct_2d.v)                   | 181 | `idct_islow` 两遍包裹 |
| 11| `mcu_buffer`         | [src/mcu_buffer.v](../../rtl/src/mcu_buffer.v)             | 114 | `y_blk[4]/cb_blk/cr_blk` |
| 12| `chroma_upsample`    | [src/chroma_upsample.v](../../rtl/src/chroma_upsample.v)   |  19 | `chroma_upsample_nn` |
| 13| `line_buffer`        | [src/line_buffer.v](../../rtl/src/line_buffer.v)           |  63 | 输出像素行缓存 |
| 14| `mcu_line_copy`      | [src/mcu_line_copy.v](../../rtl/src/mcu_line_copy.v)       | 124 | `copy_block_*` |
| 15| `pixel_out`          | [src/pixel_out.v](../../rtl/src/pixel_out.v)               | 112 | `write_pixel` (光栅扫描) |
| 16| `block_sequencer`    | [src/block_sequencer.v](../../rtl/src/block_sequencer.v)   | 230 | `decoder.c` 主循环 |
| 17| `axi_lite_slave`     | [src/axi_lite_slave.v](../../rtl/src/axi_lite_slave.v)     | 264 | CSR |
| 18| `axi_stream_fifo`    | [src/axi_stream_fifo.v](../../rtl/src/axi_stream_fifo.v)   |  70 | 输入 / 输出缓冲 |
| 19| `jpeg_axi_top`       | [src/jpeg_axi_top.v](../../rtl/src/jpeg_axi_top.v)         | 480 | 顶层互联 |
| **合计** | | | **3 078** 行 | |

---

## 3. 顶层数据流

```
          ┌──────────────────────────── AXI-Lite (CSR) ──────────────────┐
          │                                                              │
    s_bs_*─►┌──────────┐   byte (8b)   ┌──────────┐ ┌──────────────┐    │
(AXI-Stream)│ in_fifo  ├──────────────►│  router  ├►│header_parser │──┐ │
            │ DEPTH=32 │               │(data_mode│ └──────┬───────┘  │ │
            └──────────┘               │  mux)    │        │ QT/HT    │ │
                                       └──┬───────┘        ▼          │ │
                                          │        ┌──────────────┐   │ │
                                          └────────►bitstream_    │   │ │
                                                   │  unpack     │   │ │
                                                   └──────┬──────┘   │ │
                                                          │peek      │ │
                                                          ▼          │ │
                                                   ┌──────────────┐  │ │
                                                   │huffman_      │  │ │
                                                   │  decoder     │  │ │
                                                   └──────┬───────┘  │ │
                                                     coef │ DC pred  │ │
                                                          ▼          │ │
                                                   ┌──────────────┐  │ │
                                                   │dequant_izz   │  │ │
                                                   └──────┬───────┘  │ │
                                                          ▼          │ │
                                                   ┌──────────────┐  │ │
                                                   │  idct_2d     │  │ │
                                                   └──────┬───────┘  │ │
                                                8 px/cyc │           │ │
                                                          ▼          │ │
                                                   ┌──────────────┐  │ │
                                                   │ mcu_buffer   │◄──block_sequencer
                                                   │(16×16 + 8×8) │  │ │
                                                   └──────┬───────┘  │ │
                                                          ▼          │ │
                                                   ┌──────────────┐  │ │
                                                   │mcu_line_copy │  │ │
                                                   └──────┬───────┘  │ │
                                                          ▼          │ │
                                                   ┌──────────────┐  │ │
                                                   │ line_buffer  │  │ │
                                                   │(16 scan lns) │  │ │
                                                   └──────┬───────┘  │ │
                                                          ▼          │ │
                                                   ┌──────────────┐  │ │
                                                   │  pixel_out   │  │ │
                                                   │ (raster→YCbCr│  │ │
                                                   └──────┬───────┘  │ │
                                                          ▼          │ │
                                                   ┌──────────────┐  │ │
                                                   │ out_fifo     │──┼─► m_px_*
                                                   │ DEPTH=32     │  │ │
                                                   └──────────────┘  │ │
                                                                     └─►irq
```

---

## 4. 接口总览

### 4.1 外部 (顶层 `jpeg_axi_top`)

| 接口 | 宽度 / 协议 | 方向 | 说明 |
|---|---|---|---|
| `aclk` | 1 | in | 600 MHz 系统时钟 |
| `aresetn` | 1 | in | 异步低电平复位 |
| `csr_*` | 12b 地址 / 32b 数据 AXI4-Lite | slave | 见 `docs/regmap.md` |
| `s_bs_*` | 8b + tlast AXI-Stream | slave | JPEG 码流 |
| `m_px_*` | 24b + tuser(SOF) + tlast(EOL) AXI-Stream | master | YCbCr 4:4:4 像素 |
| `irq` | 1 | out | `|(INT_STATUS & INT_EN)` |

### 4.2 内部关键握手

| 发送方 → 接收方 | 协议 | 说明 |
|---|---|---|
| in_fifo → router → hp/bs | valid/ready (byte) | 由 `header_parser.data_mode` 分发 |
| bs → huffman | peek_win[15:0] + bit_cnt_o[6:0] + consume_n/req | 逐位匹配消耗 |
| huffman → dequant | coef_wr/coef_nat_idx/coef_val | 64 系数流入 |
| dequant → idct | dq_wr/idx/val/done | natural-order 64 系数 |
| idct → mcu_buffer | pix_valid + pix_row + 8×px | 8 像素/cyc |
| block_sequencer → * | 各 start/done 脉冲 | 全局编排 |
| line_buffer → pixel_out | rd_addr / rd_data | 组合读 |
| pixel_out → out_fifo | AXI-Stream 24b | |

---

## 5. 关键设计点

### 5.1 Bitstream 解包（`bitstream_unpack`）
- 32-bit 右对齐 shift register，`bit_cnt ≤ 24` 才吸纳新字节
- 组合 `peek_win` 取高 16 bits；`consume_n ≤ 16` 单拍消耗
- `ff_wait` 锁存 0xFF 字节，下一字节决定：`00` → 丢弃（stuff 解除），非 0 → marker (撤回已入 0xFF + 置位 `marker_detected`)
- 输出 `bit_cnt_o[6:0]`（≥7 bits 避免自包饱和时溢出）供 Huffman 判断可用位

### 5.2 Huffman 解码（`huffman_decoder`）
- 实现 T.81 A.6.2 逐位匹配算法（与 C 模型 `huff_decode_symbol` bit-exact）
- 13 状态 FSM：`S_WDC → S_DC_LOOK → S_DC_SIZE → S_DC_AMP → S_DC_WR → S_WAC → S_AC_{LOOK,SIZE,AMP,WR}` → `S_DONE`
- `MAXCODE[l]` 18-bit signed，`0x3FFFF` 作为 `-1` sentinel（空长度）
- `ZIGZAG` 64 entry 常量表，AC 写入 `coef_nat_idx`
- DC `size > 11` 或 AC `k ≥ 64` → `ERR_BAD_HUFFMAN`

### 5.3 反量化 + IDCT
- `dequant_izz`：系数先 natural-order 写入 64×16b 缓冲，然后按 index 读 QT 流式输出（65 cycles/block）
- `idct_1d`：纯组合 JDCT_ISLOW（12 个 14-bit FIX 常数，C 模型 bit-exact）
  - 所有中间量 32-bit signed（与 C `int32_t` 一致，含 wrap）
- `idct_2d`：两个 `idct_1d` 实例（Pass 1 / Pass 2 独立），16 cycles/block 完成
  - Pass1: `DESCALE(x, 11) = (x + 1024) >>> 11`，写 `ws[]` 32-bit
  - Pass2: `DESCALE(x, 18) = (x + 131072) >>> 18 + 128`，饱和 [0,255]

### 5.4 MCU 汇编 & 输出
- `mcu_buffer` 16×16 Y + 8×8 Cb + 8×8 Cr 单缓冲
- `mcu_line_copy`：MCU 结束后把内容复制到 `line_buffer`（16 Y + 8 C scan lines），按 `mcu_col_idx` 横向偏移
- `line_buffer` 参数化 MAX_W=4096（≈96 KB SRAM；综合时推断 BRAM）
- `chroma_upsample`：4:2:0 nearest-neighbor，`c_row = y_row>>1, c_col = y_col>>1`（组合）
- `pixel_out` 光栅扫描 16 × W 像素 / MCU 行，AXI-Stream 24b `{Y,Cb,Cr}`

### 5.5 顶层编排（`block_sequencer`）
- 9 状态 FSM：`IDLE→START_BLK→WAIT_HUF→WAIT_IDCT→NEXT_BLK→MCU_COPY→NEXT_MCU→ROW_OUT→DONE`
- `blk_idx` 0..5：Y0/Y1/Y2/Y3/Cb/Cr，多路选择 DC/AC Huffman sel + QT sel
- DC 预测写回与 `h_dc_pred_upd` 脉冲同拍
- `mx`/`my` 扫描 MCU 行/列，行末触发 `row_ready`
- 帧末 `frame_done_o`（同步到 `axi_lite_slave.ev_done`）

### 5.6 AXI-Lite CSR
- 地址译码严格按 [regmap.md](../regmap.md)
- `START` 仅在 `BUSY=0` 时生效；`SOFT_RESET` 清 sticky + ICR + frame_done
- `INT_STATUS` 用户 W1C；`irq = |(INT_STATUS & INT_EN)`
- SLVERR 场景：`wstrb != 0xF`、`CONFIG.OUT_FMT=1`、`CONFIG.OUT_RGB=1`
- 错误 sticky：`err_code_sticky |= err_code_in` 每拍累积，软复位清零

---

## 6. 资源粗估（综合前）

| 类别 | 估计 | 备注 |
|---|---|---|
| 触发器 (FF) | ~ 3 k | 主要 FSM + 寄存器接口 |
| LUT / 组合 | ~ 15 k 门 | IDCT 含 22 乘法 + 大量加法 |
| SRAM | ~ 96 KB | line_buffer 16×4096 Y + 2×8×2048 C |
| RAM (BRAM) | ~ 4 KB | qtable(256B) + htable(~3.5KB 含 mincode/maxcode) + mcu_buffer(384B) + FIFO(256B×2) |
| 乘法器 | 22 | IDCT 两个 8-point (11 mul/inst × 2 = 22) + dequant(1) = 23 |

> 具体数字在 Phase 4 综合后更新。

---

## 7. 已知限制 & 待办

| 项 | 说明 | 计划 |
|---|---|---|
| Huffman 两级 LUT 加速 | 当前逐位匹配（最坏 16 cyc/符号），未达 1 px/cyc | Phase 4 改造 |
| Ping-pong buffer | 当前单缓冲，MCU 间不能流水 | Phase 4 改造 |
| DRI (restart marker) | 不支持（与 spec 一致） | v2.0 |
| `data_mode` 字节路由 | 顶层简单 MUX；header 段结束 `data_mode` 高后，bitstream_unpack 开始吸纳字节。marker 检测（0xFF 非 stuff）会触发 `marker_detected` 但顶层暂未处理（EOI 目前由 block_sequencer 帧末结构性识别） | Phase 3 仿真后评估是否需要额外 EOI 校验 |
| ABORT 功能 | CTRL.ABORT 复用 softrst 链，未强制清 output FIFO 中残余 | Phase 3 补充 |
| `axi_lite_slave` 不支持 outstanding 事务 | 单 beat 模式（AXI-Lite 合法） | 保留 |

---

## 8. 验证计划（Phase 3 预告）

1. **Verilator 级差分测试**：
   - 将 `jpeg_axi_top` 实例化到 C++ testbench
   - 按 AXI-Stream 馈入 `c_model/tests/jpegs/*.jpg` 1210 张
   - 对比 RTL 输出像素 vs C 模型 `jpeg_decode()` 结果
   - 目标：**1210/1210 bit-exact**
2. **Cocotb / SystemVerilog 单元级测试**：
   - `bitstream_unpack`：随机字节流 + stuff + marker
   - `huffman_decoder`：随机 BITS/HUFFVAL + 随机码字
   - `idct_2d`：随机系数 vs C `idct_islow`
3. **AXI4-Lite Protocol Checker**：`cocotb-axi-lite` 检查握手合法性
4. **覆盖率目标**：line + toggle + FSM 状态达 **100%**

---

## 9. 构建 & Lint

```bash
cd rtl
verilator --lint-only -Wall -Iinclude --top-module jpeg_axi_top \
  src/jpeg_axi_top.v src/axi_lite_slave.v src/axi_stream_fifo.v \
  src/bitstream_unpack.v src/header_parser.v src/qtable_ram.v src/htable_ram.v \
  src/huffman_decoder.v src/dc_predictor.v src/dequant_izz.v \
  src/idct_1d.v src/idct_2d.v src/mcu_buffer.v src/chroma_upsample.v \
  src/line_buffer.v src/mcu_line_copy.v src/block_sequencer.v src/pixel_out.v
```

**实测**：Verilator 5.046，walltime 0.037 s，**0 warnings**，exit 0 ✅

---

## 10. 下阶段准入条件（Phase 3 — 功能仿真）

- [x] 所有模块通过 `verilator --lint-only -Wall`
- [x] 顶层 `jpeg_axi_top` elaborate 成功
- [ ] 完成 C++ testbench 驱动 AXI + Stream
- [ ] 1210 / 1210 JPEG 解码与 C 模型 bit-exact
- [ ] 单元测试覆盖 100% （line + FSM）
- [ ] AXI4-Lite 协议检查零违规

请审阅本报告，确认后进入 **Phase 3：RTL 功能仿真与单元测试**。
