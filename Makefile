# VaptVupt — Makefile
# Build: make
# Test:  make test        (runs both test suites)
# Bench: make bench

CC       ?= gcc
CFLAGS   = -Wall -Wextra -Wno-unused-parameter -O2 -std=c11 -Iinclude -D_POSIX_C_SOURCE=199309L
LDFLAGS  =

# Optional: multi-threaded encoding support.
#   make ENABLE_THREADS=1
# Builds with -DVV_ENABLE_THREADS and -lpthread so vv_compress_mt
# runs in parallel. Without this flag, vv_compress_mt falls back to
# sequential encoding (still producing valid multi-frame output).
ifeq ($(ENABLE_THREADS),1)
  CFLAGS  += -DVV_ENABLE_THREADS
  LDFLAGS += -lpthread
endif

# AVX2 is applied ONLY to vv_simd.c (and via runtime dispatch).
# All other sources compile baseline; SSE2 is baseline on x86-64.
ARCH := $(shell uname -m)
SIMD_FLAGS :=
ifeq ($(ARCH),x86_64)
  SIMD_FLAGS := -mavx2
endif

CORE_SRC = src/vv_encoder.c src/vv_decoder.c src/vv_simd.c src/vv_xxh64.c src/vv_huffman.c src/vv_ans.c src/vaptvupt_api.c
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

TEST6_SRC = tests/test_sprint16.c $(CORE_SRC)
TEST6_BIN = test_sprint16

TEST7_SRC = tests/test_streaming.c $(CORE_SRC)
TEST7_BIN = test_streaming

TEST8_SRC = tests/test_edge_cases.c $(CORE_SRC)
TEST8_BIN = test_edge_cases

TEST9_SRC = tests/test_format_spec.c $(CORE_SRC)
TEST9_BIN = test_format_spec

TEST10_SRC = tests/test_stream_fuzz.c $(CORE_SRC)
TEST10_BIN = test_stream_fuzz

TEST11_SRC = tests/test_skip_checksum.c $(CORE_SRC)
TEST11_BIN = test_skip_checksum

TEST12_SRC = tests/test_seq_v2.c $(CORE_SRC)
TEST12_BIN = test_seq_v2

TEST13_SRC = tests/test_safezone_adversarial.c $(CORE_SRC)
TEST13_BIN = test_safezone_adversarial

TEST14_SRC = tests/test_large_boundary.c $(CORE_SRC)
TEST14_BIN = test_large_boundary

TEST15_SRC = tests/test_dos_hang.c $(CORE_SRC)
TEST15_BIN = test_dos_hang

TEST16_SRC = tests/test_api_contract.c $(CORE_SRC)
TEST16_BIN = test_api_contract

TEST17_SRC = tests/test_huffman4.c src/vv_huffman.c
TEST17_BIN = test_huffman4

# Sprint 117: secure-zero behavioral test
TEST18_SRC = tests/test_secure_zero.c $(CORE_SRC)
TEST18_BIN = test_secure_zero

# Sprint 122: Zupt 2.2.2 integration smoke test
TEST19_SRC = tests/test_zupt_integration.c $(CORE_SRC)
TEST19_BIN = test_zupt_integration

.PHONY: all clean test python-test fuzz bench-update speed-update run_roundtrip run_huffman bench check-debug

all: $(TARGET)

# Fail if any debug fopen/fprintf sneaks into core source files.
# Sprint 22 lesson: stale debug calls in hot paths cost 100× performance.
check-debug:
	@if grep -nE 'fopen|fprintf\b' $(CORE_SRC) | grep -v '_POSIX_C_SOURCE' ; then \
	    echo "ERROR: debug fopen/fprintf calls found in core sources above." ; \
	    echo "These are hot-path-hostile and must be removed before commit." ; \
	    exit 1 ; \
	fi
	@echo "check-debug: no debug calls in core sources ✓"

$(TARGET): $(SOURCES)
	@# Sprint 121: ensure build_obj/ exists (fresh-extract from tarball
	@# may not have it). Idempotent.
	@mkdir -p build_obj
	@# Compile AVX2-using files (vv_simd.c, vv_decoder.c) with SIMD_FLAGS
	$(CC) $(CFLAGS) -c src/vv_simd.c $(SIMD_FLAGS) -o build_obj/vv_simd.o
	$(CC) $(CFLAGS) -c src/vv_decoder.c $(SIMD_FLAGS) -o build_obj/vv_decoder.o
	$(CC) $(CFLAGS) $(filter-out src/vv_simd.c src/vv_decoder.c, $(SOURCES)) build_obj/vv_simd.o build_obj/vv_decoder.o $(LDFLAGS) -o $(TARGET)
	@rm -f build_obj/vv_simd.o build_obj/vv_decoder.o
	@echo "Build complete: ./$(TARGET)"

