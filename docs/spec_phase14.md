# Phase 14 — SOF2 (progressive) header recognition + clean error-out

## Goal
为 Wave 3 (Progressive) 铺路：让 header_parser **显式识别** SOF2 (0xFFC2)
marker，并在 C 模型与 RTL 两侧**干净 error-out**（不 hang、不返回垃圾像素，
`ERR_UNSUP_SOF` 正确置位），同时保持 Phase 06..13 共 162/162 零回归。

**不** 实现任何 progressive 解码逻辑 — 那是 Phase 15–19 的事。本 phase 仅是
一块 2 天的"握手位"，保证以后逐步开启 SOF2 的每一步（coef buffer / DC-only /
AC spectral / refinement）都不会被 marker 识别问题噎住。

## Format 参考 (ISO/IEC 10918-1)

SOF2 frame header 结构与 SOF0 完全一致：
```
FFC2 Lf P Y X Nf (Ci Hi|Vi Tqi)×Nf
```
- Lf: 2B length
- P: 1B precision，8 或 12
- Y: 2B height
- X: 2B width
- Nf: 1B number of components (1..4)
- Ci/Hi|Vi/Tqi: 每 component 3B

但后续 SOS 段会给出 `Ss`/`Se`/`Ah`/`Al`，且多个 SOS 段组成 progressive scan 序列 —
这些在 Phase 15+ 才处理。

## 行为规格

### C model (已实现，Phase 14 仅补充测试)
`c_model/src/header_parser.c` 中 `jpeg_parse_headers` 的 default 分支已经包含：
```c
} else if ((marker >= MARKER_SOF2 && marker <= MARKER_SOF3) ||
           (marker >= MARKER_SOF5 && marker <= MARKER_SOF15)) {
    *err |= JPEG_ERR_UNSUP_SOF;
    return -1;
}
```
即：遇到 0xC2/0xC3/0xC5..0xCF 立即置 `JPEG_ERR_UNSUP_SOF` 并返回 `-1`。
Phase 14 只在 `decoder_test` 语料层增加 SOF2 样本验证该路径被命中。

### RTL (`rtl/src/header_parser.v`)
当前 `S_DISPATCH` 的 default 分支通过掩码 `(last_marker & 8'hF0) == 8'hC0`
已经能捕获 SOF2/3/5..15 并置 `err[ERR_UNSUP_SOF]`。为了**把"识别"这件事
显式留在 case 里**（便于 Phase 15 原地改成"parse SOF2 header"），做两件事：

1. `rtl/include/jpeg_defs.vh` 新增：
```verilog
`define MARKER_SOF2   8'hC2   // Phase 14: progressive DCT (recognized, error-out)
`define MARKER_SOF3   8'hC3   // reserved for Wave 5 lossless
```

