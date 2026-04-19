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

.PHONY: all test golden clean
all: $(BUILD_DIR)/jpeg_decode $(GOLDEN_BIN)

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
