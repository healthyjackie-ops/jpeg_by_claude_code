# JPEG Baseline Decoder ASIC IP — 项目计划

## 1. 规格概要

| 项 | 规格 |
|---|---|
| 功能 | JPEG **Decoder** only |
| 标准 | ISO/IEC 10918-1 **Baseline**, 8-bit precision |
| 色彩 | **YUV 4:2:0** only（Y 全分辨率，Cb/Cr 1/4 采样） |
| 最大分辨率 | **3840 × 2160 (4K UHD)** |
| 吞吐 | **1 Y-pixel / cycle**（峰值） |
| 目标工艺 | **7nm**（实施用开源 ASAP7 PDK） |
| 目标频率 | **600 MHz**（1.667 ns 周期；RTL 目标 1.2 ns 留 30% 余量） |
| 接口 | **AXI4-Lite**（配置/寄存器） + **AXI4-Stream**（码流输入、像素输出） |
| 输出 | YCbCr 4:4:4 或 YCbCr 4:2:0 planar（可选） |
| 内存 | On-chip SRAM 仅作 line buffer；解码后像素通过 AXI-Stream 流出 |

### 不支持（明确排除）
- Progressive / Extended / Lossless / Hierarchical
- 12-bit precision, Arithmetic coding
- Restart markers（DRI=0，可扩展）
- YUV 4:2:2 / 4:4:4 / Grayscale
- 内嵌 ICC profile / EXIF 处理（APPn 段跳过不解析）

---

## 2. 顶层架构

```
                ┌──────────────────────────────────────────────────────┐
    bitstream   │  Header    Bitstream    Huffman    Dequant    IDCT   │   YCbCr
   ──AXI-S───►│  Parser  ►  Unpacker  ►  Decoder  ►  +IZZ   ►  8×8   │──AXI-S──►
                │  (FSM)    (unstuff)    (VLD)      (×Q tbl)  (Loeffler)│
                │     │                                            ▼    │
                │     ▼                                        MCU      │
                │  QTable/                                    Assembly  │
                │  HTable                                    (4Y+Cb+Cr) │
                │  RAM                                            ▼     │
                │                                           Chroma      │
                │                                          Upsample ──► │
                └──────────────────────────────────────────────────────┘
                                     ▲
                                AXI-Lite (CSR)
```

### 模块清单与关键路径预估 (7nm @ 600MHz)

| # | 模块 | 功能 | 难度 | 预估面积 (ASAP7, GE) |
|---|---|---|---|---|
| M1 | `jpeg_axi_top` | AXI 封装、CSR、中断 | ★ | 5k |
| M2 | `header_parser` | 解析 SOI/APPn/DQT/DHT/SOF0/SOS/EOI | ★★ | 10k |
| M3 | `bitstream_fifo` | 码流 FIFO + byte stuffing 去除 | ★ | 3k |
| M4 | `huffman_decoder` | VLD（barrel shift + LUT） | ★★★★ | 25k |
| M5 | `dc_predictor` | DC 差分还原 | ★ | 1k |
| M6 | `dequant_izz` | 反量化 + 反 Zigzag | ★★ | 8k |
| M7 | `idct_2d` | 2D IDCT（Loeffler，1D+转置+1D）| ★★★★ | 40k |
| M8 | `mcu_buffer` | 6 块 × 64 系数缓冲 + line buffer | ★★ | 20k (SRAM) |
| M9 | `chroma_upsample` | 4:2:0 → 4:4:4（双线性或 cosited） | ★★ | 6k |
| M10 | `pixel_out` | 光栅扫描输出 + AXI-Stream 打包 | ★ | 4k |
| | **总计** | | | **~120k GE + SRAM** |

### 吞吐设计
- **Huffman**：1-16 bits/symbol，平均 ~5 bits → 需每周期消耗 1 symbol，barrel shifter 宽度 32 bits，滑动窗口
- **IDCT**：全流水 1 sample/cyc 输入输出，延迟 ~20 cycles；一条 IDCT 管线即可满足 0.67 pix/cyc 全 MCU 吞吐，MCU assembly 缓冲掉抖动
- **Line buffer**：16 行 × 最大 4096 Y 像素 = 64 KB（Y），Cb/Cr 各 16 KB，总 ~96 KB 片上 SRAM

