# Phase 25 — SOF3 Lossless（ISO/IEC 10918-1 Annex H）

**Status**: 25a ✅ / 25b ✅ / 25c ✅ C model 完成；Phase 26（RTL）、Phase 27（P ∈ {2..16}）后续。
**Upstream**: Phase 18c ✅（progressive + DRI），Phase 13 ✅（12-bit precision piping）
**Downstream**: Phase 26（lossless RTL），Phase 27（P=2..16），Phase 28（SOF11 lossless-arith）

---

## 0. 目标

把 decoder 扩展到 **SOF3 Lossless**（ISO 10918-1 Annex H）。Lossless 与 SOF0/1/2 是完全不同的管线：
- 没有 DCT / 量化 / IDCT
- 每个 sample 由 prediction + Huffman-coded difference 还原
- 预测器 7 选 1（Ps = 1..7，ISO Table H.1）
- 点变换 Pt（左移量，类似 progressive 的 Al）

第一步（**Phase 25a**）：最小可用 Lossless —— 灰度、P=8、Pt=0、DRI=0、所有 7 种 predictor。后续 25b/25c 再开放多分量、Pt>0、DRI。Phase 27 再开 P∈{2..16} 的位宽。

---

## 1. 算法（ISO 10918-1 H.1）

### 1.1 预测器（H.1.2.1 Table H.1）

```
Ra = cur[x-1]     (同一行、左一个)
Rb = prev[x]      (上一行、同一列)
Rc = prev[x-1]    (上一行、左一个)

Ps = 1: Px = Ra
Ps = 2: Px = Rb
Ps = 3: Px = Rc
Ps = 4: Px = Ra + Rb - Rc
Ps = 5: Px = Ra + ((Rb - Rc) >> 1)
Ps = 6: Px = Rb + ((Ra - Rc) >> 1)
Ps = 7: Px = (Ra + Rb) >> 1
```

边界规则（H.1.2.1）：
- `(x==0, y==0)` 帧首：`Px = 2^(P-Pt-1)`（P=8、Pt=0 → 128）
- `y==0, x>0`（首行后续）：`Px = Ra`（忽略 Ps）
- `x==0, y>0`（次行起首列）：`Px = Rb`（忽略 Ps）
- 其他：按 Ps 查表

### 1.2 Huffman difference（H.1.2.2 Table H.1）

Symbol SSSS ∈ {0..16}：
- `SSSS == 0`：diff = 0
- `SSSS ∈ {1..15}`：读 SSSS 位扩展，按 DC 的符号扩展（MSB=1 正值，MSB=0 取 value - ((1<<SSSS)-1)）
- `SSSS == 16`：特殊，diff = 32768，**不读**扩展位

### 1.3 重建（H.1.2）

```
sample = (Px + diff) mod 2^P          // P=8 → mask 0xFF
output = sample << Pt                  // Pt=0 → 原样输出
```

`Px + diff` 是 P+1 位有符号量；mod 2^P 把溢出回绕到 [0, 2^P-1]。对 P=8 这个 mask = 0xFF。

---

## 2. 接口

### 2.1 C model（Phase 25a 已落地）

**`src/header_parser.c`**：

```c
static int parse_sof3(bitstream_t *bs, jpeg_info_t *info, uint32_t *err) {
    info->sof_type = 3;
    return parse_sof_common(bs, info, err, /*progressive=*/0);
}
```

dispatch：`case MARKER_SOF3: → parse_sof3`。`parse_sos` 针对 SOF3 放宽 Ss/Se/Ah/Al：
- `Ss` ∈ {1..7} = predictor Ps
- `Se` 必须 == 0（ISO H.1.2）
- `Ah == 0`（reserved）
- `Al` = Pt（点变换位数，0..15）

**`src/huffman.c`**：新 helper `huff_decode_lossless_diff`：

```c
int huff_decode_lossless_diff(bitstream_t *bs, const htable_t *dc_tab,
                              int32_t *diff_out) {
    uint8_t size;
    if (huff_decode_symbol(bs, dc_tab, &size)) return -1;
    if (size > 16) return -1;
    if (size == 0)  { *diff_out = 0;     return 0; }
    if (size == 16) { *diff_out = 32768; return 0; }
    int32_t v;
    if (bs_get_bits(bs, size, &v)) return -1;
    *diff_out = v;
    return 0;
}
```

