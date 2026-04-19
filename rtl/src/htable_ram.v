// ---------------------------------------------------------------------------
// htable_ram — Huffman 表存储 + MINCODE/MAXCODE/VALPTR 构建器
//
// 对应 C: jpeg_build_huffman_tables + htable_t 内部数组
//
// 表结构 × 8（4 DC + 4 AC，通过 {ac,sel} 索引）：
//   BITS[1..16]           (ht_bits_wr 写入)
//   HUFFVAL[0..255]       (ht_val_wr  写入)
//   MINCODE[1..16]        16b
//   MAXCODE[1..16]        18b 有符号（-1 表示 length 为空）
//   VALPTR[1..16]         8b
//
// 构建 (ht_build_start 脉冲)：FSM 遍历 l=1..16 计算 M/M/V，用时 ~18 cycles
// ---------------------------------------------------------------------------
module htable_ram (
    input  wire        clk,
    input  wire        rst_n,

    // Write from header_parser
    input  wire        bits_wr,
    input  wire        ac_in,       // 0=DC,1=AC
    input  wire [1:0]  sel_in,      // Th
    input  wire [4:0]  bits_idx,    // 1..16
    input  wire [7:0]  bits_val,

    input  wire        val_wr,
    input  wire [7:0]  val_idx,
    input  wire [7:0]  val_data,

    input  wire        build_start, // 脉冲：表 (ac_in, sel_in) 已全部写入
    output reg         build_done,

    // Read ports for huffman_decoder
    input  wire        rd_ac,
    input  wire [1:0]  rd_sel,
    input  wire [4:0]  rd_l,        // 1..16
    output wire [15:0] rd_mincode,
    output wire [17:0] rd_maxcode,
    output wire [7:0]  rd_valptr,

    input  wire [7:0]  rd_huff_idx, // absolute idx into HUFFVAL
    output wire [7:0]  rd_huffval
);

    // ------------------------------------------------------------------
    // BITS:    8 tables × 32 entries × 8 bits  (索引 {tbl[2:0], l[4:0]} — 需 8-bit
    //          地址空间 256；实际只用 l=1..16 每表 17 位置，但为保证
    //          concat `{tbl, l}` 地址空间内存都在范围内，必须按 2^(3+5)=256 分配)
    // HUFFVAL: 8 tables × 256 entries × 8 bits
    // MINCODE / MAXCODE / VALPTR: 同上 256 entries
    // ------------------------------------------------------------------
    reg [7:0]  bits_mem   [0:8*32-1];
    reg [7:0]  huff_mem   [0:8*256-1];
    reg [15:0] mincode_mem[0:8*32-1];
    reg [17:0] maxcode_mem[0:8*32-1];
    reg [7:0]  valptr_mem [0:8*32-1];

    // Write (synchronous)
    wire [2:0] wr_tbl = {ac_in, sel_in};
    wire [2:0] rd_tbl = {rd_ac, rd_sel};

    // Memories are SRAM/RF targets — no reset (DQT/DHT write every live
    // entry before any read, builder FSM writes mincode/maxcode/valptr).
    always @(posedge clk) begin
        if (bits_wr)
            bits_mem[{wr_tbl, bits_idx[4:0]}] <= bits_val;
        if (val_wr)
            huff_mem[{wr_tbl, val_idx}] <= val_data;
    end

    // Read combinational
    assign rd_mincode = mincode_mem[{rd_tbl, rd_l}];
    assign rd_maxcode = maxcode_mem[{rd_tbl, rd_l}];
    assign rd_valptr  = valptr_mem [{rd_tbl, rd_l}];
    assign rd_huffval = huff_mem   [{rd_tbl, rd_huff_idx}];

    // ------------------------------------------------------------------
    // Build FSM:
    //   build_l = 1..16
    //   code, p (8b), 每 l：
    //     if BITS[l]==0: maxcode[l] = -1,             code = code << 1
    //     else:          valptr[l]=p, mincode[l]=code, maxcode[l]=code+BITS[l]-1
    //                    code = (code+BITS[l]) << 1,  p += BITS[l]
    // ------------------------------------------------------------------
    localparam [1:0] B_IDLE = 2'd0, B_READ = 2'd1, B_UPDATE = 2'd2, B_DONE = 2'd3;
    reg [1:0]  bstate;
    reg [4:0]  build_l;
    reg [15:0] build_code;
    reg [8:0]  build_p;
    reg [2:0]  build_tbl;     // 锁存构建时的表号
    reg [7:0]  bits_val_r;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            bstate     <= B_IDLE;
            build_l    <= 5'd0;
            build_code <= 16'd0;
            build_p    <= 9'd0;
            build_tbl  <= 3'd0;
            build_done <= 1'b0;
            bits_val_r <= 8'd0;
        end else begin
            build_done <= 1'b0;
            case (bstate)
                B_IDLE: if (build_start) begin
                    build_l    <= 5'd1;
                    build_code <= 16'd0;
                    build_p    <= 9'd0;
                    build_tbl  <= {ac_in, sel_in};
                    bstate     <= B_READ;
                end
                B_READ: begin
                    bits_val_r <= bits_mem[{build_tbl, build_l}];
                    bstate     <= B_UPDATE;
                end
                B_UPDATE: begin
                    if (bits_val_r == 8'd0) begin
                        maxcode_mem[{build_tbl, build_l}] <= 18'h3FFFF; // -1
                        build_code <= build_code << 1;
                    end else begin
                        valptr_mem [{build_tbl, build_l}] <= build_p[7:0];
                        mincode_mem[{build_tbl, build_l}] <= build_code;
                        maxcode_mem[{build_tbl, build_l}] <= {2'b0, build_code + {8'd0, bits_val_r} - 16'd1};
                        build_code <= (build_code + {8'd0, bits_val_r}) << 1;
                        build_p    <= build_p + {1'b0, bits_val_r};
                    end
                    if (build_l == 5'd16) begin
                        bstate <= B_DONE;
                    end else begin
                        build_l <= build_l + 5'd1;
                        bstate  <= B_READ;
                    end
                end
                B_DONE: begin
                    build_done <= 1'b1;
                    bstate     <= B_IDLE;
                end
                default: bstate <= B_IDLE;
            endcase
        end
    end

endmodule
