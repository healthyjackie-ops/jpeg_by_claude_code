// ---------------------------------------------------------------------------
// bfm.h — AXI BFMs for jpeg_axi_top testbench
//
// Provides:
//   * reg_t        : enum of CSR offsets (matches docs/regmap.md)
//   * AxiLiteBfm   : blocking write32 / read32 with full AW/W/B + AR/R handshake
//   * ByteStreamDriver : push bytes into s_bs_* with tlast on the last byte
//   * PixelSink    : drain m_px_* into {Y, Cb, Cr} planes and record geometry
// ---------------------------------------------------------------------------
#ifndef BFM_H
#define BFM_H

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "tb_common.h"

// CSR offsets ---------------------------------------------------------------
enum : uint32_t {
    REG_ID          = 0x000,
    REG_CTRL        = 0x004,
    REG_STATUS      = 0x008,
    REG_INT_EN      = 0x00C,
    REG_INT_STATUS  = 0x010,
    REG_IMG_WIDTH   = 0x014,
    REG_IMG_HEIGHT  = 0x018,
    REG_PIXEL_COUNT = 0x01C,
    REG_ERROR_CODE  = 0x020,
    REG_CONFIG      = 0x024,
    REG_SCRATCH     = 0x028,
};

enum : uint32_t {
    CTRL_START      = 1u << 0,
    CTRL_SOFT_RESET = 1u << 1,
    CTRL_ABORT      = 1u << 2,
};

enum : uint32_t {
    STATUS_BUSY        = 1u << 0,
    STATUS_FRAME_DONE  = 1u << 1,
    STATUS_ERROR       = 1u << 2,
    STATUS_INPUT_EMPTY = 1u << 3,
    STATUS_OUTPUT_FULL = 1u << 4,
    STATUS_HEADER_DONE = 1u << 5,
};

enum : uint32_t {
    IS_DONE   = 1u << 0,
    IS_ERROR  = 1u << 1,
    IS_HEADER = 1u << 2,
};

// ---------------------------------------------------------------------------
// AXI-Lite BFM
// ---------------------------------------------------------------------------
class AxiLiteBfm {
public:
    explicit AxiLiteBfm(TbCtx* tb) : tb_(tb) {}

    // write32: returns BRESP (0=OKAY, 2=SLVERR). wstrb defaults to full strobe.
    uint32_t write32(uint32_t addr, uint32_t data, uint32_t wstrb = 0xF,
                     uint32_t timeout = 256) {
        auto* d = tb_->dut.get();
        d->csr_awaddr = addr & 0xFFF;
        d->csr_awvalid = 1;
        d->csr_wdata = data;
        d->csr_wstrb = wstrb & 0xF;
        d->csr_wvalid = 1;
        d->csr_bready = 1;

        bool aw_done = false, w_done = false;
        for (uint32_t i = 0; i < timeout && !(aw_done && w_done); ++i) {
            tb_->tick();
            if (d->csr_awready && d->csr_awvalid && !aw_done) { aw_done = true; d->csr_awvalid = 0; }
            if (d->csr_wready  && d->csr_wvalid  && !w_done)  { w_done  = true; d->csr_wvalid  = 0; }
        }
        if (!(aw_done && w_done)) die("write32 AW/W timeout", addr);

        uint32_t bresp = 0;
        bool b_done = false;
        for (uint32_t i = 0; i < timeout && !b_done; ++i) {
            if (d->csr_bvalid && d->csr_bready) { bresp = d->csr_bresp; b_done = true; }
            tb_->tick();
        }
        if (!b_done) die("write32 B timeout", addr);
        d->csr_bready = 1;
        return bresp;
    }

    uint32_t read32(uint32_t addr, uint32_t* rresp = nullptr, uint32_t timeout = 256) {
        auto* d = tb_->dut.get();
        d->csr_araddr = addr & 0xFFF;
        d->csr_arvalid = 1;
        d->csr_rready = 1;

        bool ar_done = false;
        for (uint32_t i = 0; i < timeout && !ar_done; ++i) {
            tb_->tick();
            if (d->csr_arready && d->csr_arvalid) { ar_done = true; d->csr_arvalid = 0; }
        }
        if (!ar_done) die("read32 AR timeout", addr);

        uint32_t rdata = 0, resp = 0;
        bool r_done = false;
        for (uint32_t i = 0; i < timeout && !r_done; ++i) {
            if (d->csr_rvalid && d->csr_rready) {
                rdata = d->csr_rdata; resp = d->csr_rresp; r_done = true;
            }
            tb_->tick();
        }
        if (!r_done) die("read32 R timeout", addr);
        if (rresp) *rresp = resp;
        return rdata;
    }