---

## 3. 项目阶段与里程碑

| Phase | 内容 | 产出报告 | 工期 |
|---|---|---|---|
| **P0** | Spec + µArch + RegMap | `reports/00_spec_review.md` | 1 周 |
| **P1** | C 模型（位精确金标准） + 对拍 libjpeg | `reports/01_c_model_test.md` | 2 周 |
| **P2** | RTL 设计（各模块 + 顶层） | `reports/02_rtl_design.md` | 5 周 |
| **P3** | 验证：RTL vs C 对拍 + 覆盖率 100% | `reports/03_verification.md` | 4 周 |
| **P4** | 综合（Yosys + ASAP7 @ 600MHz） | `reports/04_synthesis.md` | 1 周 |
| **P5** | P&R（OpenROAD + ASAP7） | `reports/05_pnr.md` | 2 周 |
| **P6** | 最终交付包 + sign-off | `reports/06_signoff.md` | 1 周 |
| | | **合计** | **~16 周** |

---

## 4. 每阶段详细计划与报告内容

### Phase 0 — Spec Review（输出 `00_spec_review.md`）
- 顶层 spec、寄存器 map、AXI 协议细节、异常处理策略
- 性能/面积/功耗预算
- **报告内容**：签字确认 spec 完整、无冲突、可实现

### Phase 1 — C 模型（输出 `01_c_model_test.md`）
**目录**：`c_model/`
- `src/`：jpeg_decode.c（分模块函数对应 RTL 结构）
- `tests/`：单元测试（每个函数）
- `golden/`：libjpeg 对拍
**通过标准**：
- 100 张测试图（标准 JPEG 测试集 + 合成极端用例）100% 与 libjpeg 逐像素一致
- 单元测试覆盖 Huffman 所有码表、IDCT 边界、所有量化表
**报告内容**：
- 测试图集统计、PSNR=∞（位精确）、每模块测试用例数
- 函数到 RTL 模块的映射表

