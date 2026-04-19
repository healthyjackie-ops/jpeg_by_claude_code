// ---------------------------------------------------------------------------
// line_buffer — MCU 行缓冲 (16 扫描线 Y + 8 扫描线 Cb + 8 扫描线 Cr)
//
// 写侧：每完成一个 MCU (mcu_buffer 产生)，按 mcu 列索引 mx 写入本 MCU-row buf
// 读侧：一整行 MCU 完毕后，按 raster scan 读 16 × W 像素 (含 upsample)
//
// 支持最大 W = 4096 / CW = 2048
// ---------------------------------------------------------------------------
module line_buffer #(
    parameter MAX_W = 4096
) (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        soft_reset,

    // 写侧 (从 mcu_buffer 复制)
    input  wire        wr_en,
    input  wire [3:0]  wr_y_row,      // 0..15 within MCU row
    input  wire [11:0] wr_y_col_abs,  // 绝对像素列 (0..W-1)
    input  wire [7:0]  wr_y_data,

    input  wire        wr_c_en,
    input  wire [2:0]  wr_c_row,      // 0..7 within chroma MCU row
    input  wire [10:0] wr_c_col_abs,  // 0..CW-1
    input  wire [7:0]  wr_cb_data,
    input  wire [7:0]  wr_cr_data,

    // 读侧 (raster)
    input  wire [3:0]  rd_y_row,
    input  wire [11:0] rd_y_col,
    input  wire [2:0]  rd_c_row,
    input  wire [10:0] rd_c_col,
    output wire [7:0]  rd_y_data,
    output wire [7:0]  rd_cb_data,
    output wire [7:0]  rd_cr_data
);

    // 16 rows × MAX_W = 64K 最大
    reg [7:0] ybuf  [0:16*MAX_W-1];
    reg [7:0] cbbuf [0:8*(MAX_W/2)-1];
    reg [7:0] crbuf [0:8*(MAX_W/2)-1];

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            // 不在复位时清零 (避免 init 太久)；soft_reset 也不清
        end else begin
            if (wr_en)
                ybuf[{wr_y_row, wr_y_col_abs}] <= wr_y_data;
            if (wr_c_en) begin
                cbbuf[{wr_c_row, wr_c_col_abs}] <= wr_cb_data;
                crbuf[{wr_c_row, wr_c_col_abs}] <= wr_cr_data;
            end
        end
    end

    assign rd_y_data  = ybuf[{rd_y_row, rd_y_col}];
    assign rd_cb_data = cbbuf[{rd_c_row, rd_c_col}];
    assign rd_cr_data = crbuf[{rd_c_row, rd_c_col}];

    // 防止 soft_reset 未使用告警
    wire _unused = soft_reset;

endmodule
