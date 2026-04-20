// ---------------------------------------------------------------------------
// mcu_buffer — 收集 IDCT 输出的 block (Y*, Cb, Cr) 并组装成 MCU 内容
//
// Y buffer 尺寸 32×16 = 512 bytes，支持所有 Y 布局：
//   4:2:0 (16×16): blk_type 0..3 → (r_off, c_off) ∈ {(0,0),(0,8),(8,0),(8,8)}
//   4:1:1 (32×8):  blk_type 0..3 → c_off ∈ {0,8,16,24}, r_off=0 (is_411=1)
//   4:2:2 (16×8):  blk_type 0..1 → c_off ∈ {0,8}, r_off=0
//   4:4:0 (8×16):  blk_type 0 (top, r_off=0), 2 (bot, r_off=8), c_off=0
//   4:4:4/gray (8×8): blk_type 0 → (0,0)
// Cb/Cr buffer: 8×8 = 64 bytes each
//
// 时序：
//   idct_2d 每次送 8 pixel/cyc × 8 rows = 1 block，blk_done_out 脉冲时 block 完毕
//   MCU 全部 block 完成 → mcu_ready=1，等待 mcu_consume 握手
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
    //   0..3 = Y positions   4 = Cb   5 = Cr   6 = K (Phase 12 CMYK)
    input  wire [2:0]  blk_type,
    input  wire        is_411,         // Phase 11b: Y 布局为 32×8 横向 4 块
    input  wire        is_cmyk,        // Phase 12: CMYK 模式 (最后一块为 K)

    // MCU 输出就绪 & 握手
    output reg         mcu_ready,
    input  wire        mcu_consume,    // 高电平时允许输出

    // 读取 MCU 内容 (Y 最大 32×16, Cb 8×8, Cr 8×8, K 8×8) —— 按 (row, col) 地址读
    input  wire [3:0]  rd_y_row,       // 0..15
    input  wire [4:0]  rd_y_col,       // 0..31 (Phase 11b: 32 列支持)
    output wire [7:0]  rd_y_data,
    input  wire [2:0]  rd_c_row,       // 0..7
    input  wire [2:0]  rd_c_col,
    output wire [7:0]  rd_cb_data,
    output wire [7:0]  rd_cr_data,
    output wire [7:0]  rd_k_data       // Phase 12: CMYK K 读口
);

    // Y: 32×16 = 512 bytes (支持 4:1:1 32-wide 和 4:2:0 16×16)
    reg [7:0] ybuf [0:511];
    // Cb, Cr, K: 8×8 = 64 bytes each
    reg [7:0] cbbuf [0:63];
    reg [7:0] crbuf [0:63];
    reg [7:0] kbuf  [0:63];

    assign rd_y_data  = ybuf[{rd_y_row, rd_y_col}];
    assign rd_cb_data = cbbuf[{rd_c_row, rd_c_col}];
    assign rd_cr_data = crbuf[{rd_c_row, rd_c_col}];
    assign rd_k_data  = kbuf[{rd_c_row, rd_c_col}];

    // Y block 在 32×16 内的偏移（行偏移 0/8，列偏移 0/8/16/24）
    // 4:2:0 (!is_411): blk_type[1]=row bit (0=top,1=bot); blk_type[0]=col bit
    // 4:1:1 (is_411):  blk_type[1:0]=col idx (0..3); row always 0
    wire [3:0] y_r_off = is_411 ? 4'd0 : (blk_type[1] ? 4'd8 : 4'd0);
    wire [4:0] y_c_off = is_411 ? {blk_type[1:0], 3'd0}       // 0/8/16/24
                                : {1'b0, blk_type[0], 3'd0};  // 0/8

    // 本拍 pix_row 对应的 Y 行
    wire [3:0] abs_y_row = y_r_off + {1'b0, pix_row};

    // Memory writes — SRAM inference, no reset (pix_valid only high
    // after DHT/DQT/SOF/SOS parsed and IDCT runs, so reads follow writes).
    always @(posedge clk) begin
        if (pix_valid) begin
            case (blk_type)
                3'd0, 3'd1, 3'd2, 3'd3: begin
                    ybuf[{abs_y_row, y_c_off + 5'd0}] <= pix0;
                    ybuf[{abs_y_row, y_c_off + 5'd1}] <= pix1;
                    ybuf[{abs_y_row, y_c_off + 5'd2}] <= pix2;
                    ybuf[{abs_y_row, y_c_off + 5'd3}] <= pix3;
                    ybuf[{abs_y_row, y_c_off + 5'd4}] <= pix4;
                    ybuf[{abs_y_row, y_c_off + 5'd5}] <= pix5;
                    ybuf[{abs_y_row, y_c_off + 5'd6}] <= pix6;
                    ybuf[{abs_y_row, y_c_off + 5'd7}] <= pix7;
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
                3'd6: begin // Phase 12: CMYK K plane
                    kbuf[{pix_row, 3'd0}] <= pix0;
                    kbuf[{pix_row, 3'd1}] <= pix1;
                    kbuf[{pix_row, 3'd2}] <= pix2;
                    kbuf[{pix_row, 3'd3}] <= pix3;
                    kbuf[{pix_row, 3'd4}] <= pix4;
                    kbuf[{pix_row, 3'd5}] <= pix5;
                    kbuf[{pix_row, 3'd6}] <= pix6;
                    kbuf[{pix_row, 3'd7}] <= pix7;
                end
                default: ;
            endcase
        end
    end

    // mcu_ready FF — keep async reset
    // Phase 12: YCbCr/GRAY/etc. 在 blk_type=5 (Cr) 末尾触发；CMYK 在 blk_type=6 (K) 末尾触发
    wire mcu_last_blk = is_cmyk ? (blk_type == 3'd6) : (blk_type == 3'd5);
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)               mcu_ready <= 1'b0;
        else if (soft_reset)      mcu_ready <= 1'b0;
        else if (blk_done_in && mcu_last_blk)
                                  mcu_ready <= 1'b1;
        else if (mcu_consume)     mcu_ready <= 1'b0;
    end

endmodule
