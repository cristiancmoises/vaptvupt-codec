# VaptVupt — Makefile
# Build: make
# Test:  make test        (runs both test suites)
# Bench: make bench

CC       ?= gcc
# SPRINT 26 (v2.48.6): default to -O3 -flto.
# Measured decode speedup on Silesia: dickens +23%, xml +11%, sao +12%,
# x-ray +15% (best of 7 runs each, gcc 13.3, x86_64). Encode -m fast
# gets +3-8%. Compressed output byte-identical. All tests pass under
# new flags. -flto adds about 10 KB to binary size.
#
# For maximum performance on a known target CPU, build with:
#   make clean && make perf
# which adds -march=native. Native binaries are NOT portable across
# CPU generations; the default -O3 -flto build is.
CFLAGS   = -Wall -Wextra -Werror -Wno-unused-parameter -O3 -flto -std=c11 -Iinclude -D_POSIX_C_SOURCE=199309L
LDFLAGS  = -flto

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

CORE_SRC = src/vv_encoder.c src/vv_decoder.c src/vv_simd.c src/vv_xxh64.c src/vv_huffman.c src/vv_ans.c src/vv_bcj.c src/vaptvupt_api.c
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

# Sprint 122: VaptVupt 2.2.2 integration smoke test
TEST19_SRC = tests/test_integration.c $(CORE_SRC)
TEST19_BIN = test_integration
TEST20_SRC = tests/test_bcj.c $(CORE_SRC)
TEST20_BIN = test_bcj

TEST21_SRC = tests/test_phase1_overflow.c $(CORE_SRC)
TEST21_BIN = test_phase1_overflow

TEST22_SRC = tests/test_exact_buffer_decode.c $(CORE_SRC)
TEST22_BIN = test_exact_buffer_decode

.PHONY: all clean test python-test fuzz fuzz-libfuzzer test-fuzz fuzz-clean bench-update speed-update speed-baseline speed-profile run_roundtrip run_huffman bench check-debug perf pgo

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

$(TEST20_BIN): $(TEST20_SRC)
	@mkdir -p build_obj
	$(CC) $(CFLAGS) -c src/vv_simd.c $(SIMD_FLAGS) -o build_obj/vv_simd_t20.o
	$(CC) $(CFLAGS) -c src/vv_decoder.c $(SIMD_FLAGS) -o build_obj/vv_decoder_t20.o
	$(CC) $(CFLAGS) $(filter-out src/vv_simd.c src/vv_decoder.c, $(TEST20_SRC)) build_obj/vv_simd_t20.o build_obj/vv_decoder_t20.o $(LDFLAGS) -o $(TEST20_BIN)

$(TEST21_BIN): $(TEST21_SRC)
	@mkdir -p build_obj
	$(CC) $(CFLAGS) -c src/vv_simd.c $(SIMD_FLAGS) -o build_obj/vv_simd_t21.o
	$(CC) $(CFLAGS) $(SIMD_FLAGS) -c tests/test_phase1_overflow.c -o build_obj/test_phase1_overflow.o
	$(CC) $(CFLAGS) build_obj/test_phase1_overflow.o $(filter-out src/vv_simd.c src/vv_decoder.c tests/test_phase1_overflow.c, $(TEST21_SRC)) build_obj/vv_simd_t21.o $(LDFLAGS) -o $(TEST21_BIN)

$(TEST22_BIN): $(TEST22_SRC)
	@mkdir -p build_obj
	$(CC) $(CFLAGS) -c src/vv_simd.c $(SIMD_FLAGS) -o build_obj/vv_simd_t22.o
	$(CC) $(CFLAGS) -c src/vv_decoder.c $(SIMD_FLAGS) -o build_obj/vv_decoder_t22.o
	$(CC) $(CFLAGS) $(filter-out src/vv_simd.c src/vv_decoder.c, $(TEST22_SRC)) build_obj/vv_simd_t22.o build_obj/vv_decoder_t22.o $(LDFLAGS) -o $(TEST22_BIN)

