// ---------------------------------------------------------------------------
// sim_main.cpp — Verilator simulation entry for jpeg_axi_top
//
// Phase 3 skeleton: boot the DUT, apply reset, prove clocking + signal
// observability.  Real test modes (diff / unit) are stubs and will be
// fleshed out in subsequent Phase 3 tasks (BFMs, differential harness,
// unit tests).
// ---------------------------------------------------------------------------
#include "tb_common.h"
#include "bfm.h"
#include "Vjpeg_axi_top___024root.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include "decoder.h"
}

namespace {

enum class Mode { Idle, Csr, Diff, One, Unit };

struct Args {
    Mode mode = Mode::Idle;
    std::string dir;
    std::string vcd;
    std::string out;         // --out=<path.ppm> for One mode
    uint32_t start = 0;
    uint32_t count = 0;      // 0 = all
};

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s.rfind("--mode=", 0) == 0) {
            std::string m = s.substr(7);
            if      (m == "idle") a.mode = Mode::Idle;
            else if (m == "csr")  a.mode = Mode::Csr;
            else if (m == "diff") a.mode = Mode::Diff;
            else if (m == "one")  a.mode = Mode::One;
            else if (m == "unit") a.mode = Mode::Unit;
            else { std::fprintf(stderr, "unknown mode: %s\n", m.c_str()); std::exit(1); }
        } else if (s.rfind("--dir=", 0) == 0) {
            a.dir = s.substr(6);
        } else if (s.rfind("--vcd=", 0) == 0) {
            a.vcd = s.substr(6);
        } else if (s.rfind("--start=", 0) == 0) {
            a.start = static_cast<uint32_t>(std::atoi(s.substr(8).c_str()));
        } else if (s.rfind("--count=", 0) == 0) {
            a.count = static_cast<uint32_t>(std::atoi(s.substr(8).c_str()));
        } else if (s.rfind("--out=", 0) == 0) {
            a.out = s.substr(6);
        }
    }
    return a;
}

int run_idle() {
    TbCtx* tb = new TbCtx();           // leak-on-purpose: we _exit before dtor
    tb->reset(10);
    tb->run(200);

    // Sanity: ID read from CSR 0x000 (AXI-Lite AR handshake stub)
    // Real AXI-Lite BFM comes next task; this just pokes the handshake
    // and confirms the DUT responds with its ID magic.
    tb->dut->csr_araddr  = 0x000;
    tb->dut->csr_arvalid = 1;
    tb->dut->csr_rready  = 1;
    for (int i = 0; i < 16 && !tb->dut->csr_arready; ++i) tb->tick();
    tb->dut->csr_arvalid = 0;
    for (int i = 0; i < 16 && !tb->dut->csr_rvalid; ++i) tb->tick();

    if (!tb->dut->csr_rvalid) {
        std::fprintf(stderr, "[TB] IDLE: no RVALID — CSR AR handshake broken\n");
        return 1;
    }
    uint32_t id = tb->dut->csr_rdata;
    std::printf("[TB] IDLE OK: ID=0x%08X (expected 0xA17C0110), cycles=%llu\n",
                id, (unsigned long long)tb->cycle);
    return (id == 0xA17C0110u) ? 0 : 1;
}

