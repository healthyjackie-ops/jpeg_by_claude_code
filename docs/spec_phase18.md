# Phase 18 — Successive Approximation Refinement (progressive, Ah>0)

**Status**: Design brief (draft). Target: C model 18a → RTL 18b.
**Upstream**: Phase 17a (C model SOF2 spectral-selection AC, Ah=0) ✅
**Downstream**: Phase 19 (drain + 1080p progressive end-to-end)

## 0. 目标

在 Phase 17a 的谱选择基础上扩展 progressive SOF2 的第二类扫描：**successive approximation refinement**（Ah>0）。每个系数以若干次扫描逐位累加，从高位到低位。DC 与 AC 两种 refinement 都要支持。完整 SOF2 scan script（DC first + AC bands + 多轮 DC refine + 多轮 AC refine）要与 libjpeg-turbo 输出 pixel-exact。

**范围内**：
- DC refinement（Ss=Se=0，Ah>0）：每 block 读 1 bit，`coef[0] |= bit << Al`
- AC refinement（1≤Ss≤Se≤63，Ah>0）：ISO 10918-1 G.1.2.3
- EOBRUN 跨 block 状态，refinement 模式下需要"在 EOB 内仍对已有非零系数 append 校正位"
- Al 任意取值（配合 Ah = Al+1）
- 完整 libjpeg-turbo 默认 progressive script（10 scans）
- gray / 4:4:4 / 4:2:0

**范围外**：
- DRI + progressive（Phase 17c/18c 选做）
- 4:2:2 / 4:4:0 progressive 组合
- RTL refinement 路径（Phase 18b 另行）

## 1. 算法（ISO 10918-1 G.1.2）

### 1.1 DC refine（G.1.2.1.1）
```
for each block in scan:
    bit = receive_unsigned(1)
    if (bit)
        coef[0] |= (1 << Al)
```

### 1.2 AC refine（G.1.2.3）
核心思路：refinement 只能"把一个之前为 0 的位置变成 ±1<<Al"或"对已有的非零系数追加一个校正位"。每个 AC-refine 扫描仍走 Huffman (RRRR, SSSS) 编码：
- `SSSS == 0, RRRR < 15`：EOB run，长度 = 2^RRRR + extra 位。进入 EOB 期间，剩余 Se 之前的**非零**系数仍然每个追加一个校正位。
- `SSSS == 0, RRRR == 15`：ZRL — 跳过 16 个零系数位置（沿途非零系数追加校正位）。
- `SSSS == 1`：新的 ±1<<Al 系数。先读符号位决定新值，再跳过 RRRR 个零位置（沿途非零追加校正位），然后把新值放到下一个零位置。
- `SSSS > 1`：非法，refinement 模式下不允许幅度大于 1 的新系数。

校正位 `c`：`c==1 → |coef| += (1<<Al)`（保持符号方向增长），`c==0 → 不变`。

### 1.3 实现要点
- `coef_buf` 已经在之前扫描中累积；refine 就地修改
- EOBRUN 在每个 AC 扫描开始时清零（所有 scan 都独立）
- newnz 位置不需要记录（我们非 suspend-resume）

## 2. 接口

### 2.1 C model

新 helper（`huffman.h`）：
```c
/* DC refine: read 1 bit, OR into coef[0] at position Al */
int huff_decode_dc_refine(bitstream_t *bs, int16_t coef[64], uint8_t al);

/* AC refine: ISO G.1.2.3. eob_run carried across blocks. */
int huff_decode_ac_refine(bitstream_t *bs, const htable_t *ac_tab,
                          int16_t coef[64],
                          uint8_t ss, uint8_t se, uint8_t al,
                          uint32_t *eob_run);
```

### 2.2 decoder.c

