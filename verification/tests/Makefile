# ---------------------------------------------------------------------------
# Verilator C++ testbench Makefile for jpeg_axi_top
#
# Targets:
#   make            -> build obj_dir/Vjpeg_axi_top
#   make run        -> build + run a sanity simulation (reset + idle)
#   make diff       -> build + run differential test harness vs C model
#   make unit       -> build + run module-level unit tests
#   make clean      -> remove obj_dir & build artifacts
# ---------------------------------------------------------------------------

# Paths ---------------------------------------------------------------------
REPO       := $(abspath ../..)
RTL_DIR    := $(REPO)/rtl/src
INC_DIR    := $(REPO)/rtl/include
CMODEL_DIR := $(REPO)/c_model
CMODEL_BUILD := $(CMODEL_DIR)/build

# Top + sources -------------------------------------------------------------
TOP        := jpeg_axi_top
RTL_SRCS   := $(wildcard $(RTL_DIR)/*.v)

# C model objects to link as golden reference -------------------------------
CMODEL_OBJS := $(CMODEL_BUILD)/bitstream.o \
               $(CMODEL_BUILD)/header_parser.o \
               $(CMODEL_BUILD)/huffman.o \
               $(CMODEL_BUILD)/dequant.o \
               $(CMODEL_BUILD)/idct.o \
               $(CMODEL_BUILD)/chroma.o \
               $(CMODEL_BUILD)/decoder.o

# Verilator flags -----------------------------------------------------------
VERILATOR ?= verilator
VLT_DEFS  ?=
VLT_FLAGS := --cc --exe --build --trace --trace-depth 99 \
             --public-flat-rw \
             -Wall -Wno-PINCONNECTEMPTY -Wno-fatal \
             -I$(INC_DIR) -y $(RTL_DIR) \
             -O3 --x-assign fast --x-initial fast \
             --threads 1 \
             --top-module $(TOP) \
             $(VLT_DEFS) \
             -CFLAGS "-O2 -std=c++17 -I$(CMODEL_DIR)/src"

one: obj_dir/V$(TOP)
	./obj_dir/V$(TOP) --mode=one --dir=$(REPO)/verification/vectors/smoke/grad_16x16_q80.jpg --vcd=trace.vcd

# Targets -------------------------------------------------------------------
.PHONY: all run csr diff unit one clean cmodel

all: obj_dir/V$(TOP)

# Ensure C model is built (objects needed for linking)
cmodel:
	@$(MAKE) -C $(CMODEL_DIR) all

obj_dir/V$(TOP): $(RTL_SRCS) sim_main.cpp tb_common.h cmodel
	$(VERILATOR) $(VLT_FLAGS) \
	    $(RTL_DIR)/$(TOP).v sim_main.cpp \
	    -LDFLAGS "$(CMODEL_OBJS)" \
	    -o V$(TOP)

run: obj_dir/V$(TOP)
	./obj_dir/V$(TOP) --mode=idle

csr: obj_dir/V$(TOP)
	./obj_dir/V$(TOP) --mode=csr

diff: obj_dir/V$(TOP)
	./obj_dir/V$(TOP) --mode=diff --dir=$(REPO)/verification/vectors/smoke

diff-full: obj_dir/V$(TOP)
	./obj_dir/V$(TOP) --mode=diff --dir=$(REPO)/verification/vectors/full

unit: obj_dir/V$(TOP)
	./obj_dir/V$(TOP) --mode=unit

errout: obj_dir/V$(TOP)
	./obj_dir/V$(TOP) --mode=errout --dir=$(REPO)/verification/vectors/phase14

clean:
	rm -rf obj_dir *.vcd *.log