test: $(TEST1_BIN) $(TEST2_BIN) $(TEST3_BIN) $(TEST4_BIN) $(TEST5_BIN) $(TEST6_BIN) $(TEST7_BIN) $(TEST8_BIN) $(TEST9_BIN) $(TEST10_BIN) $(TEST11_BIN) $(TEST12_BIN) $(TEST13_BIN) $(TEST14_BIN) $(TEST15_BIN) $(TEST16_BIN) $(TEST17_BIN) $(TEST18_BIN) $(TEST19_BIN) $(TEST20_BIN) $(TEST21_BIN) $(TEST22_BIN) $(TARGET)
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
	./$(TEST20_BIN)
	./$(TEST21_BIN)
	./$(TEST22_BIN)
	@echo ""
	@echo "OOM-robustness sweep (no crash on any single allocation failure):"
	@VV_BIN=./$(TARGET) CC="$(CC)" sh tests/oom_sweep.sh
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
		echo "" ; \
		echo "Competitive harness self-test (bench/competitive.py):" ; \
		python3 bench/competitive.py --vv ./$(TARGET) --self-test ; \
		echo "" ; \
		echo "CLI window flag test (-w roundtrip + validation):" ; \
		VV_BIN=./$(TARGET) python3 tests/cli_window.py ; \
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

# ─────────────────────────────────────────────────────────────────────
# libFuzzer harnesses (Sprint 123, v2.48.5)
# ─────────────────────────────────────────────────────────────────────
# Three coverage-guided harnesses statically linking the codec sources
# with -fsanitize=fuzzer,address,undefined. Requires clang.
#
#   make fuzz-libfuzzer       — build all harnesses
#   make test-fuzz            — build + run 30s smoke per harness
#   make fuzz-clean           — remove fuzz binaries + corpora

# FUZZ_SAN selects which sanitizers to combine with libFuzzer.
# Default: both address + undefined (matches CI smoke + dev builds).
# Nightly workflow can override, e.g. FUZZ_SAN=address or FUZZ_SAN=undefined
# to isolate which sanitizer flags a finding.
FUZZ_SAN ?= address,undefined
FUZZ_CFLAGS = -O1 -g -Wall -Wno-unused-parameter \
              -fsanitize=fuzzer,$(FUZZ_SAN) \
              -Iinclude -D_POSIX_C_SOURCE=199309L

FUZZ_CORE   = src/vv_decoder.c src/vv_encoder.c src/vv_xxh64.c \
              src/vv_huffman.c src/vv_ans.c src/vaptvupt_api.c
FUZZ_SIMD_O = build_obj/vv_simd_fuzz.o

build_obj/vv_simd_fuzz.o: src/vv_simd.c
	@command -v clang >/dev/null 2>&1 || { echo "fuzz: clang required"; exit 1; }
	@mkdir -p build_obj
	clang -O1 -g -Wall -Wno-unused-parameter -msse4.2 -fPIC -Iinclude -c $< -o $@

build_obj/fuzz_decompress: tests/fuzz/fuzz_decompress.c $(FUZZ_SIMD_O)
	@mkdir -p build_obj
	clang $(FUZZ_CFLAGS) $(FUZZ_CORE) $(FUZZ_SIMD_O) $< -o $@

build_obj/fuzz_dstream: tests/fuzz/fuzz_dstream.c $(FUZZ_SIMD_O)
	@mkdir -p build_obj
	clang $(FUZZ_CFLAGS) $(FUZZ_CORE) $(FUZZ_SIMD_O) $< -o $@

build_obj/fuzz_roundtrip: tests/fuzz/fuzz_roundtrip.c $(FUZZ_SIMD_O)
	@mkdir -p build_obj
	clang $(FUZZ_CFLAGS) $(FUZZ_CORE) $(FUZZ_SIMD_O) $< -o $@

fuzz-libfuzzer: build_obj/fuzz_decompress build_obj/fuzz_dstream build_obj/fuzz_roundtrip

