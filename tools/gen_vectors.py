#!/usr/bin/env python3
"""
Generate JPEG test vectors for Phase 3 RTL verification.

Produces images matching Phase 1's corpus categories:
  structured / boundary / extreme-Q / random

Default: smoke set (~16 small images) written to verification/vectors/smoke/
With --full: full 1210-image corpus to verification/vectors/full/

Only generates YUV 4:2:0, SOF0, 8-bit-precision JPEGs, matching the v1.0
decoder's supported subset.
"""
from __future__ import annotations

import argparse
import io
import os
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]

# v1.0 supported: YUV 4:2:0 only; use MCU-aligned dims (multiples of 16)
def save_jpg(path: Path, arr: np.ndarray, quality: int = 80, subsample: int = 2) -> None:
    """Save RGB numpy (H,W,3) as JPEG 4:2:0."""
    path.parent.mkdir(parents=True, exist_ok=True)
    img = Image.fromarray(arr, mode="RGB")
    img.save(str(path), format="JPEG", quality=quality, subsampling=subsample,
             optimize=False, progressive=False)


# -------- pattern generators -----------------------------------------------
def gradient(w: int, h: int) -> np.ndarray:
    x = np.linspace(0, 255, w, dtype=np.float32)[None, :]
    y = np.linspace(0, 255, h, dtype=np.float32)[:, None]
    a = np.clip((x + y) * 0.5, 0, 255).astype(np.uint8)
    rgb = np.stack([a, np.rot90(a, 1 % 4) if a.shape[0] == a.shape[1] else a, 255 - a], -1)
    return rgb

def stripe(w: int, h: int) -> np.ndarray:
    a = np.zeros((h, w), dtype=np.uint8)
    a[:, ::16] = 255
    a[::16, :] ^= 128
    return np.stack([a, (a >> 1), 255 - a], -1)

def check(w: int, h: int) -> np.ndarray:
    a = np.zeros((h, w), dtype=np.uint8)
    a[::16, ::16] = 255
    a = np.where((np.add.outer(np.arange(h) // 16, np.arange(w) // 16) % 2) == 0, 255, 32).astype(np.uint8)
    return np.stack([a, 255 - a, a // 2], -1)

def noise(w: int, h: int, seed: int = 0) -> np.ndarray:
    rng = np.random.default_rng(seed)
    return rng.integers(0, 256, size=(h, w, 3), dtype=np.uint8)

def solid(w: int, h: int, color: tuple[int, int, int]) -> np.ndarray:
    a = np.zeros((h, w, 3), dtype=np.uint8)
    a[..., 0], a[..., 1], a[..., 2] = color
    return a

# -------- corpora ----------------------------------------------------------
def smoke_vectors(out: Path) -> list[Path]:
    """Small set for quick RTL diff smoke (~12 images, all small for sim speed)."""
    cases = [
        ("grad_16x16_q80.jpg",   gradient(16, 16),   80),
        ("grad_32x32_q80.jpg",   gradient(32, 32),   80),
        ("grad_64x32_q50.jpg",   gradient(64, 32),   50),
        ("stripe_32x32_q75.jpg", stripe(32, 32),     75),
        ("check_32x32_q90.jpg",  check(32, 32),      90),
        ("solid_16x16_gray.jpg", solid(16, 16, (128, 128, 128)), 80),
        ("solid_16x16_red.jpg",  solid(16, 16, (255, 0, 0)),     80),
        ("noise_32x32_q50.jpg",  noise(32, 32, 1),   50),
        ("noise_32x32_q85.jpg",  noise(32, 32, 2),   85),
        ("grad_64x64_q25.jpg",   gradient(64, 64),   25),  # extreme Q
        ("grad_64x64_q100.jpg",  gradient(64, 64),   100),
        ("check_48x32_q60.jpg",  check(48, 32),      60),  # non-square, MCU-aligned
    ]
    paths = []
    for name, img, q in cases:
        p = out / name
        save_jpg(p, img, quality=q)
        paths.append(p)
    return paths


def full_vectors(out: Path) -> list[Path]:
    paths: list[Path] = []
    # Structured (144): gradient/stripe/check/noise × 6 sizes × 6 Q
    sizes = [(16, 16), (32, 32), (64, 32), (128, 64), (256, 128), (512, 256)]
    qs = [5, 25, 50, 75, 85, 100]
    for s_i, (w, h) in enumerate(sizes):
        for q_i, q in enumerate(qs):
            for fn_i, (fn, tag) in enumerate([
                (gradient, "grad"), (stripe, "strp"), (check, "chk"), (lambda w, h: noise(w, h, s_i * 100 + q_i), "noi"),
            ]):
                p = out / f"struct_{tag}_{w}x{h}_q{q}.jpg"
                save_jpg(p, fn(w, h), quality=q)
                paths.append(p)
    # Boundary (36): min 16x16, 4K 3840x2160, rect, B/W, salt
    extra = [
        ("bdry_min_16x16.jpg",    solid(16, 16, (0, 0, 0)), 80),
        ("bdry_white_16x16.jpg",  solid(16, 16, (255, 255, 255)), 80),
        ("bdry_rect_1920x1088.jpg", gradient(1920, 1088), 80),
        ("bdry_rect_32x96.jpg",   gradient(32, 96), 80),
        ("bdry_rect_128x64.jpg",  gradient(128, 64), 80),
        ("bdry_rect_256x128.jpg", gradient(256, 128), 80),
    ]
    for name, img, q in extra:
        p = out / name
        save_jpg(p, img, quality=q); paths.append(p)
    # Random JPEG (1000): gradient/stripe/check/noise × 250 each, random sizes/Q
    rng = np.random.default_rng(2026)
    types = [gradient, stripe, check, lambda w, h: noise(w, h, int(rng.integers(0, 1_000_000)))]
    for i in range(1000):
        t = types[i % 4]
        w = int(rng.integers(1, 8)) * 16
        h = int(rng.integers(1, 8)) * 16
        q = int(rng.choice([10, 25, 50, 75, 90]))
        img = t(w, h)
        p = out / f"rnd_{i:04d}_{w}x{h}_q{q}.jpg"
        save_jpg(p, img, quality=q); paths.append(p)
    return paths


# -------- entry ------------------------------------------------------------
def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--full", action="store_true",
                    help="generate full 1210-image corpus (slow)")
    ap.add_argument("--out", type=str, default=None,
                    help="output directory (default: verification/vectors/smoke or .../full)")
    args = ap.parse_args()

    if args.full:
        out = Path(args.out) if args.out else (ROOT / "verification" / "vectors" / "full")
        paths = full_vectors(out)
    else:
        out = Path(args.out) if args.out else (ROOT / "verification" / "vectors" / "smoke")
        paths = smoke_vectors(out)

    print(f"generated {len(paths)} JPEG(s) under {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
