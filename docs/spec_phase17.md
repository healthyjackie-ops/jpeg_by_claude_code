# Phase 17 — AC Spectral Selection (progressive, Ah=0 forward scans)

**Status**: Design brief (draft). Implementation split into 17a / 17b / 17c.
**Upstream**: Phase 16c (RTL SOF2 DC-only) ✅
**Downstream**: Phase 18 (successive approximation, Ah>0 refinement)

## 0. 目标

把 SOF2 progressive 从单 scan DC-only 扩展到**多 scan 谱选择（Ah=0）**：一个 DC-only 首扫描加一个或多个 AC 谱带扫描（Ss..Se 覆盖 AC 系数子区间），每 scan 可带 point transform (Al)。扫描全部结束后再 drain coef_buffer → dequant → IDCT → pixel_out。

验收：与 libjpeg-turbo 对"Ah=0 多扫描 progressive"图像的完整解码输出 **bit-exact**，且原 baseline 与 SOF2 DC-only 回归零退步。

**范围内**：
- 任意数量 Ah=0 scan（1..10 典型）
- Ss=Se=0 DC scan + Ss∈[1,63]/Se∈[Ss,63] AC 谱带
- Al 任意（point transform），但所有 scan 的 Al 取值一致化为"所有谱带进入 coef_buf 前都已按各自 Al 左移"
- EOB run-length: EOB0..EOB14 解码
- Non-interleaved AC scan（Ns=1，符合 ISO 10918-1 G.1.1.1.1 强制约束）
- Interleaved DC scan（Ns=num_components，与 baseline 风格一致）
- Grayscale / 4:4:4 / 4:2:0 chroma 模式

**范围外**：
- Ah>0 successive approximation refinement（留给 Phase 18）
- DRI with progressive（本 phase 先强制 DRI=0，DRI 留给 18 或 17c）
- 4:2:2 / 4:4:0 / 4:1:1 / CMYK 的 progressive 组合（与 baseline 正交，后续按需再加）
- 大尺寸 (>128×128 4:2:0) RTL 内存压力（Phase 20 DDR 之前用 coef_buf reg 阵列，参数化限定）

## 1. 子阶段切分

| 子阶段 | 范围 | 风险 | 交付 |
|---|---|---|---|
| **17a** | C model 多 scan AC spectral decode（DC interleaved + AC non-interleaved），EOB run-length，Al 支持；生成 phase17 测试集；vs libjpeg-turbo bit-exact | 中 | `c_model/test_phase17` 自检通过，pixel-exact 对齐 libjpeg-turbo |
| **17b** | RTL coef_buffer 写入通路：huffman_decoder AC-only 模式（新端口 ss/se/al），multi-scan FSM（header_parser 识别 scan 间 SOS + 维护 scan_idx），drain FSM 接入 baseline dequant+idct | 高 | `make diff-phase17` 全通 |
| **17c** | 大图 + DRI progressive（可选） | 中 | DRI>0 progressive 向量 bit-exact |

## 2. 接口变化

### 2.1 C model

`jpeg_info_t`：已经在 Phase 16a 加了 scan-level 字段（`scan_ss/scan_se/scan_ah/scan_al/num_scans`）。本期无需新字段。

新 helper（`huffman.h`）：
```c
/* Phase 17a: 谱带 AC-only 首扫描（Ah=0）
 * - 从 coef[Ss..Se] (natural order) 解码
 * - amp << al 后写入
 * - eob_run: 跨 block 状态，<0 无效则从 0 起
 * 返回 0 成功，-1 失败，>0 = EOB run 还剩 n blocks 未处理（调用方跳过）。
 *
 * 约定：coef 已被初始化为 0 或含 DC scan 结果；EOB/ZRL 不会修改已有 coef。
 */
int huff_decode_ac_progressive(bitstream_t *bs,
                               const htable_t *ac_tab,
                               int16_t coef[64],
                               uint8_t ss, uint8_t se,
                               uint8_t al,
                               int *eob_run);
```

