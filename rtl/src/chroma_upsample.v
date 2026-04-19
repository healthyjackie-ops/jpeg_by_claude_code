// ---------------------------------------------------------------------------
// chroma_upsample — 4:2:0 → 4:4:4  nearest-neighbor (box filter)
//
// 对应 C: chroma_upsample_nn  (每个 8×8 C 值复制为 16×16 中对应 2×2)
//
// 组合逻辑：输入 MCU 内像素坐标 (y_row[3:0], y_col[3:0])，给出对应 Cb/Cr
// 8×8 内坐标 (c_row = y_row >> 1, c_col = y_col >> 1)。
//
// 实际 RAM 访问由 mcu_buffer 完成。
// ---------------------------------------------------------------------------
module chroma_upsample (
    input  wire [3:0] y_row,
    input  wire [3:0] y_col,
    output wire [2:0] c_row,
    output wire [2:0] c_col
);
    assign c_row = y_row[3:1];
    assign c_col = y_col[3:1];
endmodule
