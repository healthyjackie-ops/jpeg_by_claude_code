#!/usr/bin/env python3
"""
Phase 17d test vectors — SOF2 progressive Huffman on extended chroma
layouts 4:2:2 / 4:4:0 / 4:1:1.

Phases 17a-c covered gray / 4:4:4 / 4:2:0 only for the progressive Huffman
path. Phase 24c already extended the arith (SOF9/SOF10) path to 422/440/411,
creating an asymmetry: arith progressive supports more chroma modes than
Huffman progressive. This phase closes that gap by exercising SOF2 decode
across the three remaining 3-comp YCbCr layouts:

    4:2:2 → Y 2x1, chroma 1x1   MCU 16×8   (2 Y blocks horizontally)
    4:4:0 → Y 1x2, chroma 1x1   MCU 8×16   (2 Y blocks vertically)
    4:1:1 → Y 4x1, chroma 1x1   MCU 32×8   (4 Y blocks horizontally)

Each vector is encoded by cjpeg with -progressive and the matching
-sample flag; coverage spans all 4 progressive scan types:
  - DC-first (Al=1)                 : cjpeg default script
  - AC-first  (Ss>0, Ah=0)          : cjpeg default script (Al=2)
  - AC-refine (Ss>0, Ah>0)          : cjpeg default script (Al=1,0)
  - DC-refine (Ss=0, Ah>0)          : cjpeg default script (Al=0)

cjpeg's *default* progressive script emits all 4 scan types per component,
so a single encode exercises every path. Phase 17d therefore uses cjpeg's
implicit script (no -scans) and just varies image size / DRI / quality.

Each file is round-tripped through djpeg to confirm libjpeg-turbo accepts
it — that's our parity target.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "verification" / "vectors" / "phase17d"


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
    h, w, _ = arr.shape
    header = b"P6\n%d %d\n255\n" % (w, h)
    data = header + arr.tobytes()
    # Progressive Huffman path: -progressive, no -arithmetic.
    args = ["cjpeg", "-progressive", "-quality", str(quality), "-optimize"]
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
    # 4:4:0 requires H ≥ 16 MCU; 4:1:1 requires W ≥ 32 MCU. Shape sweep
    # mixes MCU-aligned and misaligned.
    chroma_modes = [
        ("422", "2x1,1x1,1x1"),
        ("440", "1x2,1x1,1x1"),
        ("411", "4x1,1x1,1x1"),
    ]
    size_cases = [
        (32,   16,  75, 0),
        (64,   32,  80, 0),
        (100,  75,  60, 0),
        (17,   13,  80, 0),     # non-MCU-aligned both axes
        (128,  96,  70, 4),     # DRI=4 MCUs
        (200, 150,  30, 0),     # low quality → many-EOB
        (160, 120,  85, 8),     # DRI=8
        (256, 256,  90, 0),     # larger, high quality
        (320, 200,  75, 1),     # DRI=1 per MCU
    ]

    cases: list[tuple[str, str, str, int, int, int, int]] = []
    for mode, samp in chroma_modes:
        for (w, h, q, r) in size_cases:
            # Per-mode minimum dims so every case has ≥ 1 full MCU.
            if mode == "440" and h < 16:
                h = 16
            if mode == "411" and w < 32:
                w = 32
            for pat in ("grad", "check", "noise"):
                cases.append((mode, samp, pat, w, h, q, r))

    for mode, samp, pat, w, h, q, r in cases:
        if pat == "grad":
            img = gradient_rgb(w, h)
        elif pat == "check":
            img = check_rgb(w, h)
        else:
            img = noise_rgb(w, h, 42)

        name = f"p17d_{mode}_{pat}_{w}x{h}_q{q}_r{r}.jpg"
        p = OUT / name
        cjpeg_encode(img, sampling=samp, quality=q, restart=r, out_path=p)
        verify_djpeg_ok(p)

    print(f"generated {len(cases)} Phase 17d vectors under {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
