#!/usr/bin/env python3
"""
Phase 25c test vectors — **SOF3 Lossless + DRI restart intervals**.

Builds on Phase 25a (SOF3 gray P=8 Pt=0) and Phase 25b (SOF3 Nf=3 RGB + Pt>0)
by exercising restart markers (RSTn) at row-aligned intervals. libjpeg-turbo's
lossless decoder requires DRI to be a multiple of MCUs-per-row (see
src/jddiffct.c:start_input_pass), so every vector here has
(restart_interval % W) == 0.

Scope exercised:
  • Nf ∈ {1, 3} (gray / RGB interleaved)
  • Ps ∈ {1..7} (all seven predictor formulae)
  • Pt ∈ {0..2}
  • Restart interval = N rows for N ∈ {1, 2, 4, 8}

Out of scope (reserved for later phases):
  • P ≠ 8 (Phase 27: 2..16-bit lossless)
  • Non-row-aligned DRI (ISO allows it; libjpeg does not; Phase 26 RTL TBD)
  • Nf ∈ {2, 4} (CMYK lossless — Phase 25c-extension if needed)
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "verification" / "vectors" / "phase25c"


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


def check_gray(w: int, h: int) -> np.ndarray:
    mask = (np.add.outer(np.arange(h) // 8, np.arange(w) // 8) % 2) == 0
    return np.where(mask, 240, 16).astype(np.uint8)


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
                          gray: bool, restart_rows: int,
                          out_path: Path) -> None:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    suf = ".pgm" if gray else ".ppm"
    with tempfile.NamedTemporaryFile(suffix=suf, delete=False) as pf:
        pnm_path = Path(pf.name)
    try:
        write_pnm(arr, pnm_path)
        lossless_arg = f"{psv},{pt}" if pt else f"{psv}"
        args = ["cjpeg", "-lossless", lossless_arg,
                "-restart", str(restart_rows)]
        if gray:
            args += ["-grayscale"]
        args += ["-outfile", str(out_path), str(pnm_path)]
        subprocess.run(args, check=True, capture_output=True)
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
    cases: list[tuple[int, int, str, int, int, int]] = []
    # (w, h, pattern, ps, pt, restart_rows)

    # Grid A: all 7 predictors × multiple DRI values × 32x32 for both gray/RGB.
    for ps in range(1, 8):
        for rr in (1, 2, 4, 8):
            cases.append((32, 32, "gray_grad",  ps, 0, rr))
            cases.append((32, 32, "gray_check", ps, 0, rr))
            cases.append((32, 32, "rgb_grad",   ps, 0, rr))
            cases.append((32, 32, "rgb_check",  ps, 0, rr))

    # Grid B: all 7 predictors × one DRI × noise pattern × 64x48 (non-square).
    for ps in range(1, 8):
        cases.append((64, 48, "gray_noise", ps, 0, 4))
        cases.append((64, 48, "rgb_noise",  ps, 0, 4))

    # Grid C: Pt>0 combined with DRI (representative sample).
    for pt in (1, 2):
        for ps in (1, 4, 7):
            cases.append((48, 48, "gray_grad",  ps, pt, 4))
            cases.append((48, 48, "rgb_grad",   ps, pt, 4))

    # Grid D: larger image with every predictor × 1 DRI row to stress
    # first-row-post-restart on every MCU row.
    for ps in (1, 2, 4, 5, 7):
        cases.append((97, 73, "rgb_noise", ps, 0, 1))

    OUT.mkdir(parents=True, exist_ok=True)
    count = 0
    for w, h, pat, ps, pt, rr in cases:
        if pat == "rgb_grad":
            img = gradient_rgb(w, h); gray = False
        elif pat == "rgb_check":
            img = check_rgb(w, h); gray = False
        elif pat == "rgb_noise":
            img = noise_rgb(w, h, seed=42 + w + h + ps + rr); gray = False
        elif pat == "gray_grad":
            img = gradient_gray(w, h); gray = True
        elif pat == "gray_check":
            img = check_gray(w, h); gray = True
        elif pat == "gray_noise":
            img = noise_gray(w, h, seed=42 + w + h + ps + rr); gray = True
        else:
            raise ValueError(pat)
        name = f"lossless25c_{pat}_ps{ps}_pt{pt}_rr{rr}_{w}x{h}.jpg"
        p = OUT / name
        cjpeg_encode_lossless(img, psv=ps, pt=pt, gray=gray,
                              restart_rows=rr, out_path=p)
        verify_djpeg_ok(p)
        count += 1

    print(f"generated {count} Phase 25c vectors under {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