**`src/decoder.c`**：新 `decode_lossless()`，顶层 `jpeg_decode` 按 `info.sof_type == 3` 分派。核心循环：

```c
for (y = 0; y < H; y++) {
    uint8_t *cur  = row;
    const uint8_t *prev = (y > 0) ? row - W : NULL;
    for (x = 0; x < W; x++) {
        int32_t Px;
        if      (y==0 && x==0) Px = 1 << (P - Pt - 1);  // 128 for P=8/Pt=0
        else if (y==0)         Px = cur[x - 1];          // Ra
        else if (x==0)         Px = prev[0];             // Rb
        else {
            Ra = cur[x - 1]; Rb = prev[x]; Rc = prev[x - 1];
            switch (Ps) { ... };    // H.1 Table H.1
        }
        int32_t diff;
        huff_decode_lossless_diff(bs, dc_tab, &diff);
        int32_t sample  = (Px + diff) & mask;   // mask = (1<<P)-1
        int32_t out_val = sample << Pt;
        if (out_val > 255) out_val = 255;       // Phase 25a: P=8, Pt=0 → 不触发
        cur[x] = (uint8_t)out_val;
    }
    row += W;
}
```

**25a scope guards**（超出即 `JPEG_ERR_UNSUP_SOF`）：
- `info->chroma == CHROMA_GRAY` （Nf == 1）
- `info->precision == 8`
- `info->dri == 0`
- `Ps ∈ {1..7}`
- `Pt ∈ {0..15}`（实际 P=8 时 Pt>0 会饱和；25b 放开后用 16-bit out）

### 2.2 golden_compare 扩展（25a）

libjpeg-turbo **不**支持 lossless 的 `raw_data_out=TRUE` 路径，必须走 `jpeg_read_scanlines`。`golden/golden_compare.c` 新增：

```c
static int peek_sof_type(const uint8_t *buf, size_t sz);   // 预扫 SOF0/1/2/3
static int libjpeg_decode_lossless(...);                   // gray-only, JCS_GRAYSCALE
```

主循环按 `peek_sof_type(.) == 3` 改走 lossless 路径。

---

## 3. 测试向量

### 3.1 Phase 25a 矩阵（`verification/vectors/phase25/`）

`tools/gen_phase25.py` 用 `cjpeg -lossless psv -grayscale` 生成 **28 张**灰度 lossless JPEG：

| Ps | 32×32 grad | 64×48 check | 97×73 noise | 192×128 grad |
|----|------------|-------------|-------------|--------------|
| 1  | ✓          | ✓           | ✓           | ✓            |
| 2  | ✓          | ✓           | ✓           | ✓            |
| 3  | ✓          | ✓           | ✓           | ✓            |
| 4  | ✓          | ✓           | ✓           | ✓            |
| 5  | ✓          | ✓           | ✓           | ✓            |
| 6  | ✓          | ✓           | ✓           | ✓            |
| 7  | ✓          | ✓           | ✓           | ✓            |

覆盖维度：
- 全部 7 种预测器
- 非 8-对齐尺寸（97×73）确保边界像素 Ra/Rb 路径正确
- 噪声图案压缩比最低、Huffman 表树最宽
- 格子图案首列/首行切换频繁（Rb 路径压测）
- 大图（192×128）验证 row pointer 计算

### 3.2 回归（Phase 25a 验收）

| 回归套 | 通过/总 |
|--------|---------|
| phase25 (new) | 28 / 28 |
| phase_prog_dri | 20 / 20 |
| phase18 | 17 / 17 |
| phase17 | 16 / 16 |
| phase16 | 14 / 14 |
| phase14 | 8 / 8 |
| phase13 | 20 / 20 |
| phase12 | 15 / 15 |
| phase11a/b | 30 / 30 |
| phase10 | 15 / 15 |
| phase09 | 15 / 15 |
| phase08 | 15 / 15 |
| phase07 | 20 / 20 |
| phase06 | 20 / 20 |
| **合计** | **253 / 253** |

单元测试（`make test`）全通。

---

## 4. 路线图

