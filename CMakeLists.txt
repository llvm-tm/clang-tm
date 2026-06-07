# CMakeLists.txt - CMake build for TM API C++ Project
# Requires CMake 3.16+
#
# Usage:
#   mkdir build && cd build
#   cmake .. -DCMAKE_BUILD_TYPE=Release
#   make -j$(nproc)
#   ctest

cmake_minimum_required(VERSION 3.16)
project(TM_API_CPP
    VERSION 0.1.0
    DESCRIPTION "LLVM Transactional Memory Plugin and Benchmarks"
    LANGUAGES CXX
)

# ============================================================================
# Options
# ============================================================================

option(BUILD_PLUGIN "Build LLVM TM plugin" ON)
option(BUILD_BENCHMARKS "Build benchmarks" ON)
option(BUILD_TESTS "Build plugin tests" ON)
option(BUILD_SHARED "Build shared plugin" ON)

# Backend selection
set(STM_BACKEND "tinystm" CACHE STRING "STM backend: tinystm, tl2, singlelock, swisstm")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find LLVM
find_package(LLVM 16 REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake at: ${LLVM_DIR}")

# Setup LLVM (this creates the tablegen, etc.)
include(${LLVM_USE_FILE})

# Find LLVM tools (handles versioned installs like llvm-22-dev on Ubuntu)
find_program(LLVM_CLANG clang-22 clang++-22 clang++ clang
    PATHS ${LLVM_TOOLS_BINARY_DIR} /usr/lib/llvm-22/bin
    NO_DEFAULT_PATH)
find_program(LLVM_OPT opt-22 opt
    PATHS ${LLVM_TOOLS_BINARY_DIR} /usr/lib/llvm-22/bin
    NO_DEFAULT_PATH)
if(NOT LLVM_CLANG)
    find_program(LLVM_CLANG clang)
endif()
if(NOT LLVM_OPT)
    find_program(LLVM_OPT opt)
endif()
message(STATUS "Using clang: ${LLVM_CLANG}")
message(STATUS "Using opt:   ${LLVM_OPT}")

# ============================================================================
# Directories
# ============================================================================

set(PLUGIN_DIR ${CMAKE_SOURCE_DIR}/plugin)
set(BACKENDS_DIR ${CMAKE_SOURCE_DIR}/backends)
set(RUNTIMES_DIR ${BACKENDS_DIR}/tm_impl)
set(BENCHMARKS_DIR ${CMAKE_SOURCE_DIR}/benchmarks)

# ============================================================================
# Plugin Library
# ============================================================================

if(BUILD_PLUGIN)
    add_library(TMInstrument SHARED
        ${PLUGIN_DIR}/passes/TMInstrumentPass.cpp
    )

    target_link_libraries(TMInstrument PRIVATE LLVM::Core LLVM::Analysis)

    set_target_properties(TMInstrument PROPERTIES
        PREFIX ""
        OUTPUT_NAME "TMInstrument"
        SUFFIX ".so"
    )

    install(TARGETS TMInstrument
        LIBRARY DESTINATION lib
    )
endif()

# ============================================================================
# Plugin Tests
# ============================================================================

if(BUILD_TESTS)
    set(TEST_NAMES
        types
        memtest
        nested
        threads
    )

    foreach(test_name ${TEST_NAMES})
        set(test_src ${CMAKE_SOURCE_DIR}/tests/plugin/${test_name}.cpp)

        # Step 1: Compile to bitcode (with -fno-inline)
        add_custom_command(OUTPUT ${test_name}.bc
            COMMAND ${LLVM_CLANG} -O1 -fno-inline -emit-llvm -c ${test_src} -o ${test_name}.bc
            DEPENDS ${test_src}
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        )

        # Step 2: Run TM instrumentation plugin
        add_custom_command(OUTPUT ${test_name}.instr.bc
            COMMAND ${LLVM_OPT} -load-pass-plugin=${CMAKE_SOURCE_DIR}/plugin/bin/libTMInstrument.so
                -passes=tm-instrument ${test_name}.bc -o ${test_name}.instr.bc
            DEPENDS ${test_name}.bc TMInstrument
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        )

        # Step 3: Optimize instrumented IR (inline cloned methods)
        add_custom_command(OUTPUT ${test_name}.opt.bc
            COMMAND ${LLVM_OPT} -O3 ${test_name}.instr.bc -o ${test_name}.opt.bc
            DEPENDS ${test_name}.instr.bc
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        )

        # Step 4: Create executable
        add_executable(${test_name}
            ${test_name}.opt.bc
            ${PLUGIN_DIR}/runtime/tm_runtime.cpp
        )

        target_link_libraries(${test_name} PRIVATE pthread)

        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
endif()

# ============================================================================
# Benchmarks
# ============================================================================

if(BUILD_BENCHMARKS)
    # Bank benchmark
    add_custom_target(bank_benchmark ALL
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/plugin/bank/Makefile bank_singlelock
        DEPENDS TMInstrument
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/plugin/bank
    )

    # Data structures
    add_custom_target(datastructures_benchmark
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/datastructures/Makefile bin/avltree_SingleGlobalLock
        DEPENDS TMInstrument
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/datastructures
    )
endif()

# ============================================================================
# Summary
# ============================================================================

message(STATUS "")
message(STATUS "TM API C++ Configuration")
message(STATUS "=========================")
message(STATUS "CMake:           ${CMAKE_MAJOR_VERSION}.${CMAKE_MINOR_VERSION}.${CMAKE_PATCH_VERSION}")
message(STATUS "Build Type:     ${CMAKE_BUILD_TYPE}")
message(STATUS "STM Backend:    ${STM_BACKEND}")
message(STATUS "LLVM Version:   ${LLVM_PACKAGE_VERSION}")
message(STATUS "Source Dir:     ${CMAKE_SOURCE_DIR}")
message(STATUS "Binary Dir:    ${CMAKE_BINARY_DIR}")
message(STATUS "")
message(STATUS "Features:")
message(STATUS "  Build Plugin:    ${BUILD_PLUGIN}")
message(STATUS "  Build Benchmarks: ${BUILD_BENCHMARKS}")
message(STATUS "  Build Tests:      ${BUILD_TESTS}")
message(STATUS "")

enable_testing()