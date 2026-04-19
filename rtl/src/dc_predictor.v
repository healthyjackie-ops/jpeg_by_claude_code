// ---------------------------------------------------------------------------
// dc_predictor — 3 通道 DC 前值寄存器 (Y/Cb/Cr)
//
// 对应 C: component_t.dc_pred，jpeg_decode 每次 huff_decode_block 前读/后写
// ---------------------------------------------------------------------------
module dc_predictor (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        soft_reset,
    input  wire        new_frame,      // 脉冲：清三通道

    input  wire [1:0]  comp_sel,       // 0=Y, 1=Cb, 2=Cr
    input  wire        wr_en,
    input  wire signed [15:0] wr_data,

    output reg  signed [15:0] rd_data_y,
    output reg  signed [15:0] rd_data_cb,
    output reg  signed [15:0] rd_data_cr
);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rd_data_y  <= 16'sd0;
            rd_data_cb <= 16'sd0;
            rd_data_cr <= 16'sd0;
        end else if (soft_reset || new_frame) begin
            rd_data_y  <= 16'sd0;
            rd_data_cb <= 16'sd0;
            rd_data_cr <= 16'sd0;
        end else if (wr_en) begin
            case (comp_sel)
                2'd0: rd_data_y  <= wr_data;
                2'd1: rd_data_cb <= wr_data;
                2'd2: rd_data_cr <= wr_data;
                default: ;
            endcase
        end
    end

endmodule
