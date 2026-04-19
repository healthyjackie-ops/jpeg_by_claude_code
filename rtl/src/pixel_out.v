// ---------------------------------------------------------------------------
// pixel_out — Raster scan 扫描 line_buffer，输出 {Y,Cb,Cr} 24-bit 流
//
// 依次扫 16 × image_width 像素 (一 MCU-row)，chroma 做 nearest-neighbor
// 通过 AXI-Stream 风格接口 (tvalid/tready/tuser=SOF/tlast=EOL)
// ---------------------------------------------------------------------------
module pixel_out (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        soft_reset,

    // 启动一次 MCU-row 输出
    input  wire        row_start,
    input  wire [11:0] img_width,      // 像素
    input  wire [11:0] img_height,     // Phase 6: 像素高度，用于最后 MCU-row 行裁剪
    input  wire        is_first_row,
    input  wire        is_last_row,
    output reg         row_done,

    // line_buffer 读口
    output reg  [3:0]  rd_y_row,
    output reg  [11:0] rd_y_col,
    output reg  [2:0]  rd_c_row,
    output reg  [10:0] rd_c_col,
    input  wire [7:0]  rd_y_data,
    input  wire [7:0]  rd_cb_data,
    input  wire [7:0]  rd_cr_data,

    // AXI-Stream 输出
    output reg         tvalid,
    input  wire        tready,
    output reg  [23:0] tdata,
    output reg         tuser_sof,      // 帧第一像素
    output reg         tlast           // EOL
);

    // 扫描状态
    reg [3:0]  y_row;       // 0..15 within MCU row
    reg [11:0] x_col;       // 0..width-1
    reg        active;

    wire [3:0]  next_y_row = y_row + 4'd1;
    wire [11:0] next_x_col = x_col + 12'd1;
    wire _unused_pix = next_y_row[0];

    // Phase 6: 非 16 对齐 — 最后 MCU-row 可能只有 (img_height[3:0]) 行有效
    wire [3:0]  last_mcu_rows  = (img_height[3:0] == 4'd0) ? 4'd15 : (img_height[3:0] - 4'd1);
    wire [3:0]  y_row_max      = is_last_row ? last_mcu_rows : 4'd15;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            y_row <= 4'd0; x_col <= 12'd0;
            active <= 1'b0; row_done <= 1'b0;
            tvalid <= 1'b0; tdata <= 24'd0;
            tuser_sof <= 1'b0; tlast <= 1'b0;
            rd_y_row <= 4'd0; rd_y_col <= 12'd0;
            rd_c_row <= 3'd0; rd_c_col <= 11'd0;
        end else if (soft_reset) begin
            active <= 1'b0; tvalid <= 1'b0;
            row_done <= 1'b0;
        end else begin
            row_done <= 1'b0;

            if (row_start && !active) begin
                active <= 1'b1;
                y_row  <= 4'd0;
                x_col  <= 12'd0;
                rd_y_row <= 4'd0;
                rd_y_col <= 12'd0;
                rd_c_row <= 3'd0;
                rd_c_col <= 11'd0;
            end

            // 组合读：本周期地址对应的数据可在下 cycle 使用
            if (active && (!tvalid || tready)) begin
                // 驱动当前 (y_row, x_col) 像素
                tvalid    <= 1'b1;
                tdata     <= {rd_y_data, rd_cb_data, rd_cr_data};
                tuser_sof <= is_first_row && (y_row == 4'd0) && (x_col == 12'd0);
                tlast     <= (x_col == (img_width - 12'd1));

                // 推进坐标（Phase 6: y_row_max 取代硬编 15）
                if (x_col == (img_width - 12'd1)) begin
                    x_col <= 12'd0;
                    if (y_row == y_row_max) begin
                        active   <= 1'b0;
                        row_done <= 1'b1;
                        y_row    <= 4'd0;
                    end else begin
                        y_row <= y_row + 4'd1;
                    end
                end else begin
                    x_col <= x_col + 12'd1;
                end

                // 读地址前送 (下 cycle 的数据)
                if (x_col == (img_width - 12'd1)) begin
                    rd_y_row <= (y_row == y_row_max) ? 4'd0 : (y_row + 4'd1);
                    rd_y_col <= 12'd0;
                    rd_c_row <= (y_row == y_row_max) ? 3'd0 : next_y_row[3:1];
                    rd_c_col <= 11'd0;
                end else begin
                    rd_y_row <= y_row;
                    rd_y_col <= next_x_col;
                    rd_c_row <= y_row[3:1];
                    rd_c_col <= next_x_col[11:1];
                end
            end else if (tready && tvalid) begin
                tvalid <= 1'b0;
            end
        end
    end

    // Phase 6: is_last_row 已被 y_row_max 消费；此处保留占位

endmodule