int run_csr() {
    TbCtx* tb = new TbCtx();
    tb->reset(10);
    AxiLiteBfm csr(tb);

    int fails = 0;
    auto expect_eq = [&](const char* what, uint32_t got, uint32_t want) {
        if (got != want) {
            std::fprintf(stderr, "[CSR] FAIL %s: got 0x%08X want 0x%08X\n",
                         what, got, want);
            ++fails;
        } else {
            std::printf("[CSR] OK   %s = 0x%08X\n", what, got);
        }
    };

    // 1. ID
    expect_eq("ID",        csr.read32(REG_ID),        0xA17C0110u);

    // 2. SOFT_RESET → BUSY=0 after reset
    csr.write32(REG_CTRL, CTRL_SOFT_RESET);
    long w = csr.wait_status(STATUS_BUSY, 0, 128);
    if (w < 0) { std::fprintf(stderr, "[CSR] FAIL wait busy=0\n"); ++fails; }
    else       { std::printf("[CSR] OK   busy=0 after SOFT_RESET (waited %ld)\n", w); }

    // 3. SCRATCH readback
    csr.write32(REG_SCRATCH, 0xDEADBEEF);
    expect_eq("SCRATCH",   csr.read32(REG_SCRATCH),   0xDEADBEEFu);

    // 4. INT_EN readback (IE_DONE | IE_ERROR)
    csr.write32(REG_INT_EN, 0x3);
    expect_eq("INT_EN",    csr.read32(REG_INT_EN),    0x3u);

    // 5. CONFIG.OUT_FMT=1 → SLVERR
    uint32_t bresp = csr.write32(REG_CONFIG, 0x1);
    if (bresp != 0x2u) {
        std::fprintf(stderr, "[CSR] FAIL OUT_FMT=1 BRESP=0x%X (expected 0x2 SLVERR)\n", bresp);
        ++fails;
    } else {
        std::printf("[CSR] OK   OUT_FMT=1 returns SLVERR\n");
    }

    // 6. Unaligned wstrb → SLVERR on SCRATCH
    bresp = csr.write32(REG_SCRATCH, 0x12345678, 0x3);
    if (bresp != 0x2u) {
        std::fprintf(stderr, "[CSR] FAIL wstrb=0x3 BRESP=0x%X (expected 0x2)\n", bresp);
        ++fails;
    } else {
        std::printf("[CSR] OK   wstrb=0x3 returns SLVERR\n");
    }

    // 7. Legal CONFIG=0
    bresp = csr.write32(REG_CONFIG, 0x0);
    if (bresp != 0) { std::fprintf(stderr, "[CSR] FAIL CONFIG=0 BRESP=0x%X\n", bresp); ++fails; }

    std::printf("[CSR] %s (fails=%d)\n", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}

// ---- File helpers --------------------------------------------------------
static std::vector<uint8_t> slurp(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); _exit(4); }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> v(sz);
    size_t r = std::fread(v.data(), 1, sz, f);
    std::fclose(f);
    if (static_cast<long>(r) != sz) { std::fprintf(stderr, "short read %s\n", path.c_str()); _exit(4); }
    return v;
}

