#!/usr/bin/env python3
"""
Phase 25b test vectors — **SOF3 Lossless multi-component + Pt>0**.

Extends Phase 25a (gray, P=8, Pt=0) to:
  - Nf = 3 RGB (interleaved scan, all components Hi=Vi=1)
  - Pt ∈ {0..4} point transform (works for both Nf=1 gray and Nf=3 RGB)
  - All 7 predictors Ps ∈ {1..7}

Still in-scope limits:
  - DRI = 0 (Phase 25c)
  - P = 8 (Phase 27 extends to 2..16)
  - Hi = Vi = 1 for all components (cjpeg default for lossless)
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "verification" / "vectors" / "phase25b"


def gradient_rgb(w: int, h: int) -> np.ndarray:
    x = np.linspace(0, 255, w, dtype=np.float32)[None, :]
    y = np.linspace(0, 255, h, dtype=np.float32)[:, None]
    r = np.clip(x, 0, 255).astype(np.uint8)
    g = np.clip(y, 0, 255).astype(np.uint8)
    b = np.clip((x + y) * 0.5, 0, 255).astype(np.uint8)
    r = np.broadcast_to(r, (h, w))
    g = np.broadcast_to(g, (h, w))
    return np.stack([r, g, b], axis=-1)


def check_rgb(w: int, h: int) -> np.ndarray:
    mask = (np.add.outer(np.arange(h) // 8, np.arange(w) // 8) % 2) == 0
    a = np.where(mask, 240, 16).astype(np.uint8)
    b = np.where(mask, 16, 240).astype(np.uint8)
    c = np.where(mask, 128, 200).astype(np.uint8)
    return np.stack([a, b, c], axis=-1)


def noise_rgb(w: int, h: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    return rng.integers(0, 256, size=(h, w, 3), dtype=np.uint8)


def gradient_gray(w: int, h: int) -> np.ndarray:
    x = np.linspace(0, 255, w, dtype=np.float32)[None, :]
    y = np.linspace(0, 255, h, dtype=np.float32)[:, None]
    return np.clip((x + y) * 0.5, 0, 255).astype(np.uint8).reshape(h, w)


def noise_gray(w: int, h: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    return rng.integers(0, 256, size=(h, w), dtype=np.uint8)


def write_pnm(arr: np.ndarray, p: Path) -> None:
    h = arr.shape[0]
    w = arr.shape[1]
    if arr.ndim == 2:
        header = b"P5\n%d %d\n255\n" % (w, h)
    else:
        header = b"P6\n%d %d\n255\n" % (w, h)
    p.write_bytes(header + arr.tobytes())


def cjpeg_encode_lossless(arr: np.ndarray, *, psv: int, pt: int,
                          gray: bool, out_path: Path) -> None:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    suf = ".pgm" if gray else ".ppm"
    with tempfile.NamedTemporaryFile(suffix=suf, delete=False) as pf:
        pnm_path = Path(pf.name)
    try:
        write_pnm(arr, pnm_path)
        lossless_arg = f"{psv},{pt}" if pt else f"{psv}"
        args = ["cjpeg", "-lossless", lossless_arg]
        if gray:
            args += ["-grayscale"]
        args += ["-outfile", str(out_path), str(pnm_path)]
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
    cases = []

    # Nf=3 RGB: all 7 predictors × {grad/check/noise} × 32x32 / 64x48 / 97x73
    for ps in range(1, 8):
        cases.append((32, 32, "rgb_grad",  ps, 0))
        cases.append((64, 48, "rgb_check", ps, 0))
        cases.append((97, 73, "rgb_noise", ps, 0))

    # Nf=3 RGB with Pt>0: a subset of predictors × all Pt (1..4)
    for pt in range(1, 5):
        cases.append((48, 48, "rgb_grad",  1, pt))
        cases.append((48, 48, "rgb_check", 4, pt))
        cases.append((97, 73, "rgb_noise", 7, pt))

    # Nf=1 gray Pt>0: quick grid
    for pt in range(1, 5):
        cases.append((32, 32, "gray_grad",  1, pt))
        cases.append((97, 73, "gray_noise", 7, pt))

    # Larger RGB check (non-trivial compile of all predictors in flight)
    for ps in (1, 4, 7):
        cases.append((192, 128, "rgb_grad", ps, 0))

    OUT.mkdir(parents=True, exist_ok=True)
    count = 0
    for w, h, pat, ps, pt in cases:
        if pat == "rgb_grad":
            img = gradient_rgb(w, h); gray = False
        elif pat == "rgb_check":
            img = check_rgb(w, h); gray = False
        elif pat == "rgb_noise":
            img = noise_rgb(w, h, seed=42 + w + h + ps); gray = False
        elif pat == "gray_grad":
            img = gradient_gray(w, h); gray = True
        else:
            img = noise_gray(w, h, seed=42 + w + h + ps); gray = True
        name = f"lossless25b_{pat}_ps{ps}_pt{pt}_{w}x{h}.jpg"
        p = OUT / name
        cjpeg_encode_lossless(img, psv=ps, pt=pt, gray=gray, out_path=p)
        verify_djpeg_ok(p)
        count += 1

    print(f"generated {count} Phase 25b vectors under {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
