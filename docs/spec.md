# JPEG Baseline Decoder — 顶层规格 (SPEC)

**Revision**: 0.1  
**Date**: 2026-04-18  
**Status**: Draft for review

---

## 1. 功能范围

### 1.1 支持
- **ISO/IEC 10918-1 Baseline sequential DCT**（SOF0）
- **8-bit precision** per sample
- **YUV 4:2:0** subsampling only（H/V sampling factors: Y=2×2, Cb=1×1, Cr=1×1）
- **3 components**（Y, Cb, Cr）
- **2 Huffman tables per class**（Y-DC/Y-AC 一对，Cb/Cr 共享 C-DC/C-AC 一对）
- **Up to 4 quantization tables**（DQT）
- **Max image size**: 3840 × 2160（4K UHD），内部支持到 8192×8192（宽高寄存器 13 bit）
- **Min image size**: 16×16（一个 MCU）
- **Image size 必须是 16 的倍数**（4:2:0 MCU 对齐）；非对齐情况由上层软件 padding 后送入，解码器不做边界裁剪
- **Markers**: SOI, APP0-APP15（跳过）, DQT, DHT, SOF0, SOS, EOI
- **Restart markers (RSTn)**: 可选支持，由 `DRI` 寄存器 / 标记控制，**首版 v1.0 不支持（DRI 必须为 0）**

### 1.2 不支持（解码器直接报 ERROR）
- SOF1-SOF15（Extended / Progressive / Lossless / Hierarchical / Arithmetic）
- 12-bit precision
- YUV 4:4:4, 4:2:2, 4:1:1, Grayscale
- Component 数 ≠ 3
- Sampling factor ≠ Y(2×2) + Cb(1×1) + Cr(1×1)
- 嵌入式 thumbnail / ICC profile / EXIF 处理（APPn 段透传跳过，不解析）

---

## 2. 接口

### 2.1 时钟与复位
| 信号 | 方向 | 说明 |
|---|---|---|
| `clk` | in | 主时钟，600 MHz 目标 |
| `rst_n` | in | 异步 assert，同步 deassert，低有效 |

**所有 AXI 通道共用同一时钟（单时钟域设计）**。

### 2.2 AXI4-Lite 从接口（CSR 配置）
| 信号 | 宽度 | 说明 |
|---|---|---|
| `s_axi_awaddr` | 12 | 地址（4 KB 空间） |
| `s_axi_awvalid/ready` | 1 | 写地址握手 |
| `s_axi_wdata` | 32 | 写数据 |
| `s_axi_wstrb` | 4 | 字节使能（只支持 0xF） |
| `s_axi_wvalid/ready` | 1 | 写数据握手 |
| `s_axi_bresp` | 2 | 00=OKAY, 10=SLVERR |
| `s_axi_bvalid/ready` | 1 | 写响应握手 |
| `s_axi_araddr` | 12 | 读地址 |
| `s_axi_arvalid/ready` | 1 | 读地址握手 |
| `s_axi_rdata` | 32 | 读数据 |
| `s_axi_rresp` | 2 | 00=OKAY, 10=SLVERR |
| `s_axi_rvalid/ready` | 1 | 读数据握手 |

### 2.3 AXI4-Stream 码流输入
| 信号 | 宽度 | 说明 |
|---|---|---|
| `s_axis_tdata` | 32 | 码流数据，**小端字节序**（byte0 = tdata[7:0]） |
| `s_axis_tvalid/tready` | 1 | 握手 |
| `s_axis_tkeep` | 4 | 字节有效（仅 TLAST 节拍非 0xF） |
| `s_axis_tlast` | 1 | 最后一个节拍（完整 JPEG 结束，含 EOI） |

**字节顺序规定**：对于 4 字节 TDATA，JPEG 流中较早出现的字节位于低位（tdata[7:0]）。
**TLAST 的节拍**由 tkeep 指示尾字节数（可能 1/2/3/4 字节）。

### 2.4 AXI4-Stream 像素输出
| 信号 | 宽度 | 说明 |
|---|---|---|
| `m_axis_tdata` | 24 | `{Y[7:0], Cb[7:0], Cr[7:0]}`，YCbCr 4:4:4 交错 |
| `m_axis_tvalid/tready` | 1 | 握手 |
| `m_axis_tuser` | 1 | SOF：一帧第一个像素为 1 |
| `m_axis_tlast` | 1 | EOL：每行最后一个像素为 1 |

**输出格式 v1.0 固定为 YCbCr 4:4:4 interleaved**。配置 `CONFIG.OUT_FMT=1` 时输出 4:2:0 planar（未来扩展保留，v1.0 不实现）。

