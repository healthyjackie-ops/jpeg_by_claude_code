# Phase 0 — Spec Review Report

**Phase**: 0 (Spec & Micro-architecture)  
**Date**: 2026-04-18  
**Status**: **Ready for Review**

---

## 1. 本阶段交付物

| 文档 | 路径 | 说明 |
|---|---|---|
| 项目计划 | [docs/plan.md](../plan.md) | 6-phase 路线图 |
| 顶层规格 | [docs/spec.md](../spec.md) | 功能/接口/错误码/验收标准 |
| 微架构 | [docs/uarch.md](../uarch.md) | 模块数据流、SRAM 清单、时序预算 |
| 寄存器映射 | [docs/regmap.md](../regmap.md) | AXI-Lite CSR 定义 |

---

## 2. 关键技术决策汇总

| # | 决策 | 理由 |
|---|---|---|
| D1 | 只做 Baseline SOF0 / YUV420 / 8-bit | 用户明确需求；复杂度可控 |
| D2 | 单时钟域 600 MHz | 避免 CDC；7nm ASAP7 下余量足 |
| D3 | 接口：AXI4-Lite（CSR）+ AXI4-Stream（码流/像素） | 业界标准；便于 SoC 集成 |
| D4 | Huffman 两级查表（L1 8-bit LUT + L2 逐位比较） | 平衡面积/吞吐；fast path 1 cyc |
| D5 | IDCT：Loeffler 2D，行列分解，**2 coef/cyc 输入** | 弥补单路 1 coef/cyc 不满足 4K@60 吞吐 |
| D6 | Chroma upsample：最近邻（box filter） | 与 libjpeg 默认对拍；面积最小 |
| D7 | 输出 v1.0 仅 YCbCr 4:4:4 interleaved | 接口最简；RGB/planar 留 v2 |
| D8 | QTable/HTable 由 DQT/DHT 段自动加载 | 减少软件负担 |
| D9 | v1.0 不支持 Restart Markers (DRI=0) | 降低 FSM 复杂度 |
| D10 | Line buffer on-chip SRAM 64+32 KB | 4K 宽度约束；全片上避免外存访问 |
| **D11** | **RTL 用 Verilog-2001**（不是 SystemVerilog） | 用户指定；工具链兼容性最好，Yosys/OpenROAD 原生支持 |

---

## 3. 关键路径与性能预测

### 3.1 吞吐复核
- **4K@60fps Y 像素需求** = 3840 × 2160 × 60 = 498 Mpix/s
- **IDCT 双路输入（2 coef/cyc）** 下 MCU 周期 = 192 cyc，输出 256 Y pixels → **1.33 Y-pix/cyc**
- 600 MHz × 1.33 = **798 Mpix/s** ✅ 超过需求 60%，留余量给 back-pressure

### 3.2 时序预算（7nm ASAP7 目标 1.2 ns）
| 路径 | 预估 | 风险 |
|---|---|---|
| Huffman L1 LUT 256-entry | 0.5 ns | 低 |
| Huffman L2 逐位比较 | 0.9 ns | 中 |
| IDCT 1D 加法树 | 1.1 ns | **高**（主要风险） |
| Dequant 乘法 16×8 | 0.6 ns | 低 |
| AXI-Lite 读 mux | 0.4 ns | 低 |

**风险缓解**：IDCT 路径若不收敛，在加法树中插 1-2 级 register（流水线深度允许，不影响吞吐）。

### 3.3 面积预估
- 逻辑 ~120 k GE
- SRAM ~111 KB（HTable 6.4 + QTable 0.5 + Transpose 0.25 + MCU Buffer 6 + Y LineBuf 64 + C LineBuf 32 + FIFO 2）
- **die 估算**（ASAP7，65% util）：~0.3-0.5 mm²

---

## 4. 未决项 / 开放问题

| # | 问题 | 处理建议 |
|---|---|---|
| Q1 | IDCT 定点舍入策略（truncate vs round-to-even） | 选 round-to-even；C 模型与 RTL 必须完全一致 |
| Q2 | 输入图像若 W/H 非 16 倍数，是否由硬件裁剪？ | **v1.0 不做**，要求软件 padding；非对齐报 ERR_SIZE_OOR |
| Q3 | 多帧解码之间 QTable/HTable 是否保留？ | 保留（新 DQT/DHT 会覆盖旧表）；软件靠 SOFT_RESET 清 |
| Q4 | 输入 AXI-Stream 允许中途长时间无数据吗？ | 允许；Huffman 等待即可，不超时 |
| Q5 | 是否需要支持 byte alignment（SOS 后有残余 bits）| JPEG 标准在 marker 前需 byte-align，Huffman 消费后剩余 bits 由 unstuff 状态机丢弃至 byte 边界 |
| Q6 | `EN_DBG_CNT` 综合参数是否默认开？ | 建议**默认开**（<1% 面积），方便 silicon bringup |

---

## 5. 风险登记（更新）

| # | 风险 | 等级 | 缓解 |
|---|---|---|---|
| R1 | IDCT 2 coef/cyc 输入面积过大 | M | 若面积超预算，降级到 1 coef/cyc，接受 4K@30fps |
| R2 | Yosys + ASAP7 对大型 SRAM 支持 | M | 用 ASAP7 的 fake SRAM 模型；必要时黑盒化 |
| R3 | Huffman L2 路径拖慢频率 | M | L2 拆成 2 级比较，接受 L2 吞吐减半 |
| R4 | 覆盖率 100% 难以达成（不可达分支） | L | 允许 pragma 标注，报告中列表说明 |
| R5 | Verilog-2001 不如 SV 简洁 → 代码冗长 | L | 严格编码规范 + 代码 review |

---

## 6. Phase 1 启动清单（预先检查）

- [ ] 开发机工具：gcc/clang ≥ 10, make, Python 3.10+
- [ ] libjpeg or libjpeg-turbo 安装（对拍参考）
- [ ] 测试图集准备：Kodak 24、USC-SIPI、用户自选
- [ ] C 模型目录结构：`c_model/{src,tests,golden,Makefile}`

---

## 7. Sign-off Checklist

Review 通过需确认以下各项：

- [ ] 规格功能范围准确（baseline/YUV420/4K/600MHz）
- [ ] 接口定义满足 SoC 集成需求（AXI-Lite + AXI-Stream）
- [ ] 寄存器映射覆盖所有控制/状态/错误/调试需求
- [ ] 微架构能达到 1 Y-pix/cyc 吞吐目标（2 coef/cyc IDCT 方案）
- [ ] 错误码定义完整，覆盖所有非法输入场景
- [ ] Verilog-2001 作为 RTL 语言已确认
- [ ] 开源工具链（Verilator/cocotb/Yosys/OpenROAD/ASAP7）已确认
- [ ] 未决项 Q1-Q6 的处理建议被接受

---

## 8. 下阶段（Phase 1）预告

**内容**：实现 C 模型（位精确金标准）  
**产出**：
- `c_model/src/*.c`：分模块 C 实现（对应 RTL M2-M10）
- `c_model/tests/`：单元测试 + 集成测试
- `c_model/golden/`：libjpeg 对拍脚本
- `reports/01_c_model_test.md`：测试报告（覆盖率、PSNR、测试图集统计）

**通过标准**：
- 100 张标准图 + 1000 张随机 JPEG 与 libjpeg **逐像素一致**
- 每模块单元测试覆盖所有边界条件

**预计工期**：2 周

---

**审阅人签字**：_______________  **日期**：_______________
