// ---------------------------------------------------------------------------
// header_parser — JPEG Marker FSM
//
// 对应 C: jpeg_parse_headers 及 parse_dqt/dht/sof0/sos/dri
//
// 字节输入：byte_in/byte_valid/byte_ready   (HEADER 段消耗)
// 解析完成后 data_mode=1，后续字节由顶层路由到 bitstream_unpack
// ---------------------------------------------------------------------------
`include "jpeg_defs.vh"

module header_parser (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        start,           // 新一帧，来自 CSR START
    input  wire        soft_reset,

    // Byte stream (HEADER 段)
    input  wire [7:0]  byte_in,
    input  wire        byte_valid,
    output reg         byte_ready,

    // 写 QTable (8-bit value, 存 natural-order 索引)
    output reg         qt_wr,
    output reg  [1:0]  qt_sel,         // Tq
    output reg  [5:0]  qt_idx,         // natural order index
    output reg  [7:0]  qt_val,

    // 写 HTable BITS / HUFFVAL
    output reg         ht_bits_wr,
    output reg         ht_val_wr,
    output reg         ht_ac,          // 0=DC, 1=AC
    output reg  [1:0]  ht_sel,         // Th (0..3)
    output reg  [4:0]  ht_bits_idx,    // 1..16
    output reg  [7:0]  ht_bits_val,
    output reg  [7:0]  ht_val_idx,
    output reg  [7:0]  ht_val_data,
    output reg         ht_build_start, // 触发 huffman_tables 构造 mincode/maxcode/valptr
    input  wire        ht_build_done,  // 等构建完再去吃下一个 DHT（避免 pulse 被吞）

    // Frame info
    output reg  [15:0] img_width,
    output reg  [15:0] img_height,
    output reg  [1:0]  comp0_qt,       // Y quant-table id
    output reg  [1:0]  comp1_qt,
    output reg  [1:0]  comp2_qt,
    output reg  [1:0]  comp0_td,       // DC Huffman tbl idx
    output reg  [1:0]  comp1_td,
    output reg  [1:0]  comp2_td,
    output reg  [1:0]  comp0_ta,       // AC Huffman tbl idx
    output reg  [1:0]  comp1_ta,
    output reg  [1:0]  comp2_ta,

    output reg         header_done,    // SOS 结束
    output reg         data_mode,      // 1=进入熵编码数据段
    output reg         frame_done,     // 遇 EOI
    output reg  [15:0] dri_interval,   // Phase 7: DRI 间隔 MCU 数（0=禁用）
    output reg  [1:0]  num_components_o,// Phase 8: 1=grayscale, 3=YCbCr 4:2:0
    output reg  [8:0]  err             // sticky
);

    // ------------------------------------------------------------------
    // FSM states (localparam, Verilog-2001)
    // ------------------------------------------------------------------
    localparam [5:0]
        S_IDLE         = 6'd0,
        S_SOI_FF       = 6'd1,
        S_SOI_D8       = 6'd2,
        S_FIND_FF      = 6'd3,
        S_FIND_MK      = 6'd4,
        S_DISPATCH     = 6'd5,
        S_LEN_HI       = 6'd6,
        S_LEN_LO       = 6'd7,
        S_SKIP         = 6'd8,
        S_DQT_PQTQ     = 6'd9,
        S_DQT_VAL      = 6'd10,
        S_DHT_TCTH     = 6'd11,
        S_DHT_BITS     = 6'd12,
        S_DHT_VAL      = 6'd13,
        S_DHT_BUILD    = 6'd14,
        S_DHT_WAIT_BLD = 6'd35,     // 等 htable_ram.build_done（避免多 DHT 时 pulse 被吞）
        S_SOF_P        = 6'd15,
        S_SOF_H_HI     = 6'd16,
        S_SOF_H_LO     = 6'd17,
        S_SOF_W_HI     = 6'd18,
        S_SOF_W_LO     = 6'd19,
        S_SOF_NF       = 6'd20,
        S_SOF_COMP_ID  = 6'd21,
        S_SOF_COMP_HV  = 6'd22,
        S_SOF_COMP_TQ  = 6'd23,
        S_SOS_NS       = 6'd24,
        S_SOS_CS       = 6'd25,
        S_SOS_TDTA     = 6'd26,
        S_SOS_SS       = 6'd27,
        S_SOS_SE       = 6'd28,
        S_SOS_AHAL     = 6'd29,
        S_DRI_HI       = 6'd30,
        S_DRI_LO       = 6'd31,
        S_DATA         = 6'd32,
        S_DONE         = 6'd33,
        S_ERROR        = 6'd34;

    reg [5:0] state;

    // Zigzag → natural order table (与 C 一致)
    reg [5:0] zz_natural [0:63];
    initial begin
        zz_natural[0]=6'd0;  zz_natural[1]=6'd1;  zz_natural[2]=6'd8;  zz_natural[3]=6'd16;
        zz_natural[4]=6'd9;  zz_natural[5]=6'd2;  zz_natural[6]=6'd3;  zz_natural[7]=6'd10;
        zz_natural[8]=6'd17; zz_natural[9]=6'd24; zz_natural[10]=6'd32; zz_natural[11]=6'd25;
        zz_natural[12]=6'd18;zz_natural[13]=6'd11;zz_natural[14]=6'd4;  zz_natural[15]=6'd5;
        zz_natural[16]=6'd12;zz_natural[17]=6'd19;zz_natural[18]=6'd26; zz_natural[19]=6'd33;
        zz_natural[20]=6'd40;zz_natural[21]=6'd48;zz_natural[22]=6'd41; zz_natural[23]=6'd34;
        zz_natural[24]=6'd27;zz_natural[25]=6'd20;zz_natural[26]=6'd13; zz_natural[27]=6'd6;
        zz_natural[28]=6'd7; zz_natural[29]=6'd14;zz_natural[30]=6'd21; zz_natural[31]=6'd28;
        zz_natural[32]=6'd35;zz_natural[33]=6'd42;zz_natural[34]=6'd49; zz_natural[35]=6'd56;
        zz_natural[36]=6'd57;zz_natural[37]=6'd50;zz_natural[38]=6'd43; zz_natural[39]=6'd36;
        zz_natural[40]=6'd29;zz_natural[41]=6'd22;zz_natural[42]=6'd15; zz_natural[43]=6'd23;
        zz_natural[44]=6'd30;zz_natural[45]=6'd37;zz_natural[46]=6'd44; zz_natural[47]=6'd51;
        zz_natural[48]=6'd58;zz_natural[49]=6'd59;zz_natural[50]=6'd52; zz_natural[51]=6'd45;
        zz_natural[52]=6'd38;zz_natural[53]=6'd31;zz_natural[54]=6'd39; zz_natural[55]=6'd46;
        zz_natural[56]=6'd53;zz_natural[57]=6'd60;zz_natural[58]=6'd61; zz_natural[59]=6'd54;
        zz_natural[60]=6'd47;zz_natural[61]=6'd55;zz_natural[62]=6'd62; zz_natural[63]=6'd63;
    end

    // Segment length counter / remaining  (seg_len 仅用于合成两字节 length)
    reg [15:0] seg_len;
    reg [15:0] remain;
    reg [7:0]  last_marker;

    // 防未使用：seg_len 被部分位使用 (初始化时 reset 也写 0)
    wire _unused_hp = |seg_len[7:0];

    // DQT sub-state
    reg [5:0]  dqt_k;         // 0..63

    // DHT sub-state
    reg [4:0]  dht_l;         // 1..16
    reg [8:0]  dht_total;     // sum BITS
    reg [8:0]  dht_idx;       // 0..total-1

    // SOF sub-state
    reg [1:0]  sof_comp_idx;  // 0..2

    // SOS sub-state
    reg [1:0]  sos_comp_idx;

    // Transient
    wire beat = byte_valid && byte_ready;

    // 默认 ready 由当前 state 决定
    // 由 state 驱动 ready / 输出
    // 为避免组合环，按常规：ready 组合逻辑，寄存在下一拍转移。
    always @(*) begin
        byte_ready = 1'b0;
        case (state)
            S_IDLE: byte_ready = 1'b1;
            S_SOI_FF, S_SOI_D8,
            S_FIND_FF, S_FIND_MK,
            S_LEN_HI, S_LEN_LO,
            S_SKIP,
            S_DQT_PQTQ, S_DQT_VAL,
            S_DHT_TCTH, S_DHT_BITS, S_DHT_VAL,
            S_SOF_P, S_SOF_H_HI, S_SOF_H_LO, S_SOF_W_HI, S_SOF_W_LO, S_SOF_NF,
            S_SOF_COMP_ID, S_SOF_COMP_HV, S_SOF_COMP_TQ,
            S_SOS_NS, S_SOS_CS, S_SOS_TDTA, S_SOS_SS, S_SOS_SE, S_SOS_AHAL,
            S_DRI_HI, S_DRI_LO:
                byte_ready = 1'b1;
            default: byte_ready = 1'b0;
        endcase
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= S_IDLE;
            seg_len <= 16'd0; remain <= 16'd0; last_marker <= 8'd0;
            dqt_k <= 6'd0; dht_l <= 5'd0; dht_total <= 9'd0; dht_idx <= 9'd0;
            sof_comp_idx <= 2'd0; sos_comp_idx <= 2'd0;
            img_width <= 16'd0; img_height <= 16'd0;
            comp0_qt <= 2'd0; comp1_qt <= 2'd0; comp2_qt <= 2'd0;
            comp0_td <= 2'd0; comp1_td <= 2'd0; comp2_td <= 2'd0;
            comp0_ta <= 2'd0; comp1_ta <= 2'd0; comp2_ta <= 2'd0;
            header_done <= 1'b0; data_mode <= 1'b0; frame_done <= 1'b0;
            dri_interval <= 16'd0;
            num_components_o <= 2'd3;
            err <= 9'd0;
            qt_wr <= 1'b0; qt_sel <= 2'd0; qt_idx <= 6'd0; qt_val <= 8'd0;
            ht_bits_wr <= 1'b0; ht_val_wr <= 1'b0;
            ht_ac <= 1'b0; ht_sel <= 2'd0;
            ht_bits_idx <= 5'd0; ht_bits_val <= 8'd0;
            ht_val_idx <= 8'd0; ht_val_data <= 8'd0;
            ht_build_start <= 1'b0;
        end else if (soft_reset) begin
            state <= S_IDLE;
            header_done <= 1'b0; data_mode <= 1'b0; frame_done <= 1'b0;
            dri_interval <= 16'd0;
            num_components_o <= 2'd3;
            err <= 9'd0;
            qt_wr <= 1'b0; ht_bits_wr <= 1'b0; ht_val_wr <= 1'b0;
            ht_build_start <= 1'b0;
        end else begin
            // pulse 默认清零
            qt_wr          <= 1'b0;
            ht_bits_wr     <= 1'b0;
            ht_val_wr      <= 1'b0;
            ht_build_start <= 1'b0;
            header_done    <= 1'b0;   // single-cycle pulse on SOS end

            case (state)
                // ------------------------------------------------------
                S_IDLE: begin
                    if (start) begin
                        state <= S_SOI_FF;
                        header_done <= 1'b0;
                        data_mode   <= 1'b0;
                        frame_done  <= 1'b0;
                        dri_interval <= 16'd0;
                        num_components_o <= 2'd3;
                        err         <= 9'd0;
                    end
                end

                // ------------------------------------------------------ SOI
                S_SOI_FF: if (beat) begin
                    if (byte_in == 8'hFF) state <= S_SOI_D8;
                    else begin err[`ERR_BAD_MARKER] <= 1'b1; state <= S_ERROR; end
                end
                S_SOI_D8: if (beat) begin
                    if (byte_in == `MARKER_SOI) state <= S_FIND_FF;
                    else begin err[`ERR_BAD_MARKER] <= 1'b1; state <= S_ERROR; end
                end

                // ------------------------------------------------------ Marker hunt
                S_FIND_FF: if (beat) begin
                    if (byte_in == 8'hFF) state <= S_FIND_MK;
                end
                S_FIND_MK: if (beat) begin
                    if (byte_in == 8'hFF) begin
                        // consecutive FF, keep hunting
                    end else begin
                        last_marker <= byte_in;
                        state <= S_DISPATCH;
                    end
                end

                // ------------------------------------------------------ Marker dispatch
                S_DISPATCH: begin
                    // 不消耗字节，纯分发
                    case (last_marker)
                        `MARKER_SOF0:  state <= S_LEN_HI;
                        `MARKER_DQT:   state <= S_LEN_HI;
                        `MARKER_DHT:   state <= S_LEN_HI;
                        `MARKER_DRI:   state <= S_LEN_HI;
                        `MARKER_SOS:   state <= S_LEN_HI;
                        `MARKER_EOI: begin
                            // 在 header 阶段见 EOI 属截断
                            err[`ERR_STREAM_TRUNC] <= 1'b1; state <= S_ERROR;
                        end
                        default: begin
                            if (last_marker[7:4] == 4'hE || last_marker == `MARKER_COM) begin
                                // APPn / COM → 跳过
                                state <= S_LEN_HI;
                            end else if ((last_marker & 8'hF0) == 8'hC0 &&
                                         last_marker != `MARKER_DHT &&
                                         last_marker != 8'hC8 /* JPG ext */) begin
                                // 非 SOF0 的 SOF
                                err[`ERR_UNSUP_SOF] <= 1'b1; state <= S_ERROR;
                            end else if (last_marker >= 8'hD0 && last_marker <= 8'hD7) begin
                                // RSTn in header 不合法
                                err[`ERR_BAD_MARKER] <= 1'b1; state <= S_ERROR;
                            end else begin
                                err[`ERR_BAD_MARKER] <= 1'b1; state <= S_ERROR;
                            end
                        end
                    endcase
                end

                // ------------------------------------------------------ Length
                S_LEN_HI: if (beat) begin
                    seg_len[15:8] <= byte_in;
                    state <= S_LEN_LO;
                end
                S_LEN_LO: if (beat) begin
                    remain <= {seg_len[15:8], byte_in} - 16'd2;
                    // 根据 last_marker 分发到子 FSM
                    case (last_marker)
                        `MARKER_DQT:  begin dqt_k <= 6'd0;  state <= S_DQT_PQTQ; end
                        `MARKER_DHT:  begin                state <= S_DHT_TCTH; end
                        `MARKER_SOF0: begin sof_comp_idx <= 2'd0; state <= S_SOF_P;  end
                        `MARKER_SOS:  begin sos_comp_idx <= 2'd0; state <= S_SOS_NS; end
                        `MARKER_DRI:  begin state <= S_DRI_HI; end
                        default:      begin state <= S_SKIP; end
                    endcase
                end

                // ------------------------------------------------------ Skip (APPn/COM)
                S_SKIP: if (beat) begin
                    if (remain <= 16'd1) begin
                        state <= S_FIND_FF;
                    end
                    remain <= remain - 16'd1;
                end

                // ------------------------------------------------------ DQT
                S_DQT_PQTQ: if (beat) begin
                    if (byte_in[7:4] != 4'd0) begin
                        err[`ERR_UNSUP_PREC] <= 1'b1; state <= S_ERROR;
                    end else begin
                        qt_sel <= byte_in[1:0];  // Tq (0..3)
                        dqt_k  <= 6'd0;
                        remain <= remain - 16'd1;
                        state  <= S_DQT_VAL;
                    end
                end
                S_DQT_VAL: if (beat) begin
                    qt_wr  <= 1'b1;
                    qt_idx <= zz_natural[dqt_k];
                    qt_val <= byte_in;
                    remain <= remain - 16'd1;
                    if (dqt_k == 6'd63) begin
                        if (remain - 16'd1 > 0) begin
                            state <= S_DQT_PQTQ;   // 下一 quant table
                        end else begin
                            state <= S_FIND_FF;
                        end
                    end else begin
                        dqt_k <= dqt_k + 6'd1;
                    end
                end

                // ------------------------------------------------------ DHT
                S_DHT_TCTH: if (beat) begin
                    // Tc / Th
                    if (byte_in[7:4] > 4'd1) begin
                        err[`ERR_BAD_MARKER] <= 1'b1; state <= S_ERROR;
                    end else begin
                        ht_ac  <= byte_in[4];   // Tc: 0=DC, 1=AC
                        ht_sel <= byte_in[1:0]; // Th
                        dht_l  <= 5'd1;
                        dht_total <= 9'd0;
                        remain <= remain - 16'd1;
                        state  <= S_DHT_BITS;
                    end
                end
                S_DHT_BITS: if (beat) begin
                    ht_bits_wr  <= 1'b1;
                    ht_bits_idx <= dht_l;
                    ht_bits_val <= byte_in;
                    dht_total   <= dht_total + {1'b0, byte_in};
                    remain      <= remain - 16'd1;
                    if (dht_l == 5'd16) begin
                        dht_idx <= 9'd0;
                        if (dht_total + {1'b0, byte_in} == 9'd0)
                            state <= S_DHT_BUILD;
                        else
                            state <= S_DHT_VAL;
                    end else begin
                        dht_l <= dht_l + 5'd1;
                    end
                end
                S_DHT_VAL: if (beat) begin
                    ht_val_wr   <= 1'b1;
                    ht_val_idx  <= dht_idx[7:0];
                    ht_val_data <= byte_in;
                    remain      <= remain - 16'd1;
                    if (dht_idx == dht_total - 9'd1) begin
                        state <= S_DHT_BUILD;
                    end else begin
                        dht_idx <= dht_idx + 9'd1;
                    end
                end
                S_DHT_BUILD: begin
                    // 单周期脉冲，触发 htable 模块内部构建 MINCODE/MAXCODE/VALPTR。
                    // 构建需要 ~18 cycles。若段内还有下一表，必须等 build_done 再继续，
                    // 否则下一次 build_start 会在 htable_ram 非 B_IDLE 状态被忽略。
                    ht_build_start <= 1'b1;
                    state <= S_DHT_WAIT_BLD;
                end
                S_DHT_WAIT_BLD: if (ht_build_done) begin
                    if (remain > 0) begin
                        state <= S_DHT_TCTH;
                    end else begin
                        state <= S_FIND_FF;
                    end
                end

                // ------------------------------------------------------ SOF0
                S_SOF_P: if (beat) begin
                    if (byte_in != 8'd8) begin
                        err[`ERR_UNSUP_PREC] <= 1'b1; state <= S_ERROR;
                    end else begin
                        remain <= remain - 16'd1;
                        state  <= S_SOF_H_HI;
                    end
                end
                S_SOF_H_HI: if (beat) begin
                    img_height[15:8] <= byte_in;
                    remain <= remain - 16'd1;
                    state <= S_SOF_H_LO;
                end
                S_SOF_H_LO: if (beat) begin
                    img_height[7:0] <= byte_in;
                    remain <= remain - 16'd1;
                    state <= S_SOF_W_HI;
                end
                S_SOF_W_HI: if (beat) begin
                    img_width[15:8] <= byte_in;
                    remain <= remain - 16'd1;
                    state <= S_SOF_W_LO;
                end
                S_SOF_W_LO: if (beat) begin
                    img_width[7:0] <= byte_in;
                    remain <= remain - 16'd1;
                    state <= S_SOF_NF;
                    // 尺寸检查在下个 cycle 校验
                end
                S_SOF_NF: if (beat) begin
                    // Phase 8: accept Nf=1 (grayscale) or Nf=3 (4:2:0)
                    if (byte_in != 8'd3 && byte_in != 8'd1) begin
                        err[`ERR_UNSUP_CHROMA] <= 1'b1; state <= S_ERROR;
                    end else if (img_width == 16'd0 || img_height == 16'd0 ||
                                 img_width > 16'd4096 || img_height > 16'd4096) begin
                        // Phase 6: 非 16 对齐尺寸由 decoder 自行裁剪，不再拒绝
                        err[`ERR_SIZE_OOR] <= 1'b1; state <= S_ERROR;
                    end else begin
                        num_components_o <= byte_in[1:0];
                        remain <= remain - 16'd1;
                        sof_comp_idx <= 2'd0;
                        state <= S_SOF_COMP_ID;
                    end
                end
                S_SOF_COMP_ID: if (beat) begin
                    // id 不校验（默认 Y=1 Cb=2 Cr=3）
                    remain <= remain - 16'd1;
                    state <= S_SOF_COMP_HV;
                end
                S_SOF_COMP_HV: if (beat) begin
                    // Phase 8: Nf=1 grayscale → Y only H=1,V=1
                    //         Nf=3 4:2:0    → Y 2x2, Cb/Cr 1x1
                    if (num_components_o == 2'd1) begin
                        if (byte_in != 8'h11) begin err[`ERR_UNSUP_CHROMA] <= 1'b1; state <= S_ERROR; end
                    end else begin
                        case (sof_comp_idx)
                            2'd0: if (byte_in != 8'h22) begin err[`ERR_UNSUP_CHROMA] <= 1'b1; state <= S_ERROR; end
                            2'd1: if (byte_in != 8'h11) begin err[`ERR_UNSUP_CHROMA] <= 1'b1; state <= S_ERROR; end
                            2'd2: if (byte_in != 8'h11) begin err[`ERR_UNSUP_CHROMA] <= 1'b1; state <= S_ERROR; end
                            default: ;
                        endcase
                    end
                    remain <= remain - 16'd1;
                    if (state != S_ERROR) state <= S_SOF_COMP_TQ;
                end
                S_SOF_COMP_TQ: if (beat) begin
                    case (sof_comp_idx)
                        2'd0: comp0_qt <= byte_in[1:0];
                        2'd1: comp1_qt <= byte_in[1:0];
                        2'd2: comp2_qt <= byte_in[1:0];
                        default: ;
                    endcase
                    remain <= remain - 16'd1;
                    // Phase 8: 终止条件按 num_components_o
                    if (sof_comp_idx == num_components_o - 2'd1) begin
                        state <= S_FIND_FF;
                    end else begin
                        sof_comp_idx <= sof_comp_idx + 2'd1;
                        state <= S_SOF_COMP_ID;
                    end
                end

                // ------------------------------------------------------ SOS
                S_SOS_NS: if (beat) begin
                    // Phase 8: Ns must match Nf from SOF (1 or 3)
                    if (byte_in[7:0] != {6'd0, num_components_o}) begin
                        err[`ERR_UNSUP_CHROMA] <= 1'b1; state <= S_ERROR;
                    end else begin
                        sos_comp_idx <= 2'd0;
                        remain <= remain - 16'd1;
                        state <= S_SOS_CS;
                    end
                end
                S_SOS_CS: if (beat) begin
                    // Cs 字节由上层 blk_sequencer 通过 comp*_td/_ta 取，此处仅推进
                    remain <= remain - 16'd1;
                    state <= S_SOS_TDTA;
                end
                S_SOS_TDTA: if (beat) begin
                    case (sos_comp_idx)
                        2'd0: begin comp0_td <= byte_in[5:4]; comp0_ta <= byte_in[1:0]; end
                        2'd1: begin comp1_td <= byte_in[5:4]; comp1_ta <= byte_in[1:0]; end
                        2'd2: begin comp2_td <= byte_in[5:4]; comp2_ta <= byte_in[1:0]; end
                        default: ;
                    endcase
                    remain <= remain - 16'd1;
                    if (sos_comp_idx == num_components_o - 2'd1) begin
                        state <= S_SOS_SS;
                    end else begin
                        sos_comp_idx <= sos_comp_idx + 2'd1;
                        state <= S_SOS_CS;
                    end
                end
                S_SOS_SS: if (beat) begin
                    if (byte_in != 8'd0) begin err[`ERR_UNSUP_SOF] <= 1'b1; state <= S_ERROR; end
                    else begin remain <= remain - 16'd1; state <= S_SOS_SE; end
                end
                S_SOS_SE: if (beat) begin
                    if (byte_in != 8'd63) begin err[`ERR_UNSUP_SOF] <= 1'b1; state <= S_ERROR; end
                    else begin remain <= remain - 16'd1; state <= S_SOS_AHAL; end
                end
                S_SOS_AHAL: if (beat) begin
                    if (byte_in != 8'd0) begin err[`ERR_UNSUP_SOF] <= 1'b1; state <= S_ERROR; end
                    else begin
                        header_done <= 1'b1;
                        data_mode   <= 1'b1;
                        state <= S_DATA;
                    end
                end

                // ------------------------------------------------------ DRI (Phase 7: 接受任意值)
                S_DRI_HI: if (beat) begin
                    dri_interval[15:8] <= byte_in;
                    remain <= remain - 16'd1; state <= S_DRI_LO;
                end
                S_DRI_LO: if (beat) begin
                    dri_interval[7:0] <= byte_in;
                    state <= S_FIND_FF;
                end

                // ------------------------------------------------------ DATA / DONE
                S_DATA: begin
                    // data_mode=1; 顶层由 marker_detected (来自 bitstream_unpack) 判 EOI
                    // 本 FSM 保持 IDLE 样的待命
                    if (frame_done || soft_reset) state <= S_DONE;
                end
                S_DONE: begin
                    // 等待下一个 start
                    if (start) state <= S_SOI_FF;
                end
                S_ERROR: begin
                    // 停在错误状态直到 soft_reset
                end

                default: state <= S_ERROR;
            endcase
        end
    end

endmodule
