#!/usr/bin/env python3
"""
Phase 17c / 18c test vectors — **progressive SOF2 + DRI (restart interval)**.

Exercises the restart-marker path inside each progressive scan:
  - DC interleaved scans: RST every N MCUs (MCU = 6 blocks for 4:2:0, 1 block
    per-component for gray/4:4:4).
  - AC non-interleaved scans: RST every N blocks (ISO F.1.2.3 — the MCU of
    a non-interleaved scan is a single data unit).

`cjpeg -restart NB` applies a global DRI marker; the same interval is enforced
by the encoder for every scan in the progressive script — which means short DRI
(1–2) stresses many restarts per AC scan, while larger DRI (8–32) mimics what a
real encoder would emit.

Matrix mixes:
  - spectral-only script (all Ah=0) → Phase 17c path
  - default + minimal refinement scripts (Ah>0) → Phase 18c path
  - gray / 4:4:4 / 4:2:0
  - MCU-aligned + non-aligned (forces Y natural != MCU-padded)
  - DRI 1, 2, 4, 8, 16
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "verification" / "vectors" / "phase_prog_dri"


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


# ---------------------------- scan scripts ------------------------------------


def script_spectral_only(chroma: str) -> str:
    """All Ah=0, Al=0 — exercises Phase 17a path but under DRI."""
    if chroma == "gray":
        return "\n".join([
            "0: 0-0, 0, 0;",       # DC
            "0: 1-5, 0, 0;",       # AC band 1
            "0: 6-63, 0, 0;",      # AC band 2
        ]) + "\n"
    return "\n".join([
        "0 1 2: 0-0, 0, 0;",       # DC interleaved
        "0: 1-5, 0, 0;",           # Y AC 1-5
        "0: 6-63, 0, 0;",          # Y AC 6-63
        "1: 1-63, 0, 0;",          # Cb AC
        "2: 1-63, 0, 0;",          # Cr AC
    ]) + "\n"


def script_default_progressive(chroma: str) -> str:
    """libjpeg-turbo default progressive (exercises Phase 18a refinement)."""
    if chroma == "gray":
        return "\n".join([
            "0: 0-0, 0, 1;",
            "0: 1-5, 0, 2;",
            "0: 6-63, 0, 2;",
            "0: 1-63, 2, 1;",
            "0: 0-0, 1, 0;",
            "0: 1-63, 1, 0;",
        ]) + "\n"
    return "\n".join([
        "0 1 2: 0-0, 0, 1;",
        "0: 1-5, 0, 2;",
        "2: 1-63, 0, 1;",
        "1: 1-63, 0, 1;",
        "0: 6-63, 0, 2;",
        "0: 1-63, 2, 1;",
        "2: 1-63, 1, 0;",
        "1: 1-63, 1, 0;",
        "0: 1-63, 1, 0;",
        "0 1 2: 0-0, 1, 0;",
    ]) + "\n"


def script_minimal_refine(chroma: str) -> str:
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


# ---------------------------- encoder -----------------------------------------


def cjpeg_encode(arr: np.ndarray, *, quality: int, chroma: str,
                 script: str, restart: int, out_path: Path) -> None:
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
                "-quality", str(quality), "-optimize",
                "-restart", f"{restart}B"]
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
    # (w, h, pattern, quality, chroma, script_kind, restart_blocks)
    # Mix of DRI values × scripts × chroma × alignment.
    cases = [
        # ---- Phase 17c: spectral-only (Ah=0) + DRI -----------------------
        (32,  32,  "grad",  80, "gray", "spectral", 1),   # RST per block
        (32,  32,  "grad",  80, "gray", "spectral", 4),
        (64,  48,  "check", 75, "gray", "spectral", 8),
        (100, 75,  "noise", 60, "gray", "spectral", 2),   # non-aligned
        (32,  32,  "grad",  75, "444",  "spectral", 2),
        (48,  48,  "check", 70, "444",  "spectral", 4),
        (97,  73,  "noise", 65, "444",  "spectral", 1),   # non-aligned, RST/blk
        (32,  32,  "grad",  75, "420",  "spectral", 1),   # RST per MCU
        (64,  48,  "check", 70, "420",  "spectral", 4),
        (100, 76,  "noise", 55, "420",  "spectral", 2),   # non-aligned
        (192, 128, "grad",  60, "420",  "spectral", 16),
        # ---- Phase 18c: refinement (Ah>0) + DRI --------------------------
        (32,  32,  "grad",  75, "gray", "default", 2),
        (64,  48,  "check", 70, "gray", "minimal", 4),
        (100, 75,  "noise", 60, "gray", "default", 1),    # non-aligned, RST/blk
        (32,  32,  "grad",  75, "444",  "default", 4),
        (97,  73,  "noise", 65, "444",  "minimal", 2),    # non-aligned
        (32,  32,  "grad",  75, "420",  "default", 2),
        (64,  48,  "check", 70, "420",  "minimal", 4),
        (100, 76,  "noise", 55, "420",  "default", 1),    # non-aligned, RST/blk
        (192, 128, "noise", 55, "420",  "default", 8),
    ]

    OUT.mkdir(parents=True, exist_ok=True)
    for w, h, pat, q, chroma, kind, r in cases:
        ch = 1 if chroma == "gray" else 3
        if pat == "grad":
            img = gradient(w, h, ch)
        elif pat == "check":
            img = check(w, h, ch)
        else:
            img = noise(w, h, ch, seed=42 + w + h)
        if kind == "spectral":
            sc = script_spectral_only(chroma)
        elif kind == "default":
            sc = script_default_progressive(chroma)
        else:
            sc = script_minimal_refine(chroma)
        name = f"pdri_{chroma}_{pat}_{kind}_{w}x{h}_q{q}_r{r}.jpg"
        p = OUT / name
        cjpeg_encode(img, quality=q, chroma=chroma, script=sc,
                     restart=r, out_path=p)
        verify_djpeg_ok(p)

    print(f"generated {len(cases)} Phase 17c/18c progressive+DRI vectors under {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