`decoder.c`：重构 `jpeg_decode` 的 SOF2 路径：
```c
if (info.sof_type == 2) {
    /* 分配 coef_buf[num_blocks][64] */
    /* loop: decode_scan() → peek marker → if SOS, parse_sos(), continue; else break */
    /* drain: for each block, dequant(qt[comp_of_block]) → idct → pixel copy */
}
```

block → component 映射：
- 4:2:0 MCU 有 6 blocks（4Y+Cb+Cr）
- 4:4:4 MCU 有 3 blocks（Y+Cb+Cr）
- Gray MCU 有 1 block（Y）

block index 空间：按扫描顺序（MCU major，MCU 内 Y0,Y1,Y2,Y3,Cb,Cr）编号，方便对应 coef_buf。

non-interleaved AC scan（Ns=1）：scan 内只含一个 component，block 顺序按该 component 的 sampling grid（num_mcu_rows × num_mcu_cols × H_i × V_i）。

### 2.2 RTL（Phase 17b 时再细化）

暂时预留：
- `huffman_decoder`: 新增端口
  ```verilog
  input  wire        ac_only_mode,
  input  wire [5:0]  scan_ss,
  input  wire [5:0]  scan_se,
  input  wire [3:0]  scan_al,
  input  wire        scan_is_dc,   // 1=DC scan, 0=AC scan
  ```
- 输出单独的 `coef_wr` 到 coef_buffer：地址 = {blk_idx, zz_idx}（natural order），写入 16-bit signed 值
- `coef_buffer` AW 需要涵盖最大 block 数。128×128 4:2:0：Y 16×16=256 blocks，Cb/Cr 8×8=64 blocks each，总 384 blocks → AW=9（512）
- 大图暂不用 RTL 跑（Phase 20 DDR 之前用 C model 交叉验证）

## 3. Phase 17a 详解（C model 多扫描）

### 3.1 coef_buf 索引方案

```
num_y_blocks  = mcu_rows * mcu_cols * H1 * V1
num_cb_blocks = mcu_rows * mcu_cols * H2 * V2
num_cr_blocks = mcu_rows * mcu_cols * H3 * V3
num_blocks_total = num_y_blocks + num_cb_blocks + num_cr_blocks
```

扫描时如果是 interleaved（Ns > 1，常见于 DC scan）：
```
for (my, mx):
    for i in Hy*Vy: decode y_block; store at coef_buf[y_base + blk_idx_in_y]
    decode cb_block; store at coef_buf[cb_base + blk_idx_in_cb]
    decode cr_block; store at coef_buf[cr_base + blk_idx_in_cr]
```

non-interleaved（Ns=1，典型 AC scan）：
```
for b in 0..num_blocks[comp]-1:
    decode ac(block coef_buf[comp_base + b], ss, se, al, eob_run)
```

### 3.2 AC 首扫描解码（核心算法）

ISO 10918-1 G.1.2.2 伪代码：
```
Decode_AC_first(block):
    if (EOBRUN > 0) {
        EOBRUN -= 1;
        return;
    }
    k = Ss
    while k ≤ Se:
        RS = huffman_decode(AC)
        SSSS = RS & 0xF
        RRRR = RS >> 4
        if SSSS == 0:
            if RRRR < 15:
                EOBRUN = (1 << RRRR) + receive(RRRR) - 1
                break
            else:
                k += 16    # ZRL
        else:
            k += RRRR
            if k > Se: error
            amp = receive_extend(SSSS)
            block[ZZ(k)] = amp << Al
            k += 1
```

### 3.3 decoder.c 改造

现有 SOF2 的 g_dc_only 模式需要保留（只处理单 DC scan），并新增 g_progressive 模式（多 scan）。或更干净地：把 SOF2 路径拆到独立函数 `decode_progressive()`，旧 `g_dc_only` 单 scan 路径自然包含。

