# Roadmap v2 — JPEG Decoder full ISO/IEC 10918-1 coverage

**Revision**: 2.0 (tier C — full ISO coverage)
**Date**: 2026-04-19
**Parent spec**: [`spec.md`](spec.md) (v1.0, baseline only)
**Status**: Planning complete, execution in progress (see Wave-1 phases below)

---

## 0. 目标

把当前 v1.0 baseline-only 解码器（SOF0 + 4:2:0 + 8-bit + 16 对齐 + 3 comp + DRI=0）扩展为**全覆盖 ISO/IEC 10918-1** 的 IP。采用 28 个微阶段、每个 2–7 天、独立可验证、每阶段提交 + bit-exact 通过。

## 1. Waves & 微阶段清单

### Wave 1 — JFIF 完整（SOF0 保留，打破采样/尺寸/组件硬编码）

| Phase | Spec | 时长 | 关键 RTL 改动 | Bit-exact 验收 |
|---|---|---|---|---|
| **6** | [非 16 对齐尺寸](spec_phase06.md) | 1–2d | `header_parser.v` 去 align check；`block_sequencer` MCU-count 向上取整；`pixel_out` H-行裁剪 | 任意 W×H ≤ 4096 的 4:2:0 JPEG |
| **7** | [Restart markers (DRI>0)](spec_phase07.md) | 2–3d | `header_parser` 存 DRI；`bitstream_unpack` 识别并吞掉 RSTn；`dc_predictor` 间隔复位 | DRI=1/4/16 的 4:2:0 |
| **8** | [灰度 1-comp](spec_phase08.md) | 2d | `header_parser` 接受 Nf=1；`block_sequencer` 单组件 MCU；`pixel_out` Y-only 输出 | 灰度 JPEG |
| **9** | [4:4:4 (H=V=1 全部)](spec_phase09.md) | 2–3d | MCU shape H×V 参数化；跳过 chroma upsample | 4:4:4 JPEG |
| **10** | [4:2:2 (水平下采样)](spec_phase10.md) | 2–3d | 16×8 MCU 支持 + 水平 upsample | 4:2:2 JPEG |
| **11** | [任意 H×V (1–4×1–4)](spec_phase11.md) | 3–5d | `block_sequencer` 组件循环全参数化；通用 upsampler | 4:4:0 / 4:1:1 / 其他 |
| **12** | [2-comp / 4-comp (CMYK)](spec_phase12.md) | 3d | Nf∈{1,2,3,4}；输出格式寄存器 | CMYK baseline |

### Wave 2 — 精度扩展

| Phase | Spec | 时长 | 验收 |
|---|---|---|---|
| **13** | [SOF1 + 12-bit 精度](spec_phase13.md) | 5–7d | 全链路位宽 P∈{8,12} 参数化 |

### Wave 3 — Progressive (SOF2)

| Phase | Spec | 时长 | 验收 |
|---|---|---|---|
| **14** | [SOF2 header 解析](spec_phase14.md) | 2d | 识别但仍 error-out |
| **15** | [Coef frame buffer](spec_phase15.md) | 3–4d | 读/写/累加端口 loop-back |
| **16** | [DC-only scan (Ss=Se=0)](spec_phase16.md) | 4–5d | DC-only SOF2 |
| **17** | [AC spectral selection](spec_phase17.md) | 5–6d | 完整扫描序列 |
| **18** | [Successive approximation](spec_phase18.md) | 5–7d | refinement scans |
| **19** | [Drain → IDCT → 输出](spec_phase19.md) | 3–4d | 1080p progressive 端到端 |
| **20** | [AXI4-master + DDR for 4K](spec_phase20.md) | 5–7d | 4K SOF2 |

### Wave 4 — Arithmetic coding

| Phase | Spec | 时长 | 验收 |
|---|---|---|---|
| **21** | [Q-coder 核](spec_phase21.md) | 5–7d | Annex D 向量 |
| **22** | [DC arith decode](spec_phase22.md) | 4d | SOF9 DC-only |
| **23** | [AC arith decode](spec_phase23.md) | 5d | SOF9 完整 |
| **24** | [SOF10 (progressive + arith)](spec_phase24.md) | 3d | SOF10 |

### Wave 5 — Lossless

| Phase | Spec | 时长 | 验收 |
|---|---|---|---|
| **25** | [SOF3 预测器 (P1..P7)](spec_phase25.md) | 5d | 预测器单元 |
| **26** | [Lossless Huffman (1–16 sizes)](spec_phase26.md) | 2d | SOF3 8-bit |
| **27** | [Lossless 2–16 bit precision](spec_phase27.md) | 3d | SOF3 16-bit |
| **28** | [SOF11 (lossless + arith)](spec_phase28.md) | 3d | SOF11 |

### Wave 6 — Hierarchical

| Phase | Spec | 时长 | 验收 |
|---|---|---|---|
| **29** | [SOF5/6/7 (DCT hierarchical)](spec_phase29.md) | 10d | 3 种 SOF |
| **30** | [SOF13/14/15 (arith hierarchical)](spec_phase30.md) | 5d | bit-exact |

### Wave 7 — Sign-off

| Phase | 范围 | 时长 |
|---|---|---|
| **31** | 全 SOF × 10 k 随机语料库 | 5d |
| **32** | Yosys 综合重收敛 | 3d |
| **33** | OpenROAD P&R + sign-off STA | 10d |

---

## 2. 执行顺序

Wave 1 → 2 → 3 → 4 → 5 → 6 → 7，严格顺序。

Wave 4/5 理论上可与 Wave 3 并行（独立通路），但为简化回归只跑一个活动分支。

## 3. 面积/功耗/性能预期

| 里程碑 | 标准单元 (GE) | SRAM/DDR | 性能 @ 600 MHz |
|---|---|---|---|
| v1.0 baseline（当前） | 45 k | 38 KB SRAM | 13 fps @ 4K 4:2:0 |
| Wave 1 完成 | ~75 k | 38 KB | 13 fps @ 4K 全采样 |
| Wave 2 完成 | ~105 k | 38 KB | 9 fps @ 4K 12-bit |
| Wave 3 完成 | ~200 k | +24 MB DDR | 8 fps @ 4K SOF2 |
| Wave 4 完成 | ~280 k | +24 MB DDR | 5 fps @ 4K SOF10 |
| Wave 5 完成 | ~320 k | +24 MB DDR | — |
| Wave 6 完成 | ~400 k | +64 MB DDR | — |

预期整芯片 die ~0.4 mm² @ ASAP7（含 SRAM tile）。

## 4. 规格-测试矩阵

每个 Phase 自带一份 `verification/vectors/phaseXX/` 目录，包含 20–100 张针对性测试图 + 预期解码结果。
Wave-结束时跑跨 Phase 回归（smoke + full + 本 Wave 新 Phase 集）。

## 5. 报告节律

每 Wave 结束写一份 `docs/reports/0X_waveY_<name>.md`。已发布：
- [`00_spec_review.md`](reports/00_spec_review.md) — 规格冻结
- [`01_c_model_test.md`](reports/01_c_model_test.md) — C 模型
- [`02_rtl_design.md`](reports/02_rtl_design.md) — 18-module RTL
- [`03_rtl_simulation.md`](reports/03_rtl_simulation.md) — Verilator + 1 150 随机
- [`04_synthesis.md`](reports/04_synthesis.md) — ASAP7 WNS +339 ps

待发布：
- `05_wave1_jfif_full.md` — Wave 1 完成报告
- `06_wave2_12bit.md`、`07_wave3_progressive.md`、…、`10_wave7_signoff.md`