    // Poll a status bit with mask & expected value; returns cycles waited or -1 on timeout
    long wait_status(uint32_t mask, uint32_t val, uint32_t max_cycles = 1'000'000) {
        for (uint32_t i = 0; i < max_cycles; ++i) {
            uint32_t s = read32(REG_STATUS);
            if ((s & mask) == (val & mask)) return static_cast<long>(i);
        }
        return -1;
    }

private:
    TbCtx* tb_;
    [[noreturn]] static void die(const char* what, uint32_t addr) {
        std::fprintf(stderr, "[AXI-Lite BFM] %s (addr=0x%03X)\n", what, addr);
        std::fflush(stderr);
        _exit(3);
    }
};

// ---------------------------------------------------------------------------
// Byte-stream driver (s_bs_*)
// ---------------------------------------------------------------------------
class ByteStreamDriver {
public:
    explicit ByteStreamDriver(TbCtx* tb) : tb_(tb) {}

    // Push entire buffer; asserts tlast on last byte. Single-beat handshake.
    // Returns number of bytes accepted.
    size_t push(const uint8_t* data, size_t n, uint32_t max_cycles = 4'000'000) {
        auto* d = tb_->dut.get();
        size_t i = 0;
        uint32_t waited = 0;
        while (i < n) {
            d->s_bs_tdata  = data[i];
            d->s_bs_tvalid = 1;
            d->s_bs_tlast  = (i == n - 1) ? 1 : 0;
            // wait for tready
            while (!d->s_bs_tready) {
                tb_->tick();
                if (++waited > max_cycles) return i;
            }
            // handshake fires on the posedge — advance a tick
            tb_->tick();
            ++i;
        }
        d->s_bs_tvalid = 0;
        d->s_bs_tlast  = 0;
        return i;
    }

private:
    TbCtx* tb_;
};

// ---------------------------------------------------------------------------
// Pixel sink (m_px_*)
//
// Phase 12: tdata 扩宽到 32b。
//   YCbCr/GRAY: {Y[31:24], Cb[23:16], Cr[15:8], 0[7:0]}
//   CMYK:        {C[31:24], M[23:16], Y[15:8], K[7:0]}
// Sink 始终存储 4 个 plane (Y/Cb/Cr 与 K)。K 在非 CMYK 帧中是 0。
// First pixel with tuser marks SOF; each EOL bumps a row counter.
// ---------------------------------------------------------------------------
class PixelSink {
public:
    std::vector<uint8_t> Y, Cb, Cr, K;
    uint32_t width = 0;   // inferred from first EOL
    uint32_t height = 0;  // number of EOL pulses
    bool saw_sof = false;

    explicit PixelSink(TbCtx* tb) : tb_(tb) {
        tb_->dut->m_px_tready = 1;
    }

    // Pump the DUT for up to max_cycles, stopping when we observe tlast + EOL
    // on the last pixel of the frame or when CSR status.frame_done is asserted
    // by the caller (external termination).  Returns total cycles consumed.
    // n_expected=0 means "drain until caller-set stop flag".
    uint32_t drain(size_t n_expected = 0, uint32_t max_cycles = 4'000'000) {
        auto* d = tb_->dut.get();
        uint32_t cur_row_len = 0;
        for (uint32_t c = 0; c < max_cycles; ++c) {
            if (d->m_px_tvalid && d->m_px_tready) {
                uint32_t px = d->m_px_tdata;
                uint8_t y  = (px >> 24) & 0xFF;
                uint8_t cb = (px >> 16) & 0xFF;
                uint8_t cr = (px >> 8)  & 0xFF;
                uint8_t k  = (px >> 0)  & 0xFF;
                Y.push_back(y); Cb.push_back(cb); Cr.push_back(cr); K.push_back(k);
                if (d->m_px_tuser) saw_sof = true;
                ++cur_row_len;
                if (d->m_px_tlast) {
                    if (width == 0) width = cur_row_len;
                    ++height;
                    cur_row_len = 0;
                }
                if (n_expected && Y.size() >= n_expected) { tb_->tick(); return c; }
            }
            tb_->tick();
        }
        return max_cycles;
    }

private:
    TbCtx* tb_;
};

#endif // BFM_H