```c
static int decode_progressive(bitstream_t *bs, jpeg_info_t *info, jpeg_decoded_t *out) {
    /* allocate coef_buf[num_blocks_total][64] = 0 */
    /* allocate component→block_base lookup */

    for (;;) {
        /* at this point, SOS just parsed, decoder ready for scan */
        if (info->scan_ah != 0) { out->err = UNSUP_SOF; return -1; }

        if (info->scan_ss == 0 && info->scan_se == 0) {
            /* DC scan (interleaved) */
            decode_dc_scan_progressive(bs, info, coef_buf);
        } else {
            /* AC scan (non-interleaved) */
            decode_ac_scan_progressive(bs, info, coef_buf);
        }

        /* Look for next marker */
        bs_align_to_byte(bs);
        peek_marker(bs, &m);
        if (m == MARKER_EOI) break;
        if (m == MARKER_SOS) {
            parse_sos_segment(bs, info);  /* updates scan_ss/se/ah/al */
            continue;
        }
        /* DHT/DQT between scans is legal in progressive; parse and continue */
        if (m == MARKER_DHT || m == MARKER_DQT || m == MARKER_DRI) {
            /* update tables */
            parse_misc(bs, info);
            continue;
        }
        out->err = UNSUP_SOF; return -1;
    }

    /* drain: for each block in coef_buf, dequant → idct → copy to pad planes */
    for (blk = 0; blk < num_blocks_total; blk++) {
        component c = block_to_comp(blk);
        dequant_block(coef_buf[blk], qt[c]);
        idct_islow(coef_buf[blk], sample_out);
        place_sample(sample_out, ...);
    }
    /* upsample + crop — same as baseline */
}
```

### 3.4 测试向量生成

`tools/gen_phase17.py`：用 `cjpeg -progressive -scans <script>` 生成。

默认 libjpeg 的 progressive script（用于 `cjpeg -progressive` 无自定义）：
```
0 1 2: 0-0, 0, 0;           # DC Y Cb Cr interleaved, Ah=0 Al=0
0: 1-5, 0, 0;               # Y AC[1..5] Ah=0 Al=0
0: 6-63, 0, 0;              # Y AC[6..63] Ah=0 Al=0
1: 1-63, 0, 0;              # Cb AC full Ah=0 Al=0
2: 1-63, 0, 0;              # Cr AC full Ah=0 Al=0
```

这个 script 覆盖 5 个 Ah=0 scan，是 Phase 17a 的典型测试场景。

计划 16 张向量：
- 灰度：grad / checker / noise × 3 尺寸
- 4:4:4：grad / checker / noise × 2 尺寸
- 4:2:0：grad / checker / noise × 2 尺寸

每张用 `djpeg` 解码得到 PGM/PPM golden，再由 C model 对齐。

### 3.5 验收

- 16/16 向量 pixel-exact 匹配 libjpeg-turbo
- baseline + DC-only 回归零退步（smoke/phase06..16 全通）

## 4. Phase 17b 详解（RTL 多扫描）

（初稿，17a 完成后再细化）

### 4.1 多 scan 状态管理

`header_parser`：保留现有 SOS 解析，去掉对第二次 SOS 的 "sof2_gate_err" 限制。引入 `scan_idx_o`，每次 SOS 后自增。

`jpeg_axi_top`：增加 scan-loop 控制：
- 收到 EOI → assert drain_start，否则继续 data_mode
- 收到第二个 SOS → 复位 bitstream 边界，重新配置 huffman_decoder 模式

### 4.2 huffman_decoder AC-only 模式

新端口 `ac_only_mode + scan_ss + scan_se + scan_al + scan_is_dc`。

DC scan (scan_is_dc=1)：维持现有 DC decode path，输出 coef[0] 为 `dc_pred << Al`。写 coef_buffer 地址 = natural_order_zz[0] = 0，只写 1 entry。

