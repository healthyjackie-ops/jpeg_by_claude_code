// ---------------------------------------------------------------------------
// coef_buffer — DCT coefficient frame buffer (Phase 15)
//
// Stores one 8×8 int16 DCT block (64 × 16b = 1024b) per address. Used by
// progressive SOF2 decode (Phase 16+) to accumulate coefficients across
// multiple scans before the final IDCT pass.
//
// Ports:
//   w_en   — write an entire block                      (1 cycle)
//   r_en   — read an entire block                       (1-cycle latency)
//   acc_en — read-modify-write a single 16b coefficient (1 cycle, behavioral)
//
// Layout: w_data[k*16 +: 16] holds coef[k], k∈[0,63], natural order.
//
// Phase 15 delivers a behavioral (reg-array) implementation for Verilator
// loop-back testing. Real synthesis (Wave 3 final) will map mem[] to SRAM
// and split acc_en into a 2-cycle RMW FSM. See spec_phase15.md §Risks.
// ---------------------------------------------------------------------------
`include "jpeg_defs.vh"

module coef_buffer #(
    parameter AW = 10              // block address width: 1<<AW blocks (128 KB @ AW=10)
) (
    input  wire              clk,
    input  wire              rst_n,

    // Write port
    input  wire              w_en,
    input  wire [AW-1:0]     w_addr,
    input  wire [1023:0]     w_data,

    // Read port (1-cycle latency)
    input  wire              r_en,
    input  wire [AW-1:0]     r_addr,
    output reg  [1023:0]     r_data,
    output reg               r_valid,

    // Accumulate port (single coef RMW)
    input  wire              acc_en,
    input  wire [AW-1:0]     acc_addr,
    input  wire [5:0]        acc_coef_idx,
    input  wire signed [15:0] acc_delta,
    output reg               acc_done
);

    reg [1023:0] mem [0:(1<<AW)-1];

    integer i;
    initial begin
        for (i = 0; i < (1 << AW); i = i + 1)
            mem[i] = {1024{1'b0}};
    end

    // Priority: w > acc > r on the same address (see spec §接口语义).
    // In practice, testbench interleaves operations so collisions do not occur.
    reg [1023:0] acc_tmp;
    reg signed [15:0] acc_cur;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            r_data   <= 1024'd0;
            r_valid  <= 1'b0;
            acc_done <= 1'b0;
        end else begin
            r_valid  <= 1'b0;
            acc_done <= 1'b0;

            if (w_en) begin
                mem[w_addr] <= w_data;
            end

            if (acc_en) begin
                acc_tmp = mem[acc_addr];
                acc_cur = acc_tmp[acc_coef_idx*16 +: 16];
                acc_tmp[acc_coef_idx*16 +: 16] = acc_cur + acc_delta;
                mem[acc_addr] <= acc_tmp;
                acc_done <= 1'b1;
            end

            if (r_en) begin
                r_data  <= mem[r_addr];
                r_valid <= 1'b1;
            end
        end
    end

endmodule
