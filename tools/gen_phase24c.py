#!/usr/bin/env python3
"""
Phase 24c test vectors — SOF9/SOF10 extended chroma modes (4:2:2 / 4:4:0 / 4:1:1).

Phases 22c/23b (SOF9 sequential arith) and 24a (SOF10 progressive arith) only
covered gray / 4:4:4 / 4:2:0. This phase folds in the three remaining 3-comp
YCbCr layouts — MCU-footprint variants that the baseline Huffman path has
supported since Phase 10/11. cjpeg supports all of them with both
``-arithmetic`` and ``-progressive -arithmetic``.

Sampling factors:
    4:2:2 → Y 2x1, chroma 1x1 — MCU 16×8  (2 Y blocks horizontally + Cb + Cr)
    4:4:0 → Y 1x2, chroma 1x1 — MCU 8×16  (2 Y blocks vertically   + Cb + Cr)
    4:1:1 → Y 4x1, chroma 1x1 — MCU 32×8  (4 Y blocks horizontally + Cb + Cr)

Each vector is round-tripped through djpeg to confirm libjpeg-turbo accepts
it — that's the parity target for the C-model golden_compare.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "verification" / "vectors" / "phase24c"


def gradient_rgb(w: int, h: int) -> np.ndarray:
    x = np.linspace(0, 255, w, dtype=np.float32)[None, :]
    y = np.linspace(0, 255, h, dtype=np.float32)[:, None]
    r = np.clip(x, 0, 255).astype(np.uint8)
    g = np.clip(y, 0, 255).astype(np.uint8)
    b = np.clip(255 - (x + y) * 0.5, 0, 255).astype(np.uint8)
    r = np.broadcast_to(r, (h, w))
    g = np.broadcast_to(g, (h, w))
    b = np.broadcast_to(b, (h, w))
    return np.stack([r, g, b], axis=-1).astype(np.uint8)


def check_rgb(w: int, h: int) -> np.ndarray:
    checker = (np.add.outer(np.arange(h) // 8, np.arange(w) // 8) % 2) == 0
    col0 = np.array([220, 64, 32], dtype=np.uint8)
    col1 = np.array([32, 128, 240], dtype=np.uint8)
    return np.where(checker[..., None], col0, col1).astype(np.uint8)


def noise_rgb(w: int, h: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    return rng.integers(0, 256, size=(h, w, 3), dtype=np.uint8)


def cjpeg_encode(arr: np.ndarray, *, sampling: str, quality: int,
                 restart: int, progressive: bool, out_path: Path) -> None:
    h, w, _ = arr.shape
    header = b"P6\n%d %d\n255\n" % (w, h)
    data = header + arr.tobytes()
    args = ["cjpeg", "-arithmetic", "-quality", str(quality)]
    if progressive:
        args.append("-progressive")
    args += ["-sample", sampling]
    if restart > 0:
        args += ["-restart", f"{restart}B"]
    args += ["-outfile", str(out_path)]
    out_path.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(args, input=data, check=True)


def verify_djpeg_ok(path: Path) -> None:
    res = subprocess.run(
        ["djpeg", "-outfile", "/dev/null", str(path)],
        capture_output=True,
    )
    if res.returncode != 0:
        raise RuntimeError(f"djpeg rejected {path}: {res.stderr!r}")


def main() -> int:
    # (mode, sampling, pattern, w, h, quality, restart, progressive)
    # Each mode gets identical shape sweep; progressive=False → SOF9, True → SOF10
    chroma_modes = [
        ("422", "2x1,1x1,1x1"),
        ("440", "1x2,1x1,1x1"),
        ("411", "4x1,1x1,1x1"),
    ]
    # Sizes mix MCU-aligned and misaligned for each mode. 4:4:0 needs H ≥ 16,
    # 4:1:1 needs W ≥ 32 for a first-MCU footprint.
    size_cases = [
        # (w, h, q, r)       — small MCU-aligned + misaligned
        (32,   16,  75, 0),
        (64,   32,  80, 0),
        (100,  75,  60, 0),
        (17,   13,  80, 0),     # non-MCU-aligned both axes
        (128,  96,  70, 4),     # DRI=4 MCUs
        (200, 150,  30, 0),     # low quality → many-EOB
        (160, 120,  85, 8),     # DRI=8
        (256, 256,  90, 0),     # larger, high quality
        (320, 200,  75, 1),     # DRI=1 per MCU
    ]

    cases: list[tuple[str, str, str, int, int, int, int, bool]] = []
    for mode, samp in chroma_modes:
        for (w, h, q, r) in size_cases:
            # Adjust minimum dims per mode so we always have at least 1 full MCU.
            if mode == "440" and h < 16:
                h = 16
            if mode == "411" and w < 32:
                w = 32
            for pat in ("grad", "check", "noise"):
                for progressive in (False, True):
                    cases.append((mode, samp, pat, w, h, q, r, progressive))

    for mode, samp, pat, w, h, q, r, progressive in cases:
        if pat == "grad":
            img = gradient_rgb(w, h)
        elif pat == "check":
            img = check_rgb(w, h)
        else:
            img = noise_rgb(w, h, 42)

        sof = "sof10" if progressive else "sof9"
        name = f"p24c_{sof}_{mode}_{pat}_{w}x{h}_q{q}_r{r}.jpg"
        p = OUT / name
        cjpeg_encode(img, sampling=samp, quality=q, restart=r,
                     progressive=progressive, out_path=p)
        verify_djpeg_ok(p)

    print(f"generated {len(cases)} Phase 24c vectors under {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
