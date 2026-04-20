#!/usr/bin/env python3
"""
Phase 25a test vectors — **SOF3 Lossless (single-component, P=8, Pt=0)**.

Exercises ISO/IEC 10918-1 Annex H.1:
  - Predictor Ps ∈ {1..7}
  - Huffman lossless DC (DHT class 0)
  - No DCT / no dequant / no IDCT
  - No restart markers (DRI=0) — Phase 25c will extend
  - 8-bit precision, point transform Pt=0 (Phase 27 will extend precision,
    Phase 25c adds Pt>0)

Matrix:
  - All 7 predictors Ps = 1..7
  - Sizes: 32x32, 64x48 (block-aligned) + 97x73 (non-aligned, arbitrary WxH)
  - Patterns: gradient, checkerboard, noise
  - Quality N/A — lossless.
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "verification" / "vectors" / "phase25"


def gradient(w: int, h: int) -> np.ndarray:
    x = np.linspace(0, 255, w, dtype=np.float32)[None, :]
    y = np.linspace(0, 255, h, dtype=np.float32)[:, None]
    return np.clip((x + y) * 0.5, 0, 255).astype(np.uint8).reshape(h, w)


def check(w: int, h: int) -> np.ndarray:
    mask = (np.add.outer(np.arange(h) // 8, np.arange(w) // 8) % 2) == 0
    return np.where(mask, 240, 16).astype(np.uint8)


def noise(w: int, h: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    return rng.integers(0, 256, size=(h, w), dtype=np.uint8)


def write_pgm(arr: np.ndarray, p: Path) -> None:
    h = arr.shape[0]
    w = arr.shape[1]
    header = b"P5\n%d %d\n255\n" % (w, h)
    p.write_bytes(header + arr.tobytes())


def cjpeg_encode_lossless(arr: np.ndarray, *, psv: int, pt: int,
                          out_path: Path) -> None:
    """cjpeg -lossless psv[,Pt] on a grayscale PGM."""
    assert arr.ndim == 2
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(suffix=".pgm", delete=False) as pf:
        pnm_path = Path(pf.name)
    try:
        write_pgm(arr, pnm_path)
        lossless_arg = f"{psv},{pt}" if pt else f"{psv}"
        args = ["cjpeg", "-lossless", lossless_arg, "-grayscale",
                "-outfile", str(out_path), str(pnm_path)]
        subprocess.run(args, check=True)
    finally:
        pnm_path.unlink(missing_ok=True)


def verify_djpeg_ok(path: Path) -> None:
    with tempfile.NamedTemporaryFile(suffix=".pnm", delete=False) as tf:
        tmp = Path(tf.name)
    try:
        res = subprocess.run(
            ["djpeg", "-outfile", str(tmp), str(path)],
            capture_output=True,
        )
        if res.returncode != 0:
            raise RuntimeError(f"djpeg rejected {path}: {res.stderr!r}")
    finally:
        tmp.unlink(missing_ok=True)


def main() -> int:
    # (w, h, pattern, ps, pt)
    # Cover all 7 predictors with a mix of patterns and sizes, plus a few Pt>0
    # so we pick up the point-transform path.
    cases = []
    for ps in range(1, 8):
        # Each predictor at 32x32 gradient, 64x48 check, 97x73 noise (non-aligned).
        cases.append((32, 32, "grad", ps, 0))
        cases.append((64, 48, "check", ps, 0))
        cases.append((97, 73, "noise", ps, 0))

    # Extra coverage: larger images for each predictor at Pt=0
    for ps in range(1, 8):
        cases.append((192, 128, "grad", ps, 0))

    OUT.mkdir(parents=True, exist_ok=True)
    count = 0
    for w, h, pat, ps, pt in cases:
        if pat == "grad":
            img = gradient(w, h)
        elif pat == "check":
            img = check(w, h)
        else:
            img = noise(w, h, seed=42 + w + h + ps)
        name = f"lossless_ps{ps}_pt{pt}_{pat}_{w}x{h}.jpg"
        p = OUT / name
        cjpeg_encode_lossless(img, psv=ps, pt=pt, out_path=p)
        verify_djpeg_ok(p)
        count += 1

    print(f"generated {count} Phase 25a lossless vectors under {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
