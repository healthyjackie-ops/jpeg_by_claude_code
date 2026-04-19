// ---------------------------------------------------------------------------
// bitstream_unpack  —  byte 流 → 比特窗口 (含 0xFF00 unstuff)
//
// 对应 C:  bs_fill_bits / bs_get_bits_u / fetch_entropy_byte
//
// 位存储方案：shreg 右对齐，shreg[bit_cnt-1:0] 有效，取高 n 位 =
//             (shreg >> (bit_cnt - n)) & ((1<<n)-1)
//
// 接口（简单 valid/ready 握手）：
//   byte_in, byte_valid, byte_ready     -- 上游送字节
//   consume_n (1..16), consume_req      -- 下游消耗窗口
//   peek_win[15:0], peek_valid (>=consume_n 位可用)
//   marker_detected / marker_byte       -- 遇到 0xFF xx (xx!=00)
// ---------------------------------------------------------------------------
module bitstream_unpack (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        flush,

    input  wire [7:0]  byte_in,
    input  wire        byte_valid,
    output wire        byte_ready,

    output reg         marker_detected,
    output reg  [7:0]  marker_byte,

    input  wire [4:0]  consume_n,
    input  wire        consume_req,
    output wire [15:0] peek_win,
    output wire        peek_valid,
    output wire [6:0]  bit_cnt_o
);

    // 32-bit 右对齐比特缓冲；最多存 32 位，每周期消耗 ≤16，补充 8 → 要求 bit_cnt ≤ 24 才能再吃
    reg [31:0] shreg;
    reg  [5:0] bit_cnt;     // 0..32
    assign bit_cnt_o = {1'b0, bit_cnt};
    reg        ff_wait;     // 已经吃进 0xFF，等待下一个字节判定

    // 可以接纳新字节的条件：空间足够 且 未 marker 暂停
    wire can_accept = (bit_cnt <= 6'd24) && !marker_detected;
    assign byte_ready = byte_valid && can_accept;

    // 下一拍的 bit_cnt / shreg
    reg [5:0]  next_bit_cnt;
    reg [31:0] next_shreg;
    reg        next_ff_wait;
    reg        next_marker_det;
    reg [7:0]  next_marker_byte;

    // ---- peek (组合，post-consume/post-load 视图) ---------------------------
    // peek_win 反映 consume+load 之后的新 shreg/bit_cnt，让 consumer 在同一 cycle
    // 看到下一个可用位 —— 否则 consume_req 的 1-cycle 寄存器延迟会导致连续迭代
    // 重复读取同一位 (huffman 逐位匹配的 bug 来源)。
    // peek_valid 保留基于当前 bit_cnt 的语义以避免 always@(*) 中的组合回路。
    wire [5:0]  shift_r = (next_bit_cnt >= 6'd16) ? (next_bit_cnt - 6'd16) : 6'd0;
    wire [31:0] shifted = next_shreg >> shift_r;
    wire [15:0] masked_hi = shifted[15:0];
    wire _unused_shifted_hi = |shifted[31:16];
    wire [15:0] lt16      = (next_bit_cnt == 6'd0) ? 16'd0 :
                            (next_shreg[15:0] << (6'd16 - next_bit_cnt));
    assign peek_win   = (next_bit_cnt >= 6'd16) ? masked_hi : lt16;
    assign peek_valid = (bit_cnt >= {1'b0, consume_n});

    always @(*) begin
        // 默认保持
        next_bit_cnt     = bit_cnt;
        next_shreg       = shreg;
        next_ff_wait     = ff_wait;
        next_marker_det  = marker_detected;
        next_marker_byte = marker_byte;

        // 1) 消耗 (先消耗，后填入)
        if (consume_req && peek_valid) begin
            // shreg 低位保留 (bit_cnt - consume_n) 位
            next_bit_cnt = bit_cnt - {1'b0, consume_n};
            // 掩码保留低 next_bit_cnt 位
            next_shreg = shreg & ~(32'hFFFF_FFFF << next_bit_cnt);
        end

        // 2) 填入新字节 (组合结合 next_bit_cnt)
        if (byte_valid && can_accept) begin
            if (ff_wait) begin
                next_ff_wait = 1'b0;
                if (byte_in == 8'h00) begin
                    // stuff 0x00 丢弃：保持已 push 的 0xFF
                end else begin
                    // 真 marker：撤回已 push 的 0xFF。
                    // 该 0xFF 位于 shreg[7:0]（push 时 shreg <<= 8 ; | byte_in），
                    // 而消耗 (consume) 只从窗口顶部剥离 bit。因此正确的撤回是
                    // 右移 8 位（丢弃低字节），再裁剪到新的 bit_cnt。
                    next_bit_cnt = next_bit_cnt - 6'd8;
                    next_shreg   = (next_shreg >> 8)
                                   & ~(32'hFFFF_FFFF << next_bit_cnt);
                    next_marker_det  = 1'b1;
                    next_marker_byte = byte_in;
                end
            end else begin
                // push 字节
                next_shreg   = (next_shreg << 8) | {24'd0, byte_in};
                next_bit_cnt = next_bit_cnt + 6'd8;
                if (byte_in == 8'hFF)
                    next_ff_wait = 1'b1;
            end
        end
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            shreg           <= 32'd0;
            bit_cnt         <= 6'd0;
            ff_wait         <= 1'b0;
            marker_detected <= 1'b0;
            marker_byte     <= 8'd0;
        end else if (flush) begin
            shreg           <= 32'd0;
            bit_cnt         <= 6'd0;
            ff_wait         <= 1'b0;
            marker_detected <= 1'b0;
            marker_byte     <= 8'd0;
        end else begin
            shreg           <= next_shreg;
            bit_cnt         <= next_bit_cnt;
            ff_wait         <= next_ff_wait;
            marker_detected <= next_marker_det;
            marker_byte     <= next_marker_byte;
`ifdef BS_DBG
            if (bit_cnt != next_bit_cnt || shreg != next_shreg) begin
                $display("  [BS] t=%0t bc=%0d->%0d shreg=0x%08X->0x%08X bv=%b can=%b br=%b by=0x%02X cr=%b pv=%b cn=%0d ff=%b->%b",
                         $time, bit_cnt, next_bit_cnt, shreg, next_shreg,
                         byte_valid, can_accept, byte_ready, byte_in,
                         consume_req, peek_valid, consume_n, ff_wait, next_ff_wait);
            end
`endif
        end
    end

endmodule