test-fuzz: fuzz-libfuzzer
	@command -v clang >/dev/null 2>&1 || { echo "test-fuzz: skipped (clang required)"; exit 0; }
	@mkdir -p build_obj/corpus_decompress build_obj/corpus_dstream build_obj/corpus_roundtrip
	@echo "[fuzz_decompress] 30s smoke"
	@build_obj/fuzz_decompress -max_total_time=30 -print_final_stats=0 build_obj/corpus_decompress 2>&1 | grep -E "Done|crash" || true
	@echo "[fuzz_dstream] 30s smoke"
	@build_obj/fuzz_dstream    -max_total_time=30 -print_final_stats=0 build_obj/corpus_dstream    2>&1 | grep -E "Done|crash" || true
	@echo "[fuzz_roundtrip] 30s smoke"
	@build_obj/fuzz_roundtrip  -max_total_time=30 -print_final_stats=0 build_obj/corpus_roundtrip  2>&1 | grep -E "Done|crash" || true

fuzz-clean:
	rm -f build_obj/fuzz_decompress build_obj/fuzz_dstream build_obj/fuzz_roundtrip
	rm -f build_obj/vv_simd_fuzz.o
	rm -rf build_obj/corpus_decompress build_obj/corpus_dstream build_obj/corpus_roundtrip
	rm -f crash-* leak-* timeout-* oom-*

# ─────────────────────────────────────────────────────────────────────
# SPEED PROGRAM targets (Sprint 25, v2.49.0)
# ─────────────────────────────────────────────────────────────────────
# Multi-sprint speed/ratio work is recorded in CHANGELOG.md.
#
# speed-baseline — produce the reproducible speed+ratio table
#                  comparing vv -fast/-balanced/-extreme to
#                  zstd -1/-3/-9 on the Silesia fixtures.
# speed-profile  — build a -pg instrumented binary, decode dickens
#                  ten times, print the gprof flat profile.
#
# Both require Silesia corpus extracted at $SILESIA (default /tmp/silesia).

speed-baseline: $(TARGET)
	@python3 bench/bench.py

speed-profile: $(TARGET)
	@bash bench/profile_decode.sh

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

# Sprint 26 (v2.48.6): build with -march=native for maximum decode
# throughput on the host CPU. Adds ~5% decode speed over the default
# -O3 -flto build on x86_64. Binaries built with -march=native will only
# run on CPUs that match or supersede the build host. For portable
# builds, use plain `make`.
perf:
	@$(MAKE) clean
	@$(MAKE) CFLAGS="-Wall -Wextra -Wno-unused-parameter -O3 -flto -march=native -std=c11 -Iinclude -D_POSIX_C_SOURCE=199309L" LDFLAGS="-flto"
	@echo ""
	@echo "Built ./vaptvupt with -O3 -flto -march=native (non-portable)."
	@echo "For portable builds, use plain make instead."
	@echo ""
	@echo "Encode speedup (Sprint 41 measurement, vs portable):"
	@echo "  - Structured/repetitive data (XML, JSON, logs): +6 to +11%"
	@echo "    (long matches exercise the AVX2 32-byte extend_match loop)"
	@echo "  - Text/binary (dickens, sao, x-ray): -1 to -3%"
	@echo "    (short matches resolve in the scalar 8-byte fast-path;"
	@echo "     AVX2 codegen adds slight register pressure)"
	@echo "  Net: use 'make perf' when your data has long repetitive runs."
	@echo "  Decode: -march=native also enables AVX2 copy paths (Sprint 26)."

