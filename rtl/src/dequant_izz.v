// ---------------------------------------------------------------------------
// dequant_izz — 反量化 (natural order 已由 huffman_decoder 通过 ZIGZAG 查完)
//
// 对应 C: dequant_block (coef_out[i] = coef_in[i] * qt[i])
//
// 数据流：
//   huffman_decoder 逐个写入 (coef_nat_idx, coef_val)，完成 block_done 脉冲后，
//   本模块读 qt[i] (i=0..63) 与 coef_buf 相乘，输出到 idct 块缓冲。
//
// 为了与 C bit-exact，qt 已在 header_parser 中按 natural order 存储。
// ---------------------------------------------------------------------------
module dequant_izz (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        soft_reset,

    // 来自 huffman_decoder
    input  wire        coef_start,
    input  wire        coef_wr,
    input  wire [5:0]  coef_nat_idx,
    input  wire signed [15:0] coef_val,
    input  wire        blk_in_done,       // huffman blk_done (表 64 coef 全部写入)

    // 当前 block 的 Q table 选择
    input  wire [1:0]  qt_sel,

    // 写 QTable 端口 (读)
    output reg  [1:0]  qt_rd_sel,
    output reg  [5:0]  qt_rd_idx,
    input  wire [15:0] qt_rd_data,

    // 输出到 IDCT (Phase 13: 32b 以承载 coef 16b signed × qt 16b unsigned)
    output reg         dq_start,          // 脉冲：新 block 开始
    output reg         dq_wr,
    output reg  [5:0]  dq_idx,            // natural order 0..63
    output reg signed [31:0] dq_val,
    output reg         dq_done
);

    // 64 × 16-bit coefficient buffer
    reg signed [15:0] coef_buf [0:63];
    integer i;

    reg [6:0] stream_cnt;  // 0..64 for natural iteration

    localparam [1:0] S_IDLE = 2'd0, S_RUN = 2'd1, S_DONE = 2'd2;
    reg [1:0] st;

    // coef_buf — small RF, writes only in sync domain (no reset). coef_start
    // clears all 64 entries combinationally (yosys will lower this to flops
    // since 64 write ports have no SRAM mapping).
    always @(posedge clk) begin
        if (coef_start) begin
            for (i=0;i<64;i=i+1) coef_buf[i] <= 16'sd0;
        end else if (coef_wr) begin
            coef_buf[coef_nat_idx] <= coef_val;
        end
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            stream_cnt <= 7'd0;
            st <= S_IDLE;
            qt_rd_sel <= 2'd0; qt_rd_idx <= 6'd0;
            dq_start <= 1'b0; dq_wr <= 1'b0;
            dq_idx <= 6'd0; dq_val <= 32'sd0; dq_done <= 1'b0;
        end else if (soft_reset) begin
            st <= S_IDLE;
            dq_start <= 1'b0; dq_wr <= 1'b0; dq_done <= 1'b0;
        end else begin
            // FSM: blk_in_done 脉冲后启动 dequant 流
            dq_start <= 1'b0;
            dq_wr    <= 1'b0;
            dq_done  <= 1'b0;

            case (st)
                S_IDLE: begin
                    if (blk_in_done) begin
                        dq_start   <= 1'b1;
                        qt_rd_sel  <= qt_sel;
                        qt_rd_idx  <= 6'd0;
                        stream_cnt <= 7'd0;
                        st         <= S_RUN;
                    end
                end
                S_RUN: begin
                    // 流水读 qt + 乘：rd_idx 预读，stream_cnt-1 的结果本 cycle 可用
                    // 简化：使用寄存读 1 cycle 后乘
                    if (stream_cnt == 7'd0) begin
                        // 第 1 拍只预读 qt[0]
                        qt_rd_idx <= 6'd1;
                        stream_cnt <= stream_cnt + 7'd1;
                    end else if (stream_cnt <= 7'd63) begin
                        // 相乘并写出 (natural_idx = stream_cnt - 1)
                        // coef 16b signed × qt 16b unsigned → 需要 17b signed 扩位
                        dq_wr  <= 1'b1;
                        dq_idx <= stream_cnt[5:0] - 6'd1;
                        dq_val <= coef_buf[stream_cnt[5:0] - 6'd1] *
                                  $signed({1'b0, qt_rd_data});
                        qt_rd_idx  <= stream_cnt[5:0] + 6'd1;
                        stream_cnt <= stream_cnt + 7'd1;
                    end else begin
                        // stream_cnt == 64：写最后一个 (idx=63)
                        dq_wr  <= 1'b1;
                        dq_idx <= 6'd63;
                        dq_val <= coef_buf[63] * $signed({1'b0, qt_rd_data});
                        st     <= S_DONE;
                    end
                end
                S_DONE: begin
                    dq_done <= 1'b1;
                    st      <= S_IDLE;
                end
                default: st <= S_IDLE;
            endcase
        end
    end

endmodule
