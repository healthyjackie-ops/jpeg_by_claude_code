#!/usr/bin/env python3
"""
Phase 27 test vectors — **SOF3 Lossless + P ∈ {2..7, 9..16}**.

Builds on Phase 25a/b/c (SOF3 P=8 covered for gray/RGB + DRI) by exercising
every non-8 precision allowed by ISO H.1.2.1. Scope:

  • P   ∈ {2, 3, 4, 5, 6, 7}  (low-precision grids F..I)
  • P   ∈ {9, 10, 11, 12, 13, 14, 15, 16}  (high-precision grids A..E)
  • Ps  ∈ {1..7}  (full predictor set on the 32x32 grid; subset on the rest)
  • Pt  ∈ {0, 2}  (Pt=0 dominant; Pt=2 exercises point-transform with P>8)
  • Nf  ∈ {1, 3}  (grayscale + RGB interleaved, H=V=1)
  • DRI ∈ {0, 2 rows}  (exercise libjpeg-turbo row-aligned restarts at P>8)

libjpeg-turbo API triad:
  • P ≤ 8   → jpeg_read_scanlines
  • 9..12   → jpeg12_read_scanlines    (J12SAMPLE = short)
  • 13..16  → jpeg16_read_scanlines    (J16SAMPLE = uint16)

Out of scope:
  • Nf ∈ {2, 4} (CMYK lossless at P>8 — future)
  • Hierarchical / arith (SOF11 = Wave 4)
"""
from __future__ import annotations

import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "verification" / "vectors" / "phase27"


# ---- Image pattern generators (P-bit range) --------------------------------


def gradient_gray(w: int, h: int, maxv: int) -> np.ndarray:
    xs = np.linspace(0, maxv, w, dtype=np.float64)[None, :]
    ys = np.linspace(0, maxv, h, dtype=np.float64)[:, None]
    g = ((xs * 0.5 + ys * 0.5)).clip(0, maxv).astype(np.uint16)
    return np.broadcast_to(g, (h, w)).copy()