`decode_progressive()` 根据 `scan_ah, scan_ss, scan_se` 路由：
- `Ah==0, Ss==0, Se==0`: DC first scan（Phase 17a）
- `Ah==0, Ss>=1`: AC first scan（Phase 17a）
- `Ah>0, Ss==0, Se==0`: DC refine（新）
- `Ah>0, Ss>=1`: AC refine（新）
- 其它：错误

## 3. 测试向量

`tools/gen_phase18.py`：用 libjpeg-turbo 默认 `cjpeg -progressive`（10 scans：DC interleaved + Y AC 1-5 Al=2 + Y AC 6-63 Al=2 + Cb AC full Al=1 + Cr AC full Al=1 + Y AC 1-63 refine Al=1 Ah=2 + … + DC refine Ah=0 Al=0→Ah=1 Al=0）。

规划 12 张向量：
- Gray 4 张（8×8/17×13/64×32/100×75）
- 4:4:4 4 张（8×8/16×16/40×32/97×73）
- 4:2:0 4 张（16×16/32×32/100×76/192×128）

每张用 cjpeg -progressive（默认 script）→ djpeg 得 PGM/PPM golden → C model 对齐 pixel-exact。

## 4. 验收

- Phase 18 vectors 全部 pixel-exact vs libjpeg-turbo
- Phase 06..17 全部零退步
- `make test` unit 全通

## 5. 风险

| 风险 | 缓解 |
|---|---|
| AC refine 的 ZRL/newnz/EOB 三路嵌套逻辑易错 | 严格对齐 libjpeg jdphuff 的 `decode_mcu_AC_refine` 结构 |
| Huffman 错位会导致整 scan 漂移 | 先做 gray/444 小尺寸单 refine scan，再扩 4:2:0 + 多 refine |
| refinement 与 spectral selection 混合（Al>0 首扫 + Ah>0 refine）的 coef 累加路径 | Phase 17a 的 al_shift 已处理首扫入缓存；refine 直接读 coef_buf 就地 OR/修正 |

---

## 6. Phase 18a 完工记录

**状态**：✅ 完成（17/17 vectors pixel-exact vs libjpeg-turbo；phase17 16/16、phase16 14/14、unit tests 全通；zero regression）。

### 6.1 实现要点
- `huff_decode_dc_refine`：单 bit 读出，`coef[0] |= (1 << al)`。
- `huff_decode_ac_refine`（ISO G.1.2.3）：按 libjpeg-turbo jdphuff 的 `decode_mcu_AC_refine` 结构改写。
  - SSSS ∈ {0, 1}；SSSS > 1 视为错误（refinement 振幅只能 ±1）。
  - RRRR 只计数 *零系数位置*；沿途遇到的非零系数每个追加一个 correction bit。
  - EOBn (SSSS=0, RRRR<15): 读 2^RRRR + extra，当前 block 剩余 [k..se] 范围内的非零仍要完成 correction，然后 `eob_run--` 返回。
  - ZRL (SSSS=0, RRRR=15): 跳过 16 个零位置（沿途非零仍 correct）。
  - Correction bit 应用：`if (cb && (*cp & p1) == 0)` — libjpeg 的"已置位则不叠加"保护。对有效流（refine 进入时 coef 永远是 `(1<<Ah)` 对齐的倍数），这个 guard 永远通过。
- 公共辅助 `rfn_apply_bit(bs, cp, p1, m1)` 抽出 correction-bit 应用逻辑，在 EOB/inter-EOB/walk 三条路径上复用。

### 6.2 `decode_progressive` 路由
- `Ah==0, Ss==0, Se==0`：DC first scan（17a）
- `Ah==0, Ss>=1`：AC first scan（17a）
- `Ah>0, Ss==0, Se==0`：DC refine（新）— 走 interleaved MCU，每 block 读 1 bit
- `Ah>0, Ss>=1`：AC refine（新）— 走 non-interleaved，natural extent 遍历

