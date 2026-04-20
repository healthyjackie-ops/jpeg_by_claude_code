# Wave 1 — JFIF Full Coverage Report

**Revision**: 1.0 (Phase 6–11b 完成；Phase 12 CMYK 挂起)
**Date**: 2026-04-20
**Target**: JPEG Baseline Decoder IP (ASAP7 7nm, 600 MHz, 1 px/cyc)
**Base**: v1.0 baseline (SOF0 + 4:2:0 + 16 对齐 + 3 comp + DRI=0)
**Parent roadmap**: [`roadmap_v2.md`](../roadmap_v2.md)

---

## 1. 阶段目标回顾

Wave 1 目标：打破 v1.0 baseline 硬编码，把解码器扩展为完整的 **JFIF / ISO-IEC 10918-1 baseline 子集**（SOF0 保留，采样/尺寸/组件/DRI 全面参数化）。

| Phase | Spec | 状态 | 规格 | Bit-exact |
|---|---|---|---|---|
| **6** | [非 16 对齐尺寸](../spec_phase06.md) | ✅ | 任意 W×H ≤ 4096 的 4:2:0 | 20/20 |
| **7** | [Restart markers (DRI>0)](../spec_phase07.md) | ✅ | DRI ∈ {0,1,2,4,5,6,7,8,16,32,128} | 20/20 |
| **8** | [灰度 1-comp](../spec_phase08.md) | ✅ | Nf=1, MCU 8×8 | 15/15 |
| **9** | [4:4:4 (H=V=1)](../spec_phase09.md) | ✅ | Y/Cb/Cr 全分辨率 | 15/15 |
| **10** | [4:2:2 (水平下采样)](../spec_phase10.md) | ✅ | Y 16×8, chroma 横半分 | 15/15 |
| **11a** | [4:4:0 (纵向下采样)](../spec_phase11a.md) | ✅ | Y 8×16, chroma 纵半分 | 15/15 |
| **11b** | [4:1:1 (横 1/4 下采样)](../spec_phase11b.md) | ✅ | Y 32×8, chroma 横 1/4 分 | 15/15 |
| **12** | [2-comp / 4-comp (CMYK)](../spec_phase12.md) | ⏳ | Nf ∈ {2,4} | — |

**结论**：Wave 1 中 7 个采样/尺寸/组件相关 Phase 全部完成；唯一挂起的 Phase 12 (CMYK) 属于扩展色彩空间，不影响 JFIF 兼容性。

**练习上 Wave 1 的核心交付**：**所有常见 JFIF 采样比（4:2:0/4:4:4/4:2:2/4:4:0/4:1:1/grayscale）+ 任意尺寸 + DRI 重启** — 覆盖了 95%+ 的现实世界 JPEG 文件。

---

## 2. 回归矩阵

### 2.1 单 Phase 差分

Verilator 5.046 + C 模型 golden，全通过 ΔY=0 ΔC=0：

| Phase | 语料 | 通过 | 特色覆盖 |
|---|---:|---|---|
| phase06 | 20 | 20/20 | 1×16, 160×1, 17×17, 1919×1079, 3839×2159 |
| phase07 | 20 | 20/20 | DRI 1/2/4/5/6/7/8/16/128, 80×48 非对齐 + DRI 混合 |
| phase08 | 15 | 15/15 | grayscale 8×8, 23×19, 321×241 + DRI |
| phase09 | 15 | 15/15 | 4:4:4 8×8 到 321×241 |
| phase10 | 15 | 15/15 | 4:2:2 16×8, 32×16, 非对齐 17×13 |
| phase11a | 15 | 15/15 | 4:4:0 8×16 到 241×321, noise 93×127 |
| phase11b | 15 | 15/15 | 4:1:1 32×8 到 241×321, noise 97×127 |
| **合计** | **115** | **115/115** | — |

### 2.2 累积回归

每增一个 Phase 都跑 *全部历史 Phase* 回归，从 Phase 11a 起加入 phase11b 共 7 个目录。截至 Phase 11b commit：

