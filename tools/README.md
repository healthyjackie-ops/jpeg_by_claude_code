# Tools

## `gen_vectors.py`

Generates the random JPEG corpus under [`../verification/vectors/full/`](../verification/vectors/full/).

```bash
python3 gen_vectors.py
# → 1 150 images: rnd_<seq>_<WxH>_q<quality>.jpg
```

Each image is:
- baseline sequential DCT (not progressive)
- YCbCr 4:2:0 sampling (`2x2,1x1,1x1`)
- 8-bit precision
- 16-aligned dimensions (MCU-compliant), 16..128 on each axis
- quality q25..q100
- Pillow-generated from patterns: solid, checker, gradient, stripe, noise, random pixels
- sanity-decoded with the C golden model before being written to disk

Requires:

```bash
pip3 install pillow
```
