#ifndef WLPDSTM_TIMING_H_
#define WLPDSTM_TIMING_H_

#include <cstdint>
#include <chrono>
#include <atomic>
#include <thread>

namespace {

inline uint64_t get_clock_count() {
#if defined(__x86_64__) || defined(_M_X64)
    uint32_t low, high;
    #if defined(__GNUC__) || defined(__clang__)
    __asm__ volatile("rdtsc" : "=a" (low), "=d" (high));
    return (uint64_t)low | (((uint64_t)high) << 32);
    #elif defined(_MSC_VER)
    return __rdtsc();
    #else
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return static_cast<uint64_t>(now);
    #endif
#elif defined(__i386__) || defined(_M_IX86)
    uint64_t ret;
    #if defined(__GNUC__) || defined(__clang__)
    __asm__ volatile ("rdtsc" : "=A" (ret));
    #elif defined(_MSC_VER)
    __asm {
        rdtsc
        mov low, eax
        mov high, edx
    }
    #else
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    ret = static_cast<uint64_t>(now);
    #endif
    return ret;
#else
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return static_cast<uint64_t>(now);
#endif
}

inline void wait_cycles(uint64_t cycles) {
    uint64_t start = get_clock_count();

    while(true) {
        uint64_t end = get_clock_count();
        if(end - start > cycles) {
            break;
        }
    }
}

inline void sleep_ns(uint64_t ns) {
    std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
}

#endif