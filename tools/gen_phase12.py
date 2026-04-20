#!/usr/bin/env python3
"""
Phase 12 test vectors — CMYK JPEG (Nf=4, all H=V=1; MCU=8x8, 4 blocks).

Uses PIL (Pillow) to write CMYK JPEGs directly. PIL writes an Adobe APP14
marker with color_transform=0 (CMYK), which libjpeg-turbo recognizes for
JCS_CMYK decoding. For `-restart` DRI, PIL doesn't expose it directly, so
we post-process with jpegtran when restart>0.
"""
from __future__ import annotations

import io
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "verification" / "vectors" / "phase12"


def gradient_cmyk(w: int, h: int) -> np.ndarray:
    """C/M/Y/K as independent smooth gradients — all 4 channels non-trivial."""
    x = np.linspace(0, 255, w, dtype=np.float32)[None, :]
    y = np.linspace(0, 255, h, dtype=np.float32)[:, None]
    c = np.clip(x, 0, 255).astype(np.uint8)
    m = np.clip(y, 0, 255).astype(np.uint8)
    ycomp = np.clip(255 - (x + y) * 0.5, 0, 255).astype(np.uint8)
    k = np.clip((x + y) * 0.25, 0, 255).astype(np.uint8)
    c = np.broadcast_to(c, (h, w))
    m = np.broadcast_to(m, (h, w))
    ycomp = np.broadcast_to(ycomp, (h, w))
    k = np.broadcast_to(k, (h, w))
    return np.stack([c, m, ycomp, k], axis=-1).astype(np.uint8)


def check_cmyk(w: int, h: int) -> np.ndarray:
    """8x8 checkerboard, two CMYK colors."""
    checker = (np.add.outer(np.arange(h) // 8, np.arange(w) // 8) % 2) == 0
    col0 = np.array([200, 50, 40, 10],  dtype=np.uint8)   # cyan-ish
    col1 = np.array([40,  180, 60, 30], dtype=np.uint8)   # magenta-ish
    return np.where(checker[..., None], col0, col1).astype(np.uint8)


def noise_cmyk(w: int, h: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    return rng.integers(0, 256, size=(h, w, 4), dtype=np.uint8)


def pil_encode_cmyk(arr: np.ndarray, quality: int, out_path: Path) -> None:
    img = Image.fromarray(arr, mode="CMYK")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    # subsampling=0 → all components 1x1; optimize for shorter Huffman.
    img.save(out_path, format="JPEG", quality=quality, subsampling=0, optimize=True)


def apply_dri(jpg_path: Path, restart_mcus: int) -> None:
    """Re-write JPEG in place with a DRI of `restart_mcus` MCUs using jpegtran."""
    tmp = jpg_path.with_suffix(".tmp.jpg")
    subprocess.run(
        ["jpegtran", "-restart", f"{restart_mcus}", "-outfile", str(tmp), str(jpg_path)],
        check=True,
    )
    tmp.replace(jpg_path)


def verify_djpeg_ok(path: Path) -> None:
    res = subprocess.run(
        ["djpeg", "-colors", "0", "-outfile", "/dev/null", str(path)],
        capture_output=True,
    )
    if res.returncode != 0:
        # Fallback: CMYK → djpeg may only emit PPM via conversion; try without colorspace
        res = subprocess.run(
            ["djpeg", "-outfile", "/dev/null", str(path)],
            capture_output=True,
        )
        if res.returncode != 0:
            raise RuntimeError(f"djpeg rejected {path}: {res.stderr!r}")


def main() -> int:
    # (w, h, pattern, quality, restart_mcus)  restart=0 → no DRI
    # For CMYK 1x1 MCU is 8x8 so any multiples of 8 are aligned.
    cases = [
        # MCU-aligned (8x8)
        (8,   8,   "grad",  75, 0),
        (16,  16,  "check", 80, 0),
        (32,  32,  "grad",  60, 0),
        (64,  64,  "noise", 85, 0),
        (128, 64,  "grad",  50, 0),
        # non-aligned
        (9,   9,   "grad",  80, 0),
        (23,  17,  "check", 70, 0),
        (57,  39,  "grad",  50, 0),
        (97,  113, "noise", 60, 0),
        (241, 321, "grad",  75, 0),
        # DRI>0 combinations (restart is in MCUs for CMYK 1x1: 1 MCU = 1 8x8)
        (64,  64,  "grad",  70, 1),
        (96,  96,  "check", 75, 4),
        (128, 128, "grad",  50, 16),
        (241, 321, "noise", 60, 32),
        # Extra mixed-size sanity
        (199, 257, "grad",  55, 0),
    ]

    generated = 0
    for w, h, pat, q, r in cases:
        if pat == "grad":
            img = gradient_cmyk(w, h)
        elif pat == "check":
            img = check_cmyk(w, h)
        else:
            img = noise_cmyk(w, h, 42)
        name = f"p12_{pat}_{w}x{h}_q{q}_r{r}.jpg"
        p = OUT / name
        pil_encode_cmyk(img, quality=q, out_path=p)
        if r > 0:
            apply_dri(p, r)
        verify_djpeg_ok(p)
        generated += 1

    print(f"generated {generated} Phase 12 CMYK vectors under {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
