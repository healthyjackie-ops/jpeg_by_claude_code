// ---------------------------------------------------------------------------
// qtable_ram — 4 × 64 × 8-bit 量化表存储 (natural order)
//   写端：header_parser 在 DQT 段写入（natural order 已重排）
//   读端：dequant_izz 按 natural index 读出
// ---------------------------------------------------------------------------
module qtable_ram (
    input  wire       clk,

    input  wire       wr_en,
    input  wire [1:0] wr_sel,   // Tq
    input  wire [5:0] wr_idx,
    input  wire [7:0] wr_data,

    input  wire [1:0] rd_sel,
    input  wire [5:0] rd_idx,
    output reg  [7:0] rd_data
);

    // 4 banks × 64 entries × 8 bits = 256 B (可综合为 distributed RAM)
    reg [7:0] mem [0:255];

    always @(posedge clk) begin
        if (wr_en)
            mem[{wr_sel, wr_idx}] <= wr_data;
        rd_data <= mem[{rd_sel, rd_idx}];
    end

endmodule
