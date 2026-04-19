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
    wire [15:0] mcu_cols = img_width  >> 4;
    wire [15:0] mcu_rows = img_height >> 4;

    localparam [3:0]
        S_IDLE     = 4'd0,
        S_START_BLK= 4'd1,
        S_WAIT_HUF = 4'd2,
        S_WAIT_IDCT= 4'd3,
        S_NEXT_BLK = 4'd4,
        S_MCU_COPY = 4'd5,
        S_NEXT_MCU = 4'd6,
        S_ROW_OUT  = 4'd7,
        S_DONE     = 4'd8;

    reg [3:0] st;
    reg [2:0] blk_idx;     // 0..5
    reg [15:0] my, mx;
    reg pending_h_done;
    reg pending_idct_done;

    // 当前 block 的参数选择
    always @(*) begin
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

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            st <= S_IDLE;
            blk_idx <= 3'd0; my <= 16'd0; mx <= 16'd0;
            pending_h_done <= 1'b0; pending_idct_done <= 1'b0;
            h_blk_start <= 1'b0;
            dcp_wr <= 1'b0; dcp_wr_data <= 16'sd0;
            mcu_copy_start <= 1'b0; mcu_col_idx <= 16'd0;
            row_ready <= 1'b0; is_first_row_o <= 1'b0; is_last_row_o <= 1'b0;
            frame_done_o <= 1'b0;
        end else if (soft_reset) begin
            st <= S_IDLE;
            h_blk_start <= 1'b0; dcp_wr <= 1'b0;
            mcu_copy_start <= 1'b0; row_ready <= 1'b0;
            frame_done_o <= 1'b0;
        end else begin
            h_blk_start    <= 1'b0;
            dcp_wr         <= 1'b0;
            mcu_copy_start <= 1'b0;
            row_ready      <= 1'b0;
            frame_done_o   <= 1'b0;

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
                    if (blk_idx == 3'd5) begin
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
                        row_ready <= 1'b1;
                        is_first_row_o <= (my == 16'd0);
                        is_last_row_o  <= (my == mcu_rows - 16'd1);
                        st <= S_ROW_OUT;
                    end else begin
                        mx <= mx + 16'd1;
                        st <= S_START_BLK;
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