static std::vector<std::string> list_jpegs(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = ::opendir(dir.c_str());
    if (!d) { std::fprintf(stderr, "no dir %s\n", dir.c_str()); return out; }
    for (struct dirent* e; (e = ::readdir(d));) {
        std::string n(e->d_name);
        if (n.size() > 4 && (n.substr(n.size() - 4) == ".jpg" ||
                             n.substr(n.size() - 5) == ".jpeg")) {
            out.push_back(dir + "/" + n);
        }
    }
    ::closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

// ---- Differential test for ONE JPEG --------------------------------------
struct DiffResult {
    bool rtl_ok = false;
    bool c_ok = false;
    bool match_geom = false;
    uint32_t max_diff_y = 0, max_diff_c = 0;
    uint32_t n_px = 0;
    uint16_t width = 0, height = 0;
    uint32_t err_code = 0;
};

static DiffResult diff_one(const std::vector<uint8_t>& jpeg,
                           bool verbose = false, uint32_t max_sim_cycles = 2'000'000,
                           const char* vcd_path = nullptr,
                           std::vector<uint8_t>* out_Y  = nullptr,
                           std::vector<uint8_t>* out_Cb = nullptr,
                           std::vector<uint8_t>* out_Cr = nullptr,
                           std::vector<uint8_t>* out_K  = nullptr) {
    DiffResult R;

    // --- Golden C decode --------------------------------------------------
    jpeg_decoded_t golden{};
    int rc = jpeg_decode(jpeg.data(), jpeg.size(), &golden);
    R.c_ok = (rc == 0);
    if (!R.c_ok) { if (verbose) std::printf("  C FAIL rc=%d err=0x%X\n", rc, golden.err); return R; }

    // If caller didn't size the cycle budget, compute it from golden pixel
    // count (~30 cycles/pixel + 500K startup, capped at 200M).
    if (max_sim_cycles == 0) {
        uint64_t need = uint64_t(golden.width) * golden.height * 30ULL + 500'000ULL;
        if (need < 2'000'000ULL)   need = 2'000'000ULL;
        if (need > 500'000'000ULL) need = 500'000'000ULL;
        max_sim_cycles = static_cast<uint32_t>(need);
    }

    // --- RTL decode via BFMs ---------------------------------------------
    TbCtx* tb = new TbCtx(vcd_path);
    tb->timeout_cycles = static_cast<uint64_t>(max_sim_cycles) + 256;
    tb->reset(12);
    AxiLiteBfm csr(tb);

    // Clear any prior state
    csr.write32(REG_CTRL, CTRL_SOFT_RESET);
    csr.wait_status(STATUS_BUSY, 0, 128);
    // START
    csr.write32(REG_CTRL, CTRL_START);
    uint32_t st0 = csr.read32(REG_STATUS);
    if (verbose) std::printf("  after START status=0x%X\n", st0);

    // --- Unified pump ----------------------------------------------------
    auto* d = tb->dut.get();
    d->m_px_tready = 1;

    std::vector<uint8_t> rY, rCb, rCr, rK;
    rY.reserve(64 * 64); rCb.reserve(64 * 64); rCr.reserve(64 * 64); rK.reserve(64 * 64);
    uint32_t cur_row_len = 0;
    uint16_t rtl_w = 0;
    uint32_t rtl_rows = 0;
    bool saw_tuser = false;
    bool dumped_htab = false;

    // Byte feeder + pixel capture via on_tick_pre so both run on EVERY tick
    // (including BFM-internal ticks during CSR transactions). Previously the
    // byte feeder was in the main loop, which left s_bs_tdata/tvalid stale
    // during csr.read32's ticks — the FIFO then accepted the same byte twice
    // when it had free space, corrupting the bitstream.
    size_t bi = 0;
    bool pending_bi_adv = false;
    tb->on_tick_pre = [&](TbCtx& t) {
        auto* dd = t.dut.get();
        // 1) pixel capture (pre-edge handshake fires at this tick)
        if (dd->m_px_tvalid && dd->m_px_tready) {
            uint32_t v = dd->m_px_tdata;
            rY.push_back((v >> 24) & 0xFF);
            rCb.push_back((v >> 16) & 0xFF);
            rCr.push_back((v >> 8)  & 0xFF);
            rK.push_back((v >> 0)  & 0xFF);
            if (dd->m_px_tuser) saw_tuser = true;
            ++cur_row_len;
            if (dd->m_px_tlast) {
                if (rtl_w == 0) rtl_w = static_cast<uint16_t>(cur_row_len);
                cur_row_len = 0;
                ++rtl_rows;
            }
        }
        // 2) commit previous tick's s_bs handshake
        if (pending_bi_adv) ++bi;
        pending_bi_adv = false;
        // 3) drive this tick's bytestream input
        if (bi < jpeg.size()) {
            dd->s_bs_tdata  = jpeg[bi];
            dd->s_bs_tvalid = 1;
            dd->s_bs_tlast  = (bi == jpeg.size() - 1);
        } else {
            dd->s_bs_tvalid = 0;
            dd->s_bs_tlast  = 0;
        }
        // 4) predict this tick's handshake (tready reflects pre-edge FIFO state)
        pending_bi_adv = (dd->s_bs_tvalid && dd->s_bs_tready);
    };

    for (uint32_t c = 0; c < max_sim_cycles; ++c) {

        // One-shot HTable dump + cycle-by-cycle huffman state while in header-done era
        if (verbose && !dumped_htab) {
            auto* r = d->rootp;
            uint32_t huf_st = r->jpeg_axi_top__DOT__u_huf__DOT__st;
            uint32_t seq_st = r->jpeg_axi_top__DOT__u_seq__DOT__st;
            if (huf_st == 1 /*S_WDC*/ && seq_st == 2 /*S_WAIT_HUF*/) {
                uint32_t shreg = r->jpeg_axi_top__DOT__u_bs__DOT__shreg;
                uint32_t bc    = r->jpeg_axi_top__DOT__u_bs__DOT__bit_cnt;
                std::printf("  === HTABLE dump (at first S_WDC, c=%u) ===\n", c);
                std::printf("  shreg=0x%08X bit_cnt=%u  (top 16b ~ peek_win)\n", shreg, bc);
                static const char* labels[4] = {"DC_0", "DC_1", "AC_0", "AC_1"};
                static const int   tbl_ids[4] = {0, 1, 4, 5};  // {ac,sel}
                // HTable address = {tbl[2:0], l[4:0]} = tbl*32 + l
                for (int t = 0; t < 4; ++t) {
                    std::printf("  -- %s (tbl=%d) --\n", labels[t], tbl_ids[t]);
                    for (int l = 1; l <= 16; ++l) {
                        int idx = (tbl_ids[t] << 5) | l;
                        uint32_t mc  = r->jpeg_axi_top__DOT__u_ht__DOT__maxcode_mem[idx];
                        uint32_t mnc = r->jpeg_axi_top__DOT__u_ht__DOT__mincode_mem[idx];
                        uint32_t vp  = r->jpeg_axi_top__DOT__u_ht__DOT__valptr_mem [idx];
                        uint32_t bv  = r->jpeg_axi_top__DOT__u_ht__DOT__bits_mem   [idx];
                        std::printf("    l=%2d BITS=%3u max=0x%05X min=0x%04X vp=%3u\n",
                                    l, bv, mc, mnc, vp);
                    }
                }
                std::puts("  === cycle-by-cycle trace (next 40 cycles) ===");
                dumped_htab = true;
            }
        }
        // Trace huffman state transitions (edge-triggered) and key moments.
        if (verbose && dumped_htab) {
            static uint32_t prev_st = 0xFFFF, prev_l = 0xFFFF;
            static uint32_t prev_seq_st = 0xFFFF;
            static int edge_cnt = 0;
            static uint32_t last_seq_blk = 0xFFFF;
            auto* r = d->rootp;
            uint32_t huf_st = r->jpeg_axi_top__DOT__u_huf__DOT__st;
            uint32_t huf_l  = r->jpeg_axi_top__DOT__u_huf__DOT__l;
            uint32_t huf_code_acc = r->jpeg_axi_top__DOT__u_huf__DOT__code_acc;
            uint32_t seq_blk = r->jpeg_axi_top__DOT__u_seq__DOT__blk_idx;
            uint32_t seq_st  = r->jpeg_axi_top__DOT__u_seq__DOT__st;
            bool blk_changed = (seq_blk != last_seq_blk);
            bool st_edge = (huf_st != prev_st) || (huf_l != prev_l);
            bool seq_edge = (seq_st != prev_seq_st);
            // Print when: block changes, or huf/seq state/l changes, up to a budget.
            if ((st_edge || blk_changed || seq_edge) && edge_cnt < 20000) {
                uint32_t bs_shreg = r->jpeg_axi_top__DOT__u_bs__DOT__shreg;
                uint32_t bs_bc    = r->jpeg_axi_top__DOT__u_bs__DOT__bit_cnt;
                uint32_t ht_rd_l  = r->jpeg_axi_top__DOT__u_huf__DOT__ht_rd_l;
                uint32_t huf_k    = r->jpeg_axi_top__DOT__u_huf__DOT__k;
                uint32_t huf_sym  = r->jpeg_axi_top__DOT__u_huf__DOT__sym;
                std::printf("  E%03d c=%u seq.st=%u blk=%u huf.st=%2u l=%2u k=%2u sym=0x%02X rd_l=%2u code_acc=0x%04X | shreg=0x%08X bc=%u\n",
                            edge_cnt, c, seq_st, seq_blk, huf_st, huf_l, huf_k, huf_sym,
                            ht_rd_l, huf_code_acc, bs_shreg, bs_bc);
                ++edge_cnt;
            }
            prev_st = huf_st;
            prev_l = huf_l;
            prev_seq_st = seq_st;
            last_seq_blk = seq_blk;

            // Byte-push trace: detect when bitstream_unpack accepts a byte
            // (byte_valid && byte_ready). byte_ready = byte_valid && can_accept.
            // can_accept = (bit_cnt <= 24 && !marker_detected).
            // Easier: watch the input-side FIFO's output handshake signals.
            auto* r2 = d->rootp;
            uint32_t bs_bc_now = r2->jpeg_axi_top__DOT__u_bs__DOT__bit_cnt;
            uint32_t bs_byte_valid = r2->jpeg_axi_top__DOT__u_bs__DOT__byte_valid;
            uint32_t bs_byte_in    = r2->jpeg_axi_top__DOT__u_bs__DOT__byte_in;
            uint32_t bs_marker_det = r2->jpeg_axi_top__DOT__u_bs__DOT__marker_detected;
            uint32_t bs_ff_wait    = r2->jpeg_axi_top__DOT__u_bs__DOT__ff_wait;
            bool bs_can_accept = (bs_bc_now <= 24) && !bs_marker_det;
            bool bs_push_fire  = bs_byte_valid && bs_can_accept;
            static int push_cnt = 0;
            if (bs_push_fire) {
                if (push_cnt < 1500) {
                    std::printf("  BYTE_PUSH #%d c=%u byte=0x%02X bc=%u ff_wait=%u\n",
                                push_cnt, c, bs_byte_in, bs_bc_now, bs_ff_wait);
                }
                ++push_cnt;
            }
        }

        tb->tick();

        // stop when frame done or error observed
        if ((c & 0x3FF) == 0x3FF) {   // every 1024 cycles
            uint32_t s = csr.read32(REG_STATUS);
            if (verbose && (c & 0xFFF) == 0xFFF) {   // every 4K cycles
                auto* r = d->rootp;
                uint32_t hp_state = r->jpeg_axi_top__DOT__u_hp__DOT__state;
                uint32_t hp_mark  = r->jpeg_axi_top__DOT__u_hp__DOT__last_marker;
                uint32_t bs_bc    = r->jpeg_axi_top__DOT__u_bs__DOT__bit_cnt;
                uint32_t seq_st   = r->jpeg_axi_top__DOT__u_seq__DOT__st;
                uint32_t seq_blk  = r->jpeg_axi_top__DOT__u_seq__DOT__blk_idx;
                uint32_t huf_st   = r->jpeg_axi_top__DOT__u_huf__DOT__st;
                uint32_t huf_l    = r->jpeg_axi_top__DOT__u_huf__DOT__l;
                std::printf("  c=%u st=0x%X err=0x%X bi=%zu/%zu n=%zu | hp.st=%u mk=0x%X bs.bc=%u seq.st=%u blk=%u huf.st=%u l=%u\n",
                            c, s, csr.read32(REG_ERROR_CODE), bi, jpeg.size(), rY.size(),
                            hp_state, hp_mark, bs_bc, seq_st, seq_blk, huf_st, huf_l);
            }
            if (s & STATUS_ERROR) break;
            if ((s & STATUS_FRAME_DONE) && !(s & STATUS_BUSY)) {
                for (int k = 0; k < 64; ++k) tb->tick();
                break;
            }
        }
    }

    uint32_t err = csr.read32(REG_ERROR_CODE);
    uint32_t status = csr.read32(REG_STATUS);
    uint32_t w_reg  = csr.read32(REG_IMG_WIDTH)  & 0xFFFF;
    uint32_t h_reg  = csr.read32(REG_IMG_HEIGHT) & 0xFFFF;
    R.err_code = err;
    R.n_px = static_cast<uint32_t>(rY.size());
    R.width  = static_cast<uint16_t>(rtl_w ? rtl_w : w_reg);
    R.height = static_cast<uint16_t>(rtl_rows ? rtl_rows : h_reg);

    if (verbose) {
        auto* r = tb->dut.get()->rootp;
        uint32_t seq_st  = r->jpeg_axi_top__DOT__u_seq__DOT__st;
        uint32_t huf_st  = r->jpeg_axi_top__DOT__u_huf__DOT__st;
        uint32_t hp_st   = r->jpeg_axi_top__DOT__u_hp__DOT__state;
        std::printf("  END state=0x%X err=0x%X nY=%zu saw_tuser=%d w_reg=%u h_reg=%u seq.st=%u huf.st=%u hp.st=%u\n",
                    status, err, rY.size(), saw_tuser, w_reg, h_reg, seq_st, huf_st, hp_st);
    }
    if (err != 0) {
        if (verbose) std::printf("  RTL ERR=0x%X status=0x%X\n", err, status);
    }
    if (!saw_tuser || rY.empty()) {
        if (verbose) std::printf("  RTL: no pixels (tuser=%d, n=%zu)\n", saw_tuser, rY.size());
        jpeg_free(&golden);
        return R;
    }

    // Geometry compare
    R.match_geom = (w_reg == golden.width) && (h_reg == golden.height)
                   && (rY.size() == size_t(golden.width) * golden.height);

    // Pixel compare
    //   CMYK  (golden.c_plane != null): rY=C rCb=M rCr=Y_cmyk rK=K
    //   Gray  (golden.cb_plane == null): rCb/rCr are 0x80 padding, rK=0
    //   YCbCr (else): rY/rCb/rCr from golden
    const bool is_cmyk = (golden.c_plane != nullptr);
    const bool is_gray = (!is_cmyk && golden.cb_plane == nullptr);
    if (R.match_geom) {
        for (size_t i = 0; i < rY.size(); ++i) {
            if (is_cmyk) {
                uint32_t dc_c = std::abs(int(rY[i])  - int(golden.c_plane[i]));
                uint32_t dc_m = std::abs(int(rCb[i]) - int(golden.m_plane[i]));
                uint32_t dc_y = std::abs(int(rCr[i]) - int(golden.y_plane_cmyk[i]));
                uint32_t dc_k = std::abs(int(rK[i])  - int(golden.k_plane[i]));
                // Reuse max_diff_y for C, max_diff_c for M/Y/K combined
                if (dc_c > R.max_diff_y) R.max_diff_y = dc_c;
                uint32_t dc = std::max(dc_m, std::max(dc_y, dc_k));
                if (dc > R.max_diff_c) R.max_diff_c = dc;
            } else {
                uint8_t gy  = golden.y_plane[i];
                uint32_t dy = std::abs(int(rY[i]) - int(gy));
                if (dy > R.max_diff_y) R.max_diff_y = dy;
                if (!is_gray) {
                    uint8_t gcb = golden.cb_plane[i];
                    uint8_t gcr = golden.cr_plane[i];
                    uint32_t dcb = std::abs(int(rCb[i]) - int(gcb));
                    uint32_t dcr = std::abs(int(rCr[i]) - int(gcr));
                    uint32_t dc = std::max(dcb, dcr);
                    if (dc > R.max_diff_c) R.max_diff_c = dc;
                } else {
                    // grayscale — RTL outputs Cb=Cr=0x80 padding
                    uint32_t dcb = std::abs(int(rCb[i]) - 0x80);
                    uint32_t dcr = std::abs(int(rCr[i]) - 0x80);
                    uint32_t dc = std::max(dcb, dcr);
                    if (dc > R.max_diff_c) R.max_diff_c = dc;
                }
            }
        }
    }

    R.rtl_ok = (err == 0);
    if (out_Y)  *out_Y  = std::move(rY);
    if (out_Cb) *out_Cb = std::move(rCb);
    if (out_Cr) *out_Cr = std::move(rCr);
    if (out_K)  *out_K  = std::move(rK);
    jpeg_free(&golden);
    // Leak TbCtx — cleaner than the mac libc++ thread teardown race.
    return R;
}

// Write YCbCr planes → PPM (P6 binary RGB) via JFIF YCbCr→RGB conversion.
static bool write_ppm_from_ycbcr(const std::string& path,
                                 const std::vector<uint8_t>& Y,
                                 const std::vector<uint8_t>& Cb,
                                 const std::vector<uint8_t>& Cr,
                                 uint16_t w, uint16_t h) {
    if (size_t(w) * h != Y.size() || Y.size() != Cb.size() || Y.size() != Cr.size()) {
        std::fprintf(stderr, "[PPM] geometry mismatch: w*h=%u Y=%zu Cb=%zu Cr=%zu\n",
                     unsigned(w) * h, Y.size(), Cb.size(), Cr.size());
        return false;
    }
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) { std::fprintf(stderr, "[PPM] open %s failed\n", path.c_str()); return false; }
    std::fprintf(fp, "P6\n%u %u\n255\n", unsigned(w), unsigned(h));
    std::vector<uint8_t> row(size_t(w) * 3);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            size_t i = size_t(y) * w + x;
            int yy = int(Y[i]);
            int cb = int(Cb[i]) - 128;
            int cr = int(Cr[i]) - 128;
            // JFIF integer approx (libjpeg identical): scale = 1<<16
            int r = yy + ((91881  * cr + 32768) >> 16);
            int g = yy - ((22554  * cb + 46802 * cr + 32768) >> 16);
            int b = yy + ((116130 * cb + 32768) >> 16);
            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;
            row[x * 3 + 0] = uint8_t(r);
            row[x * 3 + 1] = uint8_t(g);
            row[x * 3 + 2] = uint8_t(b);
        }
        std::fwrite(row.data(), 1, row.size(), fp);
    }
    std::fclose(fp);
    return true;
}

