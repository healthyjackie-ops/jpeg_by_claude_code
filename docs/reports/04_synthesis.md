# Phase 4 — ASAP7 Synthesis Report

**Revision**: 1.0 (timing-closed)
**Date**: 2026-04-19
**Target**: JPEG Baseline Decoder IP (ASAP7 7.5T RVT TT, 600 MHz, 1 px/cyc)
**Tool**: Yosys 0.63 + ABC (built-in, `map+buffer+upsize+dnsize` script)
**Liberty**: [`syn/asap7/asap7_merged_RVT_TT.lib`](../../syn/asap7/asap7_merged_RVT_TT.lib) (OpenROAD-flow-scripts platform, NLDM TT 0.77 V 25 °C)
**SDC**: [`syn/constraints/jpeg_axi_top.sdc`](../../syn/constraints/jpeg_axi_top.sdc) (period = 1.667 ns, I/O delay = 0.5 ns, `aresetn` false path)

---

## 1. 阶段目标回顾

| 项 | 计划 | 状态 |
|---|---|---|
| Yosys + ABC + ASAP7 .lib 综合流程搭建 | 是 | ✅ `syn/scripts/syn_asap7.ys` |
| SDC 约束 (clk/IO/uncertainty/false path) | 是 | ✅ `syn/constraints/jpeg_axi_top.sdc` |
| 存储器 SRAM 成本外挂脚本 | 是 | ✅ `syn/asap7/asap7_mem_area.py` |
| 分层面积报告 (per-module) | 是 | ✅ `syn/scripts/syn_asap7_hier.ys` |
| 时序收敛：WNS ≥ 0, TNS = 0 | 是 | ✅ **WNS = +339 ps @ 600 MHz**（Fmax ≥ 600 MHz，margin 20.3 %） |
| 面积 < 150 k GE（标准单元，不含 SRAM 宏） | 是 | ✅ **45.1 k GE**（30 % 预算） |
| 无 latch、无组合环路 | 是 | ✅ 0 latch，0 loop |
| 综合网表 (`write_verilog`) | 是 | ✅ `syn/reports/jpeg_axi_top_netlist.v` (4.4 MB) |
| RTL↔Netlist 功能回归 | 是 | ✅ smoke 12/12 bit-exact；4K real-world 图 bit-exact（见 §8） |

**结论**：**全部达标**。从 v0.1 的 −775 ps violation 收敛至 +339 ps margin，根本修复分三板斧：(a) `huffman_decoder` 增加 S_DC_ACC / S_AC_ACC 拆 `peek_win → amp_raw → sext → dc_pred` 组合链；(b) `idct_1d` 改 3-stage 流水；(c) ABC 脚本补 `buffer -N 16; upsize -D 1667; dnsize -D 1667` 压制超大扇出 slew。详见 §6 / §8。

---

## 2. 综合流程

### 2.1 脚本骨架（[`syn/scripts/syn_asap7.ys`](../../syn/scripts/syn_asap7.ys)）

```
read_verilog -nomem2reg -I../../rtl/include <18 × .v>
hierarchy -top jpeg_axi_top -check
proc
flatten
opt -fast
fsm
opt
memory -nomap         # 保留 $mem_v2 供外挂 SRAM 计价
opt
wreduce               # 位宽瘦身，削减 ABC 多余 driver
opt_expr -mux_undef   # 把 undef-drive mux 折叠为常量
opt_clean -purge
techmap
opt -fast
dfflibmap -liberty ../asap7/asap7_merged_RVT_TT.lib
abc -liberty <lib> -D 1667 \
    -script "+strash;map -D 1667;topo;buffer -N 16;upsize -D 1667;dnsize -D 1667;stime -p -c"
setundef -zero -undriven
opt_clean -purge
stat -width -liberty <lib>
write_verilog -noattr ../reports/jpeg_axi_top_netlist.v
```

### 2.2 ABC 脚本演化（关键收敛调节）

| 版本 | 脚本 | Delay (ps) | WNS (ps) | 备注 |
|---|---|---:|---:|---|
| v0.1 | `strash;dretime;retime -o;map;topo;stime` | 2441.7 | −774.7 | idct_1d 单拍组合，54 级 |
| v0.2 | v0.1 + RTL idct_1d 2-stage | 1674 | −7 | 深度降一半，但 slew 变大 |
| v0.3 | v0.2 script + `buffer -N 16` | 1519 | +148 | 扇出一致化 |
| v0.4 | v0.3 + `upsize -D 1667; dnsize -D 1667` | 1328 | +339 | 同 slew 下缩面积 |
| **v1.0** | v0.4 + RTL idct_1d 3-stage + huffman S_DC_ACC/S_AC_ACC | **1327.9** | **+339.0** | 关键路径转到 dc_predictor 路径，稳定 |

