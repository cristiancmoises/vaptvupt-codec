# VaptVupt — CMake build
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Usage:
#   mkdir build && cd build
#   cmake .. -DCMAKE_BUILD_TYPE=Release
#   make
#
# Options:
#   -DVV_BUILD_TESTS=ON/OFF   (default ON)
#   -DVV_ENABLE_AVX2=ON/OFF   (default ON on x86-64)

cmake_minimum_required(VERSION 3.10)
project(vaptvupt VERSION 1.6.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(VV_BUILD_TESTS "Build test suite" ON)
option(VV_ENABLE_AVX2 "Enable AVX2 code paths on x86-64" ON)

# ─── Compiler flags ───
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-Wall -Wextra -Wno-unused-parameter -O2)
    add_compile_definitions(_POSIX_C_SOURCE=199309L)
elseif(MSVC)
    add_compile_options(/W3 /O2)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

include_directories(include)

# ─── AVX2-compiled files (only if enabled) ───
set(SIMD_SOURCES src/vv_simd.c src/vv_decoder.c)

# ─── Baseline sources ───
set(CORE_SOURCES
    src/vv_encoder.c
    src/vv_xxh64.c
    src/vv_huffman.c
    src/vv_ans.c
    src/vaptvupt_api.c
)

add_library(vaptvupt_simd OBJECT ${SIMD_SOURCES})
if(VV_ENABLE_AVX2 AND (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64"))
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
        target_compile_options(vaptvupt_simd PRIVATE -mavx2)
    elseif(MSVC)
        target_compile_options(vaptvupt_simd PRIVATE /arch:AVX2)
    endif()
endif()

add_library(vaptvupt_core OBJECT ${CORE_SOURCES})

# ─── CLI executable ───
add_executable(vaptvupt src/main.c
    $<TARGET_OBJECTS:vaptvupt_simd>
    $<TARGET_OBJECTS:vaptvupt_core>
)

# ─── Static library for embedding ───
add_library(vaptvupt_lib STATIC
    $<TARGET_OBJECTS:vaptvupt_simd>
    $<TARGET_OBJECTS:vaptvupt_core>
)
set_target_properties(vaptvupt_lib PROPERTIES OUTPUT_NAME vaptvupt)

# ─── Tests ───
if(VV_BUILD_TESTS)
    enable_testing()
    foreach(test_name IN ITEMS test_roundtrip test_huffman test_ans test_sprint6 test_sprint7)
        add_executable(${test_name} tests/${test_name}.c
            $<TARGET_OBJECTS:vaptvupt_simd>
            $<TARGET_OBJECTS:vaptvupt_core>
        )
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
endif()

# ─── Install ───
install(TARGETS vaptvupt vaptvupt_lib
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)
install(FILES include/vaptvupt.h include/vaptvupt_api.h DESTINATION include)
