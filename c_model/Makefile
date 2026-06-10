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
             $(SRC_DIR)/chroma.c $(SRC_DIR)/decoder.c $(SRC_DIR)/arith.c
CORE_OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(CORE_SRCS))

MAIN_SRC := $(SRC_DIR)/main.c
MAIN_OBJ := $(BUILD_DIR)/main.o

TEST_SRCS := $(wildcard $(TEST_DIR)/test_*.c)
TEST_BINS := $(patsubst $(TEST_DIR)/test_%.c,$(BUILD_DIR)/test_%,$(TEST_SRCS))

GOLDEN_SRC := $(GOLDEN_DIR)/golden_compare.c
GOLDEN_BIN := $(BUILD_DIR)/golden_compare

.PHONY: all test golden regress errtest clean
all: $(BUILD_DIR)/jpeg_decode $(GOLDEN_BIN)

VECTORS_ROOT ?= ../verification/vectors

# Per-phase golden run: `make phase17d`, `make phase_prog_dri`, ... works for
# any verification/vectors/phase* directory. (Pattern targets can't be .PHONY,
# but no file named phaseXX ever exists here, so the recipe always runs.)
phase%: $(GOLDEN_BIN)
	@if ls $(VECTORS_ROOT)/phase$*/*.jpg >/dev/null 2>&1; then \
	  $(GOLDEN_BIN) $(VECTORS_ROOT)/phase$*/*.jpg; \
	else \
	  echo "[phase$*] no vectors in $(VECTORS_ROOT)/phase$* — run tools/gen_phase$*.py" >&2; \
	  exit 2; \
	fi

# Full-corpus regression: every vector in every directory, except phase14
# (RTL error-out vectors, not golden-comparable) and error_cases (covered by
# errtest below). This is the number to quote in landing commits —
# hand-assembled per-phase lists silently rot.
regress: $(GOLDEN_BIN)
	@find $(VECTORS_ROOT) -name '*.jpg' -not -path '*phase14*' \
	     -not -path '*error_cases*' | sort | xargs $(GOLDEN_BIN)

# Error-path coverage: corrupted vectors must be rejected (rc != 0), and on
# macOS each rejection is also checked for leaks with the system leaks tool.
ERROR_DIR := $(VECTORS_ROOT)/error_cases
errtest: $(GOLDEN_BIN) $(BUILD_DIR)/jpeg_decode
	@if ls $(ERROR_DIR)/*.jpg >/dev/null 2>&1; then \
	  $(GOLDEN_BIN) --expect-fail $(ERROR_DIR)/*.jpg; \
	else \
	  echo "[errtest] no vectors in $(ERROR_DIR) — run tools/gen_error_cases.py" >&2; \
	  exit 2; \
	fi
	@if command -v leaks >/dev/null 2>&1; then \
	  for f in $(ERROR_DIR)/*.jpg; do \
	    leaks --atExit -- $(BUILD_DIR)/jpeg_decode $$f 2>&1 | grep -q " 0 leaks" \
	      || { echo "[errtest] LEAK on $$f" >&2; exit 3; }; \
	  done; \
	  echo "[errtest] leaks: clean on all error cases"; \
	else \
	  echo "[errtest] leaks tool unavailable, leak check skipped"; \
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
