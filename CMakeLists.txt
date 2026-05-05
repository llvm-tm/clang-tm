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

# ============================================================================
# Directories
# ============================================================================

set(PLUGIN_DIR ${CMAKE_SOURCE_DIR}/llvm_tm_plugin)
set(BACKENDS_DIR ${CMAKE_SOURCE_DIR}/backends)
set(RUNTIMES_DIR ${BACKENDS_DIR}/runtimes)
set(BENCHMARKS_DIR ${CMAKE_SOURCE_DIR}/benchmarks)

# ============================================================================
# Plugin Library
# ============================================================================

if(BUILD_PLUGIN)
    add_library(TMInstrument SHARED
        ${PLUGIN_DIR}/src/TMInstrumentPass.cpp
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
        set(test_src ${PLUGIN_DIR}/test/${test_name}.cpp)

        # Compile to bitcode
        add_custom_command(OUTPUT ${test_name}.bc
            COMMAND clang -O1 -fno-inline -emit-llvm -c ${test_src} -o ${test_name}.bc
            DEPENDS ${test_src}
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        )

        # Run plugin
        add_custom_command(OUTPUT ${test_name}.opt.bc
            COMMAND opt -load-pass-plugin=${CMAKE_SOURCE_DIR}/llvm_tm_plugin/bin/libTMInstrument.so
                -passes=tm-instrument ${test_name}.bc -o ${test_name}.opt.bc
            DEPENDS ${test_name}.bc TMInstrument
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        )

        # Create executable
        add_executable(${test_name}
            ${test_name}.opt.bc
            ${PLUGIN_DIR}/tm_implementation/tm_runtime.cpp
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
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/test/bank/Makefile bank_singlelock
        DEPENDS TMInstrument
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/test/bank
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