### Phase 2 — RTL 设计（输出 `02_rtl_design.md`）
**目录**：`rtl/src/`
- 命名：每个 M1-M10 对应 `.v` 文件
- 编码规范：**Verilog-2001**，综合友好（无 latch，无异步 reset 到组合逻辑，无 `always @(*)` 中的锁存器）
- 只用 `reg`/`wire`、`always @(posedge clk)`、`always @(*)`、`parameter`、`` `define ``；不用 `logic`/`interface`/`struct`/`always_ff`/`always_comb`/`enum`/`unique`/`priority`
- 参数通过 `parameter` + `defparam` 或模块实例化参数传递；状态机状态用 `parameter` 编码
- Lint：Verilator `--lint-only -Wall` 零 warning
**通过标准**：
- 全部模块通过 lint
- 顶层仿真跑通最小 8×8 JPEG
**报告内容**：
- 每模块代码行数、端口、参数
- Lint 报告
- Pre-synthesis 快速估算（Yosys `stat`）

### Phase 3 — 验证（输出 `03_verification.md`）
**目录**：`verification/`
- **cocotb** + **Verilator** 作为仿真器
- 测试架构：
  - Unit TB：每个模块独立 TB，随机激励 + C 模型参考
  - Top TB：AXI BFM 驱动完整 JPEG，输出与 C 模型逐像素比对
- **测试向量**：
  - 随机生成 JPEG（可控 Q 值、尺寸、内容）
  - 标准图像（Lena, Baboon, Kodak 24 张, SJPEG benchmark）
  - 极端用例：全零 AC、EOB、长 zero run、最大码长、4K@4K
**覆盖率目标**：
- **Line: 100%**
- **Toggle: 100%**
- **Branch: 100%**（Verilator branch coverage）
- **Functional: 100%**（cocotb cover groups — 所有 AC/DC 码表 bin、所有 run 长度 bin、所有 block 位置）
- **FSM state + transition: 100%**
**通过标准**：
- 上述 4 类覆盖率 100%
- 1000+ 随机 JPEG 全通过
- 所有标准图通过
**报告内容**：
- 覆盖率完整表格 + HTML 报告链接
- 回归测试通过统计
- 失败用例（应为 0）

### Phase 4 — 综合（输出 `04_synthesis.md`）
**目录**：`syn/`
- 工具：Yosys + ABC + ASAP7 .lib (SLVT, 0.7V, TT, 25°C)
- 约束：`clk=1.667ns`，I/O delay 30%，false path on CSR reset
**通过标准**：
- **Timing**：WNS ≥ 0，TNS = 0（setup）
- **Area**：< 150k GE
- **无 latch、无 combinational loop**
**报告内容**：
- Timing summary（每路径组 WNS/TNS）
- Area breakdown（每模块）
- Cell count, flop count
- 最长 5 条路径分析

### Phase 5 — P&R（输出 `05_pnr.md`）
**目录**：`pnr/`
- 工具：OpenROAD（OpenLane2 flow）+ ASAP7 PDK
- Floorplan：目标 utilization 65%，方形 die
- 布线：7 层金属（ASAP7 M1-M7）
**通过标准**：
- Post-route WNS ≥ 0
- DRC clean, LVS clean
- IR drop < 5%
**报告内容**：
- Floorplan 截图、congestion map
- Post-route timing summary
- Power（dynamic + leakage，典型码流 @ 600MHz）
- Die size, utilization, M1-M7 wire length

### Phase 6 — Sign-off（输出 `06_signoff.md`）
**交付物清单**：
- RTL + TB 源码
- C 模型
- 综合/P&R 脚本
- LEF / LIB / GDS（ASAP7 产出）
- UPF（如用 power gating）
- 各阶段报告
- Databook（用户手册、寄存器说明、集成指南）

---

## 5. 目录结构

```
jpeg_by_claude_code/
├── docs/
│   ├── plan.md                    # 本文件
│   ├── spec.md                    # Phase 0 产出
│   ├── uarch.md
│   ├── regmap.md
│   └── reports/                   # 每阶段报告
│       ├── 00_spec_review.md
│       ├── 01_c_model_test.md
│       ├── 02_rtl_design.md
│       ├── 03_verification.md
│       ├── 04_synthesis.md
│       ├── 05_pnr.md
│       └── 06_signoff.md
├── c_model/                       # Phase 1
│   ├── src/        tests/         golden/
│   └── Makefile
├── rtl/                           # Phase 2
│   ├── src/{*.v}
│   └── include/
├── verification/                  # Phase 3
│   ├── cocotb/     tests/         vectors/
│   └── Makefile
├── syn/                           # Phase 4
│   ├── scripts/    constraints/   reports/
├── pnr/                           # Phase 5
│   └── scripts/    reports/
└── tools/                         # PDK、工具路径、Docker
```

---

## 6. 风险登记

| 风险 | 影响 | 缓解 |
|---|---|---|
| Huffman VLD 时序不收敛 | Critical | 用 2 级 barrel shifter + 预测查表，必要时降为 2 cyc/symbol |
| Yosys 对 7nm 综合支持有限 | High | 若 QoR 差，退路：用 OpenROAD flow 的 Yosys+ABC 组合脚本；或仅做 pre-layout 估算 |
| ASAP7 与真实 TSMC N7 偏差 | Medium | 声明"7nm-class"而非"TSMC N7"，数据作为相对参考 |
| 覆盖率 100% 可能需要剪枝 | Medium | 允许标记不可达代码（`pragma coverage off`）并在报告中列出理由 |
| 4K line buffer 面积大 | Medium | Y line buffer 用单端口 SRAM（ping-pong 行），Cb/Cr 复用 |
| AXI 背压导致流水停顿 | Low | 各模块间加 ≥16 深 FIFO，output 侧 ≥256 深 |

---

## 7. 下一步

**请确认**：
1. ✅ 工具链使用 **ASAP7 + Yosys + OpenROAD + Verilator + cocotb**（全开源，可在 M5 Max 上跑）
2. ✅ 逐 Phase 推进，每 Phase 完成后先出报告再进下一阶段
3. ✅ 第一步开始写 Phase 0：详细 spec + uarch + regmap

确认后我开始 **Phase 0**：写 `docs/spec.md`、`docs/uarch.md`、`docs/regmap.md`，并输出 `reports/00_spec_review.md`。