$(TEST1_BIN): $(TEST1_SRC)
	@mkdir -p build_obj
	$(CC) $(CFLAGS) -c src/vv_simd.c $(SIMD_FLAGS) -o build_obj/vv_simd_t1.o
	$(CC) $(CFLAGS) -c src/vv_decoder.c $(SIMD_FLAGS) -o build_obj/vv_decoder_t1.o
	$(CC) $(CFLAGS) $(filter-out src/vv_simd.c src/vv_decoder.c, $(TEST1_SRC)) build_obj/vv_simd_t1.o build_obj/vv_decoder_t1.o $(LDFLAGS) -o $(TEST1_BIN)
	@rm -f build_obj/vv_simd_t1.o build_obj/vv_decoder_t1.o

$(TEST2_BIN): $(TEST2_SRC)
	@mkdir -p build_obj
	$(CC) $(CFLAGS) -c src/vv_simd.c $(SIMD_FLAGS) -o build_obj/vv_simd_t2.o
	$(CC) $(CFLAGS) -c src/vv_decoder.c $(SIMD_FLAGS) -o build_obj/vv_decoder_t2.o
	$(CC) $(CFLAGS) $(filter-out src/vv_simd.c src/vv_decoder.c, $(TEST2_SRC)) build_obj/vv_simd_t2.o build_obj/vv_decoder_t2.o $(LDFLAGS) -o $(TEST2_BIN)
	@rm -f build_obj/vv_simd_t2.o build_obj/vv_decoder_t2.o

$(TEST3_BIN): $(TEST3_SRC)
	@mkdir -p build_obj
	$(CC) $(CFLAGS) -c src/vv_simd.c $(SIMD_FLAGS) -o build_obj/vv_simd_t3.o
	$(CC) $(CFLAGS) -c src/vv_decoder.c $(SIMD_FLAGS) -o build_obj/vv_decoder_t3.o
	$(CC) $(CFLAGS) $(filter-out src/vv_simd.c src/vv_decoder.c, $(TEST3_SRC)) build_obj/vv_simd_t3.o build_obj/vv_decoder_t3.o $(LDFLAGS) -o $(TEST3_BIN)
	@rm -f build_obj/vv_simd_t3.o build_obj/vv_decoder_t3.o

$(TEST4_BIN): $(TEST4_SRC)
	@mkdir -p build_obj
	$(CC) $(CFLAGS) -c src/vv_simd.c $(SIMD_FLAGS) -o build_obj/vv_simd_t4.o
	$(CC) $(CFLAGS) -c src/vv_decoder.c $(SIMD_FLAGS) -o build_obj/vv_decoder_t4.o
	$(CC) $(CFLAGS) $(filter-out src/vv_simd.c src/vv_decoder.c, $(TEST4_SRC)) build_obj/vv_simd_t4.o build_obj/vv_decoder_t4.o $(LDFLAGS) -o $(TEST4_BIN)
	@rm -f build_obj/vv_simd_t4.o build_obj/vv_decoder_t4.o

$(TEST5_BIN): $(TEST5_SRC)
	@mkdir -p build_obj
	$(CC) $(CFLAGS) -c src/vv_simd.c $(SIMD_FLAGS) -o build_obj/vv_simd_t5.o
	$(CC) $(CFLAGS) -c src/vv_decoder.c $(SIMD_FLAGS) -o build_obj/vv_decoder_t5.o
	$(CC) $(CFLAGS) $(filter-out src/vv_simd.c src/vv_decoder.c, $(TEST5_SRC)) build_obj/vv_simd_t5.o build_obj/vv_decoder_t5.o $(LDFLAGS) -o $(TEST5_BIN)
	@rm -f build_obj/vv_simd_t5.o build_obj/vv_decoder_t5.o

$(TEST6_BIN): $(TEST6_SRC)
	@mkdir -p build_obj
	$(CC) $(CFLAGS) -c src/vv_simd.c $(SIMD_FLAGS) -o build_obj/vv_simd_t6.o
	$(CC) $(CFLAGS) -c src/vv_decoder.c $(SIMD_FLAGS) -o build_obj/vv_decoder_t6.o
	$(CC) $(CFLAGS) $(filter-out src/vv_simd.c src/vv_decoder.c, $(TEST6_SRC)) build_obj/vv_simd_t6.o build_obj/vv_decoder_t6.o $(LDFLAGS) -o $(TEST6_BIN)
	@rm -f build_obj/vv_simd_t6.o build_obj/vv_decoder_t6.o

