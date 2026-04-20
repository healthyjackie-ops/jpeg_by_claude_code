// ---------------------------------------------------------------------------
// axi_lite_slave — JPEG 解码器 CSR (对应 docs/regmap.md)
//
// AXI4-Lite 32-bit 数据 / 12-bit 地址（4KB 空间）
//   0x000 ID         (RO, 0xA17C_01_10)
//   0x004 CTRL       (RW: START / SOFT_RESET / ABORT)
//   0x008 STATUS     (RO)
//   0x00C INT_EN     (RW)
//   0x010 INT_STATUS (RW1C)
//   0x014 IMG_WIDTH  (RO)
//   0x018 IMG_HEIGHT (RO)
//   0x01C PIXEL_COUNT(RO)
//   0x020 ERROR_CODE (RO, sticky, SOFT_RESET 清零)
//   0x024 CONFIG     (RW)
//   0x028 SCRATCH    (RW)
//   0x02C PIX_FMT    (RO, Phase 13: bit[0] = precision 0=P=8/1=P=12)
//
// 不支持 burst，按每次 1 beat 处理。
// ---------------------------------------------------------------------------
module axi_lite_slave (
    input  wire        aclk,
    input  wire        aresetn,

    // AW channel
    input  wire [11:0] s_awaddr,
    input  wire        s_awvalid,
    output reg         s_awready,

    // W channel
    input  wire [31:0] s_wdata,
    input  wire [3:0]  s_wstrb,
    input  wire        s_wvalid,
    output reg         s_wready,

    // B channel
    output reg  [1:0]  s_bresp,
    output reg         s_bvalid,
    input  wire        s_bready,

    // AR channel
    input  wire [11:0] s_araddr,
    input  wire        s_arvalid,
    output reg         s_arready,

    // R channel
    output reg  [31:0] s_rdata,
    output reg  [1:0]  s_rresp,
    output reg         s_rvalid,
    input  wire        s_rready,

    // --- 对内信号 ----------------------------------------------------
    output reg         start_pulse,        // CTRL.START 写 1 的 1 cycle 脉冲
    output reg         soft_reset_pulse,   // CTRL.SOFT_RESET 脉冲
    output reg         abort_pulse,        // CTRL.ABORT 脉冲

    output reg  [31:0] cfg_reg,            // CONFIG (bit0 OUT_FMT, bit1 OUT_RGB)
    output reg  [31:0] scratch_reg,

    // 中断
    output reg  [2:0]  int_en,             // IE_DONE / IE_ERROR / IE_HEADER
    input  wire        ev_done,            // 来自 block_sequencer.frame_done_o
    input  wire        ev_error,           // err != 0 脉冲（新错误）
    input  wire        ev_header,          // header_parser.header_done 脉冲
    output wire        irq,

    // 状态输入
    input  wire        busy_in,
    input  wire        frame_done_in,      // sticky until next START (暂未使用)
    input  wire        input_empty_in,
    input  wire        output_full_in,
    input  wire        header_done_in,
    input  wire [8:0]  err_code_in,        // 来自 header_parser.err

    input  wire [15:0] img_width_in,
    input  wire [15:0] img_height_in,
    input  wire [31:0] pixel_count_in,

    // Phase 13
    input  wire        precision_in        // 0 = P=8, 1 = P=12
);

    // ---- AW/W/AR simple handshakes ------------------------------------
    reg [11:0] aw_addr_r;
    reg [11:0] ar_addr_r;
    reg        aw_hs;
    reg        w_hs;

    // 写事务：收到 AW+W 后进入写 commit
    always @(posedge aclk or negedge aresetn) begin
        if (!aresetn) begin
            s_awready <= 1'b0;
            s_wready  <= 1'b0;
            s_bvalid  <= 1'b0;
            s_bresp   <= 2'b00;
            aw_addr_r <= 12'd0;
            aw_hs     <= 1'b0;
            w_hs      <= 1'b0;
        end else begin
            // AW
            if (!aw_hs && s_awvalid) begin
                s_awready <= 1'b1;
                aw_addr_r <= s_awaddr;
                aw_hs     <= 1'b1;
            end else begin
                s_awready <= 1'b0;
            end

            // W
            if (!w_hs && s_wvalid) begin
                s_wready <= 1'b1;
                w_hs     <= 1'b1;
            end else begin
                s_wready <= 1'b0;
            end

            // B
            if (aw_hs && w_hs && !s_bvalid) begin
                s_bvalid <= 1'b1;
                // bresp 由写逻辑在同一 always 中（下方）更新
            end else if (s_bvalid && s_bready) begin
                s_bvalid <= 1'b0;
                aw_hs    <= 1'b0;
                w_hs     <= 1'b0;
                s_bresp  <= 2'b00;
            end
        end
    end

    // ---- 写 commit 逻辑 (aw_hs && w_hs 时生效；需 s_wstrb==4'hF) -----
    wire wr_commit = aw_hs && w_hs && !s_bvalid;
    wire wr_full   = (s_wstrb == 4'hF);
    wire [11:0] wr_addr = aw_addr_r;

    // INT_STATUS sticky 位（W1C）
    reg [2:0] int_status_r;
    reg [8:0] err_code_sticky;
    reg       frame_done_sticky;

    always @(posedge aclk or negedge aresetn) begin
        if (!aresetn) begin
            cfg_reg          <= 32'd0;
            scratch_reg      <= 32'd0;
            int_en           <= 3'd0;
            int_status_r     <= 3'd0;
            start_pulse      <= 1'b0;
            soft_reset_pulse <= 1'b0;
            abort_pulse      <= 1'b0;
            err_code_sticky  <= 9'd0;
            frame_done_sticky<= 1'b0;
        end else begin
            start_pulse      <= 1'b0;
            soft_reset_pulse <= 1'b0;
            abort_pulse      <= 1'b0;

            // 错误 sticky：任何时候 err_code_in 置位即累积
            err_code_sticky <= err_code_sticky | err_code_in;

            // 中断 sticky
            if (ev_done)   int_status_r[0] <= 1'b1;
            if (ev_error)  int_status_r[1] <= 1'b1;
            if (ev_header) int_status_r[2] <= 1'b1;

            // frame_done sticky：由下次 START 清除
            if (ev_done)    frame_done_sticky <= 1'b1;

            if (wr_commit && wr_full) begin
                case (wr_addr)
                    12'h004: begin   // CTRL
                        if (s_wdata[0] && !busy_in) begin
                            start_pulse       <= 1'b1;
                            frame_done_sticky <= 1'b0;  // 新帧清 done
                        end
                        if (s_wdata[1]) begin
                            soft_reset_pulse <= 1'b1;
                            // sticky 错误在软复位时清零
                            err_code_sticky <= 9'd0;
                            int_status_r    <= 3'd0;
                            frame_done_sticky <= 1'b0;
                        end
                        if (s_wdata[2]) abort_pulse <= 1'b1;
                    end
                    12'h00C: int_en <= s_wdata[2:0];
                    12'h010: begin  // W1C
                        int_status_r <= int_status_r & ~s_wdata[2:0];
                    end
                    12'h024: cfg_reg     <= {30'd0, s_wdata[1:0]};
                    12'h028: scratch_reg <= s_wdata;
                    default: ;
                endcase
            end
        end
    end

    // ---- 写响应 bresp ------------------------------------------------
    // SLVERR 条件：wstrb ≠ 0xF；或不支持功能写（OUT_FMT=1 / OUT_RGB=1）
    always @(posedge aclk or negedge aresetn) begin
        if (!aresetn) begin
            // 默认清零，已在上方处理
        end else if (wr_commit) begin
            if (!wr_full)
                s_bresp <= 2'b10;
            else if (wr_addr == 12'h024 && (s_wdata[0] || s_wdata[1]))
                s_bresp <= 2'b10;   // 不支持 OUT_FMT=1 或 OUT_RGB=1
            else
                s_bresp <= 2'b00;
        end
    end

    // ---- 读事务 ------------------------------------------------------
    // STATUS 组合
    wire [31:0] status_word =
        {26'd0,
         header_done_in,       // [5]
         output_full_in,       // [4]
         input_empty_in,       // [3]
         (err_code_sticky != 9'd0),  // [2] ERROR
         frame_done_sticky,    // [1]
         busy_in};             // [0]

    wire [31:0] err_word = {23'd0, err_code_sticky};

    always @(posedge aclk or negedge aresetn) begin
        if (!aresetn) begin
            s_arready <= 1'b0;
            s_rvalid  <= 1'b0;
            s_rdata   <= 32'd0;
            s_rresp   <= 2'b00;
            ar_addr_r <= 12'd0;
        end else begin
            // AR
            if (!s_rvalid && s_arvalid && !s_arready) begin
                s_arready <= 1'b1;
                ar_addr_r <= s_araddr;
            end else begin
                s_arready <= 1'b0;
            end

            // R
            if (s_arready) begin
                s_rvalid <= 1'b1;
                s_rresp  <= 2'b00;
                case (ar_addr_r)
                    12'h000: s_rdata <= 32'hA17C_0110;
                    12'h004: s_rdata <= 32'd0;                   // CTRL 读 0
                    12'h008: s_rdata <= status_word;
                    12'h00C: s_rdata <= {29'd0, int_en};
                    12'h010: s_rdata <= {29'd0, int_status_r};
                    12'h014: s_rdata <= {16'd0, img_width_in};
                    12'h018: s_rdata <= {16'd0, img_height_in};
                    12'h01C: s_rdata <= pixel_count_in;
                    12'h020: s_rdata <= err_word;
                    12'h024: s_rdata <= cfg_reg;
                    12'h028: s_rdata <= scratch_reg;
                    12'h02C: s_rdata <= {31'd0, precision_in};   // Phase 13 PIX_FMT
                    default: s_rdata <= 32'd0;
                endcase
            end else if (s_rvalid && s_rready) begin
                s_rvalid <= 1'b0;
            end
        end
    end

    // ---- IRQ ---------------------------------------------------------
    assign irq = |(int_status_r & int_en);

    // frame_done_in 信号目前未使用（由 ev_done 直接驱动 sticky），保留以便扩展
    wire _unused_axi = frame_done_in;

endmodule