2. `header_parser.v::S_DISPATCH` case 里为 SOF2 开一条显式分支：
```verilog
`MARKER_SOF2:  begin
    err[`ERR_UNSUP_SOF] <= 1'b1;
    state <= S_ERROR;
end
```

**功能上**与走 default 完全一致（都是单 cycle 从 marker → S_ERROR，不读 length
不读 payload），但语义上为 Phase 15 预留：届时把这个分支从 "err+ERROR"
改成 "state ← S_LEN_HI, flag ← progressive"。

### PIX_FMT 寄存器（0x02C，Phase 13）保持不变
Phase 14 不新增寄存器位 —— SOF2 被立即 error-out，`precision_o` 尚未锁存到
有意义的值；软件通过 `ERROR_CODE.ERR_UNSUP_SOF` 已足够区分。

Phase 15 打开 SOF2 真正解码时，会在 `PIX_FMT` 新增 bit[1] = `PROGRESSIVE`。

## 测试向量 `verification/vectors/phase14/`

`tools/gen_phase14.py`，用 `cjpeg -progressive` 生成 SOF2 marker 的 JPEG：

| # | 尺寸 | 采样 | Quality | Restart | 备注 |
|---|---|---|---:|---:|---|
| 1 | 8×8     | gray  | 75 | 0 | 最小 SOF2 grayscale |
| 2 | 16×16   | 4:2:0 | 80 | 0 | MCU 边界对齐 |
| 3 | 17×13   | 4:4:4 | 75 | 0 | 非 8 对齐 |
| 4 | 32×32   | 4:2:0 | 85 | 0 | 标准 |
| 5 | 64×64   | 4:2:0 | 70 | 4 | 含 DRI |
| 6 | 128×64  | 4:2:0 | 60 | 0 | 长宽比 |
| 7 | 128×128 | 4:4:4 | 90 | 0 | 高 Q |
| 8 | 199×257 | 4:2:0 | 55 | 8 | 非对齐 + DRI |

每张均校验文件头包含 `FFC2`（工具内置 `verify_sof2()`）。

## 验证

### C 侧 — `build/phase14_check`
新增一个很小的 C 测试程序（或复用 `decoder_test --expect-err`）：
- 读入 phase14 语料
- 调 `jpeg_decode_file()`
- 断言：返回值非 0 且 `info.err & JPEG_ERR_UNSUP_SOF ≠ 0`
- 断言：未产生像素（或产生 0 像素）

最简实现：在 `c_model/test/decoder_test.c` 里对 `phase14/*.jpg` 调用现有 API，
判定 `err` 位图符合期望（期望 = `JPEG_ERR_UNSUP_SOF`）。

### RTL 侧 — `sim_main.cpp --mode=errout`
新增运行模式：
1. 复位 → SOFT_RESET → START
2. 按 AXI-Stream 喂 JPEG 字节
3. 轮询 `STATUS`，等待 `ERROR` bit 置 1（或超时 = 1M cycles）
4. 读 `ERROR_CODE`，断言 `bit[0] = 1` (ERR_UNSUP_SOF)
5. 断言：无任何 `m_px_tvalid && m_px_tready` 发生（像素侧静默）
6. 断言：未 hang（无超时）

预期结果：8/8 vectors 每张都触发 "STATUS.ERROR=1, ERR_UNSUP_SOF=1,
pixel_count=0"。

### 回归
- phase06..phase13 162/162 保持 bit-exact（SOF0/SOF1 路径不变）
- smoke 12/12 不变

## 交付物
1. `docs/spec_phase14.md`（本文件）
2. `rtl/include/jpeg_defs.vh` ＋MARKER_SOF2/SOF3 定义
3. `rtl/src/header_parser.v` ＋显式 SOF2 case
4. `tools/gen_phase14.py`
5. `verification/vectors/phase14/` ≥ 8 张 SOF2 JPEG
6. `verification/tests/sim_main.cpp` ＋`--mode=errout`
7. `verification/tests/Makefile` ＋`errout` target
8. commit + push

## 验收 checklist
- [ ] `make -C verification/tests errout DIFF_DIR=.../phase14` → 8/8 pass
- [ ] `make -C verification/tests diff DIFF_DIR=.../phase13` → 20/20（无回归）
- [ ] 所有 P=8 历史语料保持 142/142
- [ ] `ERROR_CODE.ERR_UNSUP_SOF` 在 phase14 每张后 sticky = 1
- [ ] `pixel_count` 在 phase14 每张后 = 0
- [ ] 无 timeout / hang

## 风险
1. **`cjpeg -progressive` 的实现差异**：OpenJPEG / jpeg-turbo 的 progressive
   scan 序列顺序可能不同。Phase 14 只验证 marker 识别，所以任何符合 ISO 的
   SOF2 样本都够用。
2. **libjpeg 解码 SOF2 到 16b YCbCr 的输出差异**：Phase 14 不解码，与此无关。
3. **error-out 后 FIFO 里残留字节**：SOFT_RESET 负责清空；sim_main `--mode=errout`
   会在每张 vector 之间显式发 SOFT_RESET。

---
**上游依赖**：Wave 2 完成（Phase 13c）—— ✅
**下游开启**：Phase 15 (Coef frame buffer) 可直接在 `MARKER_SOF2` 分支往 `S_LEN_HI` 迁移。