def check_gray(w: int, h: int, maxv: int) -> np.ndarray:
    chk = (np.add.outer(np.arange(h) // 8, np.arange(w) // 8) % 2) == 0
    lo = maxv // 16
    hi = maxv - maxv // 16
    return np.where(chk, hi, lo).astype(np.uint16)


def noise_gray(w: int, h: int, maxv: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    return rng.integers(0, maxv + 1, size=(h, w), dtype=np.uint16)


def gradient_rgb(w: int, h: int, maxv: int) -> np.ndarray:
    xs = np.linspace(0, maxv, w, dtype=np.float64)
    ys = np.linspace(0, maxv, h, dtype=np.float64)
    r = np.broadcast_to(xs[None, :], (h, w)).astype(np.uint16)
    g = np.broadcast_to(ys[:, None], (h, w)).astype(np.uint16)
    b = ((xs[None, :] + ys[:, None]) * 0.5).clip(0, maxv).astype(np.uint16)
    return np.stack([r, g, b], axis=-1)


def check_rgb(w: int, h: int, maxv: int) -> np.ndarray:
    chk = (np.add.outer(np.arange(h) // 8, np.arange(w) // 8) % 2) == 0
    a = np.array([maxv - maxv // 16, maxv // 16, maxv // 2], dtype=np.uint16)
    b = np.array([maxv // 16, maxv - maxv // 16, maxv * 3 // 4], dtype=np.uint16)
    return np.where(chk[..., None], a, b).astype(np.uint16)


def noise_rgb(w: int, h: int, maxv: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    return rng.integers(0, maxv + 1, size=(h, w, 3), dtype=np.uint16)


# ---- PNM writers. For P>8 we write 16-bit big-endian PNM. --------------------


def write_pgm(arr: np.ndarray, maxv: int, path: Path) -> None:
    assert arr.ndim == 2 and arr.dtype == np.uint16
    h, w = arr.shape
    with open(path, "wb") as f:
        f.write(f"P5\n{w} {h}\n{maxv}\n".encode("ascii"))
        if maxv <= 255:
            f.write(arr.astype(np.uint8).tobytes())
        else:
            f.write(arr.astype(">u2").tobytes())


def write_ppm(arr: np.ndarray, maxv: int, path: Path) -> None:
    assert arr.ndim == 3 and arr.shape[2] == 3 and arr.dtype == np.uint16
    h, w, _ = arr.shape
    with open(path, "wb") as f:
        f.write(f"P6\n{w} {h}\n{maxv}\n".encode("ascii"))
        if maxv <= 255:
            f.write(arr.astype(np.uint8).tobytes())
        else:
            f.write(arr.astype(">u2").tobytes())


# ---- cjpeg invocation ------------------------------------------------------


def cjpeg_encode_lossless(arr: np.ndarray, *, P: int, psv: int, pt: int,
                          gray: bool, restart_rows: int,
                          out_path: Path) -> None:
    """Encode a lossless JPEG at precision P with predictor Ps and Pt."""
    out_path.parent.mkdir(parents=True, exist_ok=True)
    suf = ".pgm" if gray else ".ppm"
    maxv = (1 << P) - 1  # cjpeg reads PNM exactly when maxval = 2^P - 1
    with tempfile.NamedTemporaryFile(suffix=suf, delete=False) as pf:
        pnm_path = Path(pf.name)
    try:
        if gray:
            write_pgm(arr, maxv, pnm_path)
        else:
            write_ppm(arr, maxv, pnm_path)
        lossless_arg = f"{psv},{pt}" if pt else f"{psv}"
        cmd = ["cjpeg",
               "-lossless", lossless_arg,
               "-precision", str(P)]
        if restart_rows > 0:
            cmd += ["-restart", str(restart_rows)]
        if gray:
            cmd += ["-grayscale"]
        cmd += ["-outfile", str(out_path), str(pnm_path)]
        subprocess.run(cmd, check=True, capture_output=True)
    finally:
        pnm_path.unlink(missing_ok=True)


def verify_sof3_precision(path: Path, expected_P: int) -> None:
    """Confirm produced file has SOF3 (0xFFC3) with the expected precision."""
    data = path.read_bytes()
    i = 0
    while i < len(data) - 3:
        if data[i] == 0xFF and data[i + 1] == 0xC3:
            length = struct.unpack(">H", data[i + 2:i + 4])[0]
            p = data[i + 4]
            if p != expected_P:
                raise RuntimeError(
                    f"{path}: SOF3 P={p}, expected {expected_P}")
            return
        i += 1
    raise RuntimeError(f"{path}: no SOF3 marker found")


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
    OUT.mkdir(parents=True, exist_ok=True)

    cases: list[tuple[int, int, str, int, int, int, int]] = []
    # (w, h, pattern, P, ps, pt, restart_rows)

    # Grid A: every P ∈ {9..16} × every Ps ∈ {1..7} × gray/RGB gradient × 32x32
    # Keeps the matrix tight: 8 × 7 × 2 = 112 vectors.
    for P in range(9, 17):
        for ps in range(1, 8):
            cases.append((32, 32, "gray_grad", P, ps, 0, 0))
            cases.append((32, 32, "rgb_grad",  P, ps, 0, 0))

    # Grid B: noise stress on every P × Ps ∈ {1, 4, 7} × 32x32.
    # 8 × 3 × 2 = 48 vectors.
    for P in range(9, 17):
        for ps in (1, 4, 7):
            cases.append((32, 32, "gray_noise", P, ps, 0, 0))
            cases.append((32, 32, "rgb_noise",  P, ps, 0, 0))

    # Grid C: non-square sizes × P ∈ {9, 12, 16} × Ps ∈ {1, 7}.
    # 3 × 2 × 2 = 12 vectors.
    for P in (9, 12, 16):
        for ps in (1, 7):
            cases.append((48, 32, "rgb_grad",  P, ps, 0, 0))
            cases.append((48, 32, "gray_check", P, ps, 0, 0))

    # Grid D: Pt > 0 × P ∈ {9, 12, 16} × Ps ∈ {1, 7} × Pt ∈ {1, 2}.
    # 3 × 2 × 2 × 2 = 24 vectors.
    for P in (9, 12, 16):
        for ps in (1, 7):
            for pt in (1, 2):
                cases.append((32, 32, "gray_grad", P, ps, pt, 0))
                cases.append((32, 32, "rgb_grad",  P, ps, pt, 0))

    # Grid E: DRI at P ∈ {12, 16} × Ps ∈ {1, 4, 7} × RGB gradient × 32x32.
    # 2 × 3 × 2 = 12 vectors.  Restart every 2 rows (DRI = 64 MCUs for 32-wide).
    for P in (12, 16):
        for ps in (1, 4, 7):
            cases.append((32, 32, "gray_grad", P, ps, 0, 2))
            cases.append((32, 32, "rgb_grad",  P, ps, 0, 2))

    # --- Low-precision extension grids (P ∈ {2..7}) ---
    # Phase 25a/b/c already covered P=8 extensively (215 vectors). Phase 27
    # grids A..E cover P ∈ {9..16} (208 vectors). These grids fill the ISO
    # H.1.2.1 range down to the minimum allowed precision P=2.
    #
    # Note: at low P the dynamic range is tiny (P=2 → values {0..3}), so the
    # gradient / checker patterns lose visual meaning but still exercise the
    # predictor/wrap/mask paths rigorously.

    # Grid F: every P ∈ {2..7} × every Ps ∈ {1..7} × 32x32 gray+RGB gradient.
    # 6 × 7 × 2 = 84 vectors.
    for P in range(2, 8):
        for ps in range(1, 8):
            cases.append((32, 32, "gray_grad", P, ps, 0, 0))
            cases.append((32, 32, "rgb_grad",  P, ps, 0, 0))

    # Grid G: noise stress on P ∈ {2, 4, 7} × Ps ∈ {1, 4, 7} × 32x32.
    # 3 × 3 × 2 = 18 vectors.
    for P in (2, 4, 7):
        for ps in (1, 4, 7):
            cases.append((32, 32, "gray_noise", P, ps, 0, 0))
            cases.append((32, 32, "rgb_noise",  P, ps, 0, 0))

    # Grid H: Pt > 0 at P ∈ {4, 7} × Ps ∈ {1, 7} × Pt ∈ {1, 2} × gray+RGB.
    # 2 × 2 × 2 × 2 = 16 vectors. Pt < P so both (P=4,Pt=2) and (P=7,Pt=2) OK.
    for P in (4, 7):
        for ps in (1, 7):
            for pt in (1, 2):
                cases.append((32, 32, "gray_grad", P, ps, pt, 0))
                cases.append((32, 32, "rgb_grad",  P, ps, pt, 0))

    # Grid I: DRI at P ∈ {4, 7} × Ps ∈ {1, 7} × gray+RGB gradient × 32x32.
    # 2 × 2 × 2 = 8 vectors. Restart every 2 rows.
    for P in (4, 7):
        for ps in (1, 7):
            cases.append((32, 32, "gray_grad", P, ps, 0, 2))
            cases.append((32, 32, "rgb_grad",  P, ps, 0, 2))

    count = 0
    for (w, h, pat, P, ps, pt, rr) in cases:
        maxv = (1 << P) - 1
        if pat == "gray_grad":
            img = gradient_gray(w, h, maxv); gray = True
        elif pat == "gray_check":
            img = check_gray(w, h, maxv); gray = True
        elif pat == "gray_noise":
            img = noise_gray(w, h, maxv, seed=42 + w + h + P + ps); gray = True
        elif pat == "rgb_grad":
            img = gradient_rgb(w, h, maxv); gray = False
        elif pat == "rgb_check":
            img = check_rgb(w, h, maxv); gray = False
        elif pat == "rgb_noise":
            img = noise_rgb(w, h, maxv, seed=42 + w + h + P + ps); gray = False
        else:
            raise ValueError(pat)

        name = (f"lossless27_P{P:02d}_{pat}_ps{ps}"
                f"_pt{pt}_rr{rr}_{w}x{h}.jpg")
        p = OUT / name
        cjpeg_encode_lossless(img, P=P, psv=ps, pt=pt, gray=gray,
                              restart_rows=rr, out_path=p)
        verify_sof3_precision(p, P)
        verify_djpeg_ok(p)
        count += 1

    print(f"generated {count} Phase 27 vectors under {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
