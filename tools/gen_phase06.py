#!/usr/bin/env python3
"""
Phase 6 test vectors — non-16-aligned W/H (4:2:0 subsampling only).

Produces ~20 JPEG images where W, H are not multiples of 16, covering
boundary modulos (1, 7, 15, 17, 33, 99, 127, 321, ...) and a mix of
dimensions (square / portrait / landscape / extreme-portrait).

libjpeg handles MCU padding internally when encoding non-aligned images,
so these are valid baseline 4:2:0 JPEGs where SOF0 records the actual W,H.
The RTL decoder (post-Phase 6) must crop its output to exactly W×H.
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "verification" / "vectors" / "phase06"


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


def save_jpg(path: Path, arr: np.ndarray, quality: int = 80) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    img = Image.fromarray(arr, mode="RGB")
    img.save(str(path), format="JPEG", quality=quality, subsampling=2,
             optimize=False, progressive=False)


def main() -> int:
    # Dimension pairs that test all interesting modulos vs 16
    # Each pair (w, h) — at least one is non-multiple of 16
    cases = [
        # modulos: (W%16, H%16)  covering 0,1,7,8,15 mix
        (17, 17,   "grad",  80),   # +1 / +1
        (23, 19,   "grad",  75),   # +7 / +3
        (31, 31,   "grad",  85),   # +15 / +15 (just-below-full)
        (33, 17,   "check", 80),   # +1 / +1 on next mcu
        (40, 24,   "grad",  50),   # +8 / +8
        (47, 33,   "check", 75),   # +15 / +1
        (100, 75,  "grad",  80),   # common photo-like
        (123, 45,  "check", 90),   # random
        (127, 17,  "grad",  70),   # wide-short
        (160, 1,   "grad",  80),   # degenerate 1-row
        (1, 16,    "grad",  80),   # degenerate 1-col
        (17, 100,  "grad",  60),   # tall-narrow
        (99, 99,   "check", 80),   # odd square
        (128, 63,  "grad",  75),   # W mcu-aligned, H -1
        (321, 241, "grad",  50),   # phone-photo-esque
        (640, 479, "grad",  80),   # VGA -1 row
        (800, 600, "grad",  80),   # SVGA (already aligned but tests path)
        (1023, 767,"grad",  70),   # XGA -1
        (1919, 1079,"grad", 50),   # FHD -1x1
        (3839, 2159,"grad", 85),   # 4K -1x1
    ]

    paths = []
    for w, h, pat, q in cases:
        if pat == "grad":
            img = gradient(w, h)
        elif pat == "check":
            img = check(w, h)
        else:
            img = noise(w, h, 42)
        name = f"p06_{pat}_{w}x{h}_q{q}.jpg"
        p = OUT / name
        save_jpg(p, img, quality=q)
        paths.append(p)

    print(f"generated {len(paths)} Phase 6 test vectors under {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
