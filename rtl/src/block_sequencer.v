// ---------------------------------------------------------------------------
// block_sequencer — 顺序发起 MCU 内 6 个 block (Y0,Y1,Y2,Y3,Cb,Cr) 的解码
//                   并把 MCU 内容搬运到 line_buffer
//
// 对应 C: decoder.c 中的 for (my) for (mx) for (i=0..3) Y; Cb; Cr; copy_*  流程
//
// 触发 huffman_decoder.blk_start，等 blk_done (来自 mcu_buffer 方向的 IDCT
// 完成链)，切换 blk_type，完成一个 MCU 后复制到 line_buffer，mcu_col++。
// 一行 MCU 结束后触发 pixel_out.row_start。
// ---------------------------------------------------------------------------
module block_sequencer (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        soft_reset,

    input  wire        frame_start,    // header_parser.header_done 脉冲
    input  wire [15:0] img_width,
    input  wire [15:0] img_height,
    input  wire [1:0]  num_components,// Phase 8: 1=grayscale, 3=YCbCr
    input  wire [1:0]  chroma_mode,   // Phase 9: 0=GRAY, 1=420, 2=444, 3=422(rsv)
    input  wire [1:0]  y_qt_sel,
    input  wire [1:0]  cb_qt_sel,
    input  wire [1:0]  cr_qt_sel,
    input  wire [1:0]  y_dc_sel,
    input  wire [1:0]  y_ac_sel,
    input  wire [1:0]  cb_dc_sel,
    input  wire [1:0]  cb_ac_sel,
    input  wire [1:0]  cr_dc_sel,
    input  wire [1:0]  cr_ac_sel,

    // 到 huffman_decoder
    output reg         h_blk_start,
    output reg  [1:0]  h_dc_sel,
    output reg  [1:0]  h_ac_sel,
    output reg signed [15:0] h_dc_pred_in,
    input  wire        h_blk_done,
    input  wire signed [15:0] h_dc_pred_out,
    input  wire        h_dc_pred_upd,

    // Phase 7: DRI / Restart marker 支持
    input  wire [15:0] dri_interval,   // 来自 header_parser（0=禁用）
    input  wire        marker_detected, // 来自 bitstream_unpack
    input  wire [7:0]  marker_byte,
    output reg         restart_ack,    // 1-cycle pulse：吞掉 RST + 清 shreg
    output reg         dc_restart,     // 1-cycle pulse：清 DC 预测器
    output reg         align_req,      // 1-cycle pulse：进 S_WAIT_RST 时清 shreg/bit_cnt

    // 当前 block 的 quant 表号
    output reg  [1:0]  qt_sel_out,

    // 当前 block 类型 (给 mcu_buffer)
    output reg  [2:0]  cur_blk_type,

    // 来自 idct_2d
    input  wire        idct_blk_done,

    // 与 dc_predictor
    output reg  [1:0]  dcp_sel,
    output reg         dcp_wr,
    output reg  signed [15:0] dcp_wr_data,
    input  wire signed [15:0] dcp_y,
    input  wire signed [15:0] dcp_cb,
    input  wire signed [15:0] dcp_cr,

    // MCU 完毕：把 mcu_buffer → line_buffer 的搬运控制
    output reg         mcu_copy_start,
    input  wire        mcu_copy_done,
    output reg  [15:0] mcu_col_idx,    // 当前 MCU 列 (0..mcu_cols-1)

    // 一行 MCU 结束：驱动 pixel_out
    output reg         row_ready,
    output reg         is_first_row_o,
    output reg         is_last_row_o,
    input  wire        row_done,

    // 帧完成
    output reg         frame_done_o
);

    // 计算 mcu 行列数
    // Phase 6: 非 16 对齐尺寸向上取整
    // Phase 8: grayscale → MCU = 8x8；4:2:0 → MCU = 16x16
    // Phase 9: 4:4:4 → MCU = 8x8, 3 blocks (Y, Cb, Cr)
    wire is_gray = (chroma_mode == 2'd0);
    wire is_444  = (chroma_mode == 2'd2);
    wire is_420  = (chroma_mode == 2'd1);
    wire mcu_8x8 = is_gray | is_444;       // MCU 尺寸 8x8
    wire [15:0] mcu_cols = mcu_8x8 ? ((img_width  + 16'd7)  >> 3) :
                                     ((img_width  + 16'd15) >> 4);
    wire [15:0] mcu_rows = mcu_8x8 ? ((img_height + 16'd7)  >> 3) :
                                     ((img_height + 16'd15) >> 4);
    // block 数: GRAY→1, 444→3, 420→6
    wire [2:0] last_blk = is_gray ? 3'd0 :
                          is_444  ? 3'd2 : 3'd5;
    wire _unused_ncomp = |num_components;  // 保留端口以备 sanity check

    localparam [3:0]
        S_IDLE     = 4'd0,
        S_START_BLK= 4'd1,
        S_WAIT_HUF = 4'd2,
        S_WAIT_IDCT= 4'd3,
        S_NEXT_BLK = 4'd4,
        S_MCU_COPY = 4'd5,
        S_NEXT_MCU = 4'd6,
        S_ROW_OUT  = 4'd7,
        S_DONE     = 4'd8,
        S_WAIT_RST = 4'd9;   // Phase 7: DRI 边界等 RSTn

    reg [3:0] st;
    reg [3:0] next_st_after_rst;  // Phase 7: 吞掉 RST 后回到哪
    reg [2:0] blk_idx;     // 0..5
    reg [15:0] my, mx;
    reg [15:0] restart_cnt; // Phase 7: 自上次 restart 累计 MCU 数
    reg pending_h_done;
    reg pending_idct_done;

    // 当前 block 的参数选择
    // Phase 9: 4:4:4 下 blk_idx 0/1/2 = Y/Cb/Cr；4:2:0 下 0..3=Y,4=Cb,5=Cr
    //         cur_blk_type 给 mcu_buffer 用以选择子 buffer (mcu_buffer 仍按 4:2:0
    //         的 0..3=Y,4=Cb,5=Cr 语义)，444 下 Y 用 0、Cb 用 4、Cr 用 5 以保持
    //         mcu_buffer 接口不变。
    always @(*) begin
        if (is_444) begin
            case (blk_idx)
                3'd0: begin
                    h_dc_sel   = y_dc_sel;   h_ac_sel   = y_ac_sel;
                    qt_sel_out = y_qt_sel;   dcp_sel    = 2'd0;
                    h_dc_pred_in = dcp_y;
                    cur_blk_type = 3'd0;
                end
                3'd1: begin
                    h_dc_sel   = cb_dc_sel;  h_ac_sel   = cb_ac_sel;
                    qt_sel_out = cb_qt_sel;  dcp_sel    = 2'd1;
                    h_dc_pred_in = dcp_cb;
                    cur_blk_type = 3'd4;
                end
                default: begin // blk_idx == 2
                    h_dc_sel   = cr_dc_sel;  h_ac_sel   = cr_ac_sel;
                    qt_sel_out = cr_qt_sel;  dcp_sel    = 2'd2;
                    h_dc_pred_in = dcp_cr;
                    cur_blk_type = 3'd5;
                end
            endcase
        end else begin
            case (blk_idx)
                3'd0, 3'd1, 3'd2, 3'd3: begin
                    h_dc_sel   = y_dc_sel;
                    h_ac_sel   = y_ac_sel;
                    qt_sel_out = y_qt_sel;
                    dcp_sel    = 2'd0;
                    h_dc_pred_in = dcp_y;
                end
                3'd4: begin
                    h_dc_sel   = cb_dc_sel;
                    h_ac_sel   = cb_ac_sel;
                    qt_sel_out = cb_qt_sel;
                    dcp_sel    = 2'd1;
                    h_dc_pred_in = dcp_cb;
                end
                default: begin // 5
                    h_dc_sel   = cr_dc_sel;
                    h_ac_sel   = cr_ac_sel;
                    qt_sel_out = cr_qt_sel;
                    dcp_sel    = 2'd2;
                    h_dc_pred_in = dcp_cr;
                end
            endcase
            cur_blk_type = blk_idx;
        end
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            st <= S_IDLE;
            next_st_after_rst <= S_IDLE;
            blk_idx <= 3'd0; my <= 16'd0; mx <= 16'd0;
            restart_cnt <= 16'd0;
            pending_h_done <= 1'b0; pending_idct_done <= 1'b0;
            h_blk_start <= 1'b0;
            dcp_wr <= 1'b0; dcp_wr_data <= 16'sd0;
            mcu_copy_start <= 1'b0; mcu_col_idx <= 16'd0;
            row_ready <= 1'b0; is_first_row_o <= 1'b0; is_last_row_o <= 1'b0;
            frame_done_o <= 1'b0;
            restart_ack <= 1'b0; dc_restart <= 1'b0; align_req <= 1'b0;
        end else if (soft_reset) begin
            st <= S_IDLE;
            h_blk_start <= 1'b0; dcp_wr <= 1'b0;
            mcu_copy_start <= 1'b0; row_ready <= 1'b0;
            frame_done_o <= 1'b0;
            restart_ack <= 1'b0; dc_restart <= 1'b0; align_req <= 1'b0;
            restart_cnt <= 16'd0;
        end else begin
            h_blk_start    <= 1'b0;
            dcp_wr         <= 1'b0;
            mcu_copy_start <= 1'b0;
            row_ready      <= 1'b0;
            frame_done_o   <= 1'b0;
            restart_ack    <= 1'b0;
            dc_restart     <= 1'b0;
            align_req      <= 1'b0;

            // 收集异步完成信号
            if (h_blk_done)    pending_h_done    <= 1'b1;
            if (idct_blk_done) pending_idct_done <= 1'b1;

            // DC pred 写回
            if (h_dc_pred_upd) begin
                dcp_wr      <= 1'b1;
                dcp_wr_data <= h_dc_pred_out;
            end

            case (st)
                S_IDLE: if (frame_start) begin
                    my <= 16'd0; mx <= 16'd0; blk_idx <= 3'd0;
                    st <= S_START_BLK;
                end

                S_START_BLK: begin
                    h_blk_start <= 1'b1;
                    pending_h_done <= 1'b0;
                    pending_idct_done <= 1'b0;
                    st <= S_WAIT_HUF;
                end

                S_WAIT_HUF: begin
                    if (pending_h_done) begin
                        st <= S_WAIT_IDCT;
                    end
                end

                S_WAIT_IDCT: begin
                    if (pending_idct_done) begin
                        st <= S_NEXT_BLK;
                    end
                end

                S_NEXT_BLK: begin
                    if (blk_idx == last_blk) begin
                        // MCU 完成，触发搬运
                        mcu_col_idx <= mx;
                        mcu_copy_start <= 1'b1;
                        st <= S_MCU_COPY;
                    end else begin
                        blk_idx <= blk_idx + 3'd1;
                        st <= S_START_BLK;
                    end
                end

                S_MCU_COPY: begin
                    if (mcu_copy_done) begin
                        st <= S_NEXT_MCU;
                    end
                end

                S_NEXT_MCU: begin
                    blk_idx <= 3'd0;
                    if (mx + 16'd1 == mcu_cols) begin
                        // MCU 行完成 → 触发 raster 输出
                        mx <= 16'd0;
                        is_first_row_o <= (my == 16'd0);
                        is_last_row_o  <= (my == mcu_rows - 16'd1);
                        // Phase 7: DRI 边界（非帧末行）需要先吞 RSTn 再输出
                        if (dri_interval != 16'd0 &&
                            restart_cnt + 16'd1 == dri_interval &&
                            my != mcu_rows - 16'd1) begin
                            restart_cnt <= 16'd0;
                            next_st_after_rst <= S_ROW_OUT;
                            align_req <= 1'b1;
                            st <= S_WAIT_RST;
                        end else begin
                            row_ready <= 1'b1;
                            restart_cnt <= (dri_interval != 16'd0 &&
                                            restart_cnt + 16'd1 == dri_interval) ?
                                           16'd0 : restart_cnt + 16'd1;
                            st <= S_ROW_OUT;
                        end
                    end else begin
                        mx <= mx + 16'd1;
                        // Phase 7: 行内 DRI 边界
                        if (dri_interval != 16'd0 &&
                            restart_cnt + 16'd1 == dri_interval) begin
                            restart_cnt <= 16'd0;
                            next_st_after_rst <= S_START_BLK;
                            align_req <= 1'b1;
                            st <= S_WAIT_RST;
                        end else begin
                            restart_cnt <= restart_cnt + 16'd1;
                            st <= S_START_BLK;
                        end
                    end
                end

                S_WAIT_RST: begin
                    // Phase 7: 等 bitstream_unpack 报 marker = 0xD0..0xD7
                    if (marker_detected) begin
                        if (marker_byte[7:3] == 5'b11010) begin
                            // RSTn：吞掉它，清 DC 预测器
                            restart_ack <= 1'b1;
                            dc_restart  <= 1'b1;
                            if (next_st_after_rst == S_ROW_OUT)
                                row_ready <= 1'b1;
                            st <= next_st_after_rst;
                        end else begin
                            // 出现非法 marker，终止
                            frame_done_o <= 1'b1;
                            st <= S_DONE;
                        end
                    end
                end

                S_ROW_OUT: begin
                    if (row_done) begin
                        if (my + 16'd1 == mcu_rows) begin
                            frame_done_o <= 1'b1;
                            st <= S_DONE;
                        end else begin
                            my <= my + 16'd1;
                            st <= S_START_BLK;
                        end
                    end
                end

                S_DONE: begin
                    st <= S_IDLE;
                end

                default: st <= S_IDLE;
            endcase
        end
    end

endmodule