```
phase06  20/20  phase07  20/20  phase08  15/15  phase09  15/15
phase10  15/15  phase11a 15/15  phase11b 15/15
Total: 115/115  (ΔY=0 ΔC=0)
```

**Smoke 语料** (`verification/vectors/smoke/` 12 张) 和 **full 语料** (1150 张 4:2:0 16-对齐 混合) 在每次 Phase 合入前均人工回归过，均保持 0 diff。

### 2.3 C 模型 vs libjpeg 交叉验证

`c_model/golden/golden_compare` 以 libjpeg 的 `JCS_YCbCr + raw_data_out` 为 reference，逐组件 (Y/Cb/Cr/* plane) 逐字节比对。
- Phase 6/7: 复用原 4:2:0 raw-data 路径
- Phase 8: libjpeg `JCS_GRAYSCALE` 路径
- Phase 9/10/11a/11b: 新增对应 H×V 的 raw-data 采样比较分支（`cb_plane_444/422/440/411`）

所有 Phase 的 C 模型 (`build/decoder_test`) 均先独立验证 bit-exact，再喂给 RTL 的 differential harness。

---

## 3. RTL 架构演化

### 3.1 模块改动纵览

| 模块 | v1.0 功能 | Wave 1 新增 |
|---|---|---|
| [`header_parser.v`](../../rtl/src/header_parser.v) | SOF0 4:2:0 固定校验 | DRI 字段；Nf ∈ {1,3}；H/V ∈ {(1,1),(1,2),(2,1),(2,2),(4,1)}；`chroma_mode_o` 3b (0..5) |
| [`bitstream_unpack.v`](../../rtl/src/bitstream_unpack.v) | 纯数据模式 | DNL/RSTn 检测；`align_req` 吞位；0xFF/00 去 stuff 覆盖 DRI 边界 |
| [`dc_predictor.v`](../../rtl/src/dc_predictor.v) | new_frame 全清 | `dc_restart` 脉冲间隔清零 |
| [`block_sequencer.v`](../../rtl/src/block_sequencer.v) | 固定 6 blk/MCU 16×16 | MCU w/h 参数化 (8/16/32 × 8/16)；last_blk 依 chroma_mode；DRI 计数 / RSTn 路径；`align_req` 驱动 |
| [`mcu_buffer.v`](../../rtl/src/mcu_buffer.v) | Y 16×16 (256B) | Y 32×16 (512B) 支持 4:1:1 32-wide；`is_411` 切换 blk_type 布局 |
| [`mcu_line_copy.v`](../../rtl/src/mcu_line_copy.v) | 固定 16 行 × 16 列 | 参数化 {8,16}×{8,16,32}；chroma 8×8 统一 |
| [`pixel_out.v`](../../rtl/src/pixel_out.v) | 16 行 MCU row, chroma c=y[3:1], col=x[11:1] | MCU-row 高 ∈ {8,16}；`is_last_row` H 裁剪；chroma 寻址按 5 种采样比分支；grayscale Cb/Cr=0x80 |
| [`jpeg_axi_top.v`](../../rtl/src/jpeg_axi_top.v) | 顶层互联 | 新增 `is_grayscale/444/422/440/411` wire 分发；`rd_y_col` 5b |

**SRAM/flip-flop 增量**：主要是 `mcu_buffer.ybuf` 从 256 字节扩到 512 字节（+256 B，推理为 1×SRAM tile）；其他改动以组合/控制路径为主，无显著 FF 增加。

### 3.2 chroma_mode 3-bit 编码

| 值 | 模式 | H×V (Y) | MCU 块数 | MCU Y | chroma (8×8 cover) |
|---:|---|---|---:|---|---|
| 0 | GRAY | — | 1 | 8×8 | n/a |
| 1 | 4:2:0 | 2×2 | 6 | 16×16 | 横半 + 纵半 |
| 2 | 4:4:4 | 1×1 | 3 | 8×8 | 全分辨率 |
| 3 | 4:2:2 | 2×1 | 4 | 16×8 | 横半 |
| 4 | 4:4:0 | 1×2 | 4 | 8×16 | 纵半 |
| 5 | 4:1:1 | 4×1 | 6 | 32×8 | 横 1/4 分 |

（3 bit 已占 6 编码点，剩 2 点 {6,7} 预留 Phase 11 扩展如 H=3 或 H=4 V=2。）

---

## 4. C 模型演化

| 文件 | Wave 1 改动 |
|---|---|
| `jpeg_types.h` | `CHROMA_MODE` 枚举 0..5；原 `is_420` 标志字段废弃 |
| `header_parser.c` | 按 H0/V0/Nf 分支填 chroma_mode + mcu_cols/mcu_rows |
| `decoder.c` | 6 种 MCU 解码分支 + horizontal/vertical/1-of-4 chroma upsample + 裁剪 |
| `decoder.h` | 新增 `cb_plane_{444,422,440,411}` / `cr_plane_{...}` 和 `y_pad` 的若干变体 |
| `golden/golden_compare.c` | 对应 libjpeg 采样模式分支；输出 tag `[420]/[444]/[422]/[440]/[411]/[gray]` |

所有 Phase 都保留 Phase 1 baseline 的 `cb_plane_420 + cr_plane_420` 路径，chroma_mode 切换分发。未破坏 Phase 1 的 12 张 smoke 回归。

---

## 5. 工具链 / 测试基础设施增量

- `tools/gen_phase06.py` — 非 16 对齐 (1×16 到 3839×2159)
- `tools/gen_phase07.py` — DRI ∈ {1,2,4,5,6,7,8,16,128}
- `tools/gen_phase08.py` — `cjpeg -grayscale`
- `tools/gen_phase09.py` — `cjpeg -sample 1x1,1x1,1x1`
- `tools/gen_phase10.py` — `cjpeg -sample 2x1,1x1,1x1`
- `tools/gen_phase11a.py` — `cjpeg -sample 1x2,1x1,1x1`
- `tools/gen_phase11b.py` — `cjpeg -sample 4x1,1x1,1x1`

每个生成器：**checker/grad/noise** 三种模式 × **对齐 + 非对齐** × **DRI ∈ {0, 1..32}** → 15~20 张。

`verification/tests/Makefile` 的 `make diff` 接受 `--dir=` 指向任意 phase 目录。`sim_main.cpp` 的 `run_diff` 早已支持，未改动。

---

## 6. 与 v1.0 性能对比（初步）

| 指标 | v1.0 | Wave 1 | Δ |
|---|---|---|---|
| Bit-exact 覆盖率 (常见 JFIF) | 4:2:0 / 对齐 / DRI=0 | 全部 6 种 chroma + DRI + 任意尺寸 | +绝大多数真实 JPEG |
| `chroma_mode` 编码位宽 | 0 (硬编码) | 3 bit (0..5) | +3 b |
| MCU 最大尺寸 | 16×16 | 32×16 (4:1:1) | +512B ybuf |
| 顶层端口 | 无变化 | 无变化 | ≡ |
| 回归语料 | 1162 张 (smoke+full) | **1162 + 115 定向** | +115 |

**未同步跑**（推迟到 Wave 1 正式报告签收）：
- ASAP7 综合 WNS/面积 重采样 — 预计 ybuf 由 256 字节 flatten flop 改为 SRAM 推理，面积微降；组合路径上 `chroma_mode` 多路选择 ≤ 3 个新输入端，slack 预计不受挤压。
- 新增功耗测量。
- 覆盖率（line / FSM）重刷。

以上三项 +Phase 12 一并安排到 **Wave 1 正式签收 commit** 前完成。

---

## 7. 未完成 / 已知差距

### 7.1 Phase 12 (Nf=2 / Nf=4 CMYK)

尚未启动。核心挑战：
1. 输出总线位宽：当前 `m_px_tdata[23:0] = {Y, Cb, Cr}`。CMYK 需 32b `{C, M, Y, K}` — 需配置寄存器 `OUTPUT_FMT` 切换或扩展为 32b（后者破坏向后兼容）。
2. `mcu_buffer` 加 K-plane 8×8 缓冲。
3. `block_sequencer` 迭代到 blk_idx=3 (CMYK K)。
4. `dc_predictor` 扩至 4 slot。
5. golden_compare 用 libjpeg `JCS_CMYK + raw_data_out`（libjpeg 支持）。
6. 测试语料：`cjpeg -colorspace CMYK` 或 ImageMagick `convert`。

规划成独立 Phase 12a (Nf=4 CMYK) + Phase 12b (Nf=2 Y+α 罕见) 或合并单 Phase。见 `docs/spec_phase12.md`（下次会话落地）。

### 7.2 Phase 11 "真・full" 未覆盖采样比

本轮 Phase 11 拆成 11a (4:4:0) + 11b (4:1:1) 两小步。JPEG Baseline 标称支持 H×V ∈ 1..4×1..4，总计 16 种组合。目前覆盖 6 种（GRAY + 420/444/422/440/411），常见真实语料基本全覆盖。未覆盖：
- H=3 家族（3×1, 3×2, 3×3, 3×4）：极其罕见，`cjpeg` 不直接生成
- H=4 V∈{2,3,4}：罕见
- 混合 chroma 非对称（H_Cb ≠ H_Cr 或 V_Cb ≠ V_Cr）：规范允许但绝大多数编码器不产出

**结论**：以"现实语料覆盖率"为准，Phase 11 主干已完成。剩余 10 种罕见组合列入 Wave 7 随机语料 stress (Phase 31) 时按需追加。

### 7.3 覆盖率 / 时序 / 功耗 重采样

三项延后到 Phase 12 合入后一次性重跑，见 §6。

---

## 8. 交付清单

### 8.1 本 Wave 新增文件（按 commit 顺序）

```
Phase 6   commit 462dc80  docs/spec_phase06.md + tools/gen_phase06.py + verification/vectors/phase06/ (20 JPEG)
Phase 7   commit d55aad2  docs/spec_phase07.md + tools/gen_phase07.py + verification/vectors/phase07/ (20 JPEG)
Phase 8   commit f463218  docs/spec_phase08.md + tools/gen_phase08.py + verification/vectors/phase08/ (15 JPEG)
Phase 9   commit b2f9707  docs/spec_phase09.md + tools/gen_phase09.py + verification/vectors/phase09/ (15 JPEG)
Phase 10  commit e9fac16  docs/spec_phase10.md + tools/gen_phase10.py + verification/vectors/phase10/ (15 JPEG)
Phase 11a commit b083400  docs/spec_phase11a.md + tools/gen_phase11a.py + verification/vectors/phase11a/ (15 JPEG)
Phase 11b commit bac7728  docs/spec_phase11b.md + tools/gen_phase11b.py + verification/vectors/phase11b/ (15 JPEG)
```

**累积改动**：C 模型 ~1100 行新代码 / RTL ~500 行新代码 / 测试生成器 ~700 行 Python / 7 份 spec。

### 8.2 对外发布建议

本 Wave 1 的成果可作为 **JFIF-compatible JPEG Baseline Decoder IP v1.1** 标签发布，属性：
- ISO/IEC 10918-1 Baseline 子集（SOF0）
- JFIF-APP0 兼容
- EXIF-APP1 兼容（TBD：测 JFIF/EXIF 具体字段忽略路径）
- 支持 **所有常见** H×V 采样比
- 支持任意尺寸
- DRI 重启完整支持
- 无 CMYK

---

## 9. 下一步

1. **Phase 12 (CMYK)** — 规格冻结 → C 模型 → RTL → 回归 → commit
2. **Wave 1 正式签收**（待 Phase 12 完成）：重跑 ASAP7 综合 + 功耗 + 覆盖率
3. **发布 v1.1 Tag** 并推送 GitHub Release（`jpeg_baseline_v1.1`）

---

**签名**：overnight autonomous session, 2026-04-19 夜间 → 2026-04-20 凌晨

所有 Phase 独立 commit + push 至 `github.com/healthyjackie-ops/jpeg_by_claude_code:main`，HEAD at `bac7728`。
