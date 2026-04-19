// ---------------------------------------------------------------------------
// huffman_decoder — 完整 8×8 block 熵解码 (DC+AC+zigzag)
//
// 对应 C: huff_decode_symbol + huff_decode_block
// 算法 T.81 A.6.2 (逐位匹配)
// ---------------------------------------------------------------------------
`include "jpeg_defs.vh"

module huffman_decoder (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        soft_reset,

    // Block 控制
    input  wire        blk_start,
    input  wire [1:0]  dc_sel,
    input  wire [1:0]  ac_sel,
    input  wire signed [15:0] dc_pred_in,
    output reg  signed [15:0] dc_pred_out,
    output reg         dc_pred_upd,
    output reg         blk_done,
    output reg         blk_err,

    // Bit window (bitstream_unpack)
    input  wire [15:0] peek_win,
    input  wire        peek_valid_any,    // 至少 1 位可用
    input  wire [6:0]  peek_bits_avail,   // 可用位数
    output reg  [4:0]  consume_n,
    output reg         consume_req,

    // HTable 读口
    output reg         ht_rd_ac,
    output reg  [1:0]  ht_rd_sel,
    output reg  [4:0]  ht_rd_l,
    input  wire [15:0] ht_mincode,
    input  wire [17:0] ht_maxcode,
    input  wire [7:0]  ht_valptr,
    output reg  [7:0]  ht_huff_idx,
    input  wire [7:0]  ht_huffval,

    // Coefficient 写出
    output reg         coef_start,    // 块开始 (下游清 64 个零)
    output reg         coef_wr,
    output reg  [5:0]  coef_nat_idx,
    output reg signed [15:0] coef_val
);

    // ZIGZAG table (k → natural index)
    reg [5:0] zig [0:63];
    initial begin
        zig[ 0]=6'd0;  zig[ 1]=6'd1;  zig[ 2]=6'd8;  zig[ 3]=6'd16;
        zig[ 4]=6'd9;  zig[ 5]=6'd2;  zig[ 6]=6'd3;  zig[ 7]=6'd10;
        zig[ 8]=6'd17; zig[ 9]=6'd24; zig[10]=6'd32; zig[11]=6'd25;
        zig[12]=6'd18; zig[13]=6'd11; zig[14]=6'd4;  zig[15]=6'd5;
        zig[16]=6'd12; zig[17]=6'd19; zig[18]=6'd26; zig[19]=6'd33;
        zig[20]=6'd40; zig[21]=6'd48; zig[22]=6'd41; zig[23]=6'd34;
        zig[24]=6'd27; zig[25]=6'd20; zig[26]=6'd13; zig[27]=6'd6;
        zig[28]=6'd7;  zig[29]=6'd14; zig[30]=6'd21; zig[31]=6'd28;
        zig[32]=6'd35; zig[33]=6'd42; zig[34]=6'd49; zig[35]=6'd56;
        zig[36]=6'd57; zig[37]=6'd50; zig[38]=6'd43; zig[39]=6'd36;
        zig[40]=6'd29; zig[41]=6'd22; zig[42]=6'd15; zig[43]=6'd23;
        zig[44]=6'd30; zig[45]=6'd37; zig[46]=6'd44; zig[47]=6'd51;
        zig[48]=6'd58; zig[49]=6'd59; zig[50]=6'd52; zig[51]=6'd45;
        zig[52]=6'd38; zig[53]=6'd31; zig[54]=6'd39; zig[55]=6'd46;
        zig[56]=6'd53; zig[57]=6'd60; zig[58]=6'd61; zig[59]=6'd54;
        zig[60]=6'd47; zig[61]=6'd55; zig[62]=6'd62; zig[63]=6'd63;
    end

    // --------------------- FSM ----------------------------------------
    localparam [3:0]
        S_IDLE      = 4'd0,
        S_WDC       = 4'd1,   // DC 符号：逐位匹配
        S_DC_LOOK   = 4'd2,   // huff_idx 已设，下 cycle 读 huffval
        S_DC_SIZE   = 4'd3,   // 拿到 size，写 ht_huff_idx
        S_DC_AMP    = 4'd4,   // 取 amp bits → amp_r 寄存
        S_DC_ACC    = 4'd13,  // Phase 4 时序：dc_pred_r += sext(amp_r)
        S_DC_WR     = 4'd5,   // 写 coef[0]
        S_WAC       = 4'd6,   // AC 符号逐位匹配
        S_AC_LOOK   = 4'd7,
        S_AC_SIZE   = 4'd8,
        S_AC_AMP    = 4'd9,   // 取 amp bits → amp_r 寄存
        S_AC_ACC    = 4'd14,  // Phase 4 时序：coef_val = sext(amp_r)
        S_AC_WR     = 4'd10,
        S_DONE      = 4'd11,
        S_ERR       = 4'd12;

    reg [3:0]  st;
    reg [15:0] code_acc;
    reg [4:0]  l;                   // 当前尝试的码长 1..16
    reg [7:0]  sym;
    reg [4:0]  size_r;
    reg [6:0]  k;                   // AC 索引 1..63 (7-bit 避免溢出)
    reg signed [15:0] dc_pred_r;
    // Phase 4 timing：将 amp_raw 寄存后再做 sext+add，打断
    // bitstream_unpack peek_win → amp_raw (变 shift) → sext (mask+sub) → dc_pred_r (add) 的组合长链。
    reg [15:0] amp_r;
    reg [4:0]  amp_size_r;
`ifdef HUF_DBG
    reg [31:0] dbg_cyc;             // increments on every posedge
