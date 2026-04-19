# RTL — JPEG Baseline Decoder

Verilog-2001, 18 synthesizable modules, top = `jpeg_axi_top`.

## Files

| File | Role | Flops | Notes |
|---|---|---:|---|
| [`src/jpeg_axi_top.v`](src/jpeg_axi_top.v) | Top, AXI wrappers, reset/clock stitching | 43 | AXI-Lite + 2 × AXI-Stream |
| [`src/axi_lite_slave.v`](src/axi_lite_slave.v) | CSR slave | 116 | 16 registers, see `docs/regmap.md` |
| [`src/axi_stream_fifo.v`](src/axi_stream_fifo.v) | 32-entry FIFO, byte + pixel flavor | — | `$mem_v2` split from reset block |
| [`src/bitstream_unpack.v`](src/bitstream_unpack.v) | Byte → bit stream, 0xFF stuffing | 48 | 32-bit shift register with `peek_win[15:0]` |
| [`src/header_parser.v`](src/header_parser.v) | Marker FSM, DQT/DHT/SOF0/SOS parse | 177 | drives DC/AC huffman, DQT writes |
| [`src/htable_ram.v`](src/htable_ram.v) | 4 × Huffman table (BITS/mincode/maxcode/valptr/huffval) | — | 5 × `$mem_v2` |
| [`src/huffman_decoder.v`](src/huffman_decoder.v) | DC/AC symbol decode + run-length expand | 120 | Phase 4: S_DC_ACC / S_AC_ACC added to break `peek_win → amp → sext → +dc_pred` chain |
| [`src/qtable_ram.v`](src/qtable_ram.v) | 4 × 64-byte quant tables | — | 1 × `$mem_v2` |
| [`src/dc_predictor.v`](src/dc_predictor.v) | Per-component DC history | 48 | |
| [`src/dequant_izz.v`](src/dequant_izz.v) | Dequant × zigzag inverse | 42 | 1 × `$mem_v2` coef buffer |
| [`src/idct_1d.v`](src/idct_1d.v) | JDCT_ISLOW 1-D IDCT, **3-stage pipeline** | 896 | A: input adds · B: 12 multiplies · C: final adds. Bit-exact to C int32 math. |
| [`src/idct_2d.v`](src/idct_2d.v) | Pass1 (columns) → transpose → Pass2 (rows) | ~200 | 2 × `idct_1d`; 64 fill + 10 pass + 10 pass = 84 cyc / block |
| [`src/block_sequencer.v`](src/block_sequencer.v) | MCU raster scheduling (4 Y + 1 Cb + 1 Cr) | 80 | |
| [`src/mcu_buffer.v`](src/mcu_buffer.v) | 16×16 Y + 8×8 Cb + 8×8 Cr staging | — | 3 × `$mem_v2` |
| [`src/mcu_line_copy.v`](src/mcu_line_copy.v) | MCU-slot → line-buffer row index translation | 82 | |
| [`src/line_buffer.v`](src/line_buffer.v) | `MAX_W` line buffer (up to 4096) | — | 3 × `$mem_v2` |
| [`src/chroma_upsample.v`](src/chroma_upsample.v) | 4:2:0 → 4:4:4 nearest-neighbor | — | pipelined read from line_buffer |
| [`src/pixel_out.v`](src/pixel_out.v) | AXI-Stream pixel master | 49 | `tuser` = first-pixel, `tlast` = end-of-row |

## Include

[`include/jpeg_defs.vh`](include/jpeg_defs.vh) — marker constants, `jpeg_err_t` codes, fixed-point IDCT coefficients (`FIX_0_…`), `CONST_BITS`, `PASS1_BITS`.

## Conventions

- Verilog-2001, no SystemVerilog.
- Reset: single async-low `aresetn`; most pipeline-internal flops are no-reset (`DFFHQNx1` on ASAP7).
- Clock: single `aclk`, 600 MHz target.
- Memory style: `reg [W-1:0] mem [0:D-1]` + same-clock synchronous write — Yosys keeps these as `$mem_v2` until P&R where they're swapped for SRAM macros. The sole FIFO that previously mixed async reset with memory writes (`axi_stream_fifo.v`) was split for synthesis compatibility.

## Linting

Verilator `-Wall -Wno-PINCONNECTEMPTY -Wno-fatal` is clean on all modules; Yosys `hierarchy -check` passes without unused or multiply-driven warnings after the post-Phase-4 cleanup. See [`docs/reports/02_rtl_design.md`](../docs/reports/02_rtl_design.md) for module-by-module rationale.
