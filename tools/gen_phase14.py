#!/usr/bin/env python3
"""
Phase 14 test vectors — SOF2 progressive JPEG (0xFFC2).

Generates a small set of progressive JPEGs via `cjpeg -progressive`.
These are used to validate that the decoder **recognizes SOF2 and
error-outs cleanly** (ERR_UNSUP_SOF). No decode is expected.
"""
from __future__ import annotations

import struct
import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "verification" / "vectors" / "phase14"


# ---- Simple 8-bit image patterns ------------------------------------------


def gradient_gray8(w: int, h: int) -> np.ndarray:
    xs = np.linspace(0, 255, w, dtype=np.float64)[None, :]
    ys = np.linspace(0, 255, h, dtype=np.float64)[:, None]
    g = (xs * 0.5 + ys * 0.5).clip(0, 255).astype(np.uint8)
    return np.broadcast_to(g, (h, w)).copy()


def check_gray8(w: int, h: int) -> np.ndarray:
    chk = (np.add.outer(np.arange(h) // 8, np.arange(w) // 8) % 2) == 0
    return np.where(chk, 200, 50).astype(np.uint8)


def gradient_rgb8(w: int, h: int) -> np.ndarray:
    xs = np.linspace(0, 255, w, dtype=np.float64)
    ys = np.linspace(0, 255, h, dtype=np.float64)
    r = np.broadcast_to(xs[None, :], (h, w))
    g = np.broadcast_to(ys[:, None], (h, w))
    b = ((xs[None, :] + ys[:, None]) * 0.5).clip(0, 255)
    return np.stack([r, g, b], axis=-1).astype(np.uint8)


def check_rgb8(w: int, h: int) -> np.ndarray:
    chk = (np.add.outer(np.arange(h) // 8, np.arange(w) // 8) % 2) == 0
    a = np.array([230,  20, 100], dtype=np.uint8)
    b = np.array([ 30, 180, 220], dtype=np.uint8)
    return np.where(chk[..., None], a, b).astype(np.uint8)


def noise_rgb8(w: int, h: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    return rng.integers(0, 256, size=(h, w, 3), dtype=np.uint8)


# ---- 8-bit Netpbm writer --------------------------------------------------


def write_pgm8(arr: np.ndarray, path: Path) -> None:
    h, w = arr.shape
    with open(path, "wb") as f:
        f.write(f"P5\n{w} {h}\n255\n".encode("ascii"))
        f.write(arr.tobytes())


def write_ppm8(arr: np.ndarray, path: Path) -> None:
    h, w, _ = arr.shape
    with open(path, "wb") as f:
        f.write(f"P6\n{w} {h}\n255\n".encode("ascii"))
        f.write(arr.tobytes())


# ---- cjpeg invocation -----------------------------------------------------


def cjpeg_progressive(src: Path, out: Path, quality: int,
                      sample: str | None, restart: int, grayscale: bool) -> None:
    cmd = ["cjpeg", "-progressive", "-quality", str(quality), "-optimize"]
    if grayscale:
        cmd += ["-grayscale"]
    elif sample:
        cmd += ["-sample", sample]
    if restart > 0:
        cmd += ["-restart", str(restart)]
    cmd += ["-outfile", str(out), str(src)]
    subprocess.run(cmd, check=True)


def verify_sof2(path: Path) -> None:
    """Confirm the produced file has a SOF2 marker (0xFFC2)."""
    data = path.read_bytes()
    i = 0
    while i < len(data) - 1:
        if data[i] == 0xFF and data[i + 1] == 0xC2:
            return
        i += 1
    raise RuntimeError(f"{path}: no SOF2 (0xFFC2) marker found")


# ---- main -----------------------------------------------------------------


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    tmp = OUT / ".tmp_src.pnm"

    cases = [
        # (w, h, pattern, quality, sample, restart, grayscale)
        (8,   8,   "grad",  75, None,  0,  True),
        (16,  16,  "check", 80, "2x2", 0,  False),
        (17,  13,  "grad",  75, "1x1", 0,  False),
        (32,  32,  "check", 85, "2x2", 0,  False),
        (64,  64,  "grad",  70, "2x2", 4,  False),
        (128, 64,  "grad",  60, "2x2", 0,  False),
        (128, 128, "check", 90, "1x1", 0,  False),
        (199, 257, "noise", 55, "2x2", 8,  False),
    ]

    generated = 0
    for w, h, pat, q, samp, r, gray in cases:
        if gray:
            if pat == "grad":
                arr = gradient_gray8(w, h)
            else:
                arr = check_gray8(w, h)
            write_pgm8(arr, tmp)
        else:
            if pat == "grad":
                arr = gradient_rgb8(w, h)
            elif pat == "check":
                arr = check_rgb8(w, h)
            else:
                arr = noise_rgb8(w, h, 42)
            write_ppm8(arr, tmp)

        samp_tag = "g" if gray else samp.replace("x", "")
        name = f"p14_{pat}_{w}x{h}_q{q}_s{samp_tag}_r{r}.jpg"
        jpg = OUT / name
        cjpeg_progressive(tmp, jpg, q, samp, r, gray)
        verify_sof2(jpg)
        generated += 1

    tmp.unlink(missing_ok=True)
    print(f"generated {generated} Phase 14 SOF2 vectors under {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
