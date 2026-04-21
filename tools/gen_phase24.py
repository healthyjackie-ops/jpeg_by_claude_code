#!/usr/bin/env python3
"""
Phase 24a test vectors — SOF10 progressive arithmetic coding.

Drives libjpeg-turbo cjpeg with ``-progressive -arithmetic`` (emits SOF10
marker; each scan is arith-entropy-coded). cjpeg's default progressive
script runs a sequence of scans per component:

    DC-first (Al=1) → AC band split Ss=1..5 (Al=2) → Ss=6..63 (Al=2)
                    → AC refine Ss=1..63 (Ah=2 Al=1)
                    → DC-refine (Ah=1 Al=0)
                    → AC refine Ss=1..63 (Ah=1 Al=0)

So every vector exercises all 4 progressive-arith scan types (DC-first,
DC-refine, AC-first, AC-refine) against our C-model decoder.

Covers:
- 8-bit gray  (1 comp)
- 8-bit 4:4:4 (3 comp, full-res chroma)
- 8-bit 4:2:0 (3 comp, 2×2 chroma subsample — Y blocks interleaved)
- DRI > 0 sweeps per chroma mode (stresses RSTn inside every scan type)

Output goes under ``verification/vectors/phase24/``. Each file is
round-tripped through djpeg to confirm libjpeg-turbo can decode it —
that's our parity target for the C model.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "verification" / "vectors" / "phase24"


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
    """Run cjpeg -progressive -arithmetic on a PGM/PPM input."""
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
        "-progressive",
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

    # ---- Gray (1-comp, Nf=1) ---------------------------------------
    # (mode, pattern, w, h, quality, restart)
    gray_cases = [
        ("gray", "grad",  8,   8,   75, 0),
        ("gray", "check", 16,  16,  80, 0),
        ("gray", "grad",  64,  32,  60, 0),
        ("gray", "grad",  17,  13,  80, 0),    # non-MCU-aligned edge
        ("gray", "noise", 100, 75,  50, 0),
        ("gray", "grad",  128, 96,  70, 4),    # DRI=4 MCUs
        # Phase 24a hardening: larger sizes, more DRI variants, patterns
        ("gray", "noise", 256, 256, 90, 0),
        ("gray", "check", 200, 150, 30, 0),    # low quality stresses many-EOB
        ("gray", "grad",  320, 200, 75, 1),    # DRI=1 (restart per MCU)
        ("gray", "noise", 160, 120, 85, 8),    # DRI=8
        ("gray", "noise", 128, 96,  60, 16),   # DRI=16
    ]
    cases.extend(gray_cases)

    # ---- 4:4:4 (3 comp, full-res chroma) ----------------------------
    c444_cases = [
        ("444", "grad",  8,   8,   75, 0),
        ("444", "check", 16,  16,  80, 0),
        ("444", "noise", 32,  32,  60, 0),
        ("444", "grad",  17,  13,  80, 0),    # non-MCU-aligned
        ("444", "grad",  100, 75,  50, 0),
        ("444", "noise", 128, 96,  70, 4),    # DRI=4 MCUs
        # Phase 24a hardening
        ("444", "noise", 256, 256, 90, 0),
        ("444", "check", 200, 150, 30, 0),
        ("444", "grad",  320, 200, 75, 1),    # DRI=1 (restart per MCU)
        ("444", "noise", 160, 120, 85, 8),
        ("444", "noise", 128, 96,  60, 16),
    ]
    cases.extend(c444_cases)

    # ---- 4:2:0 (3 comp, chroma subsampled 2x2) ----------------------
    c420_cases = [
        ("420", "grad",  16,  16,  75, 0),
        ("420", "check", 32,  32,  80, 0),
        ("420", "noise", 64,  64,  60, 0),
        ("420", "grad",  17,  13,  80, 0),    # non-MCU-aligned
        ("420", "grad",  100, 75,  50, 0),
        ("420", "noise", 128, 96,  70, 4),    # DRI=4 MCUs
        # Phase 24a hardening
        ("420", "noise", 256, 256, 90, 0),
        ("420", "check", 200, 150, 30, 0),
        ("420", "grad",  320, 200, 75, 1),    # DRI=1 (restart per MCU)
        ("420", "noise", 160, 120, 85, 8),
        ("420", "noise", 128, 96,  60, 16),
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

        name = f"p24_{mode}_{pat}_{w}x{h}_q{q}_r{r}.jpg"
        p = OUT / name
        cjpeg_encode(img, sampling=sampling_flag(mode), quality=q,
                     restart=r, out_path=p)
        verify_djpeg_ok(p)

    print(f"generated {len(cases)} Phase 24 SOF10 vectors under {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
