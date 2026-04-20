# Wave 2 — 12-bit Precision (SOF1 Extended Sequential) Report

**Revision**: 1.0
**Date**: 2026-04-20
**Target**: JPEG Baseline Decoder IP (ASAP7 7nm, 600 MHz, 1 px/cyc)
**Base**: Wave 1 (SOF0 + JFIF 全采样 + DRI + CMYK，130/130 bit-exact)
**Parent roadmap**: [`roadmap_v2.md`](../roadmap_v2.md)

---

## 1. 阶段目标回顾

Wave 2 的唯一 Phase 把 **全链路位宽 P∈{8,12}** 参数化，在保持 SOF0 P=8
baseline 完全不变的前提下接受 SOF1 P=12 extended sequential —
C 模型和 RTL 两侧均与 libjpeg-turbo J12 bit-exact。

| Phase | Spec | 状态 | 规格 | Bit-exact |
|---|---|---|---|---|
| **13a** | C model 12-bit 解码 | ✅ | grayscale / 4:4:4 / 4:2:0 × P=12 | 20/20 |
| **13b.1** | RTL 16b/48b bus 盘点 | ✅ | 位宽迁移计划落地 | — |
| **13b.2** | header_parser SOF1 + Pq=1 DQT | ✅ | precision_o / qt 16b / dq 32b | — |
| **13b.3** | huffman DC cap + IDCT desc + pix 16b | ✅ | DC cat ≤ 15；desc_p1/p2 按 precision 分流 | — |
| **13b.4** | mcu_buffer / line_buffer / mcu_line_copy 16b | ✅ | 所有样本通路 16b | — |
| **13b.5** | pixel_out 48b tdata + PIX_FMT CSR | ✅ | tdata {4×12b}；0x02C 暴露 precision | — |
| **13c** | RTL 全链路 P=12 bit-exact + P=8 回归 | ✅ | phase13 20/20 + 历史 142/142 | 20/20 |

**结论**：Wave 2 完成，累计 bit-exact **162/162**（Wave 1 142 + Wave 2 20）。

---

## 2. 回归矩阵

### 2.1 Phase 13 P=12 差分

Verilator 5.046 + C 模型 golden（走 `decode_p12()` 分支，`golden.*_plane16`
16-bit 比对），全通过 ΔY=0 ΔC=0：

| 子集 | 语料 | 覆盖 | 结果 |
|---|---:|---|---:|
| grayscale (sg) | 6 | 16×16 / 23×17 / 45×33 / 96×96 / 241×321 | 6/6 |
| 4:4:4 (s11) | 6 | 8×8 / 16×16 / 17×13 / 32×32 / 64×64 / 199×257 | 6/6 |
| 4:2:0 (s22) | 8 | 16×16 / 23×19 / 32×32 / 57×39 / 64×64 / 128×64 / 128×128 / 321×241 | 8/8 |
| **合计** | **20** | — | **20/20** |

包含 q50..q90 量化质量、R0..R16 restart interval、非 16 对齐尺寸
（17×13 / 199×257 / 241×321 等）。

### 2.2 累积 P=8 回归

Phase 13 引入位宽参数化后，所有 P=8 语料保持 bit-exact：

| 语料集 | 通过 |
|---|---:|
| smoke (12) | 12/12 |
| phase06 (20) | 20/20 |
| phase07 (20) | 20/20 |
| phase08 (15) | 15/15 |
| phase09 (15) | 15/15 |
| phase10 (15) | 15/15 |
| phase11a (15) | 15/15 |
| phase11b (15) | 15/15 |
| phase12 (15) | 15/15 |
| **P=8 合计** | **142/142** |

---

## 3. 关键技术决策

### 3.1 位宽策略 — 样本 16b，输出 48b

- **内部样本通路**（IDCT → mcu_buffer → mcu_line_copy → line_buffer →
  pixel_out）统一到 **16 位宽**，低 12b 有效。P=8 下高 4b 零扩展，P=12
  下占满。单一通路同时承载两种精度，无需双版本 SRAM。
