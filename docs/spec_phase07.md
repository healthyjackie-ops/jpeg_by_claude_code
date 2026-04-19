# Phase 7 — DRI > 0 (Restart Markers)

**Wave**: 1 (JFIF-full)
**Depends on**: Phase 6
**Blocks**: Phase 11/12 的 DRI + 非 4:2:0 组合、任何期望 RSTn 的生产编码器输出

## 规格 delta

| 项 | v1.0 (旧) | Phase 7 (新) |
|---|---|---|
| `DRI` 段 | 必须 `Ri=0`，否则 `ERR_UNSUP_SOF` | **接受 `Ri != 0`**；restart 间隔记录到 `dri_interval` |
| 熵流中 `0xFFD0..0xFFD7` (RSTn) | 遇到即 `ERR_BAD_MARKER` | **吞掉**：MCU 计数归零，DC 预测器清零，继续解码 |
| DC 预测器跨 RSTn | 连续（也不会出现 RSTn） | **每个 restart interval 开头清零**（3 个通道） |
| MCU 尾部 pad 位 | 无 | 进入 S_WAIT_RST 时丢弃 shreg 残留 bit（JPEG spec: entropy segment 以 1-bit 填到 byte 边界） |
| 最后一个 MCU 后 | EOI | 与旧相同；最后一个 MCU 不触发 RST 查找 |

## 算法对齐 (C 模型)

[c_model/src/decoder.c:123-159](../c_model/src/decoder.c#L123-L159)

```c
if (info.dri != 0) {
    restart_cnt++;
    int is_last = (my == info.mcu_rows - 1) && (mx == info.mcu_cols - 1);
    if (restart_cnt == info.dri && !is_last) {
        bs_align_to_byte(&bs);
        /* 读 0xFF RSTn（或检查 marker_pending） */
        if (b < MARKER_RST0 || b > MARKER_RST7) ERR_BAD_MARKER;
        /* 重置 3 个 DC 预测器 */
        info.components[0..2].dc_pred = 0;
        restart_cnt = 0;
    }
}
```

## RTL 改动

| 文件 | 改动 |
|---|---|
| [`rtl/src/header_parser.v`](../rtl/src/header_parser.v) | `parse_dri` 接受 `Ri != 0`，输出 `dri_interval`；**修复 DHT 多表解析时 `build_start` 被吞的 race（新增 `S_DHT_WAIT_BLD` 等 `build_done`）** |
| [`rtl/src/bitstream_unpack.v`](../rtl/src/bitstream_unpack.v) | 新增 `restart_ack` (清 shreg/bit_cnt/marker 状态) 与 `align_req` (仅清 shreg/bit_cnt)；`can_accept` 放宽 `ff_wait` 下必接纳（避免 MCU 尾判定 marker 卡死） |
| [`rtl/src/block_sequencer.v`](../rtl/src/block_sequencer.v) | 新增 `dri_interval` 输入、`restart_cnt` 计数、`S_WAIT_RST` 状态、`next_st_after_rst` 锚点；DRI 边界 pulse `align_req`，在 `marker_detected && marker_byte[7:3]==5'b11010` 时 pulse `restart_ack` + `dc_restart` |
| [`rtl/src/jpeg_axi_top.v`](../rtl/src/jpeg_axi_top.v) | 连 `dri_interval_w / restart_ack_w / dc_restart_w / align_req_w`；`dcp_new_frame = start_pulse \| dc_restart_w` |

**不变**：`huffman_decoder`、`dequant_izz`、`idct_2d`、`mcu_buffer/copy`、`line_buffer`、`pixel_out`。

## 关键时序

1. **MCU 完毕，restart_cnt+1 == dri_interval** 且非最后 MCU：  
   block_sequencer → `align_req` → bitstream_unpack 清 shreg/bit_cnt → 进 `S_WAIT_RST`。
2. **FIFO 继续送字节**，bitstream_unpack 识别到 `0xFFnn`（`marker_detected=1, marker_byte=nn`）。
3. block_sequencer 检查 `marker_byte[7:3] == 5'b11010`（即 0xD0..0xD7）：
   - 是 RSTn → pulse `restart_ack` (清 marker 状态)、`dc_restart` (清 DC 预测器)，回到 `S_START_BLK` 或 `S_ROW_OUT`。
   - 不是 → `frame_done_o`，状态机走 `S_DONE`。
4. `dc_predictor` 在 `new_frame = start_pulse | dc_restart` 时把 3 路预测值清零。

## 发现 & 修复（意外 bug）

解决 Phase 7 主路径时，定位到一个 v1.0 就存在的 **race condition**：  
`header_parser` 对每个 DHT 段只发单 cycle `ht_build_start` pulse，但 `htable_ram` 的 build FSM 需 ~18 cycles，期间 `B_IDLE` 以外状态忽略新 pulse。Phase 06 及更早的测试向量中，AC 表 huffvals 多（~162 字节），parse 时间够长，build 在下一 `build_start` 到来前已结束，所以没被发现。Phase 07 的 `p07_grad/noise_*` 测试里 AC 表极小（5 huffvals），parse 时间短于 build 时间 → AC_0/AC_1 的 mincode/maxcode/valptr 从未生成，huffman_decoder 扫到 l=16 无匹配 → `ERR_BAD_HUFFMAN`。

修复：`header_parser` 新增 `S_DHT_WAIT_BLD` 状态，发完 `build_start` 后等 `htable_ram.build_done` 再解析下一个 DHT 表。

## 验收

1. **Phase 6 回归**：20/20 bit-exact 通过（未引入 regression）。
2. **Smoke 回归**：12/12 bit-exact 通过。
3. **Phase 7 新向量**：[verification/vectors/phase07/](../verification/vectors/phase07/) 20 张 JPEG，`-restart NB` 覆盖 DRI ∈ {1, 2, 3, 4, 5, 6, 7, 8, 10, 16, 32, 128}，W/H 从 32×32 到 640×480，4:2:0。**20/20 bit-exact** vs libjpeg。
4. **C 模型**：[c_model/src/decoder.c](../c_model/src/decoder.c) 与 libjpeg 20/20 bit-exact。

## 已知限制

- Phase 7 仅涵盖 4:2:0 + DRI 组合。非 4:2:0 + DRI 留到 Phase 11/12。
- 非法 marker（`0xFF xx` 且 xx 不在 0xD0..0xD7）会 `frame_done_o` 而不是专用 `ERR_BAD_RST` 错误码 — 后续 Phase 如需要再细化。

## 交付

- RTL：`header_parser.v`, `bitstream_unpack.v`, `block_sequencer.v`, `jpeg_axi_top.v`
- C 模型：[`decoder.c`](../c_model/src/decoder.c), [`header_parser.c`](../c_model/src/header_parser.c)
- 测试：[`tools/gen_phase07.py`](../tools/gen_phase07.py), 20 张 Phase 7 向量
- 文档：本文件
