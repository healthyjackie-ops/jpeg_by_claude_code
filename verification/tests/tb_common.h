// ---------------------------------------------------------------------------
// tb_common.h — shared testbench helpers
//
// Wraps the Verilated jpeg_axi_top DUT with:
//   * Clock/reset generation
//   * VCD trace toggling
//   * Cycle counter + timeout
// BFMs (AXI-Lite CSR, bytestream driver, pixel sink) will be added in
// subsequent tasks — kept minimal here so the skeleton compiles and runs.
// ---------------------------------------------------------------------------
#ifndef TB_COMMON_H
#define TB_COMMON_H

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "verilated.h"
#include "verilated_vcd_c.h"
#include "Vjpeg_axi_top.h"

struct TbCtx {
    std::unique_ptr<Vjpeg_axi_top> dut;
    std::unique_ptr<VerilatedVcdC> trace;
    std::unique_ptr<VerilatedContext> ctx;
    uint64_t cycle = 0;
    uint64_t timeout_cycles = 2'000'000;
    // Optional hook invoked once per tick() call to capture streaming outputs
    // (e.g. pixel bus) so that BFM-driven ticks inside CSR transactions do not
    // silently drop bus beats. Called PRE-tick (before half_step) with `this`.
    std::function<void(TbCtx&)> on_tick_pre = nullptr;

    explicit TbCtx(const char* vcd_path = nullptr) {
        ctx = std::make_unique<VerilatedContext>();
        ctx->traceEverOn(vcd_path != nullptr);
        dut = std::make_unique<Vjpeg_axi_top>(ctx.get(), "jpeg");
        if (vcd_path) {
            trace = std::make_unique<VerilatedVcdC>();
            dut->trace(trace.get(), 99);
            trace->open(vcd_path);
        }
        // Idle all inputs
        dut->aclk = 0;
        dut->aresetn = 0;
        dut->csr_awaddr = 0;  dut->csr_awvalid = 0;
        dut->csr_wdata  = 0;  dut->csr_wstrb   = 0; dut->csr_wvalid = 0;
        dut->csr_bready = 1;
        dut->csr_araddr = 0;  dut->csr_arvalid = 0;
        dut->csr_rready = 1;
        dut->s_bs_tdata = 0;  dut->s_bs_tvalid = 0; dut->s_bs_tlast = 0;
        dut->m_px_tready = 1;
    }

    ~TbCtx() {
        if (trace) trace->close();
    }

    // Advance half cycle — monotonic time = cycle*10 + (aclk already toggled to 1 ? 5 : 10)
    void half_step() {
        dut->aclk = !dut->aclk;
        dut->eval();
        uint64_t t = cycle * 10 + (dut->aclk ? 5 : 10);
        if (trace) trace->dump(t);
    }

    // Advance one full cycle (posedge-to-posedge)
    void tick() {
        if (on_tick_pre) on_tick_pre(*this);
        half_step();   // rising edge  (aclk now 1, t = cycle*10 + 5)
        half_step();   // falling edge (aclk now 0, t = cycle*10 + 10)
        ++cycle;
        if (cycle >= timeout_cycles) {
            std::fprintf(stderr, "[TB] TIMEOUT at cycle %llu\n",
                         static_cast<unsigned long long>(cycle));
            if (trace) trace->flush();
            std::exit(2);
        }
    }

    // Hold aresetn low for n cycles
    void reset(int cycles = 8) {
        dut->aresetn = 0;
        for (int i = 0; i < cycles; ++i) tick();
        dut->aresetn = 1;
        tick();
    }

    void run(uint64_t n) {
        for (uint64_t i = 0; i < n; ++i) tick();
    }
};

#endif // TB_COMMON_H
