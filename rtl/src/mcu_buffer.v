// ---------------------------------------------------------------------------
// mcu_buffer — 收集 IDCT 输出的 6 个 8×8 block (Y0,Y1,Y2,Y3,Cb,Cr)
//               组装成 16×16 Y + 8×8 Cb + 8×8 Cr (一个 YUV420 MCU)
//
// 对应 C: decoder.c 中的 y_blk[4][64] / cb_blk / cr_blk + copy_block_* 动作
//
// 时序：
//   idct_2d 每次送 8 pixel/cyc × 8 rows = 1 block，blk_done_out 脉冲时 block 完毕
//   本模块内部计数 blk_cnt=0..5 区分块类型
//   MCU 全部 6 块完成 → mcu_ready=1，等待 mcu_consume 握手
// ---------------------------------------------------------------------------
module mcu_buffer (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        soft_reset,

    // 来自 idct_2d
    input  wire        pix_valid,
    input  wire [2:0]  pix_row,
    input  wire [7:0]  pix0, pix1, pix2, pix3, pix4, pix5, pix6, pix7,
    input  wire        blk_done_in,

    // 控制：当前 block 类型 (由 block_sequencer 驱动)
    //   0..3 = Y00,Y01,Y10,Y11   4 = Cb   5 = Cr
    input  wire [2:0]  blk_type,

    // MCU 输出就绪 & 握手
    output reg         mcu_ready,
    input  wire        mcu_consume,    // 高电平时允许输出

    // 读取 MCU 内容 (Y 16×16, Cb 8×8, Cr 8×8) —— 按 (row, col) 地址读
    input  wire [3:0]  rd_y_row,       // 0..15
    input  wire [3:0]  rd_y_col,
    output wire [7:0]  rd_y_data,
    input  wire [2:0]  rd_c_row,       // 0..7
    input  wire [2:0]  rd_c_col,
    output wire [7:0]  rd_cb_data,
    output wire [7:0]  rd_cr_data
);

    // Y: 16×16 = 256 bytes
    reg [7:0] ybuf [0:255];
    // Cb, Cr: 8×8 = 64 bytes each
    reg [7:0] cbbuf [0:63];
    reg [7:0] crbuf [0:63];

    assign rd_y_data  = ybuf[{rd_y_row, rd_y_col}];
    assign rd_cb_data = cbbuf[{rd_c_row, rd_c_col}];
    assign rd_cr_data = crbuf[{rd_c_row, rd_c_col}];

    // Y block 在 16×16 内的偏移（行偏移，列偏移）
    // blk_type 0..3: Y0=(0,0) Y1=(0,8) Y2=(8,0) Y3=(8,8)
    wire [3:0] y_r_off = blk_type[1] ? 4'd8 : 4'd0;   // blk_type[1]=1 => bottom
    wire [3:0] y_c_off = blk_type[0] ? 4'd8 : 4'd0;   // blk_type[0]=1 => right

    // 本拍 pix_row 对应的 16×16 Y 行
    wire [3:0] abs_y_row = y_r_off + {1'b0, pix_row};

    // Memory writes — SRAM inference, no reset (pix_valid only high
    // after DHT/DQT/SOF/SOS parsed and IDCT runs, so reads follow writes).
    always @(posedge clk) begin
        if (pix_valid) begin
            case (blk_type)
                3'd0, 3'd1, 3'd2, 3'd3: begin
                    ybuf[{abs_y_row, y_c_off + 4'd0}] <= pix0;
                    ybuf[{abs_y_row, y_c_off + 4'd1}] <= pix1;
                    ybuf[{abs_y_row, y_c_off + 4'd2}] <= pix2;
                    ybuf[{abs_y_row, y_c_off + 4'd3}] <= pix3;
                    ybuf[{abs_y_row, y_c_off + 4'd4}] <= pix4;
                    ybuf[{abs_y_row, y_c_off + 4'd5}] <= pix5;
                    ybuf[{abs_y_row, y_c_off + 4'd6}] <= pix6;
                    ybuf[{abs_y_row, y_c_off + 4'd7}] <= pix7;
                end
                3'd4: begin
                    cbbuf[{pix_row, 3'd0}] <= pix0;
                    cbbuf[{pix_row, 3'd1}] <= pix1;
                    cbbuf[{pix_row, 3'd2}] <= pix2;
                    cbbuf[{pix_row, 3'd3}] <= pix3;
                    cbbuf[{pix_row, 3'd4}] <= pix4;
                    cbbuf[{pix_row, 3'd5}] <= pix5;
                    cbbuf[{pix_row, 3'd6}] <= pix6;
                    cbbuf[{pix_row, 3'd7}] <= pix7;
                end
                3'd5: begin
                    crbuf[{pix_row, 3'd0}] <= pix0;
                    crbuf[{pix_row, 3'd1}] <= pix1;
                    crbuf[{pix_row, 3'd2}] <= pix2;
                    crbuf[{pix_row, 3'd3}] <= pix3;
                    crbuf[{pix_row, 3'd4}] <= pix4;
                    crbuf[{pix_row, 3'd5}] <= pix5;
                    crbuf[{pix_row, 3'd6}] <= pix6;
                    crbuf[{pix_row, 3'd7}] <= pix7;
                end
                default: ;
            endcase
        end
    end

    // mcu_ready FF — keep async reset
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)               mcu_ready <= 1'b0;
        else if (soft_reset)      mcu_ready <= 1'b0;
        else if (blk_done_in && blk_type == 3'd5)
                                  mcu_ready <= 1'b1;
        else if (mcu_consume)     mcu_ready <= 1'b0;
    end

endmodule