int run_one(const Args& a) {
    std::string path = a.dir;  // reuse --dir= flag as single file path
    if (path.empty()) { std::fprintf(stderr, "pass --dir=<file.jpg>\n"); return 2; }
    auto jpeg = slurp(path);
    std::printf("[ONE] %s (%zu bytes)\n", path.c_str(), jpeg.size());
    std::vector<uint8_t> oY, oCb, oCr;
    bool want_out = !a.out.empty();
    // When writing output, size cycle budget from image dimensions (pass 0).
    DiffResult r = diff_one(jpeg, /*verbose=*/true,
                            /*max_cycles=*/ want_out ? 0 : 2'000'000,
                            /*vcd=*/ a.vcd.empty() ? nullptr : a.vcd.c_str(),
                            want_out ? &oY  : nullptr,
                            want_out ? &oCb : nullptr,
                            want_out ? &oCr : nullptr);
    bool ok = r.rtl_ok && r.c_ok && r.match_geom
              && r.max_diff_y == 0 && r.max_diff_c == 0;
    std::printf("  %ux%u err=0x%02X ΔY=%u ΔC=%u -> %s\n",
                r.width, r.height, r.err_code, r.max_diff_y, r.max_diff_c,
                ok ? "OK" : "FAIL");
    if (want_out && !oY.empty() && r.match_geom) {
        if (write_ppm_from_ycbcr(a.out, oY, oCb, oCr, r.width, r.height))
            std::printf("  wrote PPM: %s (%ux%u)\n", a.out.c_str(), r.width, r.height);
    }
    return ok ? 0 : 1;
}

