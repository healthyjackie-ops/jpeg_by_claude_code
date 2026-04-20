// ---------------------------------------------------------------------------
// qtable_ram — 4 × 64 × 16-bit 量化表存储 (natural order)
//   写端：header_parser 在 DQT 段写入（Pq=0 时高 8b=0；Pq=1 时 MSB+LSB 合并）
//   读端：dequant_izz 按 natural index 读出
//
// Phase 13: 入口升到 16-bit 以支持 P=12 时的 Pq=1 Q-table (64×16b)。
// ---------------------------------------------------------------------------
module qtable_ram (
    input  wire        clk,

    input  wire        wr_en,
    input  wire [1:0]  wr_sel,   // Tq
    input  wire [5:0]  wr_idx,
    input  wire [15:0] wr_data,

    input  wire [1:0]  rd_sel,
    input  wire [5:0]  rd_idx,
    output reg  [15:0] rd_data
);

    // 4 banks × 64 entries × 16 bits = 512 B (可综合为 distributed RAM)
    reg [15:0] mem [0:255];

    always @(posedge clk) begin
        if (wr_en)
            mem[{wr_sel, wr_idx}] <= wr_data;
        rd_data <= mem[{rd_sel, rd_idx}];
    end

endmodule
