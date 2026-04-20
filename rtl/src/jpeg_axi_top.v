// ---------------------------------------------------------------------------
// jpeg_axi_top — JPEG Baseline 解码器顶层
//
// 外部接口：
//   1. AXI4-Lite (csr_*)        — CSR 读写 (docs/regmap.md)
//   2. AXI-Stream (s_bs_*)      — 输入 JPEG 字节流 (8b, tlast)
//   3. AXI-Stream (m_px_*)      — 输出 YCbCr 像素 (24b, tuser=SOF, tlast=EOL)
//   4. irq                      — 中断
//
// 内部数据流：
//   s_bs_* → axi_stream_fifo (深 32) → byte_router → header_parser (HEADER)
//                                               └→ bitstream_unpack (DATA)
//   bitstream_unpack → huffman_decoder → dequant_izz → idct_2d → mcu_buffer
//   block_sequencer 编排 6×block/MCU，mcu_line_copy → line_buffer → pixel_out
//   pixel_out → axi_stream_fifo → m_px_*
// ---------------------------------------------------------------------------
`include "jpeg_defs.vh"

module jpeg_axi_top (
    input  wire        aclk,
    input  wire        aresetn,

    // ---- AXI4-Lite CSR ----------------------------------------------
    input  wire [11:0] csr_awaddr,
    input  wire        csr_awvalid,
    output wire        csr_awready,
    input  wire [31:0] csr_wdata,
    input  wire [3:0]  csr_wstrb,
    input  wire        csr_wvalid,
    output wire        csr_wready,
    output wire [1:0]  csr_bresp,
    output wire        csr_bvalid,
    input  wire        csr_bready,
    input  wire [11:0] csr_araddr,
    input  wire        csr_arvalid,
    output wire        csr_arready,
    output wire [31:0] csr_rdata,
    output wire [1:0]  csr_rresp,
    output wire        csr_rvalid,
    input  wire        csr_rready,

    // ---- AXI-Stream input bitstream ---------------------------------
    input  wire [7:0]  s_bs_tdata,
    input  wire        s_bs_tvalid,
    output wire        s_bs_tready,
    input  wire        s_bs_tlast,

    // ---- AXI-Stream output pixel (Phase 13: 48b = 4 × 12b 通道槽位)
    //   YCbCr/GRAY: {Y[11:0], Cb[11:0], Cr[11:0], 12'h000}
    //   CMYK:       {C[11:0], M[11:0], Y[11:0], K[11:0]}
    //   P=8 样本 0..255 零扩至 12b；P=12 样本占满 12b。
    output wire [47:0] m_px_tdata,
    output wire        m_px_tvalid,
    input  wire        m_px_tready,
    output wire        m_px_tuser,
    output wire        m_px_tlast,

    // ---- interrupt --------------------------------------------------
    output wire        irq
);

    // ---------------- CSR -------------------------------------------
    wire start_pulse;
    wire soft_reset_pulse;
    wire abort_pulse;
    wire [31:0] cfg_reg_w;
    wire [31:0] scratch_reg_w;
    wire [2:0]  int_en_w;

    // 复位合成
    wire softrst = soft_reset_pulse | abort_pulse;

    // ---------------- Input FIFO ------------------------------------
    wire [7:0] fifo_byte;
    wire       fifo_byte_valid;
    wire       fifo_byte_ready;
    wire       fifo_byte_tlast;
    wire       in_fifo_empty, in_fifo_full;

    wire in_fifo_tuser_nc;   // unused
    axi_stream_fifo #(.DW(8), .UW(1), .DEPTH(32)) u_in_fifo (
        .clk(aclk), .rst_n(aresetn), .flush(softrst),
        .s_tdata(s_bs_tdata),   .s_tuser(1'b0),
        .s_tlast(s_bs_tlast),   .s_tvalid(s_bs_tvalid),
        .s_tready(s_bs_tready),
        .m_tdata(fifo_byte),    .m_tuser(in_fifo_tuser_nc),
        .m_tlast(fifo_byte_tlast),
        .m_tvalid(fifo_byte_valid),
        .m_tready(fifo_byte_ready),
        .empty(in_fifo_empty),  .full(in_fifo_full)
    );

    // ---------------- Byte Router (HEADER vs DATA) ------------------
    // header_parser 消耗 HEADER 段；data_mode=1 后由 bitstream_unpack 消耗
    wire data_mode;
    wire hp_byte_ready;
    wire bs_byte_ready;

    // 分发：data_mode=0 → hp；=1 → bs
    wire byte_to_hp_valid = fifo_byte_valid && !data_mode;
    wire byte_to_bs_valid = fifo_byte_valid &&  data_mode;
    assign fifo_byte_ready = data_mode ? bs_byte_ready : hp_byte_ready;

    // ---------------- Header parser ---------------------------------
    wire        qt_wr;
    wire [1:0]  qt_sel;
    wire [5:0]  qt_idx;
    wire [15:0] qt_val;                     // Phase 13: 16b (Pq=1 支持)
    wire        ht_bits_wr, ht_val_wr, ht_ac_w;
    wire [1:0]  ht_sel_w;
    wire [4:0]  ht_bits_idx_w;
    wire [7:0]  ht_bits_val_w, ht_val_idx_w, ht_val_data_w;
    wire        ht_build_start_w;
    wire [15:0] img_w_w, img_h_w;
    wire [1:0]  comp0_qt, comp1_qt, comp2_qt, comp3_qt;
    wire [1:0]  comp0_td, comp1_td, comp2_td, comp3_td;
    wire [1:0]  comp0_ta, comp1_ta, comp2_ta, comp3_ta;
    wire        header_done_w;
    wire        frame_done_hp;
    wire [8:0]  err_w;
    wire [15:0] dri_interval_w;     // Phase 7
    wire        restart_ack_w;      // Phase 7: block_seq → bitstream_unpack
    wire        dc_restart_w;       // Phase 7: block_seq → dc_predictor
    wire        align_req_w;        // Phase 7: block_seq → bitstream_unpack (drain shreg)
    wire [2:0]  num_components_w;   // Phase 12: Nf (1=gray, 3=YCbCr, 4=CMYK)
    wire [2:0]  chroma_mode_w;      // Phase 12: 0=GRAY,1=420,2=444,3=422,4=440,5=411,6=CMYK
    wire        precision_w;        // Phase 13: 0=P=8, 1=P=12
    wire [1:0]  sof_type_w;         // Phase 16a
    wire [5:0]  sos_ss_w;
    wire [5:0]  sos_se_w;
    wire [3:0]  sos_ah_w;
    wire [3:0]  sos_al_w;
    wire        is_grayscale_w = (chroma_mode_w == 3'd0);
    wire        is_444_w       = (chroma_mode_w == 3'd2);
    wire        is_422_w       = (chroma_mode_w == 3'd3);
    wire        is_440_w       = (chroma_mode_w == 3'd4);
    wire        is_411_w       = (chroma_mode_w == 3'd5);
    wire        is_cmyk_w      = (chroma_mode_w == 3'd6);  // Phase 12

    header_parser u_hp (
        .clk(aclk), .rst_n(aresetn), .start(start_pulse),
        .soft_reset(softrst),
        .byte_in(fifo_byte), .byte_valid(byte_to_hp_valid),
        .byte_ready(hp_byte_ready),
        .qt_wr(qt_wr), .qt_sel(qt_sel), .qt_idx(qt_idx), .qt_val(qt_val),
        .ht_bits_wr(ht_bits_wr), .ht_val_wr(ht_val_wr),
        .ht_ac(ht_ac_w), .ht_sel(ht_sel_w),
        .ht_bits_idx(ht_bits_idx_w), .ht_bits_val(ht_bits_val_w),
        .ht_val_idx(ht_val_idx_w), .ht_val_data(ht_val_data_w),
        .ht_build_start(ht_build_start_w),
        .ht_build_done(ht_build_done_w),
        .img_width(img_w_w), .img_height(img_h_w),
        .comp0_qt(comp0_qt), .comp1_qt(comp1_qt), .comp2_qt(comp2_qt), .comp3_qt(comp3_qt),
        .comp0_td(comp0_td), .comp1_td(comp1_td), .comp2_td(comp2_td), .comp3_td(comp3_td),
        .comp0_ta(comp0_ta), .comp1_ta(comp1_ta), .comp2_ta(comp2_ta), .comp3_ta(comp3_ta),
        .header_done(header_done_w), .data_mode(data_mode),
        .frame_done(frame_done_hp),
        .dri_interval(dri_interval_w),   // Phase 7
        .num_components_o(num_components_w), // Phase 8
        .chroma_mode_o(chroma_mode_w),   // Phase 9
        .precision_o(precision_w),       // Phase 13
        .sof_type_o(sof_type_w),         // Phase 16a
        .sos_ss_o(sos_ss_w),
        .sos_se_o(sos_se_w),
        .sos_ah_o(sos_ah_w),
        .sos_al_o(sos_al_w),
        .err(err_w)
    );

    // ---------------- QTable RAM ------------------------------------
    wire [1:0]  qt_rd_sel_w;
    wire [5:0]  qt_rd_idx_w;
    wire [15:0] qt_rd_data_w;              // Phase 13: 16b (Pq=1 支持)

    qtable_ram u_qt (
        .clk(aclk),
        .wr_en(qt_wr), .wr_sel(qt_sel), .wr_idx(qt_idx), .wr_data(qt_val),
        .rd_sel(qt_rd_sel_w), .rd_idx(qt_rd_idx_w), .rd_data(qt_rd_data_w)
    );

    // ---------------- HTable RAM ------------------------------------
    wire        ht_rd_ac_w;
    wire [1:0]  ht_rd_sel_r_w;
    wire [4:0]  ht_rd_l_w;
    wire [15:0] ht_mincode_w;
    wire [17:0] ht_maxcode_w;
    wire [7:0]  ht_valptr_w;
    wire [7:0]  ht_huff_idx_w;
    wire [7:0]  ht_huffval_w;
    wire        ht_build_done_w;

    htable_ram u_ht (
        .clk(aclk), .rst_n(aresetn),
        .bits_wr(ht_bits_wr), .ac_in(ht_ac_w), .sel_in(ht_sel_w),
        .bits_idx(ht_bits_idx_w), .bits_val(ht_bits_val_w),
        .val_wr(ht_val_wr), .val_idx(ht_val_idx_w), .val_data(ht_val_data_w),
        .build_start(ht_build_start_w), .build_done(ht_build_done_w),
        .rd_ac(ht_rd_ac_w), .rd_sel(ht_rd_sel_r_w), .rd_l(ht_rd_l_w),
        .rd_mincode(ht_mincode_w), .rd_maxcode(ht_maxcode_w),
        .rd_valptr(ht_valptr_w),
        .rd_huff_idx(ht_huff_idx_w), .rd_huffval(ht_huffval_w)
    );

    // ---------------- Bitstream unpack ------------------------------
    wire [15:0] peek_win_w;
    wire        peek_valid_any_w;
    wire [6:0]  bit_cnt_w;
    wire [4:0]  consume_n_w;
    wire        consume_req_w;
    wire        marker_detected_w;
    wire [7:0]  marker_byte_w;

    wire peek_valid_nc;
    bitstream_unpack u_bs (
        .clk(aclk), .rst_n(aresetn), .flush(softrst),
        .byte_in(fifo_byte), .byte_valid(byte_to_bs_valid),
        .byte_ready(bs_byte_ready),
        .marker_detected(marker_detected_w), .marker_byte(marker_byte_w),
        .restart_ack(restart_ack_w),       // Phase 7
        .align_req(align_req_w),           // Phase 7
        .consume_n(consume_n_w), .consume_req(consume_req_w),
        .peek_win(peek_win_w), .peek_valid(peek_valid_nc),
        .bit_cnt_o(bit_cnt_w)
    );
    assign peek_valid_any_w = (bit_cnt_w != 7'd0);

    // ---------------- DC predictor ----------------------------------
    wire [1:0]  dcp_sel_w;
    wire        dcp_wr_w;
    wire signed [15:0] dcp_wr_data_w;
    wire signed [15:0] dcp_y_w, dcp_cb_w, dcp_cr_w, dcp_k_w;
    wire        dcp_new_frame = start_pulse | dc_restart_w;  // Phase 7: RSTn 清 DC 预测器

    dc_predictor u_dcp (
        .clk(aclk), .rst_n(aresetn), .soft_reset(softrst),
        .new_frame(dcp_new_frame),
        .comp_sel(dcp_sel_w), .wr_en(dcp_wr_w), .wr_data(dcp_wr_data_w),
        .rd_data_y(dcp_y_w), .rd_data_cb(dcp_cb_w), .rd_data_cr(dcp_cr_w),
        .rd_data_k(dcp_k_w)
    );

    // ---------------- Huffman decoder -------------------------------
    wire        h_blk_start_w;
    wire [1:0]  h_dc_sel_w, h_ac_sel_w;
    wire signed [15:0] h_dc_pred_in_w;
    wire signed [15:0] h_dc_pred_out_w;
    wire        h_dc_pred_upd_w;
    wire        h_blk_done_w;
    wire        h_blk_err_w;

    wire        huf_coef_start_w;
    wire        huf_coef_wr_w;
    wire [5:0]  huf_coef_nat_idx_w;
    wire signed [15:0] huf_coef_val_w;

    // Phase 16c: SOF2 DC-only 模式 — 由 header_parser 捕获的 sof_type + Al 驱动
    wire        dc_only_mode_w = (sof_type_w == 2'd2);
    wire [3:0]  al_shift_w     = sos_al_w;

    huffman_decoder u_huf (
        .clk(aclk), .rst_n(aresetn), .soft_reset(softrst),
        .blk_start(h_blk_start_w), .dc_sel(h_dc_sel_w), .ac_sel(h_ac_sel_w),
        .dc_pred_in(h_dc_pred_in_w),
        .dc_pred_out(h_dc_pred_out_w), .dc_pred_upd(h_dc_pred_upd_w),
        .blk_done(h_blk_done_w), .blk_err(h_blk_err_w),
        .dc_only_mode(dc_only_mode_w),     // Phase 16c
        .al_shift(al_shift_w),             // Phase 16c
        .peek_win(peek_win_w), .peek_valid_any(peek_valid_any_w),
        .peek_bits_avail(bit_cnt_w),
        .consume_n(consume_n_w), .consume_req(consume_req_w),
        .ht_rd_ac(ht_rd_ac_w), .ht_rd_sel(ht_rd_sel_r_w), .ht_rd_l(ht_rd_l_w),
        .ht_mincode(ht_mincode_w), .ht_maxcode(ht_maxcode_w),
        .ht_valptr(ht_valptr_w),
        .ht_huff_idx(ht_huff_idx_w), .ht_huffval(ht_huffval_w),
        .coef_start(huf_coef_start_w), .coef_wr(huf_coef_wr_w),
        .coef_nat_idx(huf_coef_nat_idx_w), .coef_val(huf_coef_val_w)
    );

    // ---------------- Dequant + IDCT --------------------------------
    wire [1:0]  qt_sel_cur_w;          // block_sequencer 驱动
    wire        dq_start_w, dq_wr_w, dq_done_w;
    wire [5:0]  dq_idx_w;
    wire signed [31:0] dq_val_w;        // Phase 13: 32b (16×16 dequant 乘积)

    dequant_izz u_dq (
        .clk(aclk), .rst_n(aresetn), .soft_reset(softrst),
        .coef_start(huf_coef_start_w),
        .coef_wr(huf_coef_wr_w),
        .coef_nat_idx(huf_coef_nat_idx_w),
        .coef_val(huf_coef_val_w),
        .blk_in_done(h_blk_done_w),
        .qt_sel(qt_sel_cur_w),
        .qt_rd_sel(qt_rd_sel_w), .qt_rd_idx(qt_rd_idx_w),
        .qt_rd_data(qt_rd_data_w),
        .dq_start(dq_start_w), .dq_wr(dq_wr_w),
        .dq_idx(dq_idx_w), .dq_val(dq_val_w),
        .dq_done(dq_done_w)
    );

    wire        pix_valid_w;
    wire [2:0]  pix_row_w;
    // Phase 13: pix 宽度 16b (低 12b 有效)
    wire [15:0] pix0_w, pix1_w, pix2_w, pix3_w, pix4_w, pix5_w, pix6_w, pix7_w;
    wire        idct_blk_done_w;

    idct_2d u_idct (
        .clk(aclk), .rst_n(aresetn), .soft_reset(softrst),
        .precision(precision_w),         // Phase 13
        .dq_start(dq_start_w), .dq_wr(dq_wr_w),
        .dq_idx(dq_idx_w), .dq_val(dq_val_w), .dq_done(dq_done_w),
        .pix_valid(pix_valid_w), .pix_row(pix_row_w),
        .pix0(pix0_w), .pix1(pix1_w), .pix2(pix2_w), .pix3(pix3_w),
        .pix4(pix4_w), .pix5(pix5_w), .pix6(pix6_w), .pix7(pix7_w),
        .blk_done_out(idct_blk_done_w)
    );

    // ---------------- MCU buffer ------------------------------------
    wire [2:0]  cur_blk_type_w;

    wire [3:0]  mb_y_row_w;
    wire [4:0]  mb_y_col_w;            // Phase 11b: 5 位支持 0..31 (4:1:1 Y 32-wide)
    wire [2:0]  mb_c_row_w, mb_c_col_w;
    // Phase 13: 16b 样本
    wire [15:0] mb_y_data_w, mb_cb_data_w, mb_cr_data_w, mb_k_data_w;

    wire mb_ready_nc;
    mcu_buffer u_mb (
        .clk(aclk), .rst_n(aresetn), .soft_reset(softrst),
        .pix_valid(pix_valid_w), .pix_row(pix_row_w),
        .pix0(pix0_w), .pix1(pix1_w), .pix2(pix2_w), .pix3(pix3_w),
        .pix4(pix4_w), .pix5(pix5_w), .pix6(pix6_w), .pix7(pix7_w),
        .blk_done_in(idct_blk_done_w),
        .blk_type(cur_blk_type_w),
        .is_411(is_411_w),                // Phase 11b: 4:1:1 Y 32x8 布局
        .is_cmyk(is_cmyk_w),              // Phase 12
        .mcu_ready(mb_ready_nc), .mcu_consume(1'b0),
        .rd_y_row(mb_y_row_w), .rd_y_col(mb_y_col_w),
        .rd_y_data(mb_y_data_w),
        .rd_c_row(mb_c_row_w), .rd_c_col(mb_c_col_w),
        .rd_cb_data(mb_cb_data_w), .rd_cr_data(mb_cr_data_w),
        .rd_k_data(mb_k_data_w)
    );

    // ---------------- Line buffer + mcu_line_copy -------------------
    wire        lc_start_w, lc_done_w;
    wire [15:0] lc_mcu_col_w;

    // Phase 13: line_buffer 样本宽度 16b
    wire        lb_y_wr_w;
    wire [3:0]  lb_y_row_w;
    wire [11:0] lb_y_col_w;
    wire [15:0] lb_y_data_w;
    wire        lb_c_wr_w;
    wire [2:0]  lb_c_row_w;
    wire [11:0] lb_c_col_w;            // Phase 9: 4:4:4 需 12b chroma 地址
    wire [15:0] lb_cb_data_w, lb_cr_data_w;
    wire        lb_k_wr_w;              // Phase 12
    wire [2:0]  lb_k_row_w;
    wire [11:0] lb_k_col_w;
    wire [15:0] lb_k_data_w;

    mcu_line_copy u_lc (
        .clk(aclk), .rst_n(aresetn), .soft_reset(softrst),
        .start(lc_start_w), .mcu_col_idx(lc_mcu_col_w), .done(lc_done_w),
        .is_grayscale(is_grayscale_w),    // Phase 8
        .is_444(is_444_w),                // Phase 9
        .is_422(is_422_w),                // Phase 10
        .is_440(is_440_w),                // Phase 11a
        .is_411(is_411_w),                // Phase 11b
        .is_cmyk(is_cmyk_w),              // Phase 12
        .mb_y_row(mb_y_row_w), .mb_y_col(mb_y_col_w),
        .mb_c_row(mb_c_row_w), .mb_c_col(mb_c_col_w),
        .mb_y_data(mb_y_data_w),
        .mb_cb_data(mb_cb_data_w), .mb_cr_data(mb_cr_data_w),
        .mb_k_data(mb_k_data_w),
        .lb_y_wr(lb_y_wr_w), .lb_y_row(lb_y_row_w), .lb_y_col(lb_y_col_w),
        .lb_y_data(lb_y_data_w),
        .lb_c_wr(lb_c_wr_w), .lb_c_row(lb_c_row_w), .lb_c_col(lb_c_col_w),
        .lb_cb_data(lb_cb_data_w), .lb_cr_data(lb_cr_data_w),
        .lb_k_wr(lb_k_wr_w), .lb_k_row(lb_k_row_w), .lb_k_col(lb_k_col_w),
        .lb_k_data(lb_k_data_w)
    );

    wire [3:0]  po_rd_y_row_w;
    wire [11:0] po_rd_y_col_w;
    wire [2:0]  po_rd_c_row_w;
    wire [11:0] po_rd_c_col_w;         // Phase 9: 4:4:4 需 12b chroma 地址
    wire [2:0]  po_rd_k_row_w;
    wire [11:0] po_rd_k_col_w;
    // Phase 13: 16b 样本
    wire [15:0] po_rd_y_data_w, po_rd_cb_data_w, po_rd_cr_data_w, po_rd_k_data_w;

    line_buffer u_lb (
        .clk(aclk), .rst_n(aresetn), .soft_reset(softrst),
        .wr_en(lb_y_wr_w), .wr_y_row(lb_y_row_w),
        .wr_y_col_abs(lb_y_col_w), .wr_y_data(lb_y_data_w),
        .wr_c_en(lb_c_wr_w), .wr_c_row(lb_c_row_w),
        .wr_c_col_abs(lb_c_col_w),
        .wr_cb_data(lb_cb_data_w), .wr_cr_data(lb_cr_data_w),
        .wr_k_en(lb_k_wr_w), .wr_k_row(lb_k_row_w),
        .wr_k_col_abs(lb_k_col_w), .wr_k_data(lb_k_data_w),
        .rd_y_row(po_rd_y_row_w), .rd_y_col(po_rd_y_col_w),
        .rd_c_row(po_rd_c_row_w), .rd_c_col(po_rd_c_col_w),
        .rd_k_row(po_rd_k_row_w), .rd_k_col(po_rd_k_col_w),
        .rd_y_data(po_rd_y_data_w),
        .rd_cb_data(po_rd_cb_data_w), .rd_cr_data(po_rd_cr_data_w),
        .rd_k_data(po_rd_k_data_w)
    );

    // ---------------- Pixel out (raster → FIFO) --------------------
    wire        row_ready_w, row_done_w;
    wire        is_first_row_w, is_last_row_w;

    // pixel_out → output FIFO slave 接口 (Phase 13: 48b)
    wire [47:0] po_tdata;
    wire        po_tvalid;
    wire        po_tready;
    wire        po_tuser;
    wire        po_tlast;

    pixel_out u_po (
        .clk(aclk), .rst_n(aresetn), .soft_reset(softrst),
        .row_start(row_ready_w), .img_width(img_w_w[11:0]),
        .img_height(img_h_w[11:0]),   // Phase 6: H 裁剪
        .is_first_row(is_first_row_w), .is_last_row(is_last_row_w),
        .is_grayscale(is_grayscale_w), // Phase 8
        .is_444(is_444_w),             // Phase 9
        .is_422(is_422_w),             // Phase 10
        .is_440(is_440_w),             // Phase 11a
        .is_411(is_411_w),             // Phase 11b
        .is_cmyk(is_cmyk_w),           // Phase 12
        .precision(precision_w),       // Phase 13
        .row_done(row_done_w),
        .rd_y_row(po_rd_y_row_w), .rd_y_col(po_rd_y_col_w),
        .rd_c_row(po_rd_c_row_w), .rd_c_col(po_rd_c_col_w),
        .rd_k_row(po_rd_k_row_w), .rd_k_col(po_rd_k_col_w),
        .rd_y_data(po_rd_y_data_w),
        .rd_cb_data(po_rd_cb_data_w), .rd_cr_data(po_rd_cr_data_w),
        .rd_k_data(po_rd_k_data_w),
        .tvalid(po_tvalid), .tready(po_tready), .tdata(po_tdata),
        .tuser_sof(po_tuser), .tlast(po_tlast)
    );

    // ---------------- Output FIFO (Phase 13: DW=48) -----------------
    // Phase 16c: SOF2 error gate — 在 sof2_err_r 置位后阻断像素向外输出，
    // 保证 errout 测试能观察到 "UNSUP_SOF && no pixels"。
    wire out_fifo_empty, out_fifo_full;
    wire po_tvalid_gated = po_tvalid & ~sof2_gate_err;
    axi_stream_fifo #(.DW(48), .UW(1), .DEPTH(32)) u_out_fifo (
        .clk(aclk), .rst_n(aresetn), .flush(softrst),
        .s_tdata(po_tdata),  .s_tuser(po_tuser),
        .s_tlast(po_tlast),  .s_tvalid(po_tvalid_gated),
        .s_tready(po_tready),
        .m_tdata(m_px_tdata),.m_tuser(m_px_tuser),
        .m_tlast(m_px_tlast),.m_tvalid(m_px_tvalid),
        .m_tready(m_px_tready),
        .empty(out_fifo_empty), .full(out_fifo_full)
    );

    // ---------------- Block sequencer -------------------------------
    wire        frame_done_seq_w;
    wire [15:0] mcu_col_idx_from_seq;

    block_sequencer u_seq (
        .clk(aclk), .rst_n(aresetn), .soft_reset(softrst),
        .frame_start(header_done_w),
        .img_width(img_w_w), .img_height(img_h_w),
        .num_components(num_components_w),  // Phase 12: 3b
        .chroma_mode(chroma_mode_w),        // Phase 12: 0..6
        .y_qt_sel(comp0_qt), .cb_qt_sel(comp1_qt), .cr_qt_sel(comp2_qt),
        .k_qt_sel(comp3_qt),                // Phase 12
        .y_dc_sel(comp0_td), .y_ac_sel(comp0_ta),
        .cb_dc_sel(comp1_td), .cb_ac_sel(comp1_ta),
        .cr_dc_sel(comp2_td), .cr_ac_sel(comp2_ta),
        .k_dc_sel(comp3_td), .k_ac_sel(comp3_ta),   // Phase 12
        .h_blk_start(h_blk_start_w), .h_dc_sel(h_dc_sel_w),
        .h_ac_sel(h_ac_sel_w),
        .h_dc_pred_in(h_dc_pred_in_w),
        .h_blk_done(h_blk_done_w), .h_dc_pred_out(h_dc_pred_out_w),
        .h_dc_pred_upd(h_dc_pred_upd_w),
        // Phase 7: DRI / restart
        .dri_interval(dri_interval_w),
        .marker_detected(marker_detected_w),
        .marker_byte(marker_byte_w),
        .restart_ack(restart_ack_w),
        .dc_restart(dc_restart_w),
        .align_req(align_req_w),
        .qt_sel_out(qt_sel_cur_w),
        .cur_blk_type(cur_blk_type_w),
        .idct_blk_done(idct_blk_done_w),
        .dcp_sel(dcp_sel_w), .dcp_wr(dcp_wr_w), .dcp_wr_data(dcp_wr_data_w),
        .dcp_y(dcp_y_w), .dcp_cb(dcp_cb_w), .dcp_cr(dcp_cr_w),
        .dcp_k(dcp_k_w),                   // Phase 12
        .mcu_copy_start(lc_start_w), .mcu_copy_done(lc_done_w),
        .mcu_col_idx(mcu_col_idx_from_seq),
        .row_ready(row_ready_w), .is_first_row_o(is_first_row_w),
        .is_last_row_o(is_last_row_w), .row_done(row_done_w),
        .frame_done_o(frame_done_seq_w)
    );
    assign lc_mcu_col_w = mcu_col_idx_from_seq;

    // ---------------- Pixel counter (已输出像素数) -----------------
    reg [31:0] pixel_cnt_r;
    always @(posedge aclk or negedge aresetn) begin
        if (!aresetn) pixel_cnt_r <= 32'd0;
        else if (start_pulse) pixel_cnt_r <= 32'd0;
        else if (m_px_tvalid && m_px_tready) pixel_cnt_r <= pixel_cnt_r + 32'd1;
    end

    // ---------------- Busy / 事件脉冲 ------------------------------
    reg busy_r;
    always @(posedge aclk or negedge aresetn) begin
        if (!aresetn) busy_r <= 1'b0;
        else if (softrst) busy_r <= 1'b0;
        else if (start_pulse) busy_r <= 1'b1;
        else if (frame_done_seq_w) busy_r <= 1'b0;
    end

    // Phase 16c: SOF2 运行时 gate — 在 header_done 脉冲时 latch
    // 1) DRI>0：progressive + restart 留给 Phase 17
    // 2) Al>0：successive approximation / 多扫描进度 scans 留给 Phase 18。
    //    Phase 16c 只接 Al=0 的单扫描 DC-only。libjpeg 默认 progressive 脚本
    //    首扫描 Al=1，因此此 gate 也同时拦下所有默认多扫描文件。
    // 3) 多扫描保险丝：若 data 模式里再出现 SOS marker 也置错（冗余兜底）。
    reg sof2_err_r;
    always @(posedge aclk or negedge aresetn) begin
        if (!aresetn)           sof2_err_r <= 1'b0;
        else if (softrst)       sof2_err_r <= 1'b0;
        else begin
            if (header_done_w && sof_type_w == 2'd2 &&
                (dri_interval_w != 16'd0 || sos_al_w != 4'd0))
                sof2_err_r <= 1'b1;
            if (sof_type_w == 2'd2 && marker_detected_w && marker_byte_w == 8'hDA)
                sof2_err_r <= 1'b1;
        end
    end
    wire sof2_gate_err = sof2_err_r;

    // 合并错误：header_parser.err 与 huffman_decoder.blk_err (ERR_BAD_HUFFMAN 位)
    wire [8:0] err_comb = err_w
                         | ({8'd0, h_blk_err_w}  << `ERR_BAD_HUFFMAN)
                         | ({8'd0, sof2_gate_err} << `ERR_UNSUP_SOF);

    // err 事件：err_comb 任意位从 0→1
    reg [8:0] err_prev;
    always @(posedge aclk or negedge aresetn) begin
        if (!aresetn) err_prev <= 9'd0;
        else if (softrst) err_prev <= 9'd0;
        else err_prev <= err_comb;
    end
    wire ev_error_w = |(err_comb & ~err_prev);

    // header 事件：header_done_w 上升沿
    reg hp_prev;
    always @(posedge aclk or negedge aresetn) begin
        if (!aresetn) hp_prev <= 1'b0;
        else if (softrst) hp_prev <= 1'b0;
        else hp_prev <= header_done_w;
    end
    wire ev_header_w = header_done_w & ~hp_prev;

    // ---------------- AXI-Lite CSR ---------------------------------
    axi_lite_slave u_csr (
        .aclk(aclk), .aresetn(aresetn),
        .s_awaddr(csr_awaddr), .s_awvalid(csr_awvalid), .s_awready(csr_awready),
        .s_wdata(csr_wdata), .s_wstrb(csr_wstrb),
        .s_wvalid(csr_wvalid), .s_wready(csr_wready),
        .s_bresp(csr_bresp), .s_bvalid(csr_bvalid), .s_bready(csr_bready),
        .s_araddr(csr_araddr), .s_arvalid(csr_arvalid), .s_arready(csr_arready),
        .s_rdata(csr_rdata), .s_rresp(csr_rresp),
        .s_rvalid(csr_rvalid), .s_rready(csr_rready),
        .start_pulse(start_pulse),
        .soft_reset_pulse(soft_reset_pulse),
        .abort_pulse(abort_pulse),
        .cfg_reg(cfg_reg_w), .scratch_reg(scratch_reg_w),
        .int_en(int_en_w),
        .ev_done(frame_done_seq_w),
        .ev_error(ev_error_w), .ev_header(ev_header_w),
        .irq(irq),
        .busy_in(busy_r),
        .frame_done_in(frame_done_seq_w),
        .input_empty_in(in_fifo_empty),
        .output_full_in(out_fifo_full),
        .header_done_in(header_done_w),
        .err_code_in(err_comb),
        .img_width_in(img_w_w),
        .img_height_in(img_h_w),
        .pixel_count_in(pixel_cnt_r),
        .precision_in(precision_w),     // Phase 13
        .sof_type_in(sof_type_w),       // Phase 16a
        .sos_ss_in(sos_ss_w),
        .sos_se_in(sos_se_w),
        .sos_ah_in(sos_ah_w),
        .sos_al_in(sos_al_w)
    );

    // 防未使用告警
    wire _unused = &{1'b0,
                     frame_done_hp,
                     cfg_reg_w, scratch_reg_w, int_en_w,
                     fifo_byte_tlast, in_fifo_full, out_fifo_empty,
                     in_fifo_tuser_nc, peek_valid_nc, mb_ready_nc,
                     1'b0};

endmodule
