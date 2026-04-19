"""
Cost JPEG decoder memories against ASAP7 fakeram SRAM tiles and flop-RF.

Methodology
-----------
* Small memories (< FLOP_THRESHOLD_BITS ≈ 2048) map to flop register files,
  because fakeram7 macros have minimum 64D × 20W = 1280 bits and a fixed
  peripheral overhead.  At these sizes the per-bit density of flops
  (≈ 0.76 µm² / bit, derived from DFFHQNx1 = 0.29 µm² / bit × shared enable
  & muxing) is worse than SRAM but simpler, and yosys always flattens them.
* Medium/large memories (≥ FLOP_THRESHOLD_BITS) are tiled onto the catalog of
  ASAP7 fakeram macros.  We pick the macro that covers (d × w) with the
  smallest total tile area, considering both depth-stack (d / macro_d) and
  width-slice (ceil(w / macro_w)) factors.

Targets are expressed as rows of (name, depth, width, ports).  Dual-read
ports (ports="2R") cost 2× the single-port area (simple duplication).

Units are µm² throughout (lib area).  1 GE (NAND2xp33) = 0.08748 µm² so
1 µm² = 11.4 GE.
"""

import math

# ---- Catalog of ASAP7 fakeram macros (depth, width, area_um2) -----------
# Areas sampled from flow/platforms/asap7/lib/NLDM/fakeram*.lib
MACROS = [
    # depth, width, area(um²)
    ( 32,  46,   61.810),
    ( 64,  20,   53.748),
    ( 64,  21,   56.435),
    ( 64,  22,   59.122),
    ( 64,  25,   67.185),
    ( 64,  28,   75.247),
    ( 64, 256, 1517.411),
    (128,  64,  343.985),
    (256,  32,  343.985),
    (256,  34,  365.484),
    (256,  64,  687.971),
    (256, 128, 1375.941),
    (256, 256, 2751.883),
    (512,   8,  171.993),
    (2048, 39, 5243.000),   # 2K×39 ≈ 5243 µm² (approx from similar scaling)
]

# Flop-RF cost per bit (DFFHQNx1 area 0.29 µm² per flop + small mux/enable
# overhead — empirically ≈ 0.76 µm²/bit for yosys-inferred register files).
FLOP_UM2_PER_BIT = 0.76
FLOP_THRESHOLD_BITS = 2048   # below this, stay in flops

# 1 GE in ASAP7 7.5T RVT (NAND2x1 = 0.08748 µm²)
GE_PER_UM2 = 1.0 / 0.08748


def tile_cost(depth: int, width: int, mdepth: int, mwidth: int, area: float) -> float:
    dfactor = math.ceil(depth / mdepth)
    wfactor = math.ceil(width / mwidth)
    return dfactor * wfactor * area, dfactor, wfactor


def pick_macro(depth: int, width: int):
    best = None
    for md, mw, a in MACROS:
        cost, df, wf = tile_cost(depth, width, md, mw, a)
        key = cost
        if best is None or key < best[0]:
            best = (cost, md, mw, a, df, wf)
    return best


def cost_mem(name: str, depth: int, width: int, ports: int = 1):
    bits = depth * width
    if bits < FLOP_THRESHOLD_BITS:
        area = bits * FLOP_UM2_PER_BIT * ports
        macro = "flop-RF"
        stack = f"{depth}×{width}b"
    else:
        cost, md, mw, a, df, wf = pick_macro(depth, width)
        area = cost * ports
        macro = f"fakeram_{md}x{mw}"
        stack = f"{df}D×{wf}W"
        if ports > 1:
            stack += f" ×{ports}R"
    return {
        "name": name,
        "depth": depth,
        "width": width,
        "bits": bits,
        "macro": macro,
        "stack": stack,
        "area_um2": area,
        "um2_per_bit": area / max(bits, 1),
    }


# ---- JPEG decoder memories ----------------------------------------------
# Numbers verified against RTL source:
#   line_buffer.v (MAX_W=4096):   ybuf 16×4096×8, cb/crbuf 8×2048×8
#   mcu_buffer.v:                 ybuf 16×16×8, cb/crbuf 8×8×8
#   htable_ram.v:                 bits 8×32×8, huff 8×256×8, m/m/v 8×32×16/18/8
#   qtable_ram.v:                 4×64×8
#   idct_2d.v:                    inbuf 64×16, ws 64×32
#   dequant_izz.v:                coef_buf 64×16
#   axi_stream_fifo.v:            in (32 × 10b), out (32 × 26b)
# The wrapper module address is "flattened" — listed sizes are post-flatten.

MEMS = [
    # ---- Large (SRAM candidates) ----
    ("line_buffer ybuf",            16 * 4096,  8, 1),
    ("line_buffer cbbuf",            8 * 2048,  8, 1),
    ("line_buffer crbuf",            8 * 2048,  8, 1),

    # ---- Small (flop-RF expected) ----
    ("mcu_buffer ybuf",              16 * 16,  8, 1),
    ("mcu_buffer cbbuf",              8 * 8,   8, 1),
    ("mcu_buffer crbuf",              8 * 8,   8, 1),

    ("htable bits_mem",             8 * 32,    8, 1),   # 256×8
    ("htable huff_mem",             8 * 256,   8, 1),   # 2048×8
    ("htable mincode_mem",          8 * 32,   16, 1),
    ("htable maxcode_mem",          8 * 32,   18, 1),
    ("htable valptr_mem",           8 * 32,    8, 1),

    ("qtable_ram mem",              4 * 64,    8, 1),
    ("idct_2d inbuf",                    64,  16, 1),
    ("idct_2d ws",                       64,  32, 1),
    ("dequant_izz coef_buf",             64,  16, 1),

    ("axis_fifo in (8b+1L+1U)",          32,  10, 1),
    ("axis_fifo out (24b+1L+1U)",        32,  26, 1),
]


def main():
    rows = [cost_mem(*m) for m in MEMS]
    total = sum(r["area_um2"] for r in rows)
    sram = sum(r["area_um2"] for r in rows if r["macro"] != "flop-RF")
    flop = total - sram

    print()
    print(f"{'Memory':<30} {'Shape':<12} {'Macro':<18} {'Tile':<16} {'Area µm²':>10} {'µm²/bit':>8}")
    print("-" * 100)
    for r in rows:
        shape = f"{r['depth']}×{r['width']}"
        print(f"{r['name']:<30} {shape:<12} {r['macro']:<18} {r['stack']:<16} "
              f"{r['area_um2']:>10.1f} {r['um2_per_bit']:>8.3f}")
    print("-" * 100)
    print(f"{'TOTAL':<30} {'':<12} {'':<18} {'':<16} {total:>10.1f} µm²")
    print(f"  SRAM tiles         : {sram:>10.1f} µm²  ({sram/total*100:5.1f} %)")
    print(f"  Flop register-file : {flop:>10.1f} µm²  ({flop/total*100:5.1f} %)")
    print(f"  Total (µm² → GE)   : {total * GE_PER_UM2:>10.0f} GE")
    print(f"  Total (mm²)        : {total/1e6:.4f} mm²")


if __name__ == "__main__":
    main()
