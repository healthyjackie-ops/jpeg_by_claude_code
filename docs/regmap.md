# JPEG Baseline Decoder — 寄存器映射 (RegMap)

**Revision**: 0.1  
**Date**: 2026-04-18  
**Bus**: AXI4-Lite, 32-bit data, 12-bit address (4 KB)

---

## 1. 地址空间总览

| 范围 | 内容 |
|---|---|
| `0x000 - 0x0FF` | 控制/状态寄存器（CSR） |
| `0x100 - 0x1FF` | 保留 |
| `0x200 - 0x2FF` | 调试/性能计数器（可选，RO） |
| `0x300 - 0xFFF` | 保留 |

QTable / HTable **不通过 CSR 加载**，由 DQT/DHT 段自动填充。

---

## 2. 寄存器列表

### `0x000` ID (RO)
| Bits | Name | Reset | 描述 |
|---|---|---|---|
| 31:16 | `VENDOR_ID` | `0xA17C` | 固定 |
| 15:8 | `DEVICE_ID` | `0x01` | JPEG Decoder |
| 7:0 | `VERSION` | `0x10` | v1.0 |

### `0x004` CTRL (RW)
| Bits | Name | Reset | 描述 |
|---|---|---|---|
| 0 | `START` | 0 | 写 1 启动一帧解码；自动清零 |
| 1 | `SOFT_RESET` | 0 | 写 1 触发软复位（清流水线、FIFO、状态机）；自清 |
| 2 | `ABORT` | 0 | 写 1 中止当前帧，丢弃残余码流 |
| 31:3 | reserved | 0 | 写 0 读 0 |

**时序**：
- `START` 仅在 `STATUS.BUSY=0` 时生效
- `SOFT_RESET` 在任何时刻有效，复位完成后 `STATUS.BUSY=0`

### `0x008` STATUS (RO)
| Bits | Name | Reset | 描述 |
|---|---|---|---|
| 0 | `BUSY` | 0 | 解码进行中 |
| 1 | `FRAME_DONE` | 0 | 当前帧解码完成（EOI 已消费且末像素已输出）；由下次 START 清除 |
| 2 | `ERROR` | 0 | 错误状态；`ERROR_CODE` 有效 |
| 3 | `INPUT_EMPTY` | 1 | 输入 FIFO 空 |
| 4 | `OUTPUT_FULL` | 0 | 输出 FIFO 满 |
| 5 | `HEADER_DONE` | 0 | SOS 已解析，IMG_WIDTH/HEIGHT 等已就绪 |
| 31:6 | reserved | 0 | |

### `0x00C` INT_EN (RW)
| Bits | Name | Reset | 描述 |
|---|---|---|---|
| 0 | `IE_DONE` | 0 | 使能 DONE 中断 |
| 1 | `IE_ERROR` | 0 | 使能 ERROR 中断 |
| 2 | `IE_HEADER` | 0 | 使能 HEADER_DONE 中断（可选） |
| 31:3 | reserved | 0 | |

### `0x010` INT_STATUS (RW1C)
| Bits | Name | Reset | 描述 |
|---|---|---|---|
| 0 | `IS_DONE` | 0 | 写 1 清除 |
| 1 | `IS_ERROR` | 0 | 写 1 清除 |
| 2 | `IS_HEADER` | 0 | 写 1 清除 |
| 31:3 | reserved | 0 | |

`irq = |(INT_STATUS & INT_EN)`

### `0x014` IMG_WIDTH (RO)
| Bits | Name | Reset | 描述 |
|---|---|---|---|
| 15:0 | `WIDTH` | 0 | 图像宽度（像素），由 SOF0 填充 |
| 31:16 | reserved | 0 | |

### `0x018` IMG_HEIGHT (RO)
| Bits | Name | Reset | 描述 |
|---|---|---|---|
| 15:0 | `HEIGHT` | 0 | 图像高度（像素），由 SOF0 填充 |
| 31:16 | reserved | 0 | |

### `0x01C` PIXEL_COUNT (RO)
| Bits | Name | Reset | 描述 |
|---|---|---|---|
| 31:0 | `PIXELS` | 0 | 已输出像素数（用于进度/调试） |

### `0x020` ERROR_CODE (RO)
| Bits | Name | Reset | 描述 |
|---|---|---|---|
| 0 | `ERR_UNSUP_SOF` | 0 | 非 SOF0/SOF1（Phase 13 起接受 SOF1） |
| 1 | `ERR_UNSUP_PREC` | 0 | precision ≠ 8 且 ≠ 12（Phase 13 起接受 P=12） |
| 2 | `ERR_UNSUP_CHROMA` | 0 | 非 YUV420 / comp ≠ 3 |
| 3 | `ERR_BAD_HUFFMAN` | 0 | Huffman 未命中 |
| 4 | `ERR_BAD_MARKER` | 0 | 未知 marker |
| 5 | `ERR_DRI_NONZERO` | 0 | DRI ≠ 0（v1.0 不支持） |
| 6 | `ERR_SIZE_OOR` | 0 | W/H 越界或非 16 倍数 |
| 7 | `ERR_STREAM_TRUNC` | 0 | TLAST 前未见 EOI |
| 8 | `ERR_AXI_PROTOCOL` | 0 | 保留 |
| 31:9 | reserved | 0 | |

每一 bit 独立 sticky，由 `SOFT_RESET` 清零。`ERROR_CODE ≠ 0` 时 `STATUS.ERROR=1`。