- **外部 AXI-Stream tdata** 48 位 = 4 × 12b channel slot，按 MSB→LSB
  `{ch0, ch1, ch2, ch3}` 打包。YCbCr/GRAY 末槽置零；CMYK 放 K。
  消费者通过 0x02C PIX_FMT.PRECISION 或 header_parser.precision_o 得
  知样本有效位宽。

### 3.2 IDCT DESCALE — 按 precision 分流

libjpeg-turbo J12 为避免 int32 溢出使用 `PASS1_BITS=1`（P=8 用 2）。
RTL 的 `desc_p1` / `desc_p2` 把两条路径收进同一函数，用 `precision`
选常数：

|  | column pass shift | row pass shift + bias | clamp |
|---|:---:|:---:|:---:|
| P=8 | `>>>11` (bias 1024) | `>>>18` (+128) | 0..255 |
| P=12 | `>>>12` (bias 2048) | `>>>17` (+2048) | 0..4095 |

### 3.3 Huffman DC category 上限

P=12 合法 DC 类别扩到 15（P=8 最多 11）。在 `huffman_decoder.v` 把
`sym > 8'd11` 改为 `sym > 8'd15`；P=8 的合法流永远不触及 12..15，向后兼容。

### 3.4 CSR 侧新增 PIX_FMT (0x02C)

`axi_lite_slave` 新增 RO 寄存器 `PIX_FMT`，bit[0]=precision，
锁存自 header_parser 的 SOF 解析结果。软件读 `STATUS.HEADER_DONE=1`
之后读 `PIX_FMT` 即可判断即将到来的像素流位宽语义。

---

## 4. 语料生成

`verification/vectors/phase13/` 20 张测试图由 `tools/gen_p12_vectors.py`
生成（libjpeg-turbo J12 编码端）：
- SOF1 marker (`FFC1`)，P=12，Pq=1 DQT。
- 覆盖三种 chroma 模式（sg / s11 / s22）× 多种尺寸 × 多种 q × 多种
  restart interval。

Phase 13 spec 将 **P=12 + {4:2:2, 4:4:0, 4:1:1, CMYK}** 及 **P=16 lossless**
延后至 Wave 4/5，保持本 wave 微小且可收敛。

---

## 5. 交付物

- **C 模型**：`c_model/src/decoder.c::decode_p12()` 独立路径；
  `idct_islow_p12` / `dequant_block_i32`（qt 16b）已接入。
- **RTL**：`rtl/src/*.v` 全部 16b/48b 参数化；jpeg_axi_top 端口 m_px_tdata 48b。
- **CSR**：regmap.md 新增 0x02C PIX_FMT；ERR_UNSUP_PREC 描述更新。
- **语料**：verification/vectors/phase13/ 20 张。
- **回归**：smoke + phase06..phase13 全通过 bit-exact 162/162。

---

## 6. 面积/时序影响（预估）

| 资源 | Wave 1 后 | Wave 2 后 | Δ |
|---|---:|---:|---:|
| 样本 SRAM（line_buffer + mcu_buffer） | ~38 KB | ~76 KB | +38 KB (8b → 16b) |
| IDCT shift/add 逻辑 | baseline | +precision mux | 可忽略 |
| 输出 FIFO（32 深） | 32 × 32b | 32 × 48b | +512 b |
| DQ 值宽 | 16b | 32b | +16b / entry |

预估门数 ~75 k → ~105 k（符合 roadmap_v2 §3 表）。时序方面，P=12
IDCT 加乘树位宽不变（仍 32b 累加），shift+bias 数值变化但拓扑不变，
预期不动 WNS。

---

## 7. 下一步 — Wave 3 启动

- **Phase 14**：SOF2 header 识别 + error-out 报错（2d）。
- **Phase 15**：Coef frame buffer loop-back（3–4d）。
- **Phase 16**：DC-only scan。
- 完整计划见 `roadmap_v2.md` §1 Wave 3。