int run_diff(const Args& a) {
    std::string dir = a.dir.empty() ? "../vectors/smoke" : a.dir;
    auto files_all = list_jpegs(dir);
    if (files_all.empty()) {
        std::fprintf(stderr, "[DIFF] no JPEGs in %s (try: python3 tools/gen_vectors.py)\n",
                     dir.c_str());
        return 2;
    }
    // Slice: [start, start+count). count=0 means "to end".
    size_t lo = std::min<size_t>(a.start, files_all.size());
    size_t hi = a.count == 0 ? files_all.size()
                             : std::min<size_t>(files_all.size(), lo + a.count);
    std::vector<std::string> files(files_all.begin() + lo, files_all.begin() + hi);
    std::printf("[DIFF] %zu/%zu vectors from %s (range %zu..%zu)\n",
                files.size(), files_all.size(), dir.c_str(), lo, hi);

    // Abort after N failures unless JPEG_DIFF_FAIL_LIMIT overrides it.
    const char* fail_env = std::getenv("JPEG_DIFF_FAIL_LIMIT");
    int fail_limit = fail_env ? std::atoi(fail_env) : 3;

    int pass = 0, fail = 0;
    std::vector<std::string> fail_names;
    for (const auto& f : files) {
        auto jpeg = slurp(f);
        // Verbose mode on first JPEG so we can see startup behavior
        bool verbose = (pass + fail == 0);
        // diff_one sizes cycle budget from the golden decode's pixel count.
        DiffResult r = diff_one(jpeg, verbose, /*max_sim_cycles=*/0);
        bool ok = r.rtl_ok && r.c_ok && r.match_geom
                  && r.max_diff_y == 0 && r.max_diff_c == 0;
        std::printf("  %-40s  %ux%u  err=0x%02X  ΔY=%u ΔC=%u  %s\n",
                    f.substr(f.find_last_of('/') + 1).c_str(),
                    r.width, r.height, r.err_code, r.max_diff_y, r.max_diff_c,
                    ok ? "OK" : "FAIL");
        if (ok) ++pass;
        else { ++fail; fail_names.push_back(f); }
        if (fail_limit > 0 && fail >= fail_limit) {
            std::printf("[DIFF] aborting after %d failures (set JPEG_DIFF_FAIL_LIMIT=0 to disable)\n", fail);
            break;
        }
    }
    std::printf("[DIFF] %d/%zu passed\n", pass, files.size());
    if (!fail_names.empty()) {
        std::printf("[DIFF] first failures:\n");
        for (size_t i = 0; i < fail_names.size() && i < 20; ++i)
            std::printf("  FAIL %s\n", fail_names[i].c_str());
    }
    return fail ? 1 : 0;
}

int run_unit() {
    std::fprintf(stderr, "[TB] --mode=unit not implemented yet (BFMs pending)\n");
    return 77;
}

} // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Args a = parse_args(argc, argv);
    int rc = 0;
    switch (a.mode) {
        case Mode::Idle: rc = run_idle(); break;
        case Mode::Csr:  rc = run_csr();  break;
        case Mode::Diff: rc = run_diff(a); break;
        case Mode::One:  rc = run_one(a); break;
        case Mode::Unit: rc = run_unit(); break;
    }
    std::fflush(stdout);
    std::fflush(stderr);
    // Bypass global dtors (Verilator/macOS libc++ thread-pool teardown race)
    _exit(rc);
}