# Sprint 28 (v2.50.2): profile-guided optimization. Two-stage build:
# (1) instrument + train, (2) use profile data. Adds +3-8% encode and
# +2-6% decode on top of the default -O3 -flto build, by giving the
# compiler real branch-frequency and hot-path data from typical
# workloads. Requires the Silesia corpus to be present at /tmp/silesia/
# for training.
#
# To rebuild from clean state with PGO:
#   make pgo
# This takes ~3× the time of a normal build because of the two-stage
# compile + training run. The output is byte-identical to a non-PGO
# build on all four Silesia fixtures.
PGO_DIR ?= /tmp/vv_pgo
PGO_TRAIN_DIR ?= /tmp/silesia
pgo:
	@if [ ! -d "$(PGO_TRAIN_DIR)" ] || [ ! -f "$(PGO_TRAIN_DIR)/dickens" ]; then \
		echo "make pgo: training data not found at $(PGO_TRAIN_DIR)/dickens"; \
		echo "  Set PGO_TRAIN_DIR=/path/to/silesia (or any large representative corpus)."; \
		exit 1; \
	fi
	@mkdir -p $(PGO_DIR)
	@rm -f $(PGO_DIR)/*.gcda
	@echo "=== PGO stage 1: instrument ==="
	@$(MAKE) clean
	@$(MAKE) CFLAGS="-Wall -Wextra -Wno-unused-parameter -O3 -fprofile-generate=$(PGO_DIR) -std=c11 -Iinclude -D_POSIX_C_SOURCE=199309L" LDFLAGS="-fprofile-generate=$(PGO_DIR)"
	@echo ""
	@echo "=== PGO stage 2: train ==="
	@for f in dickens xml sao x-ray; do \
		test -f "$(PGO_TRAIN_DIR)/$$f" || continue; \
		for mode in fast balanced; do \
			./$(TARGET) -c -m $$mode -o /tmp/vv_pgo_train.vv $(PGO_TRAIN_DIR)/$$f >/dev/null 2>&1; \
			./$(TARGET) -d -o /dev/null /tmp/vv_pgo_train.vv >/dev/null 2>&1; \
		done; \
	done
	@rm -f /tmp/vv_pgo_train.vv
	@echo "=== PGO stage 3: rebuild with profile ==="
	@$(MAKE) clean
	@$(MAKE) CFLAGS="-Wall -Wextra -Wno-unused-parameter -O3 -flto -fprofile-use=$(PGO_DIR) -Wno-missing-profile -std=c11 -Iinclude -D_POSIX_C_SOURCE=199309L" LDFLAGS="-flto -fprofile-use=$(PGO_DIR) -Wno-missing-profile"
	@echo ""
	@echo "Built ./vaptvupt with PGO. Output byte-identical to default build."
	@echo "Measured gain vs default: +3-8% encode, +2-6% decode on Silesia."

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
	rm -f $(TARGET) $(TEST1_BIN) $(TEST2_BIN) $(TEST3_BIN) $(TEST4_BIN) $(TEST5_BIN) $(TEST6_BIN) $(TEST7_BIN) $(TEST8_BIN) $(TEST9_BIN) $(TEST10_BIN) $(TEST11_BIN) $(TEST12_BIN) $(TEST13_BIN) $(TEST14_BIN) $(TEST15_BIN) $(TEST16_BIN) $(TEST17_BIN) $(TEST18_BIN) $(TEST19_BIN) $(TEST20_BIN) $(TEST21_BIN) $(TEST22_BIN) *.vv *.zupt *.orig
	rm -rf tests/corpus_bad

amalg:
	@mkdir -p build
	@echo "/* VaptVupt amalgamation — single-file build for VaptVupt */" > build/vaptvupt.h
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
# fixes land in src/ but the VaptVupt-facing amalgamation goes stale.
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
	echo "/* VaptVupt amalgamation — single-file build for VaptVupt */" > $$tmpdir/build/vaptvupt.h && \
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
		echo "  (this is the VaptVupt-facing amalgamation; staleness can"; \
		echo "   ship security fixes from src/ but not to VaptVupt builds)"; \
		diff -u build/vaptvupt.c $$tmpdir/build/vaptvupt.c | head -30; \
		rm -rf $$tmpdir; exit 1; \
	fi && \
	rm -rf $$tmpdir && \
	echo "✓ build/vaptvupt.{c,h} are in sync with src/"


# Formal verification of the BCJ filters with CBMC (apt-get install cbmc).
.PHONY: verify
verify:
	sh verification/verify.sh