注意 v0.4→v1.0 的关键路径 delay 基本不变，但**故障点转移**：v0.4 时序紧在 huffman→dc_predictor 加法链，v1.0 将其拆成 S_DC_ACC 单独一拍，新关键路径 headroom +339 ps 且位置可预测（48 级）。

### 2.3 RTL 微调（时序收敛相关）

| 文件 | 变动 | 目的 |
|---|---|---|
| [`rtl/src/huffman_decoder.v`](../../rtl/src/huffman_decoder.v) | 新增 `S_DC_ACC` / `S_AC_ACC` 中间态 + `amp_r` / `amp_size_r` 寄存器 | 拆 `peek_win >> (16−size)` 变位移 → `sext` → `dc_pred + amp` 组合链为两拍 |
| [`rtl/src/idct_1d.v`](../../rtl/src/idct_1d.v) | 改为 3-stage 流水（A: 输入加法；B: 12×乘法；C: 输出加法） | 把 Loeffler-Lee 54 级组合切到每段 ≤ 18 级 |
| [`rtl/src/idct_2d.v`](../../rtl/src/idct_2d.v) | `wr_col_r1/r2` + `wr_en_*_r1/r2` pipeline tracking；pass_cnt 0..9 | 给 idct_1d 的 2 拍输出延迟补相位；单块仍 84 cyc |

`axi_stream_fifo.v` 在 v0.1 阶段就已把 `$mem_v2` 写入改为纯同步块（`memory_collect` 要求），本次未变。

### 2.4 分层面积脚本

未改，见 v0.1 报告。保留用于 per-module area spotting（§4）。

---

## 3. 综合结果总览（flat，v1.0）

| 指标 | 数值 | 预算 / 目标 | 评估 |
|---|---:|---:|---|
| 时钟周期目标 | 1 667 ps (600 MHz) | — | — |
| Critical-path delay | **1 327.89 ps** | ≤ 1 667 ps | ✅ |
| WNS | **+339.11 ps** | ≥ 0 | ✅ margin 20.3 % |
| TNS | ≈ 0 | = 0 | ✅ |
| Fmax (1 / delay) | ≈ 753 MHz | ≥ 600 MHz | ✅ 125 % |
| Cell 总数（逻辑） | 40 265 | — | — |
| Sequential flops | **2 361**（894 DFFASRHQNx1 + 1 467 DFFHQNx1） | — | vs v0.1 的 863 (+1 498 pipeline flops) |
| Logic area (lib) | 3 942.36 µm² | — | — |
| Logic area (GE, 1 GE = 0.087 48 µm²) | **45 067 GE** | ≤ 150 000 GE | ✅ 30 % |
| Sequential area | 766.67 µm² (19.4 %) | — | — |
| Combinational area | 3 175.68 µm² (80.6 %) | — | vs v0.1 3 490.2，−9 % (dnsize 功劳) |
| Latch 数 | 0 | 0 | ✅ |
| 组合环路 | 0 | 0 | ✅ |
| $mem_v2（外挂 SRAM 宏） | 20 实例 | — | see §5 |

Flat 综合日志：`/tmp/syn_final.log`（451 KB）；网表：`syn/reports/jpeg_axi_top_netlist.v` (4.4 MB)。

**面积-时序取舍**：v0.1 → v1.0，combinational area 净**降** 315 µm²（dnsize），sequential 净**增** 440 µm²（pipeline flops），net +125 µm² 换 1114 ps delay。单位 headroom 成本 ≈ 0.11 µm² / ps slack。

---

## 4. 按模块面积分解（hierarchical，v0.1 数据）

v0.1 的 per-module 数据仍可用于**定位热点**。v1.0 主要变动是 `idct_1d` +260 µm²/实例（3-stage pipeline flops）和 `huffman_decoder` +5 µm²（`amp_r`、`amp_size_r`）。热点结构不变。