AC scan (scan_is_dc=0, ac_only_mode=1)：
- skip DC decode
- read RS; if SSSS==0 and RRRR<15 → EOB run of `2^RRRR + extra`
- if SSSS==0 and RRRR==15 → ZRL (k += 16)
- else → k += RRRR; amp = receive(SSSS); coef[ZZ[k]] = amp << Al; k++
- 循环 k ≤ Se
- EOB run 状态跨 block 保留（scan-level counter）

coef_buffer 写端口：地址 = {blk_idx, zz_idx[5:0]}，不再整 block 一次性写。

### 4.3 coef_buffer 地址

AW 需要：
- 128×128 4:2:0: 384 blocks → blk_aw=9, + 6-bit coef_idx = 15 bits = 32 K 16-bit entries = 64 KB reg（仍在可合成范围内，但是用 SRAM tile 更合适）
- Phase 17b 先用 reg 阵列参数化，default AW=9；大图（Phase 20）再换 DDR-backed tile

### 4.4 drain FSM

EOI 后，从 coef_buffer 线性读出 → dequant_izz → idct_2d → mcu_buffer → pixel_out。一个 block 一 pass。

### 4.5 验收

- phase17 16/16 RTL bit-exact vs C model
- phase06..16 零退步

## 5. 风险清单

| 风险 | 缓解 |
|---|---|
| 1) cjpeg -scans 脚本语法/行为与我的理解不符 | 先用默认 `-progressive` 生成（无 -scans），libjpeg 自带 script；自定义 -scans 留给后续 |
| 2) AC first scan EOB run 跨 block 状态管理 | C model 用 scan-level static `eob_run` 计数；RTL 用 scan-level 寄存器 |
| 3) non-interleaved AC scan 的 block 遍历顺序 | ISO G.1.1.1 明确规定：按该 component 的 raster order，即 my × mx × Vi × Hi |
| 4) DHT/DQT inside scan（libjpeg 罕见但合法） | C model 用通用 parse_misc 循环；RTL 暂只接受 scan 之间连续 SOS 或 EOI |
| 5) 4:2:0 non-interleaved AC scan 的 block 归属 | block_to_comp(blk) 查 block_base 数组 |
| 6) coef_buf 内存（1080p 4:2:0 ≈ 3 MB） | C model calloc；RTL 只测小图，大图在 Phase 20 DDR |

## 6. 交付节奏

| Day | 内容 | Commit 点 |
|---|---|---|
| 1 | spec + C model skeleton（decode_progressive 框架 + 无 AC scan 路径） | `Phase 17 spec + C model skeleton` |
| 2 | C model AC first-scan decode + EOB run | `Phase 17a.1: C model AC scan decode` |
| 3 | 向量生成 + test_phase17 harness + 16/16 bit-exact | `Phase 17a: C model phase17 16/16 pass` |
| 4 | RTL huffman_decoder AC-only mode + coef_buffer 地址改 | `Phase 17b.1: RTL AC-only mode + coef wr` |
| 5 | drain FSM + multi-scan top-level + 16/16 RTL bit-exact | `Phase 17b: RTL phase17 16/16 pass` |
| 6 | 回归 + 报告 | `Phase 17 report` |

## 7. 不在范围

- Ah>0 refinement（Phase 18）
- DRI progressive（Phase 17c 选做或 Phase 18 顺带）
- 4:2:2/4:4:0/4:1:1 progressive（未来按需）
- Arithmetic-coded progressive（Wave 4）
- 大于 128×128 4:2:0 RTL 内存（Phase 20 DDR）

---

**回归基线（本 brief 写入时）**：
- smoke 12/12 ✅
- phase06..13 150/150 ✅
- phase14 errout 8/8 ✅
- phase15 coef_unit 5/5 ✅
- phase16 DC-only 14/14 ✅

