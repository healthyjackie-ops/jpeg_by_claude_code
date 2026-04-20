# Phase 16 — SOF2 DC-only Scan (progressive decode, first scan)

**Status**: Design brief (draft). Implementation split into 16a / 16b / 16c.
**Upstream**: Phase 14 (SOF2 header recognize + error-out) ✅, Phase 15 (coef_buffer loop-back) ✅
**Downstream**: Phase 17 (AC spectral selection), Phase 18 (successive approximation)

## 0. 目标

实现 SOF2 (progressive) 首扫描 **DC-only, Al=0** 的端到端解码：
- 文件只包含一个 scan：`Ss=Se=0, Ah=0, Al=0`
- 每 block 的 DC coefficient 被 huffman 解出后 → 存入 `coef_buffer` 的 coef[0] 位置
- 其余 63 个 AC coefficient 视为 0
- 扫描结束（遇 EOI）后 drain coef_buffer → dequant → IDCT → pixel_out
- 验收：与 libjpeg-turbo 对 DC-only 图像的完整解码输出 **bit-exact**

SOF2 正确解码多 scan 图像要等到 Phase 17（AC 扫描组合）之后；Phase 16 只处理**单 scan DC-only** 的子集，为后续累加机制铺路。

## 1. 子阶段切分

| 子阶段 | 范围 | 风险 | 交付 |
|---|---|---|---|
| **16a** | SOS 扫描参数 (Ss/Se/Ah/Al) 捕获 + CSR/端口暴露；原 baseline 错误检查下沉到 decoder gate | 低 | C/RTL 均能读出任何 scan 的 Ss/Se/Ah/Al；baseline 回归零影响 |
| **16b** | C model SOF2 DC-only 解码全通；生成 DC-only 测试集；C model vs libjpeg-turbo bit-exact | 中 | `c_model/test_phase16` 自检通过 |
| **16c** | RTL SOF2 DC-only 通路：coef_buffer 接入 jpeg_axi_top；huffman_decoder DC-only 模式；drain FSM；bit-exact vs C model | 高 | `make diff-phase16` 全通 |

## 2. 接口变化

### 2.1 C model (`jpeg_info_t` 扩展)

```c
typedef struct {
    /* ... existing fields ... */
    uint8_t  sof_type;        /* 0=SOF0, 1=SOF1, 2=SOF2, 3=SOF3 */
    /* Scan-level params (16a) — 每次遇到 SOS 都会更新 */
    uint8_t  scan_ss;         /* 0..63 */
    uint8_t  scan_se;         /* 0..63 */
    uint8_t  scan_ah;         /* 0..13 */
    uint8_t  scan_al;         /* 0..13 */
    uint8_t  num_scans;       /* 进 decode 循环后累加 */
} jpeg_info_t;
```

### 2.2 RTL (`header_parser` 输出端口)

```verilog
output reg [1:0]  sof_type_o,      // 0=SOF0,1=SOF1,2=SOF2,3=SOF3
output reg [5:0]  sos_ss_o,
output reg [5:0]  sos_se_o,
output reg [3:0]  sos_ah_o,
output reg [3:0]  sos_al_o,
output reg        sos_valid_o      // pulses on SOS end (1 cycle)
```

### 2.3 CSR (`axi_lite_slave`)

| Addr | Reg | Bits | RW | 说明 |
|---|---|---|---|---|
| 0x40 | SCAN_PARAMS | [5:0]=Ss,[13:8]=Se,[19:16]=Ah,[23:20]=Al,[25:24]=SofType | RO | 来自 header_parser，每次 SOS 后刷新 |

## 3. Phase 16a 详解 (先行交付)

### 3.1 RTL 变更

`rtl/src/header_parser.v`:
```verilog
// S_SOS_SS: 原来只接受 ss=0，现在照单全收
S_SOS_SS: if (beat) begin
    sos_ss_o <= byte_in[5:0];
    remain   <= remain - 16'd1;
    state    <= S_SOS_SE;
end
// S_SOS_SE / S_SOS_AHAL 同理，去掉硬检查

// S_SOS_AHAL 结尾 pulse sos_valid_o
S_SOS_AHAL: if (beat) begin
    sos_ah_o <= byte_in[7:4];
    sos_al_o <= byte_in[3:0];
    sos_valid_o <= 1'b1;      // 1-cycle pulse
    header_done <= 1'b1;
    data_mode   <= 1'b1;
    state       <= S_DATA;
end
```