`endif

    // 符号匹配 (组合)
    wire [15:0] new_code = {code_acc[14:0], peek_win[15]};
    // 未使用位告警抑制
    wire _unused_hf = |{ht_mincode[15:8], code_acc[15]};
    wire signed [17:0] new_code_s = {2'b00, new_code};
    wire match_new = ($signed(new_code_s) <= $signed(ht_maxcode));
    wire [7:0]  huff_idx_new = ht_valptr + new_code[7:0] - ht_mincode[7:0];

    // Sign-extend: amp_raw (size bits, 右对齐低 size 位) → signed
    function signed [15:0] sext;
        input [15:0] raw;  // size bits 在低端
        input [4:0]  sz;
        reg   [15:0] mask;
        begin
            if (sz == 0) sext = 16'sd0;
            else begin
                mask = (16'd1 << sz) - 16'd1;
                if (raw[sz-1])
                    sext = $signed(raw & mask);
                else
                    sext = $signed((raw & mask) - mask);
            end
        end
    endfunction

    // AMP 原始值 (取 peek_win 高 size_r 位)
    wire [15:0] amp_raw = peek_win >> (5'd16 - size_r);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            st <= S_IDLE;
            code_acc <= 16'd0; l <= 5'd0; sym <= 8'd0;
            size_r <= 5'd0; k <= 7'd0;
            dc_pred_r <= 16'sd0;
            amp_r <= 16'd0; amp_size_r <= 5'd0;
            consume_n <= 5'd0; consume_req <= 1'b0;
            ht_rd_ac <= 1'b0; ht_rd_sel <= 2'd0; ht_rd_l <= 5'd1;
            ht_huff_idx <= 8'd0;
            coef_start <= 1'b0; coef_wr <= 1'b0;
            coef_nat_idx <= 6'd0; coef_val <= 16'sd0;
            blk_done <= 1'b0; blk_err <= 1'b0;
            dc_pred_out <= 16'sd0; dc_pred_upd <= 1'b0;
`ifdef HUF_DBG
            dbg_cyc <= 32'd0;
`endif
        end else if (soft_reset) begin
            st <= S_IDLE;
            consume_req <= 1'b0; coef_wr <= 1'b0; coef_start <= 1'b0;
            blk_done <= 1'b0; blk_err <= 1'b0; dc_pred_upd <= 1'b0;
        end else begin
            // 默认 pulse 清零
            consume_req <= 1'b0;
            coef_wr     <= 1'b0;
            coef_start  <= 1'b0;
            blk_done    <= 1'b0;
            blk_err     <= 1'b0;
            dc_pred_upd <= 1'b0;
`ifdef HUF_DBG
            dbg_cyc <= dbg_cyc + 32'd1;
`endif

            case (st)
                // ------------------------------------------------------
                S_IDLE: if (blk_start) begin
                    dc_pred_r   <= dc_pred_in;
                    code_acc    <= 16'd0;
                    l           <= 5'd1;
                    k           <= 7'd1;
                    coef_start  <= 1'b1;
                    ht_rd_ac    <= 1'b0;          // DC
                    ht_rd_sel   <= dc_sel;
                    ht_rd_l     <= 5'd1;
                    st          <= S_WDC;
                end

                // ------------------------------------------------------ DC symbol
                S_WDC: if (peek_valid_any) begin
                    consume_n   <= 5'd1;
                    consume_req <= 1'b1;
                    code_acc    <= new_code;
                    if (match_new) begin
                        ht_huff_idx <= huff_idx_new;
                        st          <= S_DC_LOOK;
                    end else if (l == 5'd16) begin
                        blk_err <= 1'b1;
                        st      <= S_ERR;
                    end else begin
                        l       <= l + 5'd1;
                        ht_rd_l <= l + 5'd1;
                    end
                end

                S_DC_LOOK: begin
                    // 一个 cycle 延迟，确保 huffval 数据可用
                    sym <= ht_huffval;
                    st  <= S_DC_SIZE;
                end

                S_DC_SIZE: begin
                    if (sym > 8'd11) begin
                        blk_err <= 1'b1; st <= S_ERR;
                    end else if (sym == 8'd0) begin
                        // diff = 0，直接写 DC
                        st <= S_DC_WR;
                    end else begin
                        size_r <= sym[4:0];
                        st     <= S_DC_AMP;
                    end
                end

                S_DC_AMP: begin
                    // Phase 4：只寄存 amp_raw / size_r，sext+add 延到 S_DC_ACC。
                    if (peek_bits_avail >= {2'b00, size_r}) begin
                        consume_n   <= size_r;
                        consume_req <= 1'b1;
                        amp_r       <= amp_raw;
                        amp_size_r  <= size_r;
                        st          <= S_DC_ACC;
                    end
                end

                S_DC_ACC: begin
                    dc_pred_r <= dc_pred_r + sext(amp_r, amp_size_r);
                    st        <= S_DC_WR;
                end

                S_DC_WR: begin
                    coef_wr      <= 1'b1;
                    coef_nat_idx <= 6'd0;
                    coef_val     <= dc_pred_r;
                    // 准备 AC
                    code_acc  <= 16'd0;
                    l         <= 5'd1;
                    ht_rd_ac  <= 1'b1;
                    ht_rd_sel <= ac_sel;
                    ht_rd_l   <= 5'd1;
                    st        <= S_WAC;
                end

                // ------------------------------------------------------ AC symbol
                S_WAC: if (peek_valid_any) begin
                    consume_n   <= 5'd1;
                    consume_req <= 1'b1;
                    code_acc    <= new_code;
                    if (match_new) begin
                        ht_huff_idx <= huff_idx_new;
                        st          <= S_AC_LOOK;
                    end else if (l == 5'd16) begin
                        blk_err <= 1'b1; st <= S_ERR;
                    end else begin
                        l       <= l + 5'd1;
                        ht_rd_l <= l + 5'd1;
                    end
                end

                S_AC_LOOK: begin
`ifdef HUF_DBG
                    $display("[RTL HUF] dbgc=%0d S_AC_LOOK ht_huffval=0x%02h huff_idx=%0d ht_rd_l=%0d k=%0d",
                             dbg_cyc, ht_huffval, ht_huff_idx, ht_rd_l, k);