| Sub-phase | 范围 | 时长 | 关键点 |
|-----------|------|------|--------|
| 25a ✅ | gray P=8 Pt=0 DRI=0 Ps 1-7 | 1d | 7 predictors × 3-4 patterns |
| 25b | 多分量（Nf ∈ {1,2,3,4}）+ Pt>0 输出 | 2d | RGB-lossless；每分量独立 predictor state；Pt 用 16-bit intermediate |
| 25c | SOF3 + DRI | 1d | 复用 phase 17c 的 `prog_handle_restart` 骨架 |
| 26 (RTL) | lossless RTL path | 2-3d | predictor unit + Huffman lossless decode + write-back to framebuf |
| 27 | P ∈ {2..16} | 3d | 16-bit framebuf / output；Huffman P-1..16 分位 |
| 28 | SOF11 lossless-arith | 3d | Q-coder hook into lossless predictor |

---

## 5. 25c 完工记录（DRI restart markers）

**提交**：<hash-to-fill-in>
**日期**：2026-04-21

### 5.1 范围与扩展

Phase 25c 在 25a/25b 基础上补齐 **DRI 重启标记**：
- 任意 `DRI > 0`，限定为 **MCU 行的整数倍**（对齐 libjpeg-turbo 解码约束）
- 所有组合仍保持 Phase 25b 覆盖：Nf ∈ {1,3}，Ps ∈ {1..7}，Pt ∈ {0..2} 实测
- 越界 DRI（非行整倍数）→ 返回 `JPEG_ERR_UNSUP_SOF`

### 5.2 ISO vs libjpeg-turbo 语义（踩坑核心）

**教科书错觉**：ISO H.1.2.2 只说“每个 restart interval 的第一个 sample 用 `2^(P-Pt-1)`”。
最初的实现按字面理解写成 per-sample `after_restart[c]` 标志——**这是错的**，仅 Ps=1 碰巧通过。

**libjpeg-turbo 真实语义**（`src/jdlossls.c` + `src/jddiffct.c`）：
1. 有两套 predictor 回调：
   - `jpeg_undifference_first_row`：第一列 = `2^(P-Pt-1)`，其他列 = **Ra**（忽略 Ps）
   - `jpeg_undifference<Ps>`：第一列 = **Rb**（忽略 Ps），其他列按 Ps 查表
2. `start_pass_lossless` 将 `predict_undifference[ci]` 设为 `first_row`；跑完一行后 `first_row` 自身把回调替换成 `undifference<Ps>`。
3. `process_restart` 调用 `start_pass_lossless`，等价于把 `predict_undifference[ci]` 重新指回 `first_row`——**整个 RST 之后的下一整行都用 1-D Ra 预测**。
4. `start_input_pass` 强制 `restart_interval % MCUs_per_row == 0`，保证 RST 必然落在行边界。

### 5.3 修复实现（`src/decoder.c`）

- 放弃 per-sample `after_restart[c]`，改用 per-row `first_row_mode[c]`：
  - 初始全 1
  - 每行末：若本行结尾处命中 RST，`first_row_mode[c] = 1`；否则置 0
- predictor 分支：
  ```c
  if (first_row_mode[c]) {
      Px = (x == 0) ? initial_pred : cur[x-1];   // Ra
  } else if (x == 0) {
      Px = prev[0];                              // Rb（无论 Ps）
  } else {
      /* Ps 查表：Ra / Rb / Rc / Ra+Rb-Rc / ... / (Ra+Rb)>>1 */
  }
  ```
- MCU 末尾：若 `dri != 0 && mcu_count == dri && mcu_idx < total_mcus`，调用 `prog_handle_restart` + 复位 `mcu_count` + 标记 `rst_at_row_end`
- 入口 guard：`info->dri % W != 0` → `JPEG_ERR_UNSUP_SOF`（W 即 MCUs_per_row，在 Hi=Vi=1 前提下）

### 5.4 调试踪迹

1. 初版 DRI 修复只过 Ps=1，其余全挂 → 怀疑 Huffman。
2. 实测 gray Ps=2+DRI=2 过，gray Ps=4+DRI=2 挂，RGB 三种均挂。
3. 发现 gray 测试挂的唯一是 Ps=4（Ra+Rb-Rc），而 RGB 挂 Ps=2/4/7——“梯度图像上 Ra≈Rb”的巧合导致 gray Ps=2 误过。
4. 克隆 libjpeg-turbo 源码扫读 `jdlossls.c`，发现 `jpeg_undifference_first_row` 在整行上持续使用 Ra，立刻锁定语义偏差。
5. 换成 `first_row_mode[c]` 后 16/16 scratch 全过，143/143 phase25c 正式向量全过。