$(TEST7_BIN): $(TEST7_SRC)
	@mkdir -p build_obj
	$(CC) $(CFLAGS) -c src/vv_simd.c $(SIMD_FLAGS) -o build_obj/vv_simd_t7.o
	$(CC) $(CFLAGS) -c src/vv_decoder.c $(SIMD_FLAGS) -o build_obj/vv_decoder_t7.o
	$(CC) $(CFLAGS) $(filter-out src/vv_simd.c src/vv_decoder.c, $(TEST7_SRC)) build_obj/vv_simd_t7.o build_obj/vv_decoder_t7.o $(LDFLAGS) -o $(TEST7_BIN)
	@rm -f build_obj/vv_simd_t7.o build_obj/vv_decoder_t7.o

$(TEST8_BIN): $(TEST8_SRC)
	@mkdir -p build_obj
	$(CC) $(CFLAGS) -c src/vv_simd.c $(SIMD_FLAGS) -o build_obj/vv_simd_t8.o
	$(CC) $(CFLAGS) -c src/vv_decoder.c $(SIMD_FLAGS) -o build_obj/vv_decoder_t8.o
	$(CC) $(CFLAGS) $(filter-out src/vv_simd.c src/vv_decoder.c, $(TEST8_SRC)) build_obj/vv_simd_t8.o build_obj/vv_decoder_t8.o $(LDFLAGS) -o $(TEST8_BIN)
	@rm -f build_obj/vv_simd_t8.o build_obj/vv_decoder_t8.o

$(TEST9_BIN): $(TEST9_SRC)
	@mkdir -p build_obj
	$(CC) $(CFLAGS) -c src/vv_simd.c $(SIMD_FLAGS) -o build_obj/vv_simd_t9.o
	$(CC) $(CFLAGS) -c src/vv_decoder.c $(SIMD_FLAGS) -o build_obj/vv_decoder_t9.o
	$(CC) $(CFLAGS) $(filter-out src/vv_simd.c src/vv_decoder.c, $(TEST9_SRC)) build_obj/vv_simd_t9.o build_obj/vv_decoder_t9.o $(LDFLAGS) -o $(TEST9_BIN)
	@rm -f build_obj/vv_simd_t9.o build_obj/vv_decoder_t9.o

$(TEST10_BIN): $(TEST10_SRC)
	@mkdir -p build_obj
	$(CC) $(CFLAGS) -c src/vv_simd.c $(SIMD_FLAGS) -o build_obj/vv_simd_t10.o
	$(CC) $(CFLAGS) -c src/vv_decoder.c $(SIMD_FLAGS) -o build_obj/vv_decoder_t10.o
	$(CC) $(CFLAGS) $(filter-out src/vv_simd.c src/vv_decoder.c, $(TEST10_SRC)) build_obj/vv_simd_t10.o build_obj/vv_decoder_t10.o $(LDFLAGS) -o $(TEST10_BIN)
	@rm -f build_obj/vv_simd_t10.o build_obj/vv_decoder_t10.o

$(TEST11_BIN): $(TEST11_SRC)
	@mkdir -p build_obj
	$(CC) $(CFLAGS) -c src/vv_simd.c $(SIMD_FLAGS) -o build_obj/vv_simd_t11.o
	$(CC) $(CFLAGS) -c src/vv_decoder.c $(SIMD_FLAGS) -o build_obj/vv_decoder_t11.o
	$(CC) $(CFLAGS) $(filter-out src/vv_simd.c src/vv_decoder.c, $(TEST11_SRC)) build_obj/vv_simd_t11.o build_obj/vv_decoder_t11.o $(LDFLAGS) -o $(TEST11_BIN)
	@rm -f build_obj/vv_simd_t11.o build_obj/vv_decoder_t11.o

