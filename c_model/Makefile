CC      ?= clang
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror -std=c11 -g
LDFLAGS ?=
LIBJPEG_CFLAGS := $(shell pkg-config --cflags libjpeg 2>/dev/null)
LIBJPEG_LIBS   := $(shell pkg-config --libs libjpeg 2>/dev/null)

SRC_DIR   := src
TEST_DIR  := tests
GOLDEN_DIR:= golden
BUILD_DIR := build

CORE_SRCS := $(SRC_DIR)/bitstream.c $(SRC_DIR)/header_parser.c \
             $(SRC_DIR)/huffman.c $(SRC_DIR)/dequant.c $(SRC_DIR)/idct.c \
             $(SRC_DIR)/chroma.c $(SRC_DIR)/decoder.c
CORE_OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(CORE_SRCS))

MAIN_SRC := $(SRC_DIR)/main.c
MAIN_OBJ := $(BUILD_DIR)/main.o

TEST_SRCS := $(wildcard $(TEST_DIR)/test_*.c)
TEST_BINS := $(patsubst $(TEST_DIR)/test_%.c,$(BUILD_DIR)/test_%,$(TEST_SRCS))

GOLDEN_SRC := $(GOLDEN_DIR)/golden_compare.c
GOLDEN_BIN := $(BUILD_DIR)/golden_compare

.PHONY: all test golden phase16 phase17 phase18 phase_prog_dri phase25 phase25b clean
all: $(BUILD_DIR)/jpeg_decode $(GOLDEN_BIN)

VECTORS_ROOT ?= ../verification/vectors
PHASE16_DIR  := $(VECTORS_ROOT)/phase16
PHASE17_DIR  := $(VECTORS_ROOT)/phase17
PHASE18_DIR  := $(VECTORS_ROOT)/phase18
PHASE_PROG_DRI_DIR := $(VECTORS_ROOT)/phase_prog_dri
PHASE25_DIR  := $(VECTORS_ROOT)/phase25
PHASE25B_DIR := $(VECTORS_ROOT)/phase25b

phase16: $(GOLDEN_BIN)
	@if ls $(PHASE16_DIR)/*.jpg >/dev/null 2>&1; then \
	  $(GOLDEN_BIN) $(PHASE16_DIR)/*.jpg; \
	else \
	  echo "[phase16] no vectors in $(PHASE16_DIR) — run tools/gen_phase16.py" >&2; \
	  exit 2; \
	fi

phase17: $(GOLDEN_BIN)
	@if ls $(PHASE17_DIR)/*.jpg >/dev/null 2>&1; then \
	  $(GOLDEN_BIN) $(PHASE17_DIR)/*.jpg; \
	else \
	  echo "[phase17] no vectors in $(PHASE17_DIR) — run tools/gen_phase17.py" >&2; \
	  exit 2; \
	fi

phase18: $(GOLDEN_BIN)
	@if ls $(PHASE18_DIR)/*.jpg >/dev/null 2>&1; then \
	  $(GOLDEN_BIN) $(PHASE18_DIR)/*.jpg; \
	else \
	  echo "[phase18] no vectors in $(PHASE18_DIR) — run tools/gen_phase18.py" >&2; \
	  exit 2; \
	fi

phase_prog_dri: $(GOLDEN_BIN)
	@if ls $(PHASE_PROG_DRI_DIR)/*.jpg >/dev/null 2>&1; then \
	  $(GOLDEN_BIN) $(PHASE_PROG_DRI_DIR)/*.jpg; \
	else \
	  echo "[phase_prog_dri] no vectors in $(PHASE_PROG_DRI_DIR) — run tools/gen_phase_prog_dri.py" >&2; \
	  exit 2; \
	fi

phase25: $(GOLDEN_BIN)
	@if ls $(PHASE25_DIR)/*.jpg >/dev/null 2>&1; then \
	  $(GOLDEN_BIN) $(PHASE25_DIR)/*.jpg; \
	else \
	  echo "[phase25] no vectors in $(PHASE25_DIR) — run tools/gen_phase25.py" >&2; \
	  exit 2; \
	fi

phase25b: $(GOLDEN_BIN)
	@if ls $(PHASE25B_DIR)/*.jpg >/dev/null 2>&1; then \
	  $(GOLDEN_BIN) $(PHASE25B_DIR)/*.jpg; \
	else \
	  echo "[phase25b] no vectors in $(PHASE25B_DIR) — run tools/gen_phase25b.py" >&2; \
	  exit 2; \
	fi

$(BUILD_DIR):
	@mkdir -p $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(SRC_DIR) -c $< -o $@

$(BUILD_DIR)/jpeg_decode: $(CORE_OBJS) $(MAIN_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR)/test_%: $(TEST_DIR)/test_%.c $(CORE_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(SRC_DIR) $^ -o $@ $(LDFLAGS)

$(GOLDEN_BIN): $(GOLDEN_SRC) $(CORE_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LIBJPEG_CFLAGS) -I$(SRC_DIR) $^ -o $@ $(LDFLAGS) $(LIBJPEG_LIBS)

test: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do echo "== RUN $$t =="; $$t; done; echo "ALL TESTS PASSED"

golden: $(GOLDEN_BIN)

clean:
	rm -rf $(BUILD_DIR)
