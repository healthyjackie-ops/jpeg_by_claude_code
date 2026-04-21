#!/usr/bin/env python3
"""
Phase 13b-prog test vectors — SOF2 progressive Huffman at P=12.

Phase 13a covered SOF0/SOF1 + P=12 (gray/444/420); Phase 13b-prog extends
the same gray/444/420 subset to SOF2 (0xFFC2) progressive Huffman. The
C model path is `decode_progressive` with its new uint16 drain branch.

cjpeg's default progressive script (no `-scans`) emits the full set of
scan types (DC-first, AC-first, AC-refine, DC-refine), so a single
encode exercises every path — matching the Phase 17d approach.

CMYK + P=12 and extended chroma (4:2:2/4:4:0/4:1:1) + P=12 remain future
work (same scope line as Phase 13a).
"""
from __future__ import annotations

import struct
import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "verification" / "vectors" / "phase13b_prog"


# ---- 16-bit image generators ----------------------------------------------


def gradient_gray16(w: int, h: int) -> np.ndarray:
    xs = np.linspace(0, 65535, w, dtype=np.float64)[None, :]
    ys = np.linspace(0, 65535, h, dtype=np.float64)[:, None]
    g = (xs * 0.5 + ys * 0.5).clip(0, 65535).astype(np.uint16)
    return np.broadcast_to(g, (h, w)).copy()


def check_gray16(w: int, h: int) -> np.ndarray:
    chk = (np.add.outer(np.arange(h) // 8, np.arange(w) // 8) % 2) == 0
    return np.where(chk, 56000, 4096).astype(np.uint16)


def noise_gray16(w: int, h: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    return rng.integers(0, 65536, size=(h, w), dtype=np.uint16)


def gradient_rgb16(w: int, h: int) -> np.ndarray:
    xs = np.linspace(0, 65535, w, dtype=np.float64)
    ys = np.linspace(0, 65535, h, dtype=np.float64)
    r = np.broadcast_to(xs[None, :], (h, w))
    g = np.broadcast_to(ys[:, None], (h, w))
    b = ((xs[None, :] + ys[:, None]) * 0.5).clip(0, 65535)
    return np.stack([r, g, b], axis=-1).astype(np.uint16)


def check_rgb16(w: int, h: int) -> np.ndarray:
    chk = (np.add.outer(np.arange(h) // 8, np.arange(w) // 8) % 2) == 0
    a = np.array([60000,  3000, 30000], dtype=np.uint16)
    b = np.array([ 3000, 50000, 40000], dtype=np.uint16)
    return np.where(chk[..., None], a, b).astype(np.uint16)


def noise_rgb16(w: int, h: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    return rng.integers(0, 65536, size=(h, w, 3), dtype=np.uint16)


# ---- 16-bit Netpbm writers -------------------------------------------------


def write_pgm16(arr: np.ndarray, path: Path) -> None:
    assert arr.ndim == 2 and arr.dtype == np.uint16
    h, w = arr.shape
    with open(path, "wb") as f:
        f.write(f"P5\n{w} {h}\n65535\n".encode("ascii"))
        f.write(arr.astype(">u2").tobytes())


def write_ppm16(arr: np.ndarray, path: Path) -> None:
    assert arr.ndim == 3 and arr.shape[2] == 3 and arr.dtype == np.uint16
    h, w, _ = arr.shape
    with open(path, "wb") as f:
        f.write(f"P6\n{w} {h}\n65535\n".encode("ascii"))
        f.write(arr.astype(">u2").tobytes())


# ---- cjpeg invocation ------------------------------------------------------


def cjpeg_encode(src_pnm: Path, out_jpg: Path, quality: int,
                 sample: str | None, restart: int, grayscale: bool) -> None:
    cmd = ["cjpeg", "-precision", "12", "-progressive",
           "-quality", str(quality), "-optimize"]
    if grayscale:
        cmd += ["-grayscale"]
    elif sample:
        cmd += ["-sample", sample]
    if restart > 0:
        cmd += ["-restart", f"{restart}B"]
    cmd += ["-outfile", str(out_jpg), str(src_pnm)]
    subprocess.run(cmd, check=True)


def verify_sof2_p12(path: Path) -> None:
    """Confirm SOF2 (0xFFC2) with P=12 — sanity-check the encoder output."""
    data = path.read_bytes()
    i = 0
    while i < len(data) - 3:
        if data[i] == 0xFF and data[i + 1] == 0xC2:
            p = data[i + 4]
            if p != 12:
                raise RuntimeError(f"{path}: SOF2 has P={p}, expected 12")
            return
        i += 1
    raise RuntimeError(f"{path}: no SOF2 (0xFFC2) marker found")


# ---- main ------------------------------------------------------------------


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    tmp = OUT / ".tmp_src.pnm"

    cases = [
        # (w, h, pattern, quality, sample, restart, grayscale)
        # grayscale — DC + AC spectral + refine in a single encode
        (8,   8,   "grad",  75, None,    0,  True),
        (16,  16,  "check", 80, None,    0,  True),
        (23,  17,  "grad",  50, None,    0,  True),
        (96,  96,  "check", 75, None,    4,  True),
        (241, 321, "noise", 60, None,    0,  True),
        # 4:4:4 YCbCr
        (8,   8,   "grad",  75, "1x1",   0,  False),
        (16,  16,  "check", 85, "1x1",   0,  False),
        (32,  32,  "grad",  60, "1x1",   0,  False),
        (64,  64,  "noise", 85, "1x1",   1,  False),
        (199, 257, "grad",  55, "1x1",   0,  False),
        # 4:2:0 YCbCr
        (16,  16,  "grad",  75, "2x2",   0,  False),
        (32,  32,  "check", 80, "2x2",   0,  False),
        (64,  64,  "grad",  70, "2x2",   1,  False),
        (128, 64,  "grad",  50, "2x2",   0,  False),
        (321, 241, "noise", 60, "2x2",   4,  False),
        (57,  39,  "grad",  50, "2x2",   0,  False),
        # non-aligned sizes
        (17,  13,  "grad",  80, "1x1",   0,  False),
        (23,  19,  "check", 75, "2x2",   0,  False),
        (45,  33,  "grad",  70, None,    0,  True),
        (128, 128, "grad",  90, "2x2",  16,  False),
    ]

    generated = 0
    for w, h, pat, q, samp, r, gray in cases:
        if gray:
            if pat == "grad":
                arr = gradient_gray16(w, h)
            elif pat == "check":
                arr = check_gray16(w, h)
            else:
                arr = noise_gray16(w, h, 42)
            write_pgm16(arr, tmp)
        else:
            if pat == "grad":
                arr = gradient_rgb16(w, h)
            elif pat == "check":
                arr = check_rgb16(w, h)
            else:
                arr = noise_rgb16(w, h, 42)
            write_ppm16(arr, tmp)

        samp_tag = "g" if gray else samp.replace("x", "")
        name = f"p13bp_{pat}_{w}x{h}_q{q}_s{samp_tag}_r{r}.jpg"
        jpg = OUT / name
        cjpeg_encode(tmp, jpg, q, samp, r, gray)
        verify_sof2_p12(jpg)
        generated += 1

    tmp.unlink(missing_ok=True)
    print(f"generated {generated} Phase 13b-prog SOF2+P=12 vectors under {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
