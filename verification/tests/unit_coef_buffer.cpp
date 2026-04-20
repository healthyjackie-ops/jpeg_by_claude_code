// ---------------------------------------------------------------------------
// unit_coef_buffer.cpp — Loop-back unit test for rtl/src/coef_buffer.v (Phase 15)
//
// Tests:
//   1. WRITE+READ — write N random blocks, read back sequentially, bit-exact
//   2. Non-sequential reads — reverse and random order
//   3. ACCUMULATE (RMW) — seed zeros, apply deltas, read back and verify sum
//   4. OVERWRITE — write fresh values over accumulate result, verify bit-exact
// ---------------------------------------------------------------------------
#include "Vcoef_buffer.h"
#include "verilated.h"
#include "verilated_vcd_c.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <unistd.h>
#include <vector>

namespace {

// A DCT block: 64 × int16 = 1024 bits.
using Block = std::array<uint16_t, 64>;

// ---- Helpers --------------------------------------------------------------

Block random_block(std::mt19937& rng) {
    Block b{};
    for (auto& c : b) c = static_cast<uint16_t>(rng() & 0xFFFFu);
    return b;
}

// Pack Block into Verilator-wide 1024b port (32 × uint32 little-endian).
void pack_block(VlWide<32>& dst, const Block& b) {
    for (int k = 0; k < 32; ++k) {
        uint32_t lo = b[k * 2];
        uint32_t hi = b[k * 2 + 1];
        dst.at(k) = lo | (hi << 16);
    }
}

Block unpack_block(const VlWide<32>& src) {
    Block b{};
    for (int k = 0; k < 32; ++k) {
        uint32_t w = src.at(k);
        b[k * 2]     = static_cast<uint16_t>(w & 0xFFFFu);
        b[k * 2 + 1] = static_cast<uint16_t>((w >> 16) & 0xFFFFu);
    }
    return b;
}

struct Tb {
    std::unique_ptr<VerilatedContext> ctx = std::make_unique<VerilatedContext>();
    std::unique_ptr<Vcoef_buffer> dut;
    uint64_t cycle = 0;

    Tb() {
        dut = std::make_unique<Vcoef_buffer>(ctx.get(), "u");
        dut->clk = 0;
        dut->rst_n = 0;
        dut->w_en = 0;
        dut->r_en = 0;
        dut->acc_en = 0;
    }

    void tick() {
        dut->clk = 1; dut->eval();
        dut->clk = 0; dut->eval();
        ++cycle;
    }

    void reset() {
        dut->rst_n = 0;
        for (int i = 0; i < 4; ++i) tick();
        dut->rst_n = 1;
        tick();
    }
};

int fails = 0;

void expect(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++fails;
    }
}

// ---- Test 1+2: write random blocks, read back in various orders -----------

int test_wr_rd() {
    Tb tb;
    tb.reset();
    std::mt19937 rng(0x15A15A15u);

    const int N = 512;
    std::vector<Block> golden(N);
    for (int i = 0; i < N; ++i) golden[i] = random_block(rng);

    // --- Write N blocks ---
    for (int i = 0; i < N; ++i) {
        tb.dut->w_en = 1;
        tb.dut->w_addr = i;
        pack_block(tb.dut->w_data, golden[i]);
        tb.tick();
    }
    tb.dut->w_en = 0;
    tb.tick();

    // --- Test 1: sequential read ---
    int t1_fails = 0;
    for (int i = 0; i < N; ++i) {
        tb.dut->r_en = 1;
        tb.dut->r_addr = i;
        tb.tick();
        tb.dut->r_en = 0;
        // r_valid asserts on the edge after r_en=1; value is already on r_data
        if (!tb.dut->r_valid) {
            ++t1_fails;
            if (t1_fails < 5) std::fprintf(stderr, "Test1 block %d: r_valid not asserted\n", i);
            continue;
        }
        Block got = unpack_block(tb.dut->r_data);
        if (got != golden[i]) {
            ++t1_fails;
            if (t1_fails < 5) {
                std::fprintf(stderr, "Test1 block %d mismatch: got[0]=%u want[0]=%u\n",
                             i, got[0], golden[i][0]);
            }
        }
    }
    tb.dut->r_en = 0;
    tb.tick();
    if (t1_fails == 0) std::printf("  [COEF] Test1 sequential  %d/%d OK\n", N, N);
    else               std::printf("  [COEF] Test1 sequential  %d/%d FAIL (+%d)\n", N - t1_fails, N, t1_fails);
    fails += t1_fails;

    // --- Test 2: reverse read ---
    int t2_fails = 0;
    for (int i = N - 1; i >= 0; --i) {
        tb.dut->r_en = 1;
        tb.dut->r_addr = i;
        tb.tick();
        Block got = unpack_block(tb.dut->r_data);
        if (got != golden[i]) {
            ++t2_fails;
            if (t2_fails < 5)
                std::fprintf(stderr, "Test2 block %d reverse mismatch: got[0]=%u want[0]=%u\n",
                             i, got[0], golden[i][0]);
        }
    }
    tb.dut->r_en = 0;
    tb.tick();
    if (t2_fails == 0) std::printf("  [COEF] Test2 reverse     %d/%d OK\n", N, N);
    else               std::printf("  [COEF] Test2 reverse     %d/%d FAIL (+%d)\n", N - t2_fails, N, t2_fails);
    fails += t2_fails;

    // --- Test 2b: random-order read ---
    std::vector<int> indices(N);
    for (int i = 0; i < N; ++i) indices[i] = i;
    std::shuffle(indices.begin(), indices.end(), rng);
    int t2b_fails = 0;
    for (int i : indices) {
        tb.dut->r_en = 1;
        tb.dut->r_addr = i;
        tb.tick();
        Block got = unpack_block(tb.dut->r_data);
        if (got != golden[i]) {
            ++t2b_fails;
            if (t2b_fails < 5)
                std::fprintf(stderr, "Test2b block %d random mismatch\n", i);
        }
    }
    tb.dut->r_en = 0;
    tb.tick();
    if (t2b_fails == 0) std::printf("  [COEF] Test2b random     %d/%d OK\n", N, N);
    else                std::printf("  [COEF] Test2b random     %d/%d FAIL (+%d)\n", N - t2b_fails, N, t2b_fails);
    fails += t2b_fails;

    return t1_fails + t2_fails + t2b_fails;
}

