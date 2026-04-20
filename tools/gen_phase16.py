#!/usr/bin/env python3
"""
Phase 16b test vectors — progressive SOF2, single DC-only scan.

Uses libjpeg-turbo `cjpeg -progressive -scans <script>` to emit JPEGs whose
only scan has Ss=Se=0, Ah=Al=0. Each 8x8 block therefore carries a single
DC coefficient; AC coefficients are zero. Decoded output is a block-averaged
version of the input — perfectly valid, just visually blocky.

Coverage:
  - grayscale (1 component), MCU=8x8
  - 4:4:4 (3 components, H=V=1), MCU=8x8
  - 4:2:0 (3 components, H=V=2/1/1), MCU=16x16

Each mode mixes 8-aligned + non-aligned dims, multiple patterns, multiple
quality levels. libjpeg-turbo is used as golden reference (djpeg must decode
each vector cleanly).
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "verification" / "vectors" / "phase16"


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


def cjpeg_encode_dc_only(arr: np.ndarray, *, quality: int,
                         chroma: str, out_path: Path) -> None:
    """Encode with cjpeg in progressive mode, single DC-only scan."""
    ch = 1 if arr.ndim == 2 else 3
    if chroma == "gray":
        assert ch == 1, "gray needs 1-channel input"
        scan_script = "0: 0-0, 0, 0;\n"
    elif chroma in ("420", "444"):
        assert ch == 3, f"{chroma} needs 3-channel input"
        scan_script = "0 1 2: 0-0, 0, 0;\n"
    else:
        raise ValueError(f"bad chroma {chroma!r}")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", suffix=".scans", delete=False) as sf:
        sf.write(scan_script)
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
    # (w, h, pattern, quality, chroma)
    cases = [
        # Grayscale — MCU 8x8
        (8,   8,   "grad",  75, "gray"),
        (16,  16,  "check", 80, "gray"),
        (64,  32,  "grad",  60, "gray"),
        (17,  13,  "grad",  80, "gray"),
        (100, 75,  "noise", 60, "gray"),
        # 4:4:4 — MCU 8x8
        (8,   8,   "grad",  80, "444"),
        (16,  16,  "check", 70, "444"),
        (40,  32,  "grad",  75, "444"),
        (97,  73,  "noise", 65, "444"),
        # 4:2:0 — MCU 16x16
        (16,  16,  "grad",  75, "420"),
        (32,  32,  "check", 70, "420"),
        (64,  48,  "grad",  60, "420"),
        (100, 76,  "noise", 60, "420"),
        (128, 64,  "grad",  80, "420"),
    ]

    OUT.mkdir(parents=True, exist_ok=True)
    for w, h, pat, q, chroma in cases:
        ch = 1 if chroma == "gray" else 3
        if pat == "grad":
            img = gradient(w, h, ch)
        elif pat == "check":
            img = check(w, h, ch)
        else:
            img = noise(w, h, ch, seed=42 + w + h)
        name = f"p16_{chroma}_{pat}_{w}x{h}_q{q}.jpg"
        p = OUT / name
        cjpeg_encode_dc_only(img, quality=q, chroma=chroma, out_path=p)
        verify_djpeg_ok(p)

    print(f"generated {len(cases)} Phase 16 DC-only vectors under {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