### `0x024` CONFIG (RW)
| Bits | Name | Reset | 描述 |
|---|---|---|---|
| 0 | `OUT_FMT` | 0 | 0 = YCbCr 4:4:4 interleaved (v1.0 唯一); 1 = planar (v1.0 写 1 返回 SLVERR) |
| 1 | `OUT_RGB` | 0 | 0 = YCbCr 输出; 1 = RGB 输出（v1.0 不支持，写 1 SLVERR） |
| 31:2 | reserved | 0 | |

### `0x028` SCRATCH (RW)
| Bits | Name | Reset | 描述 |
|---|---|---|---|
| 31:0 | `SCRATCH` | 0 | 软件自用；硬件不读 |

### `0x02C` PIX_FMT (RO) — Phase 13
| Bits | Name | Reset | 描述 |
|---|---|---|---|
| 0 | `PRECISION` | 0 | 0 = P=8（SOF0 baseline）；1 = P=12（SOF1 extended sequential）。由 header_parser 在 SOF 解析完成时锁存；软件据此判断输出像素的有效位宽 |
| 31:1 | reserved | 0 | |

**用法**：软件读 `STATUS.HEADER_DONE=1` 后读 `PIX_FMT` 判断像素流格式。当 `PRECISION=1` 时，输出 AXI-Stream `tdata[47:0]` 的每个 12b 通道槽位填满 12b 有效样本（0..4095）；当 `PRECISION=0` 时，样本值占低 8b（0..255），高 4b 为零扩展。

---

## 2.1 输出像素流格式（m_px_*）

| 信号 | 宽度 | 说明 |
|---|---|---|
| `m_px_tdata` | 48b | **Phase 13** — 4 × 12b 通道槽位，MSB→LSB: `{ch0, ch1, ch2, ch3}` |
| `m_px_tuser` | 1b | SOF（帧首像素） |
| `m_px_tlast` | 1b | EOL（每扫描线末像素） |
| `m_px_tvalid/tready` | 1b | 标准 AXI-Stream 握手 |

**通道映射**（由 header_parser 的 chroma_mode 决定）：

| 模式 | ch0 | ch1 | ch2 | ch3 |
|---|---|---|---|---|
| Grayscale | Y | 0x080 (P=8) / 0x800 (P=12) | 同 ch1 | 0 |
| YCbCr (4:4:4 / 4:2:0 / 4:2:2 / 4:4:0 / 4:1:1) | Y | Cb | Cr | 0 |
| CMYK | C | M | Y | K |

**精度编码**：
- P=8: 样本值 0..255 写入每个 12b 槽位的低 8b（bits [7:0]），高 4b（bits [11:8]）零扩展。
- P=12: 样本值 0..4095 直接写入 12b 槽位。

---

## 3. 调试计数器（`0x200-0x2FF`, RO）

v1.0 可选实现，便于性能分析与调试。**若综合启用 `EN_DBG_CNT`，以下寄存器生效**。

### `0x200` CYC_FRAME
当前帧解码总周期数（START 到 DONE）

### `0x204` CYC_HEADER
Header 解析周期数

### `0x208` CYC_ENTROPY_STALL
Huffman/entropy 饥饿周期数（等待输入 FIFO）

### `0x20C` CYC_OUTPUT_STALL
输出 stream 背压周期数（等待 TREADY）

### `0x210` MCU_COUNT
当前帧已解码的 MCU 数

### `0x214` HUFFMAN_L2_COUNT
Huffman Level-2 慢路径命中次数（debug）

---

## 4. 访问规则

| 规则 | 说明 |
|---|---|
| 未实现地址读 | 返回 0，BRESP = OKAY |
| 未实现地址写 | 忽略，BRESP = OKAY |
| RO 寄存器写 | 忽略，BRESP = OKAY |
| RW 寄存器保留位写 | 忽略，读回 0 |
| 不支持功能写（OUT_FMT=1, OUT_RGB=1）| 忽略，BRESP = SLVERR |
| `wstrb ≠ 0xF` | 忽略写，BRESP = SLVERR |
| AXI-Lite burst / 非对齐 | 按标准拒绝（SLVERR） |

---

## 5. 典型软件序列（伪代码）

```c
// 初始化
write(ID)  -> 检查 0xA17C_01_10
write(CTRL, 0x2);               // SOFT_RESET
while (read(STATUS) & 0x1);     // wait BUSY=0
write(INT_EN, 0x3);             // enable DONE + ERROR

// 每帧
write(CTRL, 0x1);               // START
// 通过 DMA 把 JPEG 文件送入 AXI-Stream（最后节拍带 TLAST）
// ... 等待 irq ...
status = read(INT_STATUS);
if (status & 0x2) {              // ERROR
    err = read(ERROR_CODE);
    // 处理错误
    write(CTRL, 0x2);            // SOFT_RESET
} else if (status & 0x1) {       // DONE
    w = read(IMG_WIDTH);
    h = read(IMG_HEIGHT);
    // 像素已通过输出 stream 接收
}
write(INT_STATUS, status);       // 清中断
```

---

## 6. 未来扩展（v2.0+ 预留）

| 功能 | 预留寄存器 |
|---|---|
| YUV422 / 444 support | `CONFIG.CHROMA[4:2]` |
| RGB 输出 | `CONFIG.OUT_RGB` |
| Planar 输出 | `CONFIG.OUT_FMT` |
| DRI 支持 | ERROR bit 5 可清除 |
| 12-bit 精度 | `PIX_FMT.PRECISION` (0x02C, Phase 13 已实现) |
| 多 slice 并行 | `0x100-0x1FF` 预留分片 base |