---

## 8. Phase 17a 完工记录

**状态**：✅ 完成（16/16 向量 pixel-exact vs libjpeg-turbo，phase16 14/14 无回退，unit tests 全通）。

### 8.1 实现要点
- `huffman.c::huff_decode_ac_progressive()`：ISO 10918-1 G.1.2.2。EOBn 的 extra bits 必须用 **unsigned** 读（`bs_get_bits_u`），不能复用 sign-extend 版本；否则 r>0 时 EOB run 会被误解为负数而乱跳。
- `decoder.c::decode_progressive()`：单入口多扫描循环，coef_buf 在扫描间累积，EOI 后统一 drain → dequant → IDCT → plane place → upsample。
- `parse_sos` 放宽 `Ns==num_components` 约束，允许 SOF2 的 Ns=1 AC 非交错扫描；新增 `scan_comp_idx[]` 供 AC 扫描定位目标分量。
- `jpeg_parse_between_scans()`：扫描之间合法 marker 序列（DHT/DQT/DRI/COM/APPn/SOS/EOI）全部吃掉再继续。

### 8.2 最后一公里 bug：non-interleaved AC 扫描块数
ISO 10918-1 A.2.3 规定非交错扫描的块数 = ceil(Xi/8) × ceil(Yi/8)（组件自然块范围），**不是** MCU 对齐的块网格。对 4:2:0 图像在宽或高不是 16 倍数时，Y 组件的自然块范围小于 MCU-padded 范围：

- 例：100×76 4:2:0 → Y 自然 13×10=130 blocks，MCU-padded 14×10=140 blocks
- DC 交错扫描写满 140 MCU-padded（含边缘列）；AC 非交错扫描只访问 130 自然块

修复：`comp_grid_t` 加 `nat_rows/nat_cols` 字段；AC 扫描用 2D 遍历 natural extent，coef_buf 索引用 MCU-padded stride `blk_cols` 保持与 DC 一致。边缘 MCU-pad 块的 AC 保持为 0，不影响 IDCT 输出（drain 时这些块也不会越过 crop 边界进入 pixel plane）。

### 8.3 测试向量矩阵（`verification/vectors/phase17/`）
16 张 `cjpeg -progressive -scans` 生成向量：
- Gray 5 张：grad/check/noise × {8×8, 17×13, 100×75}，full + split 扫描脚本
- 4:4:4 4 张：grad/check/noise × {8×8, 16×16, 40×32, 97×73}
- 4:2:0 7 张：grad/check/noise × {16×16, 32×32, 64×48, 100×76, 128×64, 192×128, 96×64}

其中 `p17_420_noise_split_100x76_q60` 是 MCU-对齐-bug 触发向量。

### 8.4 交付
- `Makefile`：新增 `phase17` target + PHASE17_DIR
- `c_model/tools/dump_coefs.c`：libjpeg 的 `jpeg_read_coefficients()` 包装，block-by-block coef dump，用于 bug 排查

## 9. Phase 17d — SOF2 Huffman extended chroma (4:2:2 / 4:4:0 / 4:1:1)

**Status**: ✅ **COMPLETE** — 81/81 phase17d bit-exact (2026-04-21).

Phases 17a/b/c + 18a/b/c covered gray / 4:4:4 / 4:2:0 only. Phase 24c
then extended the **arith** progressive path (SOF10) to 4:2:2 / 4:4:0 /
4:1:1, which left the Huffman progressive path (SOF2) trailing. Phase
17d closes that asymmetry by folding the same three 3-comp YCbCr
layouts into `decode_progressive`.

| Mode | Sampling | MCU | Y blocks | Cb/Cr blocks |
|---|---|---|---|---|
| 4:2:2 | Y 2×1, chroma 1×1 | 16×8  | 2 (horizontal) | 1 each |
| 4:4:0 | Y 1×2, chroma 1×1 | 8×16  | 2 (vertical)   | 1 each |
| 4:1:1 | Y 4×1, chroma 1×1 | 32×8  | 4 (horizontal) | 1 each |

