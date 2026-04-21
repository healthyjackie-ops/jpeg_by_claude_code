#!/usr/bin/env python3
"""
Phase 12c-arith test vectors — SOF9 / SOF10 CMYK.

PIL doesn't emit arithmetic-coded JPEG directly (its `arithmetic=True`
kwarg is silently ignored by Pillow's libjpeg binding). Workflow:

    PIL  →  baseline CMYK (SOF0, Huffman)
    jpegtran -arithmetic                → SOF9  (sequential arith)
    jpegtran -arithmetic -progressive   → SOF10 (progressive arith)
    optional: -restart N   → add DRI

libjpeg-turbo must be built with --with-arith-{enc,dec} (default on for
jpeg-turbo 3.x homebrew build).
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
OUT9  = ROOT / "verification" / "vectors" / "phase12c_sof9"
OUT10 = ROOT / "verification" / "vectors" / "phase12c_sof10"


def gradient_cmyk(w: int, h: int) -> np.ndarray:
    x = np.linspace(0, 255, w, dtype=np.float32)[None, :]
    y = np.linspace(0, 255, h, dtype=np.float32)[:, None]
    c = np.clip(x, 0, 255).astype(np.uint8)
    m = np.clip(y, 0, 255).astype(np.uint8)
    ycomp = np.clip(255 - (x + y) * 0.5, 0, 255).astype(np.uint8)
    k = np.clip((x + y) * 0.25, 0, 255).astype(np.uint8)
    c = np.broadcast_to(c, (h, w))
    m = np.broadcast_to(m, (h, w))
    ycomp = np.broadcast_to(ycomp, (h, w))
    k = np.broadcast_to(k, (h, w))
    return np.stack([c, m, ycomp, k], axis=-1).astype(np.uint8)


def check_cmyk(w: int, h: int) -> np.ndarray:
    checker = (np.add.outer(np.arange(h) // 8, np.arange(w) // 8) % 2) == 0
    col0 = np.array([200, 50,  40, 10], dtype=np.uint8)
    col1 = np.array([40,  180, 60, 30], dtype=np.uint8)
    return np.where(checker[..., None], col0, col1).astype(np.uint8)


def noise_cmyk(w: int, h: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    return rng.integers(0, 256, size=(h, w, 4), dtype=np.uint8)


def pil_baseline_cmyk(arr: np.ndarray, quality: int, out_path: Path) -> None:
    img = Image.fromarray(arr, mode="CMYK")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    img.save(out_path, format="JPEG", quality=quality, subsampling=0, optimize=True)


def jpegtran_convert(in_path: Path, out_path: Path,
                     *, progressive: bool, restart_mcus: int) -> None:
    args = ["jpegtran", "-arithmetic"]
    if progressive:
        args += ["-progressive"]
    if restart_mcus > 0:
        args += ["-restart", str(restart_mcus)]
    args += ["-outfile", str(out_path), str(in_path)]
    subprocess.run(args, check=True)


def verify_djpeg_ok(path: Path) -> None:
    res = subprocess.run(
        ["djpeg", "-outfile", "/dev/null", str(path)],
        capture_output=True,
    )
    if res.returncode != 0:
        raise RuntimeError(f"djpeg rejected {path}: {res.stderr!r}")


def main() -> int:
    # (w, h, pattern, quality, restart_mcus)
    cases = [
        (8,   8,   "grad",  75, 0),
        (16,  16,  "check", 80, 0),
        (32,  32,  "grad",  60, 0),
        (64,  64,  "noise", 85, 0),
        (128, 64,  "grad",  50, 0),
        (9,   9,   "grad",  80, 0),
        (23,  17,  "check", 70, 0),
        (57,  39,  "grad",  50, 0),
        (97,  113, "noise", 60, 0),
        (241, 321, "grad",  75, 0),
        (64,  64,  "grad",  70, 1),
        (96,  96,  "check", 75, 4),
        (128, 128, "grad",  50, 16),
        (241, 321, "noise", 60, 32),
        (199, 257, "grad",  55, 0),
        (48,  200, "check", 65, 2),
        (200, 48,  "noise", 78, 8),
    ]

    # One baseline temp → two conversions (SOF9 + SOF10).
    import tempfile
    total = 0
    for w, h, pat, q, r in cases:
        if pat == "grad":
            img = gradient_cmyk(w, h)
        elif pat == "check":
            img = check_cmyk(w, h)
        else:
            img = noise_cmyk(w, h, 42)
        with tempfile.NamedTemporaryFile(suffix=".jpg", delete=False) as f:
            base = Path(f.name)
        try:
            pil_baseline_cmyk(img, q, base)

            p9 = OUT9 / f"p12c_sof9_{pat}_{w}x{h}_q{q}_r{r}.jpg"
            p9.parent.mkdir(parents=True, exist_ok=True)
            jpegtran_convert(base, p9, progressive=False, restart_mcus=r)
            verify_djpeg_ok(p9)

            p10 = OUT10 / f"p12c_sof10_{pat}_{w}x{h}_q{q}_r{r}.jpg"
            p10.parent.mkdir(parents=True, exist_ok=True)
            jpegtran_convert(base, p10, progressive=True, restart_mcus=r)
            verify_djpeg_ok(p10)

            total += 2
        finally:
            base.unlink(missing_ok=True)

    print(f"generated {total} Phase 12c arith vectors "
          f"({len(cases)} SOF9 + {len(cases)} SOF10)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
