# C Reference Model

Golden decoder used by the Verilator differential harness. Wraps libjpeg-turbo to emit bit-exact YCbCr planes for comparison against RTL output.

## Build

```bash
make            # → build/*.o + build/jpeg_cmodel
make test       # → runs test_vectors on verification/vectors/*
```

## Entry points

| File | Role |
|---|---|
| [`src/decoder.c`](src/decoder.c) · [`include/decoder.h`](include/decoder.h) | `jpeg_decode(bytes, len, &out)` — one-shot full decode returning `width`, `height`, `err`, and planar `y/cb/cr` upsampled to 4:4:4 |
| [`src/bitstream.c`](src/bitstream.c) | Byte-level reader with 0xFF-stuffing, marker detection, bit-packed access |
| [`src/header_parser.c`](src/header_parser.c) | Marker FSM: SOI → (APP/DQT/DHT/SOF0)* → SOS → scan → EOI |
| [`src/huffman.c`](src/huffman.c) | Canonical huffman table build + `hufDecode()` symbol lookup |
| [`src/dequant.c`](src/dequant.c) | Zigzag inverse + per-component quant multiply |
| [`src/idct.c`](src/idct.c) | JDCT_ISLOW integer IDCT, **bit-exact reference** for `rtl/src/idct_1d.v` |
| [`src/chroma.c`](src/chroma.c) | 4:2:0 → 4:4:4 nearest-neighbor upsample |

## Used by

- `verification/tests/sim_main.cpp::diff_one` — golden path for every RTL comparison.
- `tools/gen_vectors.py` — sanity-decodes every generated JPEG before committing it to the corpus.

## Why libjpeg-turbo

libjpeg-turbo's integer IDCT is bit-identical to `JDCT_ISLOW` — the same algorithm Verilog reproduces. This is what lets us assert **ΔY=0 ΔC=0** instead of a PSNR threshold. Any divergence means an RTL bug, not a rounding difference.

See [`docs/reports/01_c_model_test.md`](../docs/reports/01_c_model_test.md) for test coverage.
