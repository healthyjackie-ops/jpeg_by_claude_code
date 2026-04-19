// ---------------------------------------------------------------------------
// idct_2d — 8×8 IDCT (Pass1 列, Pass2 行；JDCT_ISLOW bit-exact)
//
// 对应 C: idct_islow (c_model/src/idct.c)
//
// 时序（Phase 4 优化：idct_1d 为 3 级流水以满足 600 MHz）：
//   64 coef 输入 (dq_wr/dq_idx/dq_val)      → 64 cycles
//   Pass1: 10 cycles (0..7 喂数 + 8,9 drain)
//   Pass2: 10 cycles (同上)
//   纯 IDCT 20 cyc，单块总延迟 84 cyc（较 2-stage 的 82 cyc +2 拍）。
// ---------------------------------------------------------------------------
`include "jpeg_defs.vh"

module idct_2d (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        soft_reset,

    // 来自 dequant_izz
    input  wire        dq_start,
    input  wire        dq_wr,
    input  wire [5:0]  dq_idx,
    input  wire signed [15:0] dq_val,
    input  wire        dq_done,

    // 输出 8 像素/cyc (pass2 过程中)
    output reg         pix_valid,
    output reg  [2:0]  pix_row,         // 0..7
    output reg  [7:0]  pix0, pix1, pix2, pix3, pix4, pix5, pix6, pix7,
    output reg         blk_done_out
);

    // 输入缓冲 64 × int16 (natural order)
    reg signed [15:0] inbuf [0:63];
    // 转置缓冲 64 × int32
    reg signed [31:0] ws    [0:63];

    // ------------------------------------------------------------------
    // FSM
    // ------------------------------------------------------------------
    localparam [2:0] S_IDLE=3'd0, S_FILL=3'd1, S_P1=3'd2, S_P2=3'd3, S_DONE=3'd4;
    reg [2:0] st;
    reg [3:0] pass_cnt;      // 0..9 (8 个有效 feed + 2 个 drain tick)

    // 在 0..7 期间才是有效 feed，cycle 8,9 是 drain (让 idct_1d 3 级流水的残留 2 拍输出被写)
    wire       fed_valid = (pass_cnt <= 4'd7);

    // Pass1 组合输入（按当前 col）
    wire [2:0] col = pass_cnt[2:0];
    wire signed [31:0] p1_in0 = {{16{inbuf[{3'd0, col}][15]}}, inbuf[{3'd0, col}]};
    wire signed [31:0] p1_in1 = {{16{inbuf[{3'd1, col}][15]}}, inbuf[{3'd1, col}]};
    wire signed [31:0] p1_in2 = {{16{inbuf[{3'd2, col}][15]}}, inbuf[{3'd2, col}]};
    wire signed [31:0] p1_in3 = {{16{inbuf[{3'd3, col}][15]}}, inbuf[{3'd3, col}]};
    wire signed [31:0] p1_in4 = {{16{inbuf[{3'd4, col}][15]}}, inbuf[{3'd4, col}]};
    wire signed [31:0] p1_in5 = {{16{inbuf[{3'd5, col}][15]}}, inbuf[{3'd5, col}]};
    wire signed [31:0] p1_in6 = {{16{inbuf[{3'd6, col}][15]}}, inbuf[{3'd6, col}]};
    wire signed [31:0] p1_in7 = {{16{inbuf[{3'd7, col}][15]}}, inbuf[{3'd7, col}]};

    wire signed [31:0] p1_o0, p1_o1, p1_o2, p1_o3, p1_o4, p1_o5, p1_o6, p1_o7;
    idct_1d u_p1 (
        .clk(clk),
        .in0(p1_in0), .in1(p1_in1), .in2(p1_in2), .in3(p1_in3),
        .in4(p1_in4), .in5(p1_in5), .in6(p1_in6), .in7(p1_in7),
        .out0(p1_o0), .out1(p1_o1), .out2(p1_o2), .out3(p1_o3),
        .out4(p1_o4), .out5(p1_o5), .out6(p1_o6), .out7(p1_o7)
    );

    // Pass2 组合输入（按当前 row）
    wire [2:0] row = pass_cnt[2:0];
    wire signed [31:0] p2_in0 = ws[{row, 3'd0}];
    wire signed [31:0] p2_in1 = ws[{row, 3'd1}];
    wire signed [31:0] p2_in2 = ws[{row, 3'd2}];
    wire signed [31:0] p2_in3 = ws[{row, 3'd3}];
    wire signed [31:0] p2_in4 = ws[{row, 3'd4}];
    wire signed [31:0] p2_in5 = ws[{row, 3'd5}];
    wire signed [31:0] p2_in6 = ws[{row, 3'd6}];
    wire signed [31:0] p2_in7 = ws[{row, 3'd7}];

    wire signed [31:0] p2_o0, p2_o1, p2_o2, p2_o3, p2_o4, p2_o5, p2_o6, p2_o7;
    // 复用同一 idct_1d 实例会有冲突（两套输入），故用第二个实例
    idct_1d u_p2 (
        .clk(clk),
        .in0(p2_in0), .in1(p2_in1), .in2(p2_in2), .in3(p2_in3),
        .in4(p2_in4), .in5(p2_in5), .in6(p2_in6), .in7(p2_in7),
        .out0(p2_o0), .out1(p2_o1), .out2(p2_o2), .out3(p2_o3),
        .out4(p2_o4), .out5(p2_o5), .out6(p2_o6), .out7(p2_o7)
    );

    // ------------------------------------------------------------------
    // Pipeline tracking: idct_1d output is 2 cycles delayed (3-stage pipeline).
    // wr_col_r2/wr_en_*_r2 记录 2 拍之前的 feed 状态；用来在输出可用时写入 ws / pix。
    // ------------------------------------------------------------------
    reg [2:0] wr_col_r1, wr_col_r2;
    reg       wr_en_p1_r1, wr_en_p1_r2;
    reg       wr_en_p2_r1, wr_en_p2_r2;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            wr_col_r1   <= 3'd0; wr_col_r2   <= 3'd0;
            wr_en_p1_r1 <= 1'b0; wr_en_p1_r2 <= 1'b0;
            wr_en_p2_r1 <= 1'b0; wr_en_p2_r2 <= 1'b0;
        end else if (soft_reset) begin
            wr_col_r1   <= 3'd0; wr_col_r2   <= 3'd0;
            wr_en_p1_r1 <= 1'b0; wr_en_p1_r2 <= 1'b0;
            wr_en_p2_r1 <= 1'b0; wr_en_p2_r2 <= 1'b0;
        end else begin
            wr_col_r1   <= col;
            wr_col_r2   <= wr_col_r1;
            wr_en_p1_r1 <= (st == S_P1) && fed_valid;
            wr_en_p1_r2 <= wr_en_p1_r1;
            wr_en_p2_r1 <= (st == S_P2) && fed_valid;
            wr_en_p2_r2 <= wr_en_p2_r1;
        end
    end

    // DESCALE 宏化
    // Pass1: shift = 11 (CONST_BITS - PASS1_BITS)，round bias = 1<<10
    // Pass2: shift = 18 (CONST_BITS + PASS1_BITS + 3)，round bias = 1<<17
    function signed [31:0] desc_p1;
        input signed [31:0] x;
        begin
            desc_p1 = (x + 32'sd1024) >>> 11;
        end
    endfunction

    function [7:0] desc_p2;
        input signed [31:0] x;
        reg signed [31:0] y;
        begin
            y = (x + 32'sd131072) >>> 18;
            y = y + 32'sd128;
            if (y < 0) desc_p2 = 8'd0;
            else if (y > 32'sd255) desc_p2 = 8'd255;
            else desc_p2 = y[7:0];
        end
    endfunction

    // ------------------------------------------------------------------
    // Memory writes — no reset, inferable as SRAM/RF. ws 使用 pipelined
    // wr_col_r2 (对应 2 拍之前 feed 的 col)，写入 p1_o* 来自 idct_1d 的流水线输出。
    always @(posedge clk) begin
        if (dq_wr)
            inbuf[dq_idx] <= dq_val;
        if (wr_en_p1_r2) begin
            ws[{3'd0, wr_col_r2}] <= desc_p1(p1_o0);
            ws[{3'd1, wr_col_r2}] <= desc_p1(p1_o1);
            ws[{3'd2, wr_col_r2}] <= desc_p1(p1_o2);
            ws[{3'd3, wr_col_r2}] <= desc_p1(p1_o3);
            ws[{3'd4, wr_col_r2}] <= desc_p1(p1_o4);
            ws[{3'd5, wr_col_r2}] <= desc_p1(p1_o5);
            ws[{3'd6, wr_col_r2}] <= desc_p1(p1_o6);
            ws[{3'd7, wr_col_r2}] <= desc_p1(p1_o7);
        end
    end

    // ------------------------------------------------------------------
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            st <= S_IDLE;
            pass_cnt <= 4'd0;
            pix_valid <= 1'b0; pix_row <= 3'd0;
            pix0<=8'd0; pix1<=8'd0; pix2<=8'd0; pix3<=8'd0;
            pix4<=8'd0; pix5<=8'd0; pix6<=8'd0; pix7<=8'd0;
            blk_done_out <= 1'b0;
        end else if (soft_reset) begin
            st <= S_IDLE; pass_cnt <= 4'd0;
            pix_valid <= 1'b0; blk_done_out <= 1'b0;
        end else begin
            pix_valid    <= 1'b0;
            blk_done_out <= 1'b0;

            case (st)
                S_IDLE: if (dq_start) begin
                    st <= S_FILL;
                end
                S_FILL: if (dq_done) begin
                    st <= S_P1;
                    pass_cnt <= 4'd0;
                end
                S_P1: begin
                    if (pass_cnt == 4'd9) begin
                        pass_cnt <= 4'd0;
                        st <= S_P2;
                    end else begin
                        pass_cnt <= pass_cnt + 4'd1;
                    end
                end
                S_P2: begin
                    if (wr_en_p2_r2) begin
                        pix_valid <= 1'b1;
                        pix_row   <= wr_col_r2;    // 2 拍之前喂的 row
                        pix0 <= desc_p2(p2_o0);
                        pix1 <= desc_p2(p2_o1);
                        pix2 <= desc_p2(p2_o2);
                        pix3 <= desc_p2(p2_o3);
                        pix4 <= desc_p2(p2_o4);
                        pix5 <= desc_p2(p2_o5);
                        pix6 <= desc_p2(p2_o6);
                        pix7 <= desc_p2(p2_o7);
                    end
                    if (pass_cnt == 4'd9) begin
                        pass_cnt <= 4'd0;
                        st <= S_DONE;
                    end else begin
                        pass_cnt <= pass_cnt + 4'd1;
                    end
                end
                S_DONE: begin
                    blk_done_out <= 1'b1;
                    st <= S_IDLE;
                end
                default: st <= S_IDLE;
            endcase
        end
    end

endmodule