### 5.5 测试矩阵（`verification/vectors/phase25c/`）

143 张向量（`tools/gen_phase25c.py`）：

| Grid | 规模 | 覆盖 |
|------|------|------|
| A: 7 predictors × 4 DRI × 2 pattern × 2 mode | 32×32 | gray + RGB × (grad/check) × Ps 1..7 × RR ∈ {1,2,4,8} |
| B: noise ptr × 7 predictors | 64×48 | gray+RGB × noise × Ps 1..7，RR=4 |
| C: Pt ∈ {1,2} × 代表 Ps × gray/RGB | 48×48 | Ps ∈ {1,4,7}，RR=4 |
| D: 每行都有 RST 的 stress | 97×73 | RGB noise × Ps {1,2,4,5,7}，RR=1 |

### 5.6 回归汇总（Phase 25c 验收）

| 套件 | 通过/总 |
|------|---------|
| phase25c (new) | 143 / 143 |
| phase25b | 44 / 44 |
| phase25 | 28 / 28 |
| phase_prog_dri | 20 / 20 |
| phase18 | 17 / 17 |
| phase17 | 16 / 16 |
| phase16 | 14 / 14 |
| phase14 | 8 / 8 |
| phase13 | 20 / 20 |
| phase12 | 15 / 15 |
| phase11a/b | 30 / 30 |
| phase10 | 15 / 15 |
| phase09 | 15 / 15 |
| phase08 | 15 / 15 |
| phase07 | 20 / 20 |
| phase06 | 20 / 20 |
| **合计** | **440 / 440** |

单元测试全通，跨所有 Wave-1/2/3 管线无退步。

---

## 6. 25b 完工记录（Nf=3 RGB + Pt>0）

**提交**：<hash-to-fill-in>
**日期**：2026-04-21

### 5.1 范围与扩展

Phase 25b 在 25a 基础上扩展：
- **Nf = 3 RGB interleaved scan**（Hi=Vi=1 所有分量，与 cjpeg 默认一致）
- **Pt > 0 point transform**（0..4 实际测过，理论支持 0..15）
- 仍保持 DRI = 0、P = 8、不支持 non-interleaved 多分量扫描

### 5.2 关键实现改动（`src/decoder.c`、`decoder.h`、`golden_compare.c`）

1. **predictor 与 output 分离的两级缓冲**：ISO H.1.2 规定 predictor 邻居 Ra/Rb/Rc 在 **pre-Pt-shift domain** 比较（P-Pt 位宽）。
   - Pt = 0：内部域 == 输出域，`int_planes[c] = out_planes[c]`，零额外分配
   - Pt > 0：分配 `int_scratch[c]`（8-bit 内部域够 P=8），decode 写入 scratch，EOS 之后 `dst[i] = src[i] << Pt`
   - `mask = (1 << (P - Pt)) - 1`（25a 的 0xFF 改为 (1 << (P-Pt))-1）

2. **多分量 interleaved 扫描**：外层 y / x，内层 `for (si = 0..Ns-1) { c = scan_comp_idx[si]; ... }`。每分量各自取 `row_c[c]`、`prev_c[c]` 指针，predictor state 互不干扰。

3. **Huffman 表按 component 查表**：`dc_tabs[c] = &info->htables_dc[components[c].td]`。cjpeg 默认 RGB lossless 三分量共享同一张表 Td=0，但代码支持不同表号。

4. **输出映射**：Nf=3 时 `y_plane/cb_plane/cr_plane` 分别接 R/G/B。新增 `jpeg_decoded_t.is_rgb_lossless` 标志让下游消费者跳过 YCbCr→RGB。

5. **golden_compare**：`libjpeg_decode_lossless` 为 Nf=3 改 `out_color_space = JCS_RGB`，`jpeg_read_scanlines` 拿到交织的 R/G/B 行，再拆成 3 个 plane 以便按 4:4:4 路径比较。`cb_width/cb_height = W×H`。

### 5.3 踩坑记

