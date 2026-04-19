// ---------------------------------------------------------------------------
// idct_1d — 3-stage pipelined 8-point 1D IDCT (JDCT_ISLOW，bit-exact 对齐 C int32)
//
// 输入: 8 × signed32                (cycle N)
// 输出: 8 × signed32 (未 DESCALE)   (cycle N+2)
//
// Stage A (register pre-mult inputs):
//   - 输入直接 pass-through: ra_in2/in6，ra_q0..q3 (= in7/in5/in3/in1)
//   - 预加: ra_z2a_z3a = in2+in6, ra_tmp0_sum = in0+in4, ra_tmp0_dif = in0-in4
//           ra_z1o = in7+in1, ra_z2o = in5+in3, ra_z3o = in7+in3, ra_z4o = in5+in1
//           ra_sum_z3z4 = ra_z3o + ra_z4o (pre-compute 3-operand sum)
// Stage B (multiplies → register): 全部 mult 落到 r_*m / r_z1_e / r_z3a_nC7 / r_z2a_C3
//   - 同时 tmp0e/tmp1e 也在此拍注入 (只是 shift，零延迟)
// Stage C (final adds): 组合 → output
//
// 总共 Stage-A + Stage-B 寄存器 = 14+14 = 28 × 32-bit = 896 flops / instance
// (vs 2-stage 时 14 × 32 = 448)。增加 ~260 µm² per top (2 实例)。
//
// 依赖：消费者 (idct_2d) 用 pipeline tracking 保证只在 feed 后 2 拍才采纳输出。
// ---------------------------------------------------------------------------
`include "jpeg_defs.vh"

module idct_1d (
    input  wire                clk,
    input  wire signed [31:0] in0, in1, in2, in3, in4, in5, in6, in7,
    output wire signed [31:0] out0, out1, out2, out3, out4, out5, out6, out7
);

    wire signed [31:0] C0  = {17'd0, `FIX_0_298631336};
    wire signed [31:0] C1  = {17'd0, `FIX_0_390180644};
    wire signed [31:0] C2  = {17'd0, `FIX_0_541196100};
    wire signed [31:0] C3  = {17'd0, `FIX_0_765366865};
    wire signed [31:0] C4  = {17'd0, `FIX_0_899976223};
    wire signed [31:0] C5  = {17'd0, `FIX_1_175875602};
    wire signed [31:0] C6  = {17'd0, `FIX_1_501321110};
    wire signed [31:0] C7  = {17'd0, `FIX_1_847759065};
    wire signed [31:0] C8  = {17'd0, `FIX_1_961570560};
    wire signed [31:0] C9  = {17'd0, `FIX_2_053119869};
    wire signed [31:0] C10 = {17'd0, `FIX_2_562915447};
    wire signed [31:0] C11 = {17'd0, `FIX_3_072711026};

    // =================================================================
    // Stage A (combinational): compute input sums
    // =================================================================
    wire signed [31:0] a_z1o       = in7 + in1;        // q0+q3
    wire signed [31:0] a_z2o       = in5 + in3;        // q1+q2
    wire signed [31:0] a_z3o       = in7 + in3;        // q0+q2
    wire signed [31:0] a_z4o       = in5 + in1;        // q1+q3
    wire signed [31:0] a_sum_z3z4  = a_z3o + a_z4o;    // for z5o = * C5
    wire signed [31:0] a_z2a_z3a   = in2 + in6;        // for z1_e = * C2
    wire signed [31:0] a_tmp0_sum  = in0 + in4;
    wire signed [31:0] a_tmp0_dif  = in0 - in4;

    // =================================================================
    // Stage A registers (14 × 32-bit = 448 flops / instance). No reset.
    // =================================================================
    reg signed [31:0] ra_z1o, ra_z2o, ra_z3o, ra_z4o, ra_sum_z3z4;
    reg signed [31:0] ra_z2a_z3a, ra_tmp0_sum, ra_tmp0_dif;
    reg signed [31:0] ra_z2a, ra_z3a;       // in2, in6 pass-through for scalar mults
    reg signed [31:0] ra_q0, ra_q1, ra_q2, ra_q3; // in7,in5,in3,in1 pass-through

    always @(posedge clk) begin
        ra_z1o      <= a_z1o;
        ra_z2o      <= a_z2o;
        ra_z3o      <= a_z3o;
        ra_z4o      <= a_z4o;
        ra_sum_z3z4 <= a_sum_z3z4;
        ra_z2a_z3a  <= a_z2a_z3a;
        ra_tmp0_sum <= a_tmp0_sum;
        ra_tmp0_dif <= a_tmp0_dif;
        ra_z2a      <= in2;
        ra_z3a      <= in6;
        ra_q0       <= in7;  // q0
        ra_q1       <= in5;  // q1
        ra_q2       <= in3;  // q2
        ra_q3       <= in1;  // q3
    end

    // =================================================================
    // Stage B (combinational): all multiplies + pass-through shifts
    // =================================================================
    wire signed [31:0] b_z1_e    = ra_z2a_z3a * C2;
    wire signed [31:0] b_z3a_nC7 = ra_z3a * (-C7);
    wire signed [31:0] b_z2a_C3  = ra_z2a * ( C3);
    wire signed [31:0] b_tmp0e   = ra_tmp0_sum <<< `CONST_BITS;
    wire signed [31:0] b_tmp1e   = ra_tmp0_dif <<< `CONST_BITS;

    wire signed [31:0] b_q0m = ra_q0 * ( C0);
    wire signed [31:0] b_q1m = ra_q1 * ( C9);
    wire signed [31:0] b_q2m = ra_q2 * ( C11);
    wire signed [31:0] b_q3m = ra_q3 * ( C6);

    wire signed [31:0] b_z1m = ra_z1o * (-C4);
    wire signed [31:0] b_z2m = ra_z2o * (-C10);
    wire signed [31:0] b_z3m = ra_z3o * (-C8);
    wire signed [31:0] b_z4m = ra_z4o * (-C1);
    wire signed [31:0] b_z5o = ra_sum_z3z4 * C5;

    // =================================================================
    // Stage B registers (14 × 32-bit = 448 flops / instance). No reset.
    // =================================================================
    reg signed [31:0] r_z1_e, r_z3a_nC7, r_z2a_C3;
    reg signed [31:0] r_tmp0e, r_tmp1e;
    reg signed [31:0] r_q0m, r_q1m, r_q2m, r_q3m;
    reg signed [31:0] r_z1m, r_z2m, r_z3m, r_z4m;
    reg signed [31:0] r_z5o;

    always @(posedge clk) begin
        r_z1_e    <= b_z1_e;
        r_z3a_nC7 <= b_z3a_nC7;
        r_z2a_C3  <= b_z2a_C3;
        r_tmp0e   <= b_tmp0e;
        r_tmp1e   <= b_tmp1e;
        r_q0m     <= b_q0m;
        r_q1m     <= b_q1m;
        r_q2m     <= b_q2m;
        r_q3m     <= b_q3m;
        r_z1m     <= b_z1m;
        r_z2m     <= b_z2m;
        r_z3m     <= b_z3m;
        r_z4m     <= b_z4m;
        r_z5o     <= b_z5o;
    end

    // =================================================================
    // Stage C (combinational): final adds only
    // =================================================================
    wire signed [31:0] tmp2e = r_z1_e + r_z3a_nC7;
    wire signed [31:0] tmp3e = r_z1_e + r_z2a_C3;

    wire signed [31:0] tmp10 = r_tmp0e + tmp3e;
    wire signed [31:0] tmp13 = r_tmp0e - tmp3e;
    wire signed [31:0] tmp11 = r_tmp1e + tmp2e;
    wire signed [31:0] tmp12 = r_tmp1e - tmp2e;

    wire signed [31:0] z3s = r_z3m + r_z5o;
    wire signed [31:0] z4s = r_z4m + r_z5o;

    wire signed [31:0] ot0 = r_q0m + r_z1m + z3s;
    wire signed [31:0] ot1 = r_q1m + r_z2m + z4s;
    wire signed [31:0] ot2 = r_q2m + r_z2m + z3s;
    wire signed [31:0] ot3 = r_q3m + r_z1m + z4s;

    assign out0 = tmp10 + ot3;
    assign out7 = tmp10 - ot3;
    assign out1 = tmp11 + ot2;
    assign out6 = tmp11 - ot2;
    assign out2 = tmp12 + ot1;
    assign out5 = tmp12 - ot1;
    assign out3 = tmp13 + ot0;
    assign out4 = tmp13 - ot0;

endmodule