### 6.3 测试向量矩阵（`verification/vectors/phase18/`）
17 张 `cjpeg -progressive -scans` 生成向量：
- Gray 6 张：`default`（6 scans：DC Al=1 → Y AC 1-5 Al=2 → Y AC 6-63 Al=2 → Y AC refine Ah=2 Al=1 → DC refine Ah=1 Al=0 → Y AC refine Ah=1 Al=0）；`minimal`（4 scans）
- 4:4:4 5 张：`default`（10 scans，含 DC/Y/Cb/Cr 多轮 refinement）+ `minimal`（8 scans）
- 4:2:0 6 张：`default` + `minimal`，含 192×128 噪声 + 100×76 噪声（非 MCU 对齐）

覆盖维度：Al 1→0 DC refine、Al 2→1→0 AC refine、EOBn within refine、ZRL within refine、natural extent mismatch（Y 100×76）。

### 6.4 首次运行即 17/17 bit-exact
Phase 17a 的 EOBn-unsigned-bits + natural-extent 两个坑已经踩过，18a 纯新逻辑上线就稳。

---

## 7. Phase 17c / 18c 完工记录（progressive + DRI）

**状态**：✅ 完成（20/20 `phase_prog_dri` 向量 pixel-exact vs libjpeg-turbo；phase06-18 全部零退步；unit tests 全通）。

### 7.1 实现要点
- 新增静态辅助 `prog_handle_restart(bs, info, num_comps, *eob_run)`：字节对齐 → 消费 RSTn（`0xFF`/`MARKER_RST0..7`，兼容 `marker_pending`/`last_marker` 已 latch 的情况）→ 复位所有分量 `dc_pred`、AC scan 额外复位 `eob_run`。与 baseline 在 `decoder.c:469-508` 的 RST 流程完全一致。
- `jpeg_decode()` 移除 `if (info.sof_type == 2 && info.dri != 0) UNSUP_SOF` 拒绝。
- `decode_progressive()` DC 支路（interleaved，Ns=num_comps）：每 `info->dri` 个 MCU 触发 RST，最后一个 MCU 后**不**再等 RST（紧跟 EOI / 下个 SOS）。
- `decode_progressive()` AC 支路（non-interleaved，Ns=1）：依据 ISO F.1.2.3 —— 非 interleaved scan 的 "MCU" 即单个 data unit（8×8 block），所以 DRI 计数以 block 为单位。最后一个 block 后跳过 RST。
- 两条路径的重启计数器都是 scan-local（进入 scan 时清零），与 libjpeg-turbo `jdhuff.c`/`jdphuff.c` 的 `restarts_to_go` 语义一致。

### 7.2 测试向量矩阵（`verification/vectors/phase_prog_dri/`）
20 张 `cjpeg -progressive -scans -restart NB` 向量，DRI ∈ {1, 2, 4, 8, 16}：
- Spectral-only（全 Ah=0）11 张：gray/444/420，覆盖 MCU 对齐（32×32/48×48/64×48/192×128）+ 非对齐（97×73 444、100×75 gray、100×76 420）
- Refinement（Ah>0，default 或 minimal 脚本）9 张：同上尺寸池 + gray default 100×75 r=1（每 block 都 RST，最严酷的 AC-refine + RST 交互）

关键覆盖点：
- RST 发生在 DC interleaved scan 内（6-block MCU）
- RST 发生在 AC 非-interleaved scan 内（1-block MCU）→ AC-refine 的 EOB run 被强制在 RST 处清零
- RST 发生在自然范围 != MCU-padded 的 Y 分量内（100×76 420、97×73 444）
- DRI=1 情形：每个 data unit 都触发 RST（压力测试）
- DRI=16 情形：跨多行 Y MCU

### 7.3 首次运行即 20/20 bit-exact
因复用 baseline 已稳定的 RST-consume 逻辑，加上 18a 已 nail 住 AC-refine 的 EOBn / natural-extent 坑位，新路径上线即稳。