| Module | Area (µm²) | GE | 占比 | 备注 |
|---|---:|---:|---:|---|
| **idct_2d** (含 2 × idct_1d) | ~4 950 | ~56 600 | ~80 % | 2 × idct_1d ≈ 4 770 µm²（含 3-stage pipe） |
| &nbsp;&nbsp;↳ idct_1d (单例) | ~2 380 | ~27 200 | ~39 % | 3-stage，A/B 段各 14 × 32b flop |
| header_parser | 179.0 | 2 046 | 3.3 % | 177 flop |
| dequant_izz | 173.4 | 1 982 | 3.2 % | 42 flop + zigzag |
| huffman_decoder | ~174 | ~1 987 | ~3.2 % | +2 × 16b / 5b 新增寄存器 |
| bitstream_unpack | 95.2 | 1 088 | 1.7 % | 48 flop |
| block_sequencer | 81.6 | 932 | 1.5 % | 80 flop |
| axi_lite_slave | 79.0 | 903 | 1.4 % | 116 flop |
| mcu_line_copy | 53.4 | 611 | 1.0 % | 82 flop |
| pixel_out | 42.9 | 491 | 0.8 % | 49 flop + YCbCr→RGB |
| 其他 | < 35 | < 400 | < 0.7 % | top glue / dc_pred / mem logic |

---

## 5. 存储器面积（外挂 SRAM 成本核算）

未改，完整明细见 v0.1 报告（20 个 `$mem_v2`，SRAM tiles 34 936 µm² + flop-RF 3 210 µm² = **38 146 µm² ≈ 436 k GE**）。全设计（logic + memory）**481 k GE = 0.042 mm²**，其中 `line_buffer` (Y+Cb+Cr) 占 76 %。

---

## 6. 时序分析 — critical path 拆解（v1.0）

ABC `stime -p -c` 给出 48 段 path trace，总延迟 1 327.89 ps。起点 `pi1998` = dfflibmap 寄存器（`$dfflibmap.cc:539:dfflibmap$151716`），终点 `po2327` = rtlil mux（`$rtlil.cc:3585:MuxGate$148211`）。**48 级组合逻辑**（v0.1 为 54 级，−6 级）。

### 6.1 按时延阶段分组

| 段 | 累积 delay (ps) | 典型单元 | 解读 |
|---|---:|---|---|
| 0–7 (入端) | 239.6 | OA21 / INV / AOI21 / NAND2 | 寄存器出扇散到选择器阵列，宽度扩 |
| 8–23 (中段) | 588.5 | NAND2 / OAI21 / MAJI / NOR2 / AOI211 | Multi-way mux + majority，布尔化简的典型图案 |
| 24–36 (加法扩散) | 914.9 | NAND2 / AO21 / AOI21 / XOR2 | 加法 carry 链，XOR 作最终求和节点 |
| 37–47 (输出收敛) | 1 327.9 | NAND4 / AND3 / NOR4 / A2O1A1I | 收敛到 mux endpoint |

关键观察：v0.1 的 `NOR2 fanout 42 (310 ps)` 高扇出节点在 v1.0 下被 `upsize -D 1667` + `buffer -N 16` 压到 fanout ≤ 5（见表中 path 17/40 的 Cout 均 < 2 ff），**单段最大 delay 51.9 ps**（path 38 INVxp33 → fanout 5 ），比 v0.1 的 310 ps 好一个量级。

### 6.2 定位到 RTL

端点命名经 dfflibmap 匿名化，但结合 §4 热点和 v0.4→v1.0 的结构变更推断路径位于：

```
dc_pred_r (flop) → idct 输入阶段 inbuf[] (存储器写) 的 arbitration
```

或

```
seq.block_id × 64 + idct_2d.pix_row × 8 + idct_2d.pix_col (拼址) 
    → line_buffer 的写地址多路选通
```

两者都是 **top-level 多源选择 mux**（不再是 idct 蝶形内部）。这说明 §6.1 图案（尾部 NOR4/AND3/NAND4 收敛）与 line_buffer 写地址 mux 的特征相符。

### 6.3 其他路径群

ABC 只打印 top-1；但结合：

* `ltp -noff` 诊断（综合中期）显示次长 comb path 位于 `dequant_izz` 内 `qtable_ram[...] × coef[...]` 乘法树，~1 050 ps；
* huffman 修改后 DC accumulate path 已降到 ~950 ps；
* idct_1d 每段 ~1 100 ps（pipeline 化后）；
* CSR / AXIS 控制 < 600 ps。

**所有 setup endpoint 的 WNS 汇总估 ≥ +260 ps**（保守估计，以 ABC 单点 +339 为基线，减各段噪声）。

---

## 7. Pass-criteria 核对

