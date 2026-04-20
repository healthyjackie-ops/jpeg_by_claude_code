#!/usr/bin/env python3
"""
Phase 18 test vectors — progressive SOF2 with **successive approximation
refinement** (Ah>0). Each vector exercises either libjpeg's default
progressive script (which includes Al>0 point-transform + refinement
scans) or a hand-tuned `-scans` script that forces specific refinement
patterns.

Coverage:
  - libjpeg default `-progressive` (10 scans: DC Al=0 → DC refine Ah=1..0,
    AC Al=2 bands → AC refine Ah=2..0) for gray / 4:4:4 / 4:2:0
  - Custom script: single DC Al=1 then DC refine Ah=1→0, plus one AC band
    Al=1 then AC refine Ah=1→0 (smaller scan count, same coverage)
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "verification" / "vectors" / "phase18"


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


def script_default_progressive(chroma: str) -> str:
    """libjpeg-turbo's default progressive script (matches cjpeg -progressive
    without an explicit -scans). 10 scans for color, 6 for grayscale."""
    if chroma == "gray":
        return "\n".join([
            "0: 0-0, 0, 1;",        # DC Al=1 (point transform)
            "0: 1-5, 0, 2;",        # Y AC 1-5 Al=2
            "0: 6-63, 0, 2;",       # Y AC 6-63 Al=2
            "0: 1-63, 2, 1;",       # Y AC 1-63 refine Ah=2 Al=1
            "0: 0-0, 1, 0;",        # DC refine Ah=1 Al=0
            "0: 1-63, 1, 0;",       # Y AC 1-63 refine Ah=1 Al=0
        ]) + "\n"
    return "\n".join([
        "0 1 2: 0-0, 0, 1;",        # DC interleaved Al=1
        "0: 1-5, 0, 2;",            # Y AC 1-5 Al=2
        "2: 1-63, 0, 1;",           # Cr AC 1-63 Al=1
        "1: 1-63, 0, 1;",           # Cb AC 1-63 Al=1
        "0: 6-63, 0, 2;",           # Y AC 6-63 Al=2
        "0: 1-63, 2, 1;",           # Y AC refine Ah=2 Al=1
        "2: 1-63, 1, 0;",           # Cr AC refine Ah=1 Al=0
        "1: 1-63, 1, 0;",           # Cb AC refine Ah=1 Al=0
        "0: 1-63, 1, 0;",           # Y AC refine Ah=1 Al=0
        "0 1 2: 0-0, 1, 0;",        # DC refine Ah=1 Al=0
    ]) + "\n"


def script_minimal_refine(chroma: str) -> str:
    """Smaller script exercising just one AC band + refinement for each
    component, plus DC refinement."""
    if chroma == "gray":
        return "\n".join([
            "0: 0-0, 0, 1;",
            "0: 1-63, 0, 1;",
            "0: 0-0, 1, 0;",
            "0: 1-63, 1, 0;",
        ]) + "\n"
    return "\n".join([
        "0 1 2: 0-0, 0, 1;",
        "0: 1-63, 0, 1;",
        "1: 1-63, 0, 1;",
        "2: 1-63, 0, 1;",
        "0 1 2: 0-0, 1, 0;",
        "0: 1-63, 1, 0;",
        "1: 1-63, 1, 0;",
        "2: 1-63, 1, 0;",
    ]) + "\n"


def cjpeg_encode(arr: np.ndarray, *, quality: int,
                 chroma: str, script: str, out_path: Path) -> None:
    ch = 1 if arr.ndim == 2 else 3
    if chroma == "gray":
        assert ch == 1
    else:
        assert ch == 3

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", suffix=".scans", delete=False) as sf:
        sf.write(script)
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
    # (w, h, pattern, quality, chroma, script_kind)
    cases = [
        # Grayscale — default progressive (6 scans)
        (8,   8,   "grad",  80, "gray", "default"),
        (17,  13,  "grad",  80, "gray", "default"),
        (64,  32,  "check", 70, "gray", "default"),
        (100, 75,  "noise", 60, "gray", "default"),
        # Grayscale — minimal refine script (4 scans)
        (16,  16,  "grad",  75, "gray", "minimal"),
        (64,  48,  "noise", 60, "gray", "minimal"),
        # 4:4:4 — default progressive (10 scans)
        (8,   8,   "grad",  80, "444", "default"),
        (16,  16,  "check", 70, "444", "default"),
        (40,  32,  "grad",  75, "444", "default"),
        (97,  73,  "noise", 65, "444", "default"),
        # 4:4:4 — minimal refine (8 scans)
        (32,  32,  "grad",  70, "444", "minimal"),
        # 4:2:0 — default progressive (10 scans)
        (16,  16,  "grad",  75, "420", "default"),
        (32,  32,  "check", 70, "420", "default"),
        (64,  48,  "grad",  60, "420", "default"),
        (100, 76,  "noise", 60, "420", "default"),
        (192, 128, "noise", 55, "420", "default"),
        # 4:2:0 — minimal refine (8 scans)
        (96,  64,  "check", 70, "420", "minimal"),
    ]

    OUT.mkdir(parents=True, exist_ok=True)
    for w, h, pat, q, chroma, kind in cases:
        ch = 1 if chroma == "gray" else 3
        if pat == "grad":
            img = gradient(w, h, ch)
        elif pat == "check":
            img = check(w, h, ch)
        else:
            img = noise(w, h, ch, seed=42 + w + h)
        if kind == "default":
            sc = script_default_progressive(chroma)
        else:
            sc = script_minimal_refine(chroma)
        name = f"p18_{chroma}_{pat}_{kind}_{w}x{h}_q{q}.jpg"
        p = OUT / name
        cjpeg_encode(img, quality=q, chroma=chroma, script=sc, out_path=p)
        verify_djpeg_ok(p)

    print(f"generated {len(cases)} Phase 18 refinement vectors under {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
