#!/usr/bin/env python3
"""
Phase 7 test vectors — DRI (Define Restart Interval) != 0.

Uses libjpeg-turbo `cjpeg -restart N` to encode each image with an
RSTn marker every N MCUs. For each of a handful of source images we
produce several variants with different DRI values (1, 2, 4, 8, 16)
so the RTL decoder must correctly handle frequent restarts as well
as large ones. All vectors are 4:2:0, baseline SOF0, 3-component.
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "verification" / "vectors" / "phase07"


def gradient(w: int, h: int) -> np.ndarray:
    x = np.linspace(0, 255, w, dtype=np.float32)[None, :]
    y = np.linspace(0, 255, h, dtype=np.float32)[:, None]
    a = np.clip((x + y) * 0.5, 0, 255).astype(np.uint8)
    b = np.clip(x + 0, 0, 255).astype(np.uint8).repeat(h, 0).reshape(h, w)
    c = np.clip(y + 0, 0, 255).astype(np.uint8).repeat(w, 1).reshape(h, w)
    return np.stack([a, b, c], -1)


def check(w: int, h: int) -> np.ndarray:
    a = np.where(
        (np.add.outer(np.arange(h) // 8, np.arange(w) // 8) % 2) == 0, 255, 32
    ).astype(np.uint8)
    return np.stack([a, 255 - a, a // 2], -1)


def noise(w: int, h: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    return rng.integers(0, 256, size=(h, w, 3), dtype=np.uint8)


def cjpeg_encode(arr: np.ndarray, quality: int, restart: int,
                 out_path: Path) -> None:
    """Pipe a PPM through cjpeg to produce a DRI-enabled JPEG."""
    h, w, _ = arr.shape
    ppm = b"P6\n%d %d\n255\n" % (w, h) + arr.tobytes()
    # cjpeg: -restart N puts RSTn every N MCU rows by default;
    # appending 'B' means every N MCU *blocks* (what we want).
    args = [
        "cjpeg",
        "-quality", str(quality),
        "-sample", "2x2,1x1,1x1",     # force 4:2:0
        "-restart", f"{restart}B",
        "-optimize",
        "-outfile", str(out_path),
    ]
    out_path.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(args, input=ppm, check=True)


def verify_djpeg_ok(path: Path) -> None:
    """Sanity check: libjpeg can decode it back without error."""
    res = subprocess.run(
        ["djpeg", "-outfile", "/dev/null", str(path)],
        capture_output=True,
    )
    if res.returncode != 0:
        raise RuntimeError(f"djpeg rejected {path}: {res.stderr!r}")


def main() -> int:
    # (w, h, pattern, quality, restart_blocks)
    # Mix of:
    #   - very frequent RSTn (1, 2 MCUs)
    #   - moderate (4, 8, 16)
    #   - aligned + non-aligned dims
    cases = [
        (32, 32,   "grad",  80, 1),
        (32, 32,   "grad",  80, 2),
        (32, 32,   "grad",  80, 4),
        (48, 48,   "check", 75, 1),
        (48, 48,   "check", 75, 3),
        (64, 32,   "grad",  50, 2),
        (64, 64,   "noise", 85, 4),
        (64, 64,   "grad",  25, 8),
        (80, 48,   "check", 80, 5),
        (96, 64,   "grad",  70, 6),
        (100, 75,  "grad",  80, 1),
        (100, 75,  "grad",  80, 7),
        (127, 93,  "check", 75, 10),
        (160, 120, "grad",  50, 16),
        (160, 120, "noise", 60, 1),
        (200, 100, "grad",  90, 4),
        (321, 241, "grad",  60, 32),
        (321, 241, "grad",  60, 1),
        (640, 480, "grad",  75, 128),
        (640, 480, "grad",  75, 16),
    ]

    paths = []
    for w, h, pat, q, r in cases:
        if pat == "grad":
            img = gradient(w, h)
        elif pat == "check":
            img = check(w, h)
        else:
            img = noise(w, h, 42)
        name = f"p07_{pat}_{w}x{h}_q{q}_r{r}.jpg"
        p = OUT / name
        cjpeg_encode(img, quality=q, restart=r, out_path=p)
        verify_djpeg_ok(p)
        paths.append(p)

    print(f"generated {len(paths)} Phase 7 test vectors under {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