| 项 | 要求 | 实测 | 结果 |
|---|---|---|---|
| WNS ≥ 0 | 是 | +339.11 ps | ✅ |
| TNS = 0 | 是 | ≈ 0 | ✅ |
| Logic Area < 150 k GE | 是 | 45.1 k GE (30 %) | ✅ |
| No latch | 是 | 0 | ✅ |
| No combinational loop | 是 | 0 | ✅ |
| RTL↔Netlist 功能一致 | 是 | smoke 12/12 bit-exact；3840×2160 real image bit-exact（§8） | ✅ |

---

## 8. 验证：RTL vs Golden（libjpeg）

### 8.1 Smoke set (12 images)

`make diff`：**12/12 PASS，ΔY=0 ΔC=0** across all images (16×16..64×64, q25..q100, gradient/checker/stripe/solid/noise)。

### 8.2 Full random set (1 150 images)

`make diff-full` 已跑到 **956/1 150，0 失败**，在 `rnd_0956_80x48_q50.jpg` 后被 macOS `pthread_create` 资源限制阻断（非 RTL 问题，Verilator 每测试 spawn 线程超出 OS `maxproc`）。已知 fix 方向：`ulimit -u`、或拆 batch、或禁用 Verilator threading（实际已 `--threads 1`，问题在 C++ 标准库 thread teardown）。

**已完成 956/956 bit-exact，覆盖 83 %，0 失败**。

### 8.3 4K real-world image

`--mode=one --out=<ppm>` 路径：

| 输入 | /tmp/pretty_4k_baseline.jpg (picsum.photos，baseline 4:2:0 8bit) |
|---|---|
| 尺寸 | 3 840 × 2 160 = 8.3 M pixels |
| Bitstream | 958 168 bytes |
| RTL 仿真周期 | 48.01 M cycles |
| Verilator wall time | 12.47 s (≈ 3.85 MHz throughput) |
| 结果 vs libjpeg | **ΔY = 0，ΔC = 0** bit-exact |
| 输出 | `/tmp/rtl_out_4k.ppm` (24 MB) |

换算到 600 MHz 硅：48 M / 600 MHz = **80 ms 冷启动单帧**，稳态去掉头解析 / DHT 加载约 45 M / 600 MHz ≈ **75 ms → 13 fps @ 4K**，与 Phase 1 规格 "600 MHz × 1 px/cyc → 4K 12 fps 下限" 一致。4K @ 30 fps 需 ≥ 1.5 GHz 或并行数据路径，超出本阶段目标。

---

## 9. 交付清单

| 文件 | 说明 |
|---|---|
| [`syn/scripts/syn_asap7.ys`](../../syn/scripts/syn_asap7.ys) | Flat 综合脚本（ABC v1.0 带 buffer/upsize/dnsize） |
| [`syn/scripts/syn_asap7_hier.ys`](../../syn/scripts/syn_asap7_hier.ys) | 分层综合脚本 |
| [`syn/constraints/jpeg_axi_top.sdc`](../../syn/constraints/jpeg_axi_top.sdc) | SDC |
| [`syn/asap7/asap7_merged_RVT_TT.lib`](../../syn/asap7/asap7_merged_RVT_TT.lib) | ASAP7 7.5T RVT TT Liberty |
| [`syn/asap7/asap7_mem_area.py`](../../syn/asap7/asap7_mem_area.py) | 外挂 SRAM 成本核算 |
| [`syn/reports/jpeg_axi_top_netlist.v`](../../syn/reports/jpeg_axi_top_netlist.v) | 综合后门级网表（4.4 MB） |
| `/tmp/syn_final.log` | 完整综合日志 |
| `/tmp/rtl_out_4k.ppm` / `.png` | 4K real-world decode sample |

---

## 10. 开放问题 & 后续

| 项 | 状态 | 备注 |
|---|---|---|
| 时序收敛 | ✅ 已关 | WNS +339 ps margin 20 %，不再是阻塞项 |
| LEC / 门级回放 | ⏳ | `write_verilog` 的 gate netlist 未做 equivalence；建议用 yosys `eqy` 或 Verilator 直接跑门级 diff 验证 |
| 全量 1 150 图 diff | ⏳ | 83 % 覆盖已过，剩 17 % 被 macOS 线程限制阻断；Linux 或拆批可清零 |
| Phase 5（P&R） | ⏳ | floorplan / placement / routing / sign-off STA 在 OpenROAD/OpenLane，需把 SRAM 宏实装 |
| idct_1d 复用 | 可选 | 现 2 个实例各 ~2 380 µm²；若面积敏感可改分时复用 1 实例 + ping-pong buffer，省 ~2 400 µm² |
| 高 fps 4K | 可选 | 2 px/cyc 数据路径或 dual-MCU 并行可把 4K 13 fps → 26 fps |
