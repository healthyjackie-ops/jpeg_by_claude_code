// ---------------------------------------------------------------------------
// axi_stream_fifo — 同步单时钟 FIFO，带 AXI-Stream 握手封装
//                   用于输入字节流 / 输出像素流缓冲
//
// 参数：
//   DW   数据宽度 (输入 8, 输出 24)
//   UW   tuser 宽度 (0 表示无 tuser)
//   DEPTH FIFO 深度 (必须 2 的幂)
// ---------------------------------------------------------------------------
module axi_stream_fifo #(
    parameter DW    = 8,
    parameter UW    = 1,
    parameter DEPTH = 32
) (
    input  wire          clk,
    input  wire          rst_n,
    input  wire          flush,

    // slave (write)
    input  wire [DW-1:0] s_tdata,
    input  wire [UW-1:0] s_tuser,
    input  wire          s_tlast,
    input  wire          s_tvalid,
    output wire          s_tready,

    // master (read)
    output wire [DW-1:0] m_tdata,
    output wire [UW-1:0] m_tuser,
    output wire          m_tlast,
    output wire          m_tvalid,
    input  wire          m_tready,

    output wire          empty,
    output wire          full
);

    localparam AW = $clog2(DEPTH);

    reg [DW+UW+1-1:0] mem [0:DEPTH-1];
    reg [AW:0] wptr, rptr;

    wire [AW:0] cnt = wptr - rptr;
    assign full    = (cnt == DEPTH);
    assign empty   = (cnt == 0);

    assign s_tready = !full;
    assign m_tvalid = !empty;

    wire do_wr = s_tvalid && !full;
    wire do_rd = m_tready && !empty;

    // Memory writes — pure sync, no reset (FIFO storage is SRAM/RF target;
    // consumers only read after do_wr advances wptr, so stale init is never
    // observed). Pointers still get async reset below.
    always @(posedge clk) begin
        if (do_wr)
            mem[wptr[AW-1:0]] <= {s_tuser, s_tlast, s_tdata};
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            wptr <= 0; rptr <= 0;
        end else if (flush) begin
            wptr <= 0; rptr <= 0;
        end else begin
            if (do_wr) wptr <= wptr + 1'b1;
            if (do_rd) rptr <= rptr + 1'b1;
        end
    end

    assign {m_tuser, m_tlast, m_tdata} = mem[rptr[AW-1:0]];

endmodule
