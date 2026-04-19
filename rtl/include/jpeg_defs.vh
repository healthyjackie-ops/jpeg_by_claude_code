// ---------------------------------------------------------------------------
// jpeg_defs.vh — JPEG Decoder 公共定义 (Verilog-2001)
// ---------------------------------------------------------------------------
`ifndef JPEG_DEFS_VH
`define JPEG_DEFS_VH

// Markers
`define MARKER_SOI    8'hD8
`define MARKER_EOI    8'hD9
`define MARKER_SOF0   8'hC0
`define MARKER_DHT    8'hC4
`define MARKER_DQT    8'hDB
`define MARKER_DRI    8'hDD
`define MARKER_SOS    8'hDA
`define MARKER_COM    8'hFE

// Error code bits (对应 C: jpeg_err_t)
`define ERR_UNSUP_SOF       0
`define ERR_UNSUP_PREC      1
`define ERR_UNSUP_CHROMA    2
`define ERR_BAD_HUFFMAN     3
`define ERR_BAD_MARKER      4
`define ERR_DRI_NONZERO     5
`define ERR_SIZE_OOR        6
`define ERR_STREAM_TRUNC    7

// IDCT fixed-point constants (JDCT_ISLOW，与 C 模型 bit-exact)
// 需 15 bit 宽 (25172 需 15b)
`define FIX_0_298631336   15'd02446
`define FIX_0_390180644   15'd03196
`define FIX_0_541196100   15'd04433
`define FIX_0_765366865   15'd06270
`define FIX_0_899976223   15'd07373
`define FIX_1_175875602   15'd09633
`define FIX_1_501321110   15'd12299
`define FIX_1_847759065   15'd15137
`define FIX_1_961570560   15'd16069
`define FIX_2_053119869   15'd16819
`define FIX_2_562915447   15'd20995
`define FIX_3_072711026   15'd25172

`define CONST_BITS  5'd13
`define PASS1_BITS  5'd2

`endif