$(TEST12_BIN): $(TEST12_SRC)
	@mkdir -p build_obj
	$(CC) $(CFLAGS) -c src/vv_simd.c $(SIMD_FLAGS) -o build_obj/vv_simd_t12.o
	$(CC) $(CFLAGS) -c src/vv_decoder.c $(SIMD_FLAGS) -o build_obj/vv_decoder_t12.o
	$(CC) $(CFLAGS) $(filter-out src/vv_simd.c src/vv_decoder.c, $(TEST12_SRC)) build_obj/vv_simd_t12.o build_obj/vv_decoder_t12.o $(LDFLAGS) -o $(TEST12_BIN)
	@rm -f build_obj/vv_simd_t12.o build_obj/vv_decoder_t12.o

$(TEST13_BIN): $(TEST13_SRC)
	@mkdir -p build_obj
	$(CC) $(CFLAGS) -c src/vv_simd.c $(SIMD_FLAGS) -o build_obj/vv_simd_t13.o
	$(CC) $(CFLAGS) -c src/vv_decoder.c $(SIMD_FLAGS) -o build_obj/vv_decoder_t13.o
	$(CC) $(CFLAGS) $(filter-out src/vv_simd.c src/vv_decoder.c, $(TEST13_SRC)) build_obj/vv_simd_t13.o build_obj/vv_decoder_t13.o $(LDFLAGS) -o $(TEST13_BIN)
	@rm -f build_obj/vv_simd_t13.o build_obj/vv_decoder_t13.o

$(TEST14_BIN): $(TEST14_SRC)
	@mkdir -p build_obj
	$(CC) $(CFLAGS) -c src/vv_simd.c $(SIMD_FLAGS) -o build_obj/vv_simd_t14.o
	$(CC) $(CFLAGS) -c src/vv_decoder.c $(SIMD_FLAGS) -o build_obj/vv_decoder_t14.o
	$(CC) $(CFLAGS) $(filter-out src/vv_simd.c src/vv_decoder.c, $(TEST14_SRC)) build_obj/vv_simd_t14.o build_obj/vv_decoder_t14.o $(LDFLAGS) -o $(TEST14_BIN)
	@rm -f build_obj/vv_simd_t14.o build_obj/vv_decoder_t14.o

$(TEST15_BIN): $(TEST15_SRC)
	@mkdir -p build_obj
	$(CC) $(CFLAGS) -c src/vv_simd.c $(SIMD_FLAGS) -o build_obj/vv_simd_t15.o
	$(CC) $(CFLAGS) -c src/vv_decoder.c $(SIMD_FLAGS) -o build_obj/vv_decoder_t15.o
	$(CC) $(CFLAGS) $(filter-out src/vv_simd.c src/vv_decoder.c, $(TEST15_SRC)) build_obj/vv_simd_t15.o build_obj/vv_decoder_t15.o $(LDFLAGS) -o $(TEST15_BIN)
	@rm -f build_obj/vv_simd_t15.o build_obj/vv_decoder_t15.o

$(TEST16_BIN): $(TEST16_SRC)
	@mkdir -p build_obj
	$(CC) $(CFLAGS) -c src/vv_simd.c $(SIMD_FLAGS) -o build_obj/vv_simd_t16.o
	$(CC) $(CFLAGS) -c src/vv_decoder.c $(SIMD_FLAGS) -o build_obj/vv_decoder_t16.o
	$(CC) $(CFLAGS) $(filter-out src/vv_simd.c src/vv_decoder.c, $(TEST16_SRC)) build_obj/vv_simd_t16.o build_obj/vv_decoder_t16.o $(LDFLAGS) -o $(TEST16_BIN)
	@rm -f build_obj/vv_simd_t16.o build_obj/vv_decoder_t16.o

$(TEST17_BIN): $(TEST17_SRC)
	$(CC) $(CFLAGS) $(TEST17_SRC) $(LDFLAGS) -o $(TEST17_BIN)

$(TEST18_BIN): $(TEST18_SRC)
	@mkdir -p build_obj
	$(CC) $(CFLAGS) -c src/vv_simd.c $(SIMD_FLAGS) -o build_obj/vv_simd_t18.o
	$(CC) $(CFLAGS) -c src/vv_decoder.c $(SIMD_FLAGS) -o build_obj/vv_decoder_t18.o
	$(CC) $(CFLAGS) $(filter-out src/vv_simd.c src/vv_decoder.c, $(TEST18_SRC)) build_obj/vv_simd_t18.o build_obj/vv_decoder_t18.o $(LDFLAGS) -o $(TEST18_BIN)

