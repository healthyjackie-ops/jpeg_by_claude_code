# Phase 1 — C Model Test Report

**Phase**: 1 (C Reference Implementation)  
**Date**: 2026-04-18  
**Status**: **PASS** — Ready for Phase 2 (RTL Design)

---

## 1. 交付物

| 路径 | 行数 | 说明 |
|---|---|---|
| [c_model/src/jpeg_types.h](../../c_model/src/jpeg_types.h) | 103 | 数据结构、常量、错误码 |
| [c_model/src/bitstream.c/h](../../c_model/src/bitstream.c) | 100 + 26 | 位流读取、byte-stuffing 去除、符号扩展 |
| [c_model/src/header_parser.c/h](../../c_model/src/header_parser.c) | 266 + 11 | SOI/DQT/DHT/SOF0/SOS/EOI marker 解析 |
| [c_model/src/huffman.c/h](../../c_model/src/huffman.c) | 77 + 15 | 变长码解码 + RLE + zigzag 回填 |
| [c_model/src/dequant.c/h](../../c_model/src/dequant.c) | 7 + 8 | 反量化（natural order） |
| [c_model/src/idct.c/h](../../c_model/src/idct.c) | 162 + 8 | **JDCT_ISLOW bit-exact IDCT** |
| [c_model/src/chroma.c/h](../../c_model/src/chroma.c) | 14 + 9 | 4:2:0 → 4:4:4 nearest-neighbor upsample |
| [c_model/src/decoder.c/h](../../c_model/src/decoder.c) | 143 + 21 | 顶层解码管线 |
| [c_model/src/main.c](../../c_model/src/main.c) | 78 | CLI：JPEG → PPM / YUV420 |
| [c_model/tests/*.c](../../c_model/tests/) | 251 | 单元测试（bitstream / huffman / idct / errors） |
| [c_model/golden/golden_compare.c](../../c_model/golden/golden_compare.c) | 162 | libjpeg 对拍工具 |
| [c_model/Makefile](../../c_model/Makefile) | 41 | 构建系统 |
| | **1461** | |

---

## 2. 构建

```bash
cd c_model && make all
# 产出:
#   build/jpeg_decode     — CLI 解码器
#   build/golden_compare  — libjpeg 对拍
#   build/test_*          — 单元测试
```

**编译器警告等级**：`-O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror -std=c11`
**结果**：✅ 0 warning, 0 error

---

## 3. 关键技术决策

### 3.1 IDCT 算法选择
采用 **`JDCT_ISLOW`**（libjpeg 整数慢速 IDCT）算法，constants & 精度 bit-exact 对齐 libjpeg：
- `CONST_BITS = 13` / `PASS1_BITS = 2`
- 12 个固定点乘法常数（LL&M + AA&N 混合）
- Pass 1：**列方向**（与 libjpeg jidctint.c 一致）
- Pass 2：**行方向**
- Round-to-nearest-half-up via `DESCALE(x, n) = (x + (1<<(n-1))) >> n`

**教训（调试中发现）**：Pass 1/2 顺序影响 fixed-point rounding，必须与 libjpeg 匹配（列先 / 行后），否则有 ±1 LSB 差异。

### 3.2 Zigzag / Dequant 顺序
**Bug 修复（Phase 1 调试中发现）**：DQT 段存储 quant table 为 zigzag 顺序。实现时必须在加载时 **重排为 natural order**，使 dequant 在 natural order 做乘法与 coef 布局一致。此 bug 初版产生 maxDiff=6，修复后降为 ±1 LSB。

### 3.3 Chroma Upsampling
采用 **nearest-neighbor (box filter)**，对应 libjpeg `do_fancy_upsampling=FALSE`。
- RTL 友好（无滤波器，仅地址复制）
- 与 libjpeg 默认解码（raw_data_out + JDCT_ISLOW）完全一致

---

## 4. 测试结果

### 4.1 Golden 对拍（libjpeg JDCT_ISLOW + raw_data_out）

| 测试集 | 数量 | 通过 | 失败 | Y maxDiff | C maxDiff |
|---|---|---|---|---|---|
| 结构化（gradient / stripe / check / noise） | 144 | 144 | 0 | **0** | **0** |
| 边界用例（min 16×16, 4K 3840×2160, 矩形, 黑白, salt） | 36 | 36 | 0 | **0** | **0** |
| 极端质量（Q5 / Q10 / Q25 / Q100）+ 自然图像 | 30 | 30 | 0 | **0** | **0** |
| 随机 JPEG（4 种内容类型 × 8 种 Q 值） | 1000 | 1000 | 0 | **0** | **0** |
| **合计** | **1210** | **1210 (100%)** | **0** | **0** | **0** |

**结论**：C 模型与 libjpeg `JDCT_ISLOW + do_fancy_upsampling=FALSE` **逐像素 bit-exact 一致**。

### 4.2 单元测试

| 测试 | 用例数 | 覆盖 |
|---|---|---|
| `test_bitstream` | 5 | 基础读取、byte-stuffing、marker detect、变长位读、符号扩展 |
| `test_huffman` | 2 | 符号解码、DC=0 + AC EOB 的全零块 |
| `test_idct` | 5 | 全零、DC-only、负 DC、AC 混合、饱和（+/−） |
| `test_errors` | 3 | 空输入、错 magic、截断流 |
| **合计** | **15** | 全部 **PASS** |

### 4.3 性能（参考数据）

| 指标 | 数值 |
|---|---|
| 4K 图像（3840×2160）CPU 解码时间 | 0.17 s |
| 吞吐（单核 CPU） | ~49 Mpix/s |
| 二进制大小（stripped） | ~30 KB |

**注**：C 模型主要用于 golden 参考，不追求性能。目标 ASIC 吞吐 600+ Mpix/s（RTL 实现）。

---

## 5. 覆盖的 JPEG 特性

| 特性 | 支持 | 测试 |
|---|---|---|
| SOF0 Baseline 8-bit | ✅ | 所有用例 |
| YUV 4:2:0 sampling | ✅ | 所有用例 |
| 可编程 Q table（DQT 0-3） | ✅ | Q=5~100 |
| 可编程 Huffman 表（DHT） | ✅ | libjpeg 默认表 + 各质量对应的自适应表 |
| APPn / COM 段跳过 | ✅ | PIL 默认带 APP0 (JFIF) |
| DC 差分预测（Y/Cb/Cr 独立） | ✅ | 所有多 MCU 图 |
| EOB / ZRL (run=16) | ✅ | 低质量（Q=5）高压缩场景 |
| Byte-stuffing (0xFF00 → 0xFF) | ✅ | 随机码流中频繁出现 |
| 4K 最大分辨率 | ✅ | 3840×2160 q80 用例 |
| 非正方形尺寸（16 倍数） | ✅ | 32×96, 128×64, 256×128, 1920×1088 |

---

## 6. 故意不支持（按 spec 设计）

| 非法输入 | 期望错误码 | 实测 |
|---|---|---|
| 非 JPEG magic（0xFF 0xD8） | `ERR_BAD_MARKER` | ✅ `0x10` |
| 截断码流（EOI 前结束） | `ERR_STREAM_TRUNC` | ✅ `0x80` |
| 空码流（仅 SOI） | `ERR_STREAM_TRUNC` | ✅ `0x80` |
| Progressive / Extended（SOF≠SOF0） | `ERR_UNSUP_SOF` | 代码路径已实现，测试 TODO |
| 非 YUV420 sampling | `ERR_UNSUP_CHROMA` | 代码路径已实现，测试 TODO |
| 非 8-bit precision | `ERR_UNSUP_PREC` | 代码路径已实现，测试 TODO |
| DRI ≠ 0 | `ERR_DRI_NONZERO` | 代码路径已实现，测试 TODO |
| W/H 非 16 倍数或超 4K | `ERR_SIZE_OOR` | 代码路径已实现，测试 TODO |

**备注**：上述 "TODO" 项 Phase 3（RTL 验证）时补充专门 TB 用例，这里 C 模型代码路径已就绪。

---

## 7. 已知限制

1. C 模型按 **功能正确性** 编写，未按 RTL 流水线模型实现（这是合理的 — 作为 golden reference，位精确比周期精确重要）
2. Huffman 解码使用标准 A.6.2 算法（逐位比较），RTL 将用两级 LUT 优化
3. 乘法/加法直接用 C 算符，RTL 将用移位-加法分解 — **需保证位运算顺序产生相同结果**
4. Chroma upsample 代码简洁，RTL 实现时需注意 line buffer 读写顺序

---

## 8. RTL 对拍策略（为 Phase 2/3 预留）

C 模型每个函数对应 RTL 一个模块，预计对拍点：

| C 函数 | RTL 模块 | 对拍数据 |
|---|---|---|
| `bs_get_bits` | `bitstream_unpack.v` | 消耗 bits / buffer 内容 |
| `huff_decode_symbol` | `huffman_decoder.v` | (symbol, length) 流 |
| `huff_decode_block` | `huffman_decoder.v` + `dc_predictor.v` | 64×int16 natural-order coef |
| `dequant_block` | `dequant_izz.v` | 反量化后 64×int16 |
| `idct_islow` | `idct_2d.v` | 8×8 uint8 像素块 |
| `chroma_upsample_nn` | `chroma_upsample.v` | 全帧 Cb/Cr 4:4:4 平面 |
| `jpeg_decode` | 顶层 | 最终 Y/Cb/Cr planes |

RTL 仿真 TB 将通过 DPI 或文件 I/O 在每个对拍点与 C 模型比对。

---

## 9. Phase 1 Sign-off

- [x] C 模型所有模块实现
- [x] `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror` 零警告编译
- [x] 1210 JPEG 100% 与 libjpeg bit-exact 一致
- [x] 单元测试 15/15 通过
- [x] 错误路径基本测试通过
- [x] 支持 4K 最大分辨率
- [x] 代码行数合理（~1461 行，含测试）

---

## 10. 下阶段（Phase 2）预告

**内容**：Verilog-2001 RTL 实现，对应 C 模型每个模块  
**模块优先级**：
1. `bitstream_unpack.v` + `header_parser.v`（控制路径，FSM 为主）
2. `huffman_decoder.v`（核心难点，两级 LUT）
3. `dequant_izz.v` + `idct_2d.v`（算术路径，时序关键）
4. `mcu_buffer.v` + `chroma_upsample.v`（缓存 + 简单逻辑）
5. `jpeg_axi_top.v`（AXI4-Lite + AXI4-Stream 封装）

**通过标准**：
- Verilator `--lint-only -Wall` 零警告
- 每模块独立 TB 跑通
- Pre-synthesis 用 Yosys `stat` 粗估面积

**预计工期**：5 周