### 2.5 中断
| 信号 | 方向 | 说明 |
|---|---|---|
| `irq` | out | 高电平有效，由 `INT_EN & INT_STATUS` 产生；写 1 清除 `INT_STATUS` |

中断源：
- `DONE`: 一帧解码完成（EOI 被消费且最后一个像素已输出）
- `ERROR`: 解码错误（详见 `ERROR_CODE`）

---

## 3. 操作流程（软件侧）

```
1. 软件 reset:        写 CTRL.SOFT_RESET=1，等待 STATUS.BUSY=0
2. 配置中断:          写 INT_EN = 0x3 (DONE + ERROR)
3. 启动解码:          写 CTRL.START=1
4. 驱动码流:          向 AXI-Stream 输入推送整个 JPEG 文件（含 SOI..EOI）
                      必须以 TLAST 结束最后一个节拍
5. 消费像素:          从 AXI-Stream 输出接收 Y×W×H 个像素（按光栅顺序）
                      TUSER 标记帧首，TLAST 标记每行尾
6. 等待中断:          DONE 触发；读 STATUS.FRAME_DONE=1
                      或 ERROR 触发；读 ERROR_CODE
7. 清中断:            写 INT_STATUS 对应 bit = 1
8. 下一帧:            重新写 CTRL.START=1（不需要 reset）
```

**注意**：
- 软件不需要预配置图像宽高 — 由 SOF0 段解析后自动写入 `IMG_WIDTH/HEIGHT`（RO）
- 软件不需要预加载 QTable/HTable — 由 DQT/DHT 段解析后自动加载
- 如果 JPEG 不合法（非 baseline / 非 420 / Huffman code 错），ERROR 立即触发，解码器停止接受新码流，直到软件 CTRL.SOFT_RESET=1

---

## 4. 性能指标

| 指标 | 目标 | 说明 |
|---|---|---|
| 时钟频率 | **600 MHz** @ 7nm (ASAP7) | RTL 约束 1.2 ns，留 30% 余量 |
| 峰值吞吐 | **1 Y-pixel / cycle** | 即 600 Mpix/s Y |
| 实际吞吐 | ~0.83 Y-pix/cyc avg | 受 MCU 内部 IDCT 限制 |
| 4K@60fps 能力 | ✅ 满足（需 498 Mpix/s Y） | |
| 首像素延迟 | < 500 cycles | header 解析 + 1 MCU |
| 硬件面积 | < 150k GE + ~96 KB SRAM | ASAP7 预估 |
| 功耗 | < 50 mW（典型 JPEG @ 600MHz） | post-layout |

---

## 5. 错误码定义

`ERROR_CODE` 寄存器 bit 定义：

| Bit | 名称 | 含义 |
|---|---|---|
| 0 | `ERR_UNSUP_SOF` | SOF 不是 SOF0 |
| 1 | `ERR_UNSUP_PREC` | precision ≠ 8 |
| 2 | `ERR_UNSUP_CHROMA` | 不是 YUV420 / 非 3 component |
| 3 | `ERR_BAD_HUFFMAN` | Huffman 解码找不到匹配码 |
| 4 | `ERR_BAD_MARKER` | 遇到未知/非法 marker |
| 5 | `ERR_DRI_NONZERO` | v1.0 不支持 DRI |
| 6 | `ERR_SIZE_OOR` | W/H > 4096 或 = 0 或非 16 倍数 |
| 7 | `ERR_STREAM_TRUNC` | TLAST 到来前未见 EOI |
| 8 | `ERR_AXI_PROTOCOL` | AXI 协议违例（保留） |
| 31:9 | reserved | 0 |

---

## 6. 验证验收标准（详见 verification 报告）

- **功能**：100 张标准图 + 1000 张随机 JPEG，与 libjpeg 逐像素一致
- **性能**：4K 图像解码时钟周期数 ≤ 5 × 宽 × 高 / 4（即平均 > 0.8 pix/cyc）
- **覆盖率**：line/toggle/branch/functional 均 100%
- **异常**：每个 `ERR_*` 至少 1 个诱发用例，均正确上报

---

## 7. 交付

- RTL (**Verilog-2001**, `.v` 源文件) + 顶层 wrapper
- C 模型（golden）
- 完整 TB（cocotb + Verilator）
- 综合脚本（Yosys）+ 约束（SDC）
- P&R 脚本（OpenROAD）+ ASAP7 配置
- 每阶段报告 + Databook
