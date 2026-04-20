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
    input  wire        is_grayscale,   // Phase 8: 1=MCU-row 8 行, Cb/Cr 输出 0x80
    input  wire        is_444,         // Phase 9: 1=chroma 全分辨率，c_col=x_col, c_row=y_row[2:0]
    input  wire        is_422,         // Phase 10: 1=chroma 横半分辨率，c_col=x_col[11:1], c_row=y_row[2:0]
    output reg         row_done,

    // line_buffer 读口
    output reg  [3:0]  rd_y_row,
    output reg  [11:0] rd_y_col,
    output reg  [2:0]  rd_c_row,
    output reg  [11:0] rd_c_col,       // Phase 9: 扩宽至 12 位
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
    // Phase 8: grayscale MCU-row 是 8 行；非对齐时 tail 由 img_height[2:0] 决定
    // Phase 9: 4:4:4 MCU-row 也是 8 行 — 与 grayscale 复用同一份 tail 判断
    // Phase 10: 4:2:2 MCU-row 也是 8 行 — 同样复用 tail 判断
    wire mcu_8x8 = is_grayscale | is_444 | is_422;
    wire [3:0]  last_mcu_rows_color = (img_height[3:0] == 4'd0) ? 4'd15 : (img_height[3:0] - 4'd1);
    wire [3:0]  last_mcu_rows_8x8   = (img_height[2:0] == 3'd0) ? 4'd7  :
                                                                 {1'b0, img_height[2:0] - 3'd1};
    wire [3:0]  full_mcu_rows = mcu_8x8 ? 4'd7 : 4'd15;
    wire [3:0]  last_mcu_rows = mcu_8x8 ? last_mcu_rows_8x8 : last_mcu_rows_color;
    wire [3:0]  y_row_max     = is_last_row ? last_mcu_rows : full_mcu_rows;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            y_row <= 4'd0; x_col <= 12'd0;
            active <= 1'b0; row_done <= 1'b0;
            tvalid <= 1'b0; tdata <= 24'd0;
            tuser_sof <= 1'b0; tlast <= 1'b0;
            rd_y_row <= 4'd0; rd_y_col <= 12'd0;
            rd_c_row <= 3'd0; rd_c_col <= 12'd0;
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
                rd_c_col <= 12'd0;
            end

            // 组合读：本周期地址对应的数据可在下 cycle 使用
            if (active && (!tvalid || tready)) begin
                // 驱动当前 (y_row, x_col) 像素
                tvalid    <= 1'b1;
                // Phase 8: grayscale 下 Cb/Cr 固定 0x80（中灰）
                tdata     <= {rd_y_data,
                              is_grayscale ? 8'h80 : rd_cb_data,
                              is_grayscale ? 8'h80 : rd_cr_data};
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
                // Phase 9: 4:4:4 下 chroma 与 Y 同分辨率：c_row=y_row[2:0], c_col=x_col
                //         4:2:0 下 c_row=y_row[3:1], c_col=x_col[11:1]
                //         grayscale 下 c_* 不使用 (tdata Cb/Cr 被 0x80 覆盖)
                // Phase 10: 4:2:2 → c_row=y_row[2:0] (Y 行同步), c_col=x_col[11:1] (横半分)
                if (x_col == (img_width - 12'd1)) begin
                    rd_y_row <= (y_row == y_row_max) ? 4'd0 : (y_row + 4'd1);
                    rd_y_col <= 12'd0;
                    if (is_444 || is_422)
                        rd_c_row <= (y_row == y_row_max) ? 3'd0 : next_y_row[2:0];
                    else
                        rd_c_row <= (y_row == y_row_max) ? 3'd0 : next_y_row[3:1];
                    rd_c_col <= 12'd0;
                end else begin
                    rd_y_row <= y_row;
                    rd_y_col <= next_x_col;
                    if (is_444) begin
                        rd_c_row <= y_row[2:0];
                        rd_c_col <= next_x_col;
                    end else if (is_422) begin
                        rd_c_row <= y_row[2:0];
                        rd_c_col <= {1'b0, next_x_col[11:1]};
                    end else begin
                        rd_c_row <= y_row[3:1];
                        rd_c_col <= {1'b0, next_x_col[11:1]};
                    end
                end
            end else if (tready && tvalid) begin
                tvalid <= 1'b0;
            end
        end
    end

    // Phase 6: is_last_row 已被 y_row_max 消费；此处保留占位

endmodule
