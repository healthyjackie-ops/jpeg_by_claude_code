// ---------------------------------------------------------------------------
// mcu_line_copy — 把 mcu_buffer (16×16 Y + 8×8 Cb/Cr) 复制到 line_buffer
//                 在正确的 mcu_col_idx 横向偏移位置
//
// 对应 C: decoder.c 中的 copy_block_16x16_y / copy_block_8x8 调用
// ---------------------------------------------------------------------------
module mcu_line_copy (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        soft_reset,

    input  wire        start,
    input  wire [15:0] mcu_col_idx,      // 0..mcu_cols-1
    input  wire        is_grayscale,    // Phase 8: 1=only Y 8x8, skip chroma
    output reg         done,

    // 读 mcu_buffer
    output reg  [3:0]  mb_y_row,
    output reg  [3:0]  mb_y_col,
    output reg  [2:0]  mb_c_row,
    output reg  [2:0]  mb_c_col,
    input  wire [7:0]  mb_y_data,
    input  wire [7:0]  mb_cb_data,
    input  wire [7:0]  mb_cr_data,

    // 写 line_buffer
    output reg         lb_y_wr,
    output reg  [3:0]  lb_y_row,
    output reg  [11:0] lb_y_col,
    output reg  [7:0]  lb_y_data,
    output reg         lb_c_wr,
    output reg  [2:0]  lb_c_row,
    output reg  [10:0] lb_c_col,
    output reg  [7:0]  lb_cb_data,
    output reg  [7:0]  lb_cr_data
);

    // 扫描状态
    reg [4:0] y_cnt;      // 0..15 rows
    reg [3:0] col_cnt;    // 0..15 cols
    reg       phase;      // 0 = Y, 1 = Chroma (扫完 Y 后扫 Chroma 8×8)
    reg       active;

    // Phase 8: 灰度 → MCU 为 8×8, 基地址 mx*8; 彩色 → 16×16, 基地址 mx*16
    wire [11:0] y_col_base = is_grayscale ? {mcu_col_idx[8:0], 3'd0} :
                                             {mcu_col_idx[7:0], 4'd0};
    wire [10:0] c_col_base = {mcu_col_idx[7:0], 3'd0};   // mx*8
    wire _unused_mcu_hi = |mcu_col_idx[15:9];
    // Phase 8: Y 扫描范围 — grayscale 8 行 8 列, 彩色 16×16
    wire [4:0] y_cnt_max_Y = is_grayscale ? 5'd7  : 5'd15;
    wire [3:0] col_cnt_max_Y = is_grayscale ? 4'd7 : 4'd15;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            y_cnt <= 5'd0; col_cnt <= 4'd0; phase <= 1'b0; active <= 1'b0;
            done <= 1'b0;
            mb_y_row <= 4'd0; mb_y_col <= 4'd0;
            mb_c_row <= 3'd0; mb_c_col <= 3'd0;
            lb_y_wr <= 1'b0; lb_y_row <= 4'd0; lb_y_col <= 12'd0; lb_y_data <= 8'd0;
            lb_c_wr <= 1'b0; lb_c_row <= 3'd0; lb_c_col <= 11'd0;
            lb_cb_data <= 8'd0; lb_cr_data <= 8'd0;
        end else if (soft_reset) begin
            active <= 1'b0; done <= 1'b0;
            lb_y_wr <= 1'b0; lb_c_wr <= 1'b0;
        end else begin
            done    <= 1'b0;
            lb_y_wr <= 1'b0;
            lb_c_wr <= 1'b0;

            if (start && !active) begin
                active <= 1'b1;
                phase  <= 1'b0;
                y_cnt  <= 5'd0;
                col_cnt<= 4'd0;
                mb_y_row <= 4'd0;
                mb_y_col <= 4'd0;
            end else if (active) begin
                if (!phase) begin
                    // Y phase: 16×16 (color) 或 8×8 (grayscale)
                    // 本周期地址已送，写 (y_cnt, col_cnt) -1 的结果
                    // 简化：读写同周期，直接组合 forward
                    lb_y_wr   <= 1'b1;
                    lb_y_row  <= y_cnt[3:0];
                    lb_y_col  <= y_col_base + {8'd0, col_cnt};
                    lb_y_data <= mb_y_data;

                    if (col_cnt == col_cnt_max_Y) begin
                        col_cnt <= 4'd0;
                        if (y_cnt == y_cnt_max_Y) begin
                            // Phase 8: grayscale 跳过 chroma，直接结束
                            if (is_grayscale) begin
                                active <= 1'b0;
                                done   <= 1'b1;
                            end else begin
                                phase <= 1'b1;
                                y_cnt <= 5'd0;
                                mb_c_row <= 3'd0;
                                mb_c_col <= 3'd0;
                            end
                        end else begin
                            y_cnt <= y_cnt + 5'd1;
                            mb_y_row <= y_cnt[3:0] + 4'd1;
                            mb_y_col <= 4'd0;
                        end
                    end else begin
                        col_cnt <= col_cnt + 4'd1;
                        mb_y_col <= col_cnt + 4'd1;
                    end
                end else begin
                    // Chroma phase: 8 rows × 8 cols
                    lb_c_wr    <= 1'b1;
                    lb_c_row   <= y_cnt[2:0];
                    lb_c_col   <= c_col_base + {8'd0, col_cnt[2:0]};
                    lb_cb_data <= mb_cb_data;
                    lb_cr_data <= mb_cr_data;

                    if (col_cnt[2:0] == 3'd7) begin
                        col_cnt <= 4'd0;
                        if (y_cnt[2:0] == 3'd7) begin
                            active <= 1'b0;
                            done   <= 1'b1;
                        end else begin
                            y_cnt    <= y_cnt + 5'd1;
                            mb_c_row <= y_cnt[2:0] + 3'd1;
                            mb_c_col <= 3'd0;
                        end
                    end else begin
                        col_cnt  <= col_cnt + 4'd1;
                        mb_c_col <= col_cnt[2:0] + 3'd1;
                    end
                end
            end
        end
    end

endmodule