`endif
                    sym <= ht_huffval;
                    st  <= S_AC_SIZE;
                end

                S_AC_SIZE: begin
`ifdef HUF_DBG
                    $display("[RTL HUF] dbgc=%0d S_AC_SIZE sym=0x%02h k=%0d size_r_cur=%0d next_size_r=%0d l=%0d br=%s",
                             dbg_cyc, sym, k, size_r, {1'b0, sym[3:0]}, l,
                             (sym[3:0]==0) ? ((sym[7:4]==15) ? "ZRL" : "EOB") : "AMP");
`endif
                    size_r <= {1'b0, sym[3:0]};
                    if (sym[3:0] == 4'd0) begin
                        if (sym[7:4] == 4'd15) begin
                            // ZRL: run=16, k += 16
                            if (k + 7'd16 >= 7'd64) begin
                                // 末尾越界视为 EOB-like 完成
                                st <= S_DONE;
                            end else begin
                                k <= k + 7'd16;
                                // 下一个 AC 符号
                                code_acc  <= 16'd0;
                                l         <= 5'd1;
                                ht_rd_l   <= 5'd1;
                                st        <= S_WAC;
                            end
                        end else begin
                            // EOB
                            st <= S_DONE;
                        end
                    end else begin
                        // k += run (先 ；下面 AMP 结束后 k++)
                        k  <= k + {3'd0, sym[7:4]};
                        st <= S_AC_AMP;
                    end
                end

                S_AC_AMP: begin
`ifdef HUF_DBG
                    $display("[RTL HUF] dbgc=%0d S_AC_AMP k=%0d size_r=%0d peek_avail=%0d",
                             dbg_cyc, k, size_r, peek_bits_avail);
`endif
                    // Phase 4：只寄存 amp_raw / size_r，sext 延到 S_AC_ACC。
                    if (k >= 7'd64) begin
                        blk_err <= 1'b1; st <= S_ERR;
                    end else if (peek_bits_avail >= {2'b00, size_r}) begin
                        consume_n   <= size_r;
                        consume_req <= 1'b1;
                        amp_r       <= amp_raw;
                        amp_size_r  <= size_r;
                        st          <= S_AC_ACC;
                    end
                end

                S_AC_ACC: begin
                    coef_val <= sext(amp_r, amp_size_r);
                    st       <= S_AC_WR;
                end

                S_AC_WR: begin
`ifdef HUF_DBG
                    $display("[RTL HUF] dbgc=%0d S_AC_WR k=%0d coef_val=%0d zig_nat=%0d",
                             dbg_cyc, k, coef_val, zig[k[5:0]]);
`endif
                    coef_wr      <= 1'b1;
                    coef_nat_idx <= zig[k[5:0]];
                    // coef_val 已在 S_AC_ACC 中设置
                    k <= k + 7'd1;
                    if (k + 7'd1 >= 7'd64) begin
                        st <= S_DONE;
                    end else begin
                        code_acc <= 16'd0;
                        l        <= 5'd1;
                        ht_rd_l  <= 5'd1;
                        st       <= S_WAC;
                    end
                end

                // ------------------------------------------------------
                S_DONE: begin
                    dc_pred_out <= dc_pred_r;
                    dc_pred_upd <= 1'b1;
                    blk_done    <= 1'b1;
                    st          <= S_IDLE;
                end

                S_ERR: begin
                    // 停在错误状态
                    blk_err <= 1'b1;
                end

                default: st <= S_IDLE;
            endcase
        end
    end

endmodule
