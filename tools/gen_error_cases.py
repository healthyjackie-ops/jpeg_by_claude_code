#!/usr/bin/env python3
"""
Error-case vectors — corrupted JPEGs the C model must REJECT (rc != 0).

Until now error-path coverage was three synthetic cases in test_errors.c
(empty / bad magic / truncated header); the phase14 "error vectors" stopped
being errors once SOF2 support landed. Meanwhile the failure paths leaked
memory (fixed alongside this generator). This set gives every decode family
a corrupted input, exercised by `golden_compare --expect-fail` via
`make errtest` (which also leak-checks each case on macOS).

All corruptions are deterministic byte surgery on existing committed
vectors — no encoder runs, no randomness. Each generated file is verified
to actually fail decode (build/jpeg_decode rc != 0) before it is kept, so
the set can never silently contain a decodable file.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VEC = ROOT / "verification" / "vectors"
OUT = VEC / "error_cases"
DECODER = ROOT / "c_model" / "build" / "jpeg_decode"


def find_marker(data: bytes, marker: int) -> int:
    """Offset of the first 0xFF <marker> in the header region (pre-SOS)."""
    i = data.find(bytes([0xFF, marker]))
    if i < 0:
        raise RuntimeError(f"marker 0xFF{marker:02X} not found")
    return i


def strip_eoi(data: bytes) -> bytes:
    if data[-2:] != b"\xff\xd9":
        raise RuntimeError("file does not end with EOI")
    return data[:-2]


def truncate_frac(data: bytes, frac: float) -> bytes:
    n = int(len(data) * frac)
    if n >= len(data) - 2:
        raise RuntimeError("truncation point too late")
    return data[:n]


def corrupt_dht_bits(data: bytes) -> bytes:
    """Set all 16 BITS counts of the first DHT to 0xFF (sum >> 256)."""
    i = find_marker(data, 0xC4)
    b = bytearray(data)
    for k in range(16):
        b[i + 5 + k] = 0xFF
    return bytes(b)


def corrupt_sos_comp_id(data: bytes) -> bytes:
    """First SOS component selector -> 0xEE (no such frame component)."""
    i = find_marker(data, 0xDA)
    b = bytearray(data)
    b[i + 5] = 0xEE
    return bytes(b)


def truncate_in_sof(data: bytes) -> bytes:
    """Cut 3 bytes into the SOF segment (mid frame header)."""
    for m in (0xC0, 0xC1, 0xC2, 0xC3, 0xC9, 0xCA):
        i = data.find(bytes([0xFF, m]))
        if i >= 0:
            return data[: i + 5]
    raise RuntimeError("no SOF marker found")


CASES = [
    # (output name, source path, corruption)
    ("err_noeoi_baseline420.jpg",  "smoke/grad_16x16_q80.jpg",              strip_eoi),
    ("err_trunc_baseline420.jpg",  "smoke/check_32x32_q90.jpg",             lambda d: truncate_frac(d, 0.7)),
    ("err_trunchdr_baseline.jpg",  "smoke/check_32x32_q90.jpg",             truncate_in_sof),
    ("err_baddht_baseline.jpg",    "smoke/check_32x32_q90.jpg",             corrupt_dht_bits),
    ("err_badsos_baseline.jpg",    "smoke/check_32x32_q90.jpg",             corrupt_sos_comp_id),
    ("err_noeoi_prog.jpg",         "phase17/p17_420_check_full_32x32_q70.jpg", strip_eoi),
    ("err_trunc_prog.jpg",         "phase17d/p17d_411_check_100x75_q60_r0.jpg", lambda d: truncate_frac(d, 0.7)),
    ("err_baddht_prog.jpg",        "phase17d/p17d_411_check_100x75_q60_r0.jpg", corrupt_dht_bits),
    ("err_trunc_sof9.jpg",         "phase22/p22_420_check_32x32_q80_r0.jpg",   lambda d: truncate_frac(d, 0.7)),
    ("err_trunc_sof10.jpg",        "phase24/p24_420_check_200x150_q30_r0.jpg", lambda d: truncate_frac(d, 0.7)),
    ("err_trunc_lossless.jpg",     "phase25/lossless_ps1_pt0_check_64x48.jpg", lambda d: truncate_frac(d, 0.7)),
    ("err_trunc_p12.jpg",          "phase13/p13_check_16x16_q80_sg_r0.jpg",    lambda d: truncate_frac(d, 0.7)),
    ("err_badsos_p12.jpg",         "phase13/p13_check_16x16_q80_sg_r0.jpg",    corrupt_sos_comp_id),
    ("err_trunc_cmyk.jpg",         "phase12/p12_check_16x16_q80_r0.jpg",       lambda d: truncate_frac(d, 0.7)),
]


def must_fail(path: Path) -> int:
    """Run the C model; return its exit code, which must be nonzero."""
    r = subprocess.run([str(DECODER), str(path)],
                       capture_output=True, text=True)
    return r.returncode


def main() -> int:
    if not DECODER.exists():
        print(f"build {DECODER} first (make -C c_model)", file=sys.stderr)
        return 1
    OUT.mkdir(parents=True, exist_ok=True)
    for name, src, corrupt in CASES:
        data = (VEC / src).read_bytes()
        out = OUT / name
        out.write_bytes(corrupt(data))
        rc = must_fail(out)
        if rc == 0:
            out.unlink()
            print(f"[GEN-FAIL] {name}: decoder ACCEPTED the corrupted file "
                  f"(src {src}) — case dropped, investigate", file=sys.stderr)
            return 1
        print(f"[ok] {name}  (decode rc={rc})")
    print(f"generated {len(CASES)} error cases under {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