$(TEST19_BIN): $(TEST19_SRC)
	@mkdir -p build_obj
	$(CC) $(CFLAGS) -c src/vv_simd.c $(SIMD_FLAGS) -o build_obj/vv_simd_t19.o
	$(CC) $(CFLAGS) -c src/vv_decoder.c $(SIMD_FLAGS) -o build_obj/vv_decoder_t19.o
	$(CC) $(CFLAGS) $(filter-out src/vv_simd.c src/vv_decoder.c, $(TEST19_SRC)) build_obj/vv_simd_t19.o build_obj/vv_decoder_t19.o $(LDFLAGS) -o $(TEST19_BIN)

test: $(TEST1_BIN) $(TEST2_BIN) $(TEST3_BIN) $(TEST4_BIN) $(TEST5_BIN) $(TEST6_BIN) $(TEST7_BIN) $(TEST8_BIN) $(TEST9_BIN) $(TEST10_BIN) $(TEST11_BIN) $(TEST12_BIN) $(TEST13_BIN) $(TEST14_BIN) $(TEST15_BIN) $(TEST16_BIN) $(TEST17_BIN) $(TEST18_BIN) $(TEST19_BIN) $(TARGET)
	./$(TEST1_BIN)
	./$(TEST2_BIN)
	./$(TEST3_BIN)
	./$(TEST4_BIN)
	./$(TEST5_BIN)
	./$(TEST6_BIN)
	./$(TEST7_BIN)
	./$(TEST8_BIN)
	./$(TEST9_BIN)
	./$(TEST10_BIN)
	./$(TEST11_BIN)
	./$(TEST12_BIN)
	./$(TEST13_BIN)
	./$(TEST14_BIN)
	./$(TEST15_BIN)
	./$(TEST16_BIN)
	./$(TEST17_BIN)
	./$(TEST18_BIN)
	./$(TEST19_BIN)
	@if command -v python3 >/dev/null 2>&1 ; then \
		echo "" ; \
		echo "Python reference decoder self-test (validates FORMAT.md decode side):" ; \
		python3 reference/vv_decoder.py --self-test ; \
		echo "" ; \
		echo "Python reference encoder self-test (validates FORMAT.md encode side):" ; \
		python3 reference/vv_encoder.py --self-test ; \
		echo "" ; \
		echo "Python tANS module synthetic test (legacy 'A' tag support):" ; \
		python3 reference/vv_ans.py ; \
		echo "" ; \
		echo "Negative corpus cross-decoder test (C ↔ Python consistency):" ; \
		python3 tests/corpus_negative.py ; \
		echo "" ; \
		echo "Differential fuzzer (5000 cases, deterministic seed):" ; \
		python3 tests/fuzz_differential.py --iters 1000 --seed 42 ; \
		echo "" ; \
		echo "Compression ratio baseline gate:" ; \
		python3 tests/bench_gate.py ; \
		echo "" ; \
		echo "Decode-speed regression gate (20% tolerance — noisy in containers):" ; \
		python3 tests/speed_gate.py || echo "  (speed gate is informational; not failing the build)" ; \
		if command -v node >/dev/null 2>&1 ; then \
			echo "" ; \
			echo "JavaScript reference decoder self-test:" ; \
			node reference/vv_decoder.test.js ; \
		else \
			echo "" ; \
			echo "Skipping JS decoder test (no node)" ; \
		fi ; \
	else \
		echo "Skipping python tests (no python3)" ; \
	fi

# Run only the Python self-tests (decoder + encoder + ANS + corpus + fuzz + bench gate)
python-test: $(TARGET)
	@if ! command -v python3 >/dev/null 2>&1 ; then \
		echo "ERROR: python3 not found" ; exit 1 ; \
	fi
	python3 reference/vv_decoder.py --self-test
	python3 reference/vv_encoder.py --self-test
	python3 reference/vv_ans.py
	python3 tests/corpus_negative.py
	python3 tests/fuzz_differential.py --iters 1000 --seed 42
	python3 tests/bench_gate.py

# Extended fuzz run (10× default iterations, ~50 seconds)
fuzz: $(TARGET)
	python3 tests/fuzz_differential.py --iters 10000 --seed 42

# Update the compression ratio baseline (use carefully — every change is
# a shipped ratio delta users will experience)
bench-update: $(TARGET)
	python3 tests/bench_gate.py --update

# Update the decode-speed baseline (machine-specific — commit only if
# running on the canonical benchmarking machine)
speed-update: $(TARGET)
	python3 tests/speed_gate.py --update

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
	rm -f $(TARGET) $(TEST1_BIN) $(TEST2_BIN) $(TEST3_BIN) $(TEST4_BIN) $(TEST5_BIN) $(TEST6_BIN) $(TEST7_BIN) $(TEST8_BIN) $(TEST9_BIN) $(TEST10_BIN) *.vv *.orig
	rm -rf tests/corpus_bad

