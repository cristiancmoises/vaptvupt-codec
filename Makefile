# VaptVupt — Makefile
# Build: make
# Test:  make test        (runs both test suites)
# Bench: make bench

CC       ?= gcc
CFLAGS   = -Wall -Wextra -Wno-unused-parameter -O2 -std=c11 -Iinclude -D_POSIX_C_SOURCE=199309L
LDFLAGS  =

ARCH := $(shell uname -m)
ifeq ($(ARCH),x86_64)
  CFLAGS += -mavx2
endif

CORE_SRC = src/vv_encoder.c src/vv_decoder.c src/vv_simd.c src/vv_xxh64.c src/vv_huffman.c src/vv_ans.c
SOURCES  = src/main.c $(CORE_SRC)
TARGET   = vaptvupt

TEST1_SRC = tests/test_roundtrip.c $(CORE_SRC)
TEST1_BIN = test_roundtrip

TEST2_SRC = tests/test_huffman.c $(CORE_SRC)
TEST2_BIN = test_huffman

TEST3_SRC = tests/test_ans.c $(CORE_SRC)
TEST3_BIN = test_ans

TEST4_SRC = tests/test_sprint6.c $(CORE_SRC)
TEST4_BIN = test_sprint6

TEST5_SRC = tests/test_sprint7.c $(CORE_SRC)
TEST5_BIN = test_sprint7

.PHONY: all clean test run_roundtrip run_huffman bench

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) $(SOURCES) $(LDFLAGS) -o $(TARGET)
	@echo "Build complete: ./$(TARGET)"

$(TEST1_BIN): $(TEST1_SRC)
	$(CC) $(CFLAGS) $(TEST1_SRC) $(LDFLAGS) -o $(TEST1_BIN)

$(TEST2_BIN): $(TEST2_SRC)
	$(CC) $(CFLAGS) $(TEST2_SRC) $(LDFLAGS) -o $(TEST2_BIN)

$(TEST3_BIN): $(TEST3_SRC)
	$(CC) $(CFLAGS) $(TEST3_SRC) $(LDFLAGS) -o $(TEST3_BIN)

$(TEST4_BIN): $(TEST4_SRC)
	$(CC) $(CFLAGS) $(TEST4_SRC) $(LDFLAGS) -o $(TEST4_BIN)

$(TEST5_BIN): $(TEST5_SRC)
	$(CC) $(CFLAGS) $(TEST5_SRC) $(LDFLAGS) -o $(TEST5_BIN)

test: $(TEST1_BIN) $(TEST2_BIN) $(TEST3_BIN) $(TEST4_BIN) $(TEST5_BIN)
	./$(TEST1_BIN)
	./$(TEST2_BIN)
	./$(TEST3_BIN)
	./$(TEST4_BIN)
	./$(TEST5_BIN)

run_roundtrip: $(TEST1_BIN)
	./$(TEST1_BIN)

run_huffman: $(TEST2_BIN)
	./$(TEST2_BIN)

bench: $(TARGET)
	@echo "=== VaptVupt Benchmark ==="
	@if [ -f /tmp/vv_bench_data ]; then \
		./$(TARGET) -b /tmp/vv_bench_data; \
	else \
		echo "Generating test data..."; \
		dd if=/dev/urandom bs=1M count=1 of=/tmp/vv_rand.bin 2>/dev/null; \
		./$(TARGET) -b /tmp/vv_rand.bin; \
		rm -f /tmp/vv_rand.bin; \
	fi

clean:
	rm -f $(TARGET) $(TEST1_BIN) $(TEST2_BIN) $(TEST3_BIN) $(TEST4_BIN) $(TEST5_BIN) *.vv *.orig
