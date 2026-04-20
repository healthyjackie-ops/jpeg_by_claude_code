#!/usr/bin/env python3
"""
Phase 9 test vectors — 4:4:4 YCbCr (Nf=3, H=V=1 all components, MCU=8x8).

Uses libjpeg-turbo `cjpeg -sample 1x1,1x1,1x1` to emit 4:4:4 baseline JPEGs.
Input is PPM (P6, 24-bit RGB) — libjpeg converts RGB→YCbCr internally.
We mix:
  - 8-aligned dims (8x8, 16x16, 64x32)
  - non-aligned dims (17x13, 100x75, 321x241)
  - DRI>0 variants to stress 4:4:4 + restart combination
  - several quality levels + content patterns
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "verification" / "vectors" / "phase09"


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
    out = np.where(checker[..., None], col0, col1).astype(np.uint8)
    return out


def noise_rgb(w: int, h: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    return rng.integers(0, 256, size=(h, w, 3), dtype=np.uint8)


def cjpeg_encode(arr: np.ndarray, quality: int, restart: int,
                 out_path: Path) -> None:
    """Pipe a PPM (P6 RGB) through cjpeg -sample 1x1,1x1,1x1 to produce 4:4:4."""
    h, w, _ = arr.shape
    ppm = b"P6\n%d %d\n255\n" % (w, h) + arr.tobytes()
    args = [
        "cjpeg",
        "-sample", "1x1,1x1,1x1",
        "-quality", str(quality),
        "-optimize",
    ]
    if restart > 0:
        args += ["-restart", f"{restart}B"]
    args += ["-outfile", str(out_path)]
    out_path.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(args, input=ppm, check=True)


def verify_djpeg_ok(path: Path) -> None:
    res = subprocess.run(
        ["djpeg", "-outfile", "/dev/null", str(path)],
        capture_output=True,
    )
    if res.returncode != 0:
        raise RuntimeError(f"djpeg rejected {path}: {res.stderr!r}")


def main() -> int:
    # (w, h, pattern, quality, restart_blocks)  restart=0 → no DRI
    cases = [
        # 8-aligned
        (8,   8,   "grad",  75, 0),
        (16,  16,  "check", 80, 0),
        (64,  32,  "grad",  60, 0),
        (32,  64,  "noise", 85, 0),
        (128, 64,  "grad",  50, 0),
        # non-aligned
        (17,  13,  "grad",  80, 0),
        (23,  19,  "check", 70, 0),
        (100, 75,  "grad",  50, 0),
        (127, 93,  "noise", 60, 0),
        (200, 150, "grad",  75, 0),
        (321, 241, "grad",  60, 0),
        # DRI>0 combinations
        (64,  32,  "grad",  70, 1),
        (100, 75,  "check", 75, 4),
        (160, 120, "grad",  50, 16),
        (321, 241, "noise", 60, 32),
    ]

    for w, h, pat, q, r in cases:
        if pat == "grad":
            img = gradient_rgb(w, h)
        elif pat == "check":
            img = check_rgb(w, h)
        else:
            img = noise_rgb(w, h, 42)
        name = f"p09_{pat}_{w}x{h}_q{q}_r{r}.jpg"
        p = OUT / name
        cjpeg_encode(img, quality=q, restart=r, out_path=p)
        verify_djpeg_ok(p)

    print(f"generated {len(cases)} Phase 9 4:4:4 vectors under {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
