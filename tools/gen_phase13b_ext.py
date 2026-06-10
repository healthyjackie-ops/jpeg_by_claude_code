#!/usr/bin/env python3
"""
Phase 13b-prog-ext test vectors — SOF2 progressive Huffman at P=12 with
extended chroma: 4:2:2 / 4:4:0 / 4:1:1.

Phase 13b-prog covered SOF2 + P=12 for gray/444/420; this set completes the
YCbCr chroma matrix at P=12 (CMYK + P=12 stays future work). The C model
path is decode_progressive -> drain_coef_buf_to_planes, whose P=12 branch
was generalized to all sub-sampled modes after the drain unification.

Same recipe as Phase 13b-prog / 17d: cjpeg's default progressive script
(no -scans) emits DC-first + AC-first + AC-refine + DC-refine in a single
encode, so every vector exercises all four scan types.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "verification" / "vectors" / "phase13b_ext"


# ---- 16-bit image generators (shared shapes with gen_phase13b_prog) --------


def gradient_rgb16(w: int, h: int) -> np.ndarray:
    xs = np.linspace(0, 65535, w, dtype=np.float64)
    ys = np.linspace(0, 65535, h, dtype=np.float64)
    r = np.broadcast_to(xs[None, :], (h, w))
    g = np.broadcast_to(ys[:, None], (h, w))
    b = ((xs[None, :] + ys[:, None]) * 0.5).clip(0, 65535)
    return np.stack([r, g, b], axis=-1).astype(np.uint16)


def check_rgb16(w: int, h: int) -> np.ndarray:
    chk = (np.add.outer(np.arange(h) // 8, np.arange(w) // 8) % 2) == 0
    a = np.array([60000,  3000, 30000], dtype=np.uint16)
    b = np.array([ 3000, 50000, 40000], dtype=np.uint16)
    return np.where(chk[..., None], a, b).astype(np.uint16)


def noise_rgb16(w: int, h: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    return rng.integers(0, 65536, size=(h, w, 3), dtype=np.uint16)


def write_ppm16(arr: np.ndarray, path: Path) -> None:
    assert arr.ndim == 3 and arr.shape[2] == 3 and arr.dtype == np.uint16
    h, w, _ = arr.shape
    with open(path, "wb") as f:
        f.write(f"P6\n{w} {h}\n65535\n".encode("ascii"))
        f.write(arr.astype(">u2").tobytes())


# ---- cjpeg invocation ------------------------------------------------------


def cjpeg_encode(src_ppm: Path, out_jpg: Path, quality: int,
                 sample: str, restart: int) -> None:
    cmd = ["cjpeg", "-precision", "12", "-progressive",
           "-quality", str(quality), "-optimize", "-sample", sample]
    if restart > 0:
        cmd += ["-restart", f"{restart}B"]
    cmd += ["-outfile", str(out_jpg), str(src_ppm)]
    subprocess.run(cmd, check=True)


def verify_sof2_p12(path: Path) -> None:
    data = path.read_bytes()
    i = 0
    while i < len(data) - 3:
        if data[i] == 0xFF and data[i + 1] == 0xC2:
            p = data[i + 4]
            if p != 12:
                raise RuntimeError(f"{path}: SOF2 has P={p}, expected 12")
            return
        i += 1
    raise RuntimeError(f"{path}: no SOF2 (0xFFC2) marker found")


# ---- main ------------------------------------------------------------------


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    tmp = OUT / ".tmp_src.ppm"

    # 12 cases per sampling mode. Sizes stress each mode's MCU padding:
    # 4:2:2 MCU 16x8, 4:4:0 MCU 8x16, 4:1:1 MCU 32x8 — non-aligned sizes
    # included for every mode, DRI in {0, 1, 4, 16}.
    sizes = [
        # (w, h, pattern, quality, restart)
        (16,  16,  "grad",  75, 0),
        (32,  32,  "check", 80, 0),
        (64,  64,  "grad",  70, 1),
        (128, 64,  "grad",  50, 0),
        (96,  96,  "check", 75, 4),
        (128, 128, "grad",  90, 16),
        (17,  13,  "grad",  80, 0),
        (23,  19,  "check", 75, 0),
        (57,  39,  "grad",  50, 0),
        (100, 75,  "check", 60, 0),
        (199, 131, "grad",  55, 0),
        (161, 97,  "noise", 60, 4),
    ]
    samplings = [("2x1", "422"), ("1x2", "440"), ("4x1", "411")]

    generated = 0
    for samp, samp_tag in samplings:
        for w, h, pat, q, r in sizes:
            if pat == "grad":
                arr = gradient_rgb16(w, h)
            elif pat == "check":
                arr = check_rgb16(w, h)
            else:
                arr = noise_rgb16(w, h, 42)
            write_ppm16(arr, tmp)

            name = f"p13be_{samp_tag}_{pat}_{w}x{h}_q{q}_r{r}.jpg"
            jpg = OUT / name
            cjpeg_encode(tmp, jpg, q, samp, r)
            verify_sof2_p12(jpg)
            generated += 1

    tmp.unlink(missing_ok=True)
    print(f"generated {generated} Phase 13b-prog-ext SOF2+P=12 vectors under {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