### 9.1 Changes to `decode_progressive`

Same five-step shape as Phase 24c `decode_sof10` (and identical branch
layout — coef_buf + cg[c] natural grid already generalize to any
sampling; only explicit MCU-interleave + drain upsample are per-mode):

1. **Accept gate** extended from `{gray, 444, 420}` to also admit
   `{422, 440, 411}`; `is_chroma_sub` replaces `is_420` in the drain
   buffer-allocation predicate.
2. **MCU footprint** mirrors baseline:
   ```
   mcu_w = is_411 ? 32 : ((is_420 || is_422) ? 16 : 8);
   mcu_h = (is_420 || is_440) ? 16 : 8;
   CWp_sub = is_411 ? Wp>>2 : ((is_420 || is_422) ? Wp>>1 : Wp);
   CHp_sub = (is_420 || is_440) ? Hp>>1 : Hp;
   ```
3. **`cg[c]` natural block grid** per sampling — three new branches
   set `blk_rows × blk_cols` for Y (mcu-major, 2× horiz / 2× vert /
   4× horiz) and chroma (1×1). AC non-interleaved scan walks
   `nat_rows × nat_cols` on each component's grid without per-mode code.
4. **DC-first MCU interleave** — three new branches emit
   `huff_decode_dc_progressive` (or `huff_decode_dc_refine` in refine
   scans) in ISO-mandated order: Y blocks in natural scan order, then
   Cb, then Cr. DRI restart handling and AC scan loop are both mode-
   agnostic (they already walk `cg[c]` / `nat_rows × nat_cols`).
5. **Drain upsample + sub-res copy** — three new cases for chroma
   upsample (horiz 2x / horiz 4x / vert 2x) and three new cases to
   emit `cb_plane_422 / _440 / _411` (and Cr counterparts). These
   mirror the baseline Phase 10/11 helpers verbatim.

### 9.2 Vectors

`tools/gen_phase17d.py` generates **81 vectors** (3 chroma × 3 patterns
× 9 size/quality/DRI combos). Each file is encoded with cjpeg's
default progressive script, so every vector exercises all 4 scan types
(DC-first, DC-refine, AC-first, AC-refine).

- Sizes span 17×16 (non-MCU-aligned) → 320×200
- DRI ∈ {0, 1, 4, 8}
- Quality ∈ {30, 60, 70, 75, 80, 85, 90}

**81/81 pixel-exact vs libjpeg-turbo on first try, no bring-up issues.**
The coef_buf + cg[c] abstraction from Phase 24a/24c generalized the
scan loop enough that only DC-first MCU interleave + drain upsample/copy
needed new code — the AC scan loop was mode-agnostic.

### 9.3 Regression

- `make test`: ALL TESTS PASSED
- full (phase06-18 legacy, incl. phase_prog_dri + smoke): 1150/1150 Y=0 C=0
- phase22 (SOF9 sequential arith gray/444/420): 18/18
- phase24 (SOF10 progressive arith gray/444/420): 33/33
- phase24c (SOF9/SOF10 arith 422/440/411): 162/162
- phase17d (SOF2 Huffman 422/440/411): 81/81
- **Combined 1444/1444 bit-exact**

### 9.4 Remaining gaps (after Phase 17d)

After Phase 17d, SOF2 Huffman covers all 5 common 8-bit 3-comp YCbCr
chroma layouts — matching arith parity reached in Phase 24c. Still
deferred:

- **CMYK (Nf=4)** in any SOF2/SOF9/SOF10 path
- **P=12** in any SOF2/SOF9/SOF10 path (decode_progressive asserts P=8)

Both are unblockable future extensions; libjpeg-turbo supports them so
bit-exact reference decodes are available whenever the scope pickup lands.
