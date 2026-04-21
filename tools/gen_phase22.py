#!/usr/bin/env python3
"""
Phase 22c/23b test vectors — SOF9 sequential arithmetic coding.

Drives libjpeg-turbo cjpeg with ``-arithmetic`` (emits SOF9 marker and
arith entropy-coded segments instead of SOF0 + Huffman). Covers:

- 8-bit gray (1 comp)   — single DC + AC table.
- 8-bit 4:4:4 (3 comp)  — all components at full res, 3 Y+Cb+Cr tables.
- 8-bit 4:2:0 (3 comp)  — 4 Y blocks + Cb + Cr per MCU.
- DRI>0 sweeps over each chroma mode to stress RSTn arith re-prime.

Output goes under ``verification/vectors/phase22/``. Each file is
round-tripped through djpeg to confirm libjpeg-turbo can decode it —
that's our parity target for the C model.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "verification" / "vectors" / "phase22"


def gradient_gray(w: int, h: int) -> np.ndarray:
    x = np.linspace(0, 255, w, dtype=np.float32)[None, :]
    y = np.linspace(0, 255, h, dtype=np.float32)[:, None]
    g = np.clip((x + y) * 0.5, 0, 255).astype(np.uint8)
    return np.broadcast_to(g, (h, w)).astype(np.uint8)


def check_gray(w: int, h: int) -> np.ndarray:
    checker = (np.add.outer(np.arange(h) // 8, np.arange(w) // 8) % 2) == 0
    return np.where(checker, 220, 40).astype(np.uint8)


def noise_gray(w: int, h: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    return rng.integers(0, 256, size=(h, w), dtype=np.uint8)


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
                 restart: int, out_path: Path) -> None:
    """Run cjpeg -arithmetic on a PGM/PPM input."""
    if arr.ndim == 2:
        h, w = arr.shape
        header = b"P5\n%d %d\n255\n" % (w, h)
        data = header + arr.tobytes()
    else:
        h, w, _ = arr.shape
        header = b"P6\n%d %d\n255\n" % (w, h)
        data = header + arr.tobytes()

    args = [
        "cjpeg",
        "-arithmetic",
        "-quality", str(quality),
    ]
    if sampling:
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
    cases = []

    # ---- Gray (1-comp), Nf=1 ---------------------------------------
    # (mode, pattern, w, h, quality, restart)
    gray_cases = [
        ("gray", "grad",  8,   8,   75, 0),
        ("gray", "check", 16,  16,  80, 0),
        ("gray", "grad",  64,  32,  60, 0),
        ("gray", "grad",  17,  13,  80, 0),
        ("gray", "noise", 100, 75,  50, 0),
        ("gray", "grad",  128, 96,  70, 4),   # DRI
    ]
    cases.extend(gray_cases)

    # ---- 4:4:4 (3-comp, full-res chroma) ----------------------------
    c444_cases = [
        ("444", "grad",  8,   8,   75, 0),
        ("444", "check", 16,  16,  80, 0),
        ("444", "noise", 32,  32,  60, 0),
        ("444", "grad",  17,  13,  80, 0),
        ("444", "grad",  100, 75,  50, 0),
        ("444", "noise", 128, 96,  70, 4),   # DRI
    ]
    cases.extend(c444_cases)

    # ---- 4:2:0 (3-comp, chroma subsampled 2x2) ----------------------
    c420_cases = [
        ("420", "grad",  16,  16,  75, 0),
        ("420", "check", 32,  32,  80, 0),
        ("420", "noise", 64,  64,  60, 0),
        ("420", "grad",  17,  13,  80, 0),    # non-MCU-aligned
        ("420", "grad",  100, 75,  50, 0),
        ("420", "noise", 128, 96,  70, 4),   # DRI
    ]
    cases.extend(c420_cases)

    def sampling_flag(mode: str) -> str:
        if mode == "gray":
            return ""
        if mode == "444":
            return "1x1,1x1,1x1"
        if mode == "420":
            return "2x2,1x1,1x1"
        raise ValueError(mode)

    for mode, pat, w, h, q, r in cases:
        if mode == "gray":
            if pat == "grad":
                img = gradient_gray(w, h)
            elif pat == "check":
                img = check_gray(w, h)
            else:
                img = noise_gray(w, h, 42)
        else:
            if pat == "grad":
                img = gradient_rgb(w, h)
            elif pat == "check":
                img = check_rgb(w, h)
            else:
                img = noise_rgb(w, h, 42)

        name = f"p22_{mode}_{pat}_{w}x{h}_q{q}_r{r}.jpg"
        p = OUT / name
        cjpeg_encode(img, sampling=sampling_flag(mode), quality=q,
                     restart=r, out_path=p)
        verify_djpeg_ok(p)

    print(f"generated {len(cases)} Phase 22 SOF9 vectors under {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