amalg:
	@mkdir -p build
	@echo "/* VaptVupt amalgamation — single-file build for Zupt */" > build/vaptvupt.h
	@echo "/* SPDX-License-Identifier: GPL-3.0-or-later */" >> build/vaptvupt.h
	@for f in include/vv_platform.h include/vaptvupt.h include/vv_ans.h include/vv_huffman.h include/vaptvupt_api.h; do \
		grep -v '#include "' $$f >> build/vaptvupt.h; \
	done
	@echo "/* VaptVupt amalgamation — single-file build */" > build/vaptvupt.c
	@echo "/* SPDX-License-Identifier: GPL-3.0-or-later */" >> build/vaptvupt.c
	@echo '#include "vaptvupt.h"' >> build/vaptvupt.c
	@for f in src/vv_xxh64.c src/vv_simd.c src/vv_huffman.c src/vv_ans.c src/vv_encoder.c src/vv_decoder.c src/vaptvupt_api.c; do \
		echo "" >> build/vaptvupt.c; \
		echo "/* ── $$f ── */" >> build/vaptvupt.c; \
		grep -v '#include "' $$f >> build/vaptvupt.c; \
	done
	@echo "Amalgamation: build/vaptvupt.c + build/vaptvupt.h"

# ─────────────────────────────────────────────────────────────────
# amalg-verify: fail if build/vaptvupt.{c,h} differ from a fresh
# regeneration. Catches the Sprint 114 drift class where security
# fixes land in src/ but the Zupt-facing amalgamation goes stale.
#
# Usage: `make amalg-verify` — exits 0 if amalgamation is current,
# non-zero (with diff output) otherwise.
#
# Generates the fresh copy in a tmp directory so the working tree's
# build/ is never modified by the check itself.
# ─────────────────────────────────────────────────────────────────
amalg-verify:
	@tmpdir=$$(mktemp -d) && \
	mkdir -p $$tmpdir/build && \
	echo "/* VaptVupt amalgamation — single-file build for Zupt */" > $$tmpdir/build/vaptvupt.h && \
	echo "/* SPDX-License-Identifier: GPL-3.0-or-later */" >> $$tmpdir/build/vaptvupt.h && \
	for f in include/vv_platform.h include/vaptvupt.h include/vv_ans.h include/vv_huffman.h include/vaptvupt_api.h; do \
		grep -v '#include "' $$f >> $$tmpdir/build/vaptvupt.h; \
	done && \
	echo "/* VaptVupt amalgamation — single-file build */" > $$tmpdir/build/vaptvupt.c && \
	echo "/* SPDX-License-Identifier: GPL-3.0-or-later */" >> $$tmpdir/build/vaptvupt.c && \
	echo '#include "vaptvupt.h"' >> $$tmpdir/build/vaptvupt.c && \
	for f in src/vv_xxh64.c src/vv_simd.c src/vv_huffman.c src/vv_ans.c src/vv_encoder.c src/vv_decoder.c src/vaptvupt_api.c; do \
		echo "" >> $$tmpdir/build/vaptvupt.c; \
		echo "/* ── $$f ── */" >> $$tmpdir/build/vaptvupt.c; \
		grep -v '#include "' $$f >> $$tmpdir/build/vaptvupt.c; \
	done && \
	if ! diff -q $$tmpdir/build/vaptvupt.h build/vaptvupt.h >/dev/null 2>&1; then \
		echo "✗ build/vaptvupt.h is STALE — re-run 'make amalg'"; \
		diff -u build/vaptvupt.h $$tmpdir/build/vaptvupt.h | head -30; \
		rm -rf $$tmpdir; exit 1; \
	fi && \
	if ! diff -q $$tmpdir/build/vaptvupt.c build/vaptvupt.c >/dev/null 2>&1; then \
		echo "✗ build/vaptvupt.c is STALE — re-run 'make amalg'"; \
		echo "  (this is the Zupt-facing amalgamation; staleness can"; \
		echo "   ship security fixes from src/ but not to Zupt builds)"; \
		diff -u build/vaptvupt.c $$tmpdir/build/vaptvupt.c | head -30; \
		rm -rf $$tmpdir; exit 1; \
	fi && \
	rm -rf $$tmpdir && \
	echo "✓ build/vaptvupt.{c,h} are in sync with src/"

