# Verification

Verilator C++ differential harness + random JPEG vector corpus.

## Layout

```
verification/
├── tests/
│   ├── sim_main.cpp   # Entry: --mode=idle | csr | diff | diff-full | one | unit
│   ├── tb_common.h    # TbCtx: Vjpeg_axi_top + clock/reset/VCD
│   ├── bfm.h          # AxiLiteBfm (CSR transactions during simulation ticks)
│   └── Makefile       # verilator build + targets
├── cocotb/            # reserved for future module-level benches
└── vectors/
    ├── smoke/         # 12 hand-crafted, bit-exact vectors (16×16..64×64)
    └── full/          # 1 150 random vectors from tools/gen_vectors.py
```

## Test modes

| Make target | Mode | What it does |
|---|---|---|
| `make all` | — | Build `obj_dir/Vjpeg_axi_top` |
| `make run` | `--mode=idle` | Reset + CSR ID probe, proves the DUT clocks and AXI-Lite is alive |
| `make csr` | `--mode=csr` | Full CSR register RW sweep |
| `make diff` | `--mode=diff` on `vectors/smoke/` | 12 images, every pixel ΔY=0 ΔC=0 vs libjpeg |
| `make diff-full` | `--mode=diff` on `vectors/full/` | 1 150 random images |
| `make one` | `--mode=one` | Single-image decode with optional `--vcd=` and `--out=<path.ppm>` |
| `make unit` | `--mode=unit` | Reserved |
| `make clean` | — | remove obj_dir, *.vcd |

## Golden comparison

`diff_one()` in [`tests/sim_main.cpp`](tests/sim_main.cpp) runs both decoders on the same input and compares every pixel:

```c++
DiffResult r = diff_one(jpeg_bytes, verbose, max_cycles, vcd_path,
                        &rtl_Y, &rtl_Cb, &rtl_Cr);  // RTL planes filled if want_out
// r.max_diff_y == 0 && r.max_diff_c == 0  is the PASS condition
```

The golden planes are upsampled 4:4:4 (libjpeg default) to match the decoder's AXIS pixel output.

## Exporting RTL-decoded images (`--out=<path.ppm>`)

```bash
./obj_dir/Vjpeg_axi_top --mode=one \
    --dir=../../docs/images/sample_input_4k.jpg \
    --out=/tmp/rtl_4k.ppm
# → writes 24 MB P6 PPM, YCbCr→RGB via JFIF integer approximation
```

The PPM is pixel-exact to what libjpeg would produce (ΔY=0 ΔC=0); small per-pixel differences you might see against an ImageMagick decode come from rounding in the PPM consumer's YCbCr→RGB, not from the RTL.

## Test corpus generation

[`../tools/gen_vectors.py`](../tools/gen_vectors.py) produces the random 1 150-image set:

```bash
cd tools && python3 gen_vectors.py
# → verification/vectors/full/rnd_<seq>_<WxH>_q<quality>.jpg
# Each image is sanity-decoded with the C golden model before commit.
```

Patterns: solid / checker / gradient / stripe / noise / random. Dims: 16..128 on both axes (16-aligned, 4:2:0 MCU-compliant). Quality: q25..q100.

## Status

| Set | PASS | FAIL | Coverage |
|---|---:|---:|---:|
| Smoke | 12 | 0 | 12/12 (100 %) |
| Random full | 956 | 0 | 956/1 150 (83 %) — rest blocked by macOS `pthread_create` rate limit, not RTL |
| 4K real-world | 1 | 0 | — |

See [`../docs/reports/03_rtl_simulation.md`](../docs/reports/03_rtl_simulation.md) for the Phase 3 harness design and coverage rationale.
