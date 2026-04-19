# Documentation

## Core design docs

| File | Contents |
|---|---|
| [`spec.md`](spec.md) | Top-level specification — supported/unsupported JPEG subset, interfaces, limits |
| [`plan.md`](plan.md) | Phased project plan (0..6) with deliverables per phase |
| [`uarch.md`](uarch.md) | Microarchitecture — block diagram, per-module datapath, FSMs, pipeline latencies |
| [`regmap.md`](regmap.md) | AXI4-Lite CSR register map (ID, CTRL, STATUS, ERROR, IMG_WIDTH, IMG_HEIGHT, …) |

## Phase reports

Each phase has a dated, self-contained report.

| # | File | Summary |
|---|---|---|
| 00 | [`reports/00_spec_review.md`](reports/00_spec_review.md) | Scope freeze, subset review |
| 01 | [`reports/01_c_model_test.md`](reports/01_c_model_test.md) | C golden model built on libjpeg-turbo |
| 02 | [`reports/02_rtl_design.md`](reports/02_rtl_design.md) | 18-module RTL implementation |
| 03 | [`reports/03_rtl_simulation.md`](reports/03_rtl_simulation.md) | Verilator diff harness + random corpus |
| 04 | [`reports/04_synthesis.md`](reports/04_synthesis.md) | **ASAP7 synthesis, WNS +339 ps @ 600 MHz** |

## Media

`images/` holds README hero assets:

| File | What |
|---|---|
| `sample_input_4k.jpg` | Baseline 4:2:0 JPEG, 3840×2160, the input to the 4K demo |
| `rtl_decoded_4k.png` | Full-resolution PNG of what the RTL decoder produced (6.1 MB) |
| `rtl_decoded_4k_thumb.jpg` | 1280×720 JPEG thumbnail for the top-level README |