**Pt > 0 初版全挂**（dy_max = 240/208）。
第一版 decode_lossless 把 `sample << Pt` 后的值直接写进 `cur[x]`，下一轮 predictor 从 plane 里再取出来作为 Ra/Rb/Rc —— 这样 predictor state 变成 Pt-shift 之后的值，而 cjpeg 预测用的是 pre-shift 值，所以 diff 与 predictor 对不上，整张图错。

**修复**：把 predictor 读写都放在 pre-shift 域，内部存一份 `int_scratch`；解码完成再统一 `<< Pt` 写入 `out_planes`。mask 也从 `(1<<P)-1` 改为 `(1<<(P-Pt))-1`。

### 5.4 测试矩阵（`verification/vectors/phase25b/`）

44 张向量：

- **Nf=3 RGB Pt=0**：21 张（7 predictors × 3 patterns × {32×32, 64×48, 97×73}）
- **Nf=3 RGB Pt∈{1..4}**：12 张（代表性 Ps + 尺寸组合）
- **Nf=1 gray Pt∈{1..4}**：8 张
- **大图 Nf=3 RGB**：3 张（192×128，Ps ∈ {1,4,7}）

### 5.5 回归汇总（Phase 25b 验收）

| 套件 | 通过/总 |
|------|---------|
| phase25b (new) | 44 / 44 |
| phase25 | 28 / 28 |
| phase_prog_dri | 20 / 20 |
| phase18 | 17 / 17 |
| phase17 | 16 / 16 |
| phase16 | 14 / 14 |
| phase14 | 8 / 8 |
| phase13 | 20 / 20 |
| phase12 | 15 / 15 |
| phase11a/b | 30 / 30 |
| phase10 | 15 / 15 |
| phase09 | 15 / 15 |
| phase08 | 15 / 15 |
| phase07 | 20 / 20 |
| phase06 | 20 / 20 |
| **合计** | **297 / 297** |

单元测试全通。

---

## 7. 25a 完工记录

**提交**：<hash-to-fill-in>
**日期**：2026-04-21

### 5.1 实现节拍
1. `parse_sof3` + SOS 放宽（Ss∈1-7, Se=0, Ah=0）
2. `huff_decode_lossless_diff`（SSSS 0-16，特殊 16=32768 不读额外位）
3. `decode_lossless` 主循环（Ps 1-7 + 边界 Ra/Rb/initial predictor）
4. `golden_compare` 增加 SOF3 peek + lossless scanlines 路径
5. `tools/gen_phase25.py` + Makefile `phase25` target
6. 28 张 ps1..7 向量、首轮即 28/28 bit-exact

### 5.2 踩到的坑

**唯一 bug：predictor 邻居 indexing 算错。**
原版：`cur[-1]`, `prev[0]`, `prev[-1]`（`cur` 是当前行首指针，不随 x 前进）。
导致 `cur[-1]` 对 `x > 0` 的 sample 读到**上一行最后一字节**，不是左侧邻居。
smoke 测试首次 dy_max=247/244。

**定位方式**：手动 bit-trace 首 sample 的 Huffman 流。
- SOI/DQT/SOF3 后首个熵字节 `0x1e` = 0001 1110。
- DHT Huffman：code `000` = SSSS=6（3-bit）。
- 读 6 个 extra bit = `011110` = 30；MSB=0 → 负值分支 → `30 - ((1<<6)-1)` = -33。
- 首 sample: `(128 + (-33)) & 0xFF` = 95 = `0x5f`。
- djpeg 输出 PGM 在 offset 13（PGM header "P5\n32 32\n255\n" 13 字节）处就是 `0x5f`。

证明 bitstream/Huffman 完全正确，锁定到 predictor indexing。

**修复**：`cur[x-1]`, `prev[x]`, `prev[x-1]`。`prev` 声明为 `const uint8_t *`。

### 5.3 范围限制（Phase 25a 合法范围）

超出以下任一条，`decode_lossless` 立即返回 `JPEG_ERR_UNSUP_SOF`：
- 非灰度（Nf ≠ 1）
- 精度 ≠ 8
- DRI ≠ 0
- Ps ∉ {1..7}
- Pt > 15（语法不允许）

其中 **Pt ∈ {1..15}** 参数化虽已通过 SOS 接收，但当前 `out_val > 255` 饱和截断会导致 Pt>0 的测试失败；25b 改用 16-bit 输出时解决。
