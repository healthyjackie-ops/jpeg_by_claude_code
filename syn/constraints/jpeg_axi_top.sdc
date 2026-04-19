#==============================================================================
# SDC constraints for jpeg_axi_top — ASAP7 7.5T RVT TT, 600 MHz target
#
#   Clock:       aclk, period = 1.667 ns (600 MHz)
#   I/O delay:   30 % of period (0.5 ns) on both input and output sides
#   Reset:       aresetn — async, false path
#==============================================================================

# ---- Clock ------------------------------------------------------------------
create_clock -name aclk -period 1.667 [get_ports aclk]
set_clock_uncertainty 0.10 [get_clocks aclk]
set_clock_transition   0.05 [get_clocks aclk]

# ---- I/O delay --------------------------------------------------------------
set IO_DLY 0.500

# CSR AXI4-Lite (inputs)
set_input_delay  -clock aclk $IO_DLY \
    [get_ports {csr_awaddr[*] csr_awvalid csr_wdata[*] csr_wstrb[*] csr_wvalid \
                csr_bready csr_araddr[*] csr_arvalid csr_rready}]
# CSR AXI4-Lite (outputs)
set_output_delay -clock aclk $IO_DLY \
    [get_ports {csr_awready csr_wready csr_bresp[*] csr_bvalid \
                csr_arready csr_rdata[*] csr_rresp[*] csr_rvalid}]

# AXIS bitstream in
set_input_delay  -clock aclk $IO_DLY \
    [get_ports {s_bs_tdata[*] s_bs_tvalid s_bs_tlast}]
set_output_delay -clock aclk $IO_DLY [get_ports s_bs_tready]

# AXIS pixel out
set_input_delay  -clock aclk $IO_DLY [get_ports m_px_tready]
set_output_delay -clock aclk $IO_DLY \
    [get_ports {m_px_tdata[*] m_px_tvalid m_px_tuser m_px_tlast}]

# IRQ
set_output_delay -clock aclk $IO_DLY [get_ports irq]

# ---- Async reset: false path ------------------------------------------------
set_false_path -from [get_ports aresetn]