**gate downstream**：Phase 16a 需要保留 baseline 解码约束，避免非法 scan 进入 huffman_decoder 后崩盘。新增 `jpeg_axi_top` 内一个 combinational check：
```verilog
wire scan_is_baseline = (sos_ss_o == 6'd0) &&
                        (sos_se_o == 6'd63) &&
                        (sos_ah_o == 4'd0) &&
                        (sos_al_o == 4'd0) &&
                        (sof_type_o[1] == 1'b0);   // SOF0/SOF1 only

// 进入 S_DATA 时若 !scan_is_baseline → raise ERR_UNSUP_SOF
```

（在 Phase 16c 这个 gate 会放松到 "DC-only allowed if SOF2 && Ss=Se=0 && Ah=0"）

### 3.2 C model 变更

`c_model/src/header_parser.c::parse_sos()`:
```c
/* 无条件存 */
info->scan_ss = ss;
info->scan_se = se;
info->scan_ah = ah_al >> 4;
info->scan_al = ah_al & 0x0F;

/* 仅在非 SOF2 时强制 baseline 子集 */
if (info->sof_type != 2) {
    if (ss != 0 || se != 63 || ah_al != 0) {
        *err |= JPEG_ERR_UNSUP_SOF; return -1;
    }
}
```

`c_model/src/decoder.c::jpeg_decode_mcu()`:
- 进入时若 `sof_type == 2` → 当前 Phase 16a 直接 `err |= UNSUP_SOF; return -1;`（保留错误行为以保证回归）

### 3.3 验证

- 所有 baseline 向量（smoke/full/phase06..14）**零改动通过**
- 新增 `--mode=scan_params` 单元测试：对每张 baseline 测试图，解码后从 CSR 读 SCAN_PARAMS，断言 {Ss=0, Se=63, Ah=0, Al=0, SofType=0 或 1}

## 4. Phase 16b 详解 (C model progressive DC-only)

### 4.1 Decoder 循环改造

当前 `jpeg_decode_scan()` 假定单 scan、全帧、立即输出 pixel。SOF2 要改为：

```c
int jpeg_decode(jpeg_info_t *info, ...) {
    /* 1) parse headers up to first SOS */
    /* 2) loop: decode_scan(); if (peek_marker == EOI) break; parse_sos(); */
    /* 3) drain coef_buffer → dequant_izz → idct2d → upsample → output */
}
```

关键状态：
- `int16_t coef_buf[num_blocks][64]`  — 每 block 64 系数的累加区（Y + Cb + Cr 合并编址）
- `dc_pred[comp]`  — 每 component 自己的 DC predictor，每 scan 独立 reset；restart interval 同时 reset
- 每 scan 结束处理 RSTn → 重置 DC predictor

DC-only 首扫描的每 block 处理：
```c
int16_t dc_diff = huffman_decode_dc_value(comp->td);  /* 原已有 */
dc_pred[comp_idx] += dc_diff;
/* point transform — Ah=0 pass: 直接 << Al */
coef_buf[blk][0] = dc_pred[comp_idx] << info->scan_al;
/* AC=0 默认 */
```

### 4.2 Drain path

所有 scan 处理完毕后：
```c
for (each MCU, each block in MCU) {
    int16_t block[64] = coef_buf[blk];   /* natural order */
    dequant(block, qt[comp]);
    idct2d(block, sample_out);
    /* upsample + color convert 同 baseline */
}
```

### 4.3 测试向量生成

`tools/gen_phase16.py`:
- 用 libjpeg-turbo + custom jpegtran 合成 **只包含单 DC-only scan** 的文件
  - `cjpeg -progressive -scans <file>` 接受自定义 scan 脚本；
  - 单行 scan 脚本：`0,1,2: 0-0, 0, 0;`
- 16 张：grad / checker / noise × 多尺寸 × Y-only / 4:2:0
- 每张用 libjpeg-turbo 完整解码存为 PGM/PPM，作为 C model 的 golden

### 4.4 验收

- C model 在 16/16 DC-only 向量上 pixel-exact 匹配 libjpeg-turbo

