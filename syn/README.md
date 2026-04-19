# Synthesis — ASAP7 7.5T RVT TT @ 600 MHz

100 % open-source flow: **Yosys 0.63 + ABC** targeting the ASAP7 Predictive PDK, no commercial licenses required.

## Results at a glance

| | |
|---|---:|
| Target clock | 1 667 ps (600 MHz) |
| Critical path | **1 327.89 ps** · 48 levels |
| **WNS** | **+339.11 ps** (20.3 % margin) |
| **Fmax** | **≈ 753 MHz** |
| **Chip area** | **3 942.36 µm² = 45.1 k GE** (30 % of 150 k budget) |
| Sequential / combinational split | 766.67 / 3 175.68 µm² (19.4 % / 80.6 %) |
| `$mem_v2` macros | 20 (≈ 38 146 µm² SRAM estimate) |
| Flops | 2 361 (894 async-reset + 1 467 no-reset) |
| Latches / combinational loops | 0 / 0 |

Full breakdown: **[`../docs/reports/04_synthesis.md`](../docs/reports/04_synthesis.md)**.

## Layout

```
syn/
├── asap7/
│   ├── asap7_merged_RVT_TT.lib   # Liberty, TT 0.77 V 25 °C, from OpenROAD-flow-scripts
│   ├── fakeram/                  # SRAM macro library used by the area estimator
│   └── asap7_mem_area.py         # Tile-picker for $mem_v2 → SRAM macro cost
├── constraints/
│   └── jpeg_axi_top.sdc          # create_clock 1.667 ns, 0.5 ns I/O, aresetn false-path
├── scripts/
│   ├── syn_asap7.ys              # Flat synthesis — timing + final netlist
│   └── syn_asap7_hier.ys         # Hierarchical — per-module area spotting
└── reports/
    └── jpeg_axi_top_netlist.v    # Synthesized gate-level Verilog (4.4 MB)
```

## Running

```bash
cd syn/scripts
yosys syn_asap7.ys              # ~4 s on M-series Mac — full flow + final netlist
yosys syn_asap7_hier.ys         # per-module area (no flatten)

python3 ../asap7/asap7_mem_area.py < /dev/null    # SRAM macro cost summary
```

## Key flow decisions

1. **`memory -nomap`** keeps `reg [W-1:0] mem[0:D-1]` as `$mem_v2` so real SRAM macros can be swapped in at P&R. The external `asap7_mem_area.py` picks smallest-area fakeram tile per memory.
2. **Custom ABC script**
   ```
   +strash; map -D 1667; topo;
   buffer -N 16;
   upsize -D 1667; dnsize -D 1667;
   stime -p -c
   ```
   Default `abc -liberty` ran 107 min and still left high-fanout slew violations. This script runs in ~2 s and closes timing with 20 % margin. `buffer -N 16` fans out huge-cap nets to ≤ 16 loads each; `upsize`/`dnsize` rebalance drive strength around the target delay.
3. **`wreduce + opt_expr -mux_undef + opt_clean -purge`** pre-ABC eliminated 777 undriven-net warnings down to 2 benign ones (AXI-Lite `bresp` is constant `2'b00` per AXI4-Lite spec).
4. **`dfflibmap`** is called before ABC to map native Yosys `$dff`/`$adff` onto `DFFHQNx1` (no-reset, 0.29 µm²) or `DFFASRHQNx1` (async-reset, 0.38 µm²). The 2 361-flop total reflects 1 467 pipeline/data flops on the cheap cell.

## RTL changes driven by synthesis

Three RTL edits were required to close timing (detailed in [`../docs/reports/04_synthesis.md`](../docs/reports/04_synthesis.md) §2.3):

| File | Change | Impact |
|---|---|---|
| `rtl/src/huffman_decoder.v` | New `S_DC_ACC` / `S_AC_ACC` states + `amp_r` / `amp_size_r` flops | Breaks the `peek_win → variable-shift → sext → dc_pred+amp` chain into two clock cycles |
| `rtl/src/idct_1d.v` | 1-cycle combinational → **3-stage pipeline** (A: adds, B: multiplies, C: adds) | 54-level Loeffler-Lee split into 3 × ≤ 18-level segments |
| `rtl/src/idct_2d.v` | Added `wr_col_r1/r2` + `wr_en_*_r1/r2` tracking to line up the 2-cycle pipeline delay | Block latency stays at 84 cycles |

All three preserve RTL-level bit-exactness — smoke 12/12 and the 4K real image pass unchanged.