// ---- Test 3: ACCUMULATE (RMW) --------------------------------------------

int test_accumulate() {
    Tb tb;
    tb.reset();
    std::mt19937 rng(0xACCACC00u);

    const int N = 256;
    // Seed all blocks to zero
    for (int i = 0; i < N; ++i) {
        tb.dut->w_en = 1;
        tb.dut->w_addr = i;
        for (int k = 0; k < 32; ++k) tb.dut->w_data.at(k) = 0;
        tb.tick();
    }
    tb.dut->w_en = 0;
    tb.tick();

    // Track expected coefficient-accumulator per (block, coef) — sparse
    std::vector<std::array<int32_t, 64>> expected(N);
    for (auto& row : expected) row.fill(0);

    // Issue M random accumulates
    const int M = 3000;
    for (int m = 0; m < M; ++m) {
        uint32_t blk = rng() % N;
        uint32_t ci  = rng() % 64;
        int16_t delta = static_cast<int16_t>(rng() & 0xFFFFu);
        tb.dut->acc_en        = 1;
        tb.dut->acc_addr      = blk;
        tb.dut->acc_coef_idx  = ci;
        tb.dut->acc_delta     = static_cast<uint16_t>(delta);
        tb.tick();
        tb.dut->acc_en        = 0;
        // acc_done pulses for exactly 1 cycle after acc_en — check before next tick clears it
        expect(tb.dut->acc_done, "acc_done did not pulse");
        tb.tick();  // advance to idle
        expected[blk][ci] = static_cast<int16_t>(expected[blk][ci] + delta);
    }

    // Read back and verify
    int t3_fails = 0;
    for (int i = 0; i < N; ++i) {
        tb.dut->r_en = 1;
        tb.dut->r_addr = i;
        tb.tick();
        tb.dut->r_en = 0;
        Block got = unpack_block(tb.dut->r_data);
        for (int k = 0; k < 64; ++k) {
            int16_t w_got = static_cast<int16_t>(got[k]);
            int16_t w_exp = static_cast<int16_t>(expected[i][k]);
            if (w_got != w_exp) {
                ++t3_fails;
                if (t3_fails < 5)
                    std::fprintf(stderr, "Test3 block %d coef %d: got=%d want=%d\n",
                                 i, k, w_got, w_exp);
            }
        }
    }
    if (t3_fails == 0) std::printf("  [COEF] Test3 accumulate  %dx%d=%d coefs OK\n", N, 64, N * 64);
    else               std::printf("  [COEF] Test3 accumulate  FAIL (+%d mismatches)\n", t3_fails);
    fails += t3_fails;

    return t3_fails;
}

// ---- Test 4: OVERWRITE ---------------------------------------------------

int test_overwrite() {
    Tb tb;
    tb.reset();
    std::mt19937 rng(0x0FFFu);

    const int N = 256;
    // Seed with arbitrary non-zero pattern via accumulate on coef 0
    for (int i = 0; i < N; ++i) {
        tb.dut->acc_en = 1;
        tb.dut->acc_addr = i;
        tb.dut->acc_coef_idx = 0;
        tb.dut->acc_delta = 0x1234;
        tb.tick();
        tb.dut->acc_en = 0;
        tb.tick();
    }

    // Overwrite every block with fresh random values
    std::vector<Block> fresh(N);
    for (int i = 0; i < N; ++i) {
        fresh[i] = random_block(rng);
        tb.dut->w_en = 1;
        tb.dut->w_addr = i;
        pack_block(tb.dut->w_data, fresh[i]);
        tb.tick();
    }
    tb.dut->w_en = 0;
    tb.tick();

    // Read back and verify fresh value fully replaced accumulate residue
    int t4_fails = 0;
    for (int i = 0; i < N; ++i) {
        tb.dut->r_en = 1;
        tb.dut->r_addr = i;
        tb.tick();
        tb.dut->r_en = 0;
        Block got = unpack_block(tb.dut->r_data);
        if (got != fresh[i]) {
            ++t4_fails;
            if (t4_fails < 5)
                std::fprintf(stderr, "Test4 block %d overwrite mismatch: got[0]=%u want[0]=%u\n",
                             i, got[0], fresh[i][0]);
        }
    }
    if (t4_fails == 0) std::printf("  [COEF] Test4 overwrite   %d/%d OK\n", N, N);
    else               std::printf("  [COEF] Test4 overwrite   FAIL (+%d)\n", t4_fails);
    fails += t4_fails;

    return t4_fails;
}

} // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    std::printf("[COEF] coef_buffer loopback unit test\n");
    test_wr_rd();
    test_accumulate();
    test_overwrite();
    std::printf("[COEF] total fails = %d -> %s\n", fails, fails ? "FAIL" : "PASS");
    std::fflush(stdout);
    _exit(fails ? 1 : 0);
}
