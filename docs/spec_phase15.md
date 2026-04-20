# Phase 15 — DCT Coefficient Frame Buffer (loop-back)

## Goal
为 progressive (SOF2) 解码铺一块**每像素/每 block 去 hold DCT 系数**的存储：
多 scan 模式下，系数需要在各 scan 之间累积，在最后一个 scan 后再去做 IDCT。

本 phase **不实现** progressive 解码逻辑，只交付：
- 一个独立的 `coef_buffer` 模块（block 粒度 read / write / accumulate 三端口）
- Unit-level loop-back 测试：随机写若干 block、读回比对、RMW 系数累加后再读回比对
- 不改 `jpeg_axi_top` 顶层；主回归 phase06..14 零影响

## 设计尺度

### 一块 block 的容量
8×8 int16 DCT coefficient block = 64 × 16b = **1024 bit = 128 B**。

### 地址空间
- 4:2:0 1080p ≈ 8100 Y-block + 2×2025 C-block = 12 150 blocks
- 4:2:0 4K     ≈ 32 400 Y + 2×8100 C       = 48 600 blocks
- 4:4:4 4K     ≈ 32 400 × 3               = 97 200 blocks

Phase 15 的 loop-back 测试**只需 AW=10 (1024 blocks = 128 KB)** 的内部 SRAM，
等价 tile 规模。4K/progressive 时改为 DDR-backed — 由 Phase 20 兑现。

### 接口

```verilog
module coef_buffer #(
    parameter AW = 10               // block addr width (1 << AW blocks)
)(
    input  wire             clk,
    input  wire             rst_n,

    // Write port (整 block 写入)
    input  wire             w_en,
    input  wire [AW-1:0]    w_addr,
    input  wire [1023:0]    w_data,     // 64 × int16，little-endian: [15:0]=coef[0], [31:16]=coef[1], ...

    // Read port (整 block 读出; latency-1 valid)
    input  wire             r_en,
    input  wire [AW-1:0]    r_addr,
    output reg  [1023:0]    r_data,
    output reg              r_valid,    // 读命令后 1 cycle 脉冲

    // Accumulate port (单系数 RMW; 用于 successive-approximation refinement)
    input  wire             acc_en,
    input  wire [AW-1:0]    acc_addr,
    input  wire [5:0]       acc_coef_idx,    // 0..63
    input  wire signed [15:0] acc_delta,
    output reg              acc_done         // 写回完成后 1 cycle 脉冲
);
```

### 端口语义
| 端口 | 用途 (progressive 映射) | 频率 |
|---|---|---|
| `w_en` | DC-only scan 初始化整 block (Phase 16) | ~12 k blocks / frame @ 1080p |
| `r_en` | 最终 IDCT drain (Phase 19) | 全 block 扫一遍 |
| `acc_en` | AC spectral selection (Phase 17) / refinement scans (Phase 18) 更新单系数 | 多次 |

三者在 Phase 15 loop-back 测试中**互相独立**：不支持同周期写-读同一地址
（合成时转为单端口 SRAM；当前行为模型用寄存器阵列，没有端口竞争，但测试
只按"写先于读"顺序发命令，避免日后真 SRAM 时出现 data hazard）。

### 内部实现
Phase 15 阶段采用**行为级 reg 阵列**（Verilator 友好）：
```verilog
reg [1023:0] mem [0:(1<<AW)-1];
```
每 cycle 接受至多：
- 1 个 write (w_en)
- 1 个 read  (r_en)
- 1 个 accumulate (acc_en，内部 1-cycle RMW：读当前值、改 coef_idx 的 16b、
  写回；综合时需转为 2-cycle 或加 read-modify-write FSM)

优先级 (同 cycle 多 en 时 — 避免 undefined)：**write > accumulate > read**。
测试 bench 里严格错开，不会触发冲突。

## 验证 — `verification/tests/unit_coef_buffer.cpp`

独立 Verilator 驱动（不依赖 jpeg_axi_top）。

### Test 1 — WRITE+READ loopback
- 生成 512 个随机 1024b block
- 按地址顺序 `w_en=1, w_addr=i, w_data=data[i]` 连续写入
- 再按相同顺序 `r_en=1, r_addr=i`，读出 `r_data` 与 `data[i]` 逐 bit 比对
- **验收**: 512/512 bit-exact

### Test 2 — Non-sequential read
- 用 Test 1 的数据，按 reverse / random 序读回再比
- 覆盖 SRAM row decoder 不依赖连续性

### Test 3 — ACCUMULATE (RMW)
- 先写 512 个全零 block
- 对每个 block 循环：
  - 随机选择 `coef_idx ∈ [0, 63]`
  - 随机生成 delta ∈ [-32768, 32767]（int16）
  - `acc_en=1`；等 `acc_done` 脉冲
- 再全量读回；断言 block[i][coef_idx*16 +: 16] == Σ deltas (i, coef_idx)
- **验收**: 每个 (block, coef_idx) 槽位累加结果精确匹配

### Test 4 — OVERWRITE
- Test 3 完成后，对每个 block 发 `w_en=1` 写新整 block 值
- 再读回；断言新值覆盖旧 accumulate 结果
- **验收**: bit-exact

### 总合规模 ≥ 2000 ops, target runtime < 2 s @ Verilator opt

## 回归
- phase06..phase14 完全不变（coef_buffer 独立 top，未接入 jpeg_axi_top）
- smoke 12 + phase 170/170（phase13 20 + phase14 8 + 历史 142 + smoke 12 = 182/182
  — 注：此次统计把 phase14 归入总计，与 roadmap "Wave 3 启动后总计" 一致）

## 交付物
1. `docs/spec_phase15.md`（本文件）
2. `rtl/src/coef_buffer.v`
3. `verification/tests/unit_coef_buffer.cpp`
4. `verification/tests/Makefile` +`coef_unit` target
5. `make coef_unit` 全通过
6. commit + push

## 风险
1. **SRAM 行为 vs 真 SRAM**：Phase 15 用 reg 阵列，合成时 `mem[]` 会被
   Yosys/OpenROAD 识别为 register bank —— 面积很大。Wave 3 最终版需要替换为
   `SRAM_1P_128kB` 或 DDR。Phase 20 再做。
2. **单周期 RMW**：同样，综合时 `acc_en` 需拆为 2-cycle FSM (read phase →
   write phase)。Phase 15 的测试用纯行为模型，留 TODO 给 Phase 18/20。
3. **数据排布 endian**：本 phase 固定 little-endian（coef[k] 存 `[k*16 +: 16]`）。
   后续 dequant_izz 输出走 natural order，与本 buffer 默认 convention 对齐。

---
**上游依赖**: Phase 14 完成 ✅
**下游开启**: Phase 16 (DC-only scan) 首次把 coef_buffer.w_en 拉高，在 SOS 结束
后 drain 交给 IDCT
