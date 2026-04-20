#!/usr/bin/env python3
"""
Phase 17a test vectors — progressive SOF2 with spectral-selection (Ah=0) AC
scans.

Each file is encoded via `cjpeg -progressive -scans <script>` with a custom
script that yields one DC scan (interleaved across components) followed by
one or more AC scans (non-interleaved per component) — all at Ah=0. This
matches Phase 17a scope (no refinement) while exercising EOB run length,
ZRL, multi-band AC, and the drain FSM.

Coverage:
  - grayscale (1 component)         DC + {Y AC full}  (2 scans)
                                    DC + {Y AC 1-5, 6-63} (3 scans)
  - 4:4:4  (Y,Cb,Cr at H=V=1)       DC + {Y,Cb,Cr AC full} (4 scans)
                                    DC + {Y 1-5, 6-63, Cb full, Cr full} (5)
  - 4:2:0  (Y at H=V=2)             DC + {Y full, Cb full, Cr full} (4)
                                    DC + {Y 1-5, 6-63, Cb full, Cr full} (5)
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "verification" / "vectors" / "phase17"


def gradient(w: int, h: int, ch: int) -> np.ndarray:
    x = np.linspace(0, 255, w, dtype=np.float32)[None, :]
    y = np.linspace(0, 255, h, dtype=np.float32)[:, None]
    if ch == 1:
        return np.clip((x + y) * 0.5, 0, 255).astype(np.uint8).reshape(h, w)
    r = np.clip(x, 0, 255).astype(np.uint8)
    g = np.clip(y, 0, 255).astype(np.uint8)
    b = np.clip((x + y) * 0.5, 0, 255).astype(np.uint8)
    r = np.broadcast_to(r, (h, w))
    g = np.broadcast_to(g, (h, w))
    return np.stack([r, g, b], axis=-1)


def check(w: int, h: int, ch: int) -> np.ndarray:
    mask = (np.add.outer(np.arange(h) // 8, np.arange(w) // 8) % 2) == 0
    a = np.where(mask, 240, 16).astype(np.uint8)
    if ch == 1:
        return a
    b = np.where(mask, 16, 240).astype(np.uint8)
    c = np.where(mask, 128, 200).astype(np.uint8)
    return np.stack([a, b, c], axis=-1)


def noise(w: int, h: int, ch: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    if ch == 1:
        return rng.integers(0, 256, size=(h, w), dtype=np.uint8)
    return rng.integers(0, 256, size=(h, w, 3), dtype=np.uint8)


def write_pnm(arr: np.ndarray, p: Path) -> None:
    h = arr.shape[0]
    w = arr.shape[1]
    if arr.ndim == 2:
        header = b"P5\n%d %d\n255\n" % (w, h)
    else:
        header = b"P6\n%d %d\n255\n" % (w, h)
    p.write_bytes(header + arr.tobytes())


def scan_script(chroma: str, bands: str) -> str:
    """Build a cjpeg -scans script.
    bands is 'full' (1 AC scan per component) or 'split' (2 Y scans + chroma).
    """
    lines: list[str] = []
    if chroma == "gray":
        lines.append("0: 0-0, 0, 0;")  # DC
        if bands == "full":
            lines.append("0: 1-63, 0, 0;")
        else:  # split
            lines.append("0: 1-5, 0, 0;")
            lines.append("0: 6-63, 0, 0;")
    else:
        lines.append("0 1 2: 0-0, 0, 0;")  # DC interleaved
        if bands == "full":
            lines.append("0: 1-63, 0, 0;")
            lines.append("1: 1-63, 0, 0;")
            lines.append("2: 1-63, 0, 0;")
        else:  # split
            lines.append("0: 1-5, 0, 0;")
            lines.append("0: 6-63, 0, 0;")
            lines.append("1: 1-63, 0, 0;")
            lines.append("2: 1-63, 0, 0;")
    return "\n".join(lines) + "\n"


def cjpeg_encode(arr: np.ndarray, *, quality: int,
                 chroma: str, bands: str, out_path: Path) -> None:
    ch = 1 if arr.ndim == 2 else 3
    if chroma == "gray":
        assert ch == 1
    else:
        assert ch == 3

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", suffix=".scans", delete=False) as sf:
        sf.write(scan_script(chroma, bands))
        scans_path = sf.name
    try:
        with tempfile.NamedTemporaryFile(suffix=".pnm", delete=False) as pf:
            pnm_path = Path(pf.name)
        write_pnm(arr, pnm_path)

        args = ["cjpeg", "-progressive", "-scans", scans_path,
                "-quality", str(quality), "-optimize"]
        if chroma == "gray":
            args += ["-grayscale"]
        elif chroma == "420":
            args += ["-sample", "2x2,1x1,1x1"]
        elif chroma == "444":
            args += ["-sample", "1x1,1x1,1x1"]
        args += ["-outfile", str(out_path), str(pnm_path)]
        subprocess.run(args, check=True)
    finally:
        Path(scans_path).unlink(missing_ok=True)
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
    # (w, h, pattern, quality, chroma, bands)
    cases = [
        # Grayscale
        (8,   8,   "grad",  75, "gray", "full"),
        (16,  16,  "check", 80, "gray", "full"),
        (64,  32,  "grad",  60, "gray", "split"),
        (17,  13,  "grad",  80, "gray", "full"),
        (100, 75,  "noise", 60, "gray", "split"),
        # 4:4:4
        (8,   8,   "grad",  80, "444", "full"),
        (16,  16,  "check", 70, "444", "full"),
        (40,  32,  "grad",  75, "444", "split"),
        (97,  73,  "noise", 65, "444", "split"),
        # 4:2:0
        (16,  16,  "grad",  75, "420", "full"),
        (32,  32,  "check", 70, "420", "full"),
        (64,  48,  "grad",  60, "420", "split"),
        (100, 76,  "noise", 60, "420", "split"),
        (128, 64,  "grad",  80, "420", "full"),
        (192, 128, "noise", 55, "420", "split"),
        (96,  64,  "check", 70, "420", "split"),
    ]

    OUT.mkdir(parents=True, exist_ok=True)
    for w, h, pat, q, chroma, bands in cases:
        ch = 1 if chroma == "gray" else 3
        if pat == "grad":
            img = gradient(w, h, ch)
        elif pat == "check":
            img = check(w, h, ch)
        else:
            img = noise(w, h, ch, seed=42 + w + h)
        name = f"p17_{chroma}_{pat}_{bands}_{w}x{h}_q{q}.jpg"
        p = OUT / name
        cjpeg_encode(img, quality=q, chroma=chroma, bands=bands, out_path=p)
        verify_djpeg_ok(p)

    print(f"generated {len(cases)} Phase 17a progressive vectors under {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