## 5. Phase 16c 详解 (RTL SOF2 DC-only)

### 5.1 jpeg_axi_top 新增模块连接

```
  huffman_decoder                               coef_buffer
  (DC-only mode, outputs dc_val + blk_idx)  →   (w_en, w_addr=blk_idx,
                                                 w_data={960'd0, dc_val_shifted, zeros})

  After EOI:
    drain_fsm  →  coef_buffer.r_en, r_addr 0..num_blocks-1
               →  dequant_izz  →  idct_2d  →  mcu_buffer  →  pixel_out
```

新状态机（drain_fsm）：
```
S_WAIT_EOI → S_DRAIN_BLOCK (issue r_en) → S_DEQUANT → S_IDCT → S_EMIT → next block or done
```

### 5.2 huffman_decoder DC-only 模式

新增 input `dc_only_mode`。置位时：
- 解 DC value（维持原 path）
- 跳过 AC 解码循环；block_done 直接拉高
- 输出 `dc_val[15:0]` + `dc_valid`（替代 `blk_coef_valid`）

### 5.3 coef_buffer 地址空间

Phase 15 的 AW=10（1024 blocks）只够 128×128 4:2:0。1080p 4:2:0 需要 12150 blocks → AW=14 (16384)。

选项：
- A：Phase 16c 拉 AW=14 = 2 MB reg array — 合成面积不可接受但 Verilator loopback 还是可以跑
- B：Phase 16c 保持 AW=10，只测 ≤128×128 的图，大图验证留给 Phase 20 DDR
- **采 A/B 混合**：AW 参数化，默认 AW=14 for sim，AW=10 仍保留为 coef_unit 独立 top

### 5.4 验收

- 16/16 Phase 16b C-model-验证过的 DC-only 向量在 RTL 上 bit-exact（pixel 输出 vs C model）
- smoke + phase06..15 全部回归零退步（173/173：smoke 12 + phase06..12 130 + phase13 20 + phase14 8 + phase15 1 single 独立 top）

## 6. 风险清单

| 风险 | 缓解 |
|---|---|
| 1) cjpeg 的 -scans 脚本形式不稳 | 写 python helper 手工重写 SOS 段；或直接 jpegtran -scans |
| 2) coef_buffer AW=14 的 reg 阵列在 Verilator 下内存占用 > 2 GB | 改为 `always_ff` + `(* ram_style = "block" *)` attr；或用 `integer mem[...][...]` |
| 3) multi-scan DC predictor reset 与 restart interval 交互 | 先只支持 DRI=0 的 DC-only；DRI>0 留给 Phase 17 |
| 4) SOF2 + 4:2:0 MCU shape 与 scan component ordering | interleaved scan 时 Ns==num_components；非 interleaved 时 Ns=1 — Phase 16 只收 interleaved（单 scan 涵盖所有 component） |
| 5) libjpeg-turbo golden 对 Al>0 处理的细节 | Phase 16 固定 Al=0；Al>0 留给 Phase 18 |

## 7. 交付节奏（morning-self 建议）

| Day | 内容 | Commit 点 |
|---|---|---|
| 1 | Phase 16a：RTL ports + C model scan-param capture + sanity regression | `Phase 16a: SOS scan params` |
| 2–3 | Phase 16b：C model DC-only decode + vectors + test harness | `Phase 16b: C model SOF2 DC-only` |
| 4 | Phase 16c.1：RTL coef_buffer 接入 + drain FSM 框架 | `Phase 16c.1: coef_buffer hookup` |
| 5 | Phase 16c.2：huffman_decoder DC-only mode + gate 放松 + diff 测试 | `Phase 16c.2: RTL SOF2 DC-only bit-exact` |

## 8. 不在范围

- AC 扫描（Phase 17）
- Successive approximation refinement (Ah>0)（Phase 18）
- 多 scan DRI 交互（Phase 17 顺带）
- 4K progressive（Phase 20 DDR）

---

**回归基线（本 brief 写入时）**：
- smoke 12/12 ✅
- diff-full 1150 vectors（见已知 pthread_create 问题，≥900 iter 后需分批跑）
- errout (phase14 SOF2) 8/8 ✅
- coef_unit (phase15) 5/5 tests ✅
