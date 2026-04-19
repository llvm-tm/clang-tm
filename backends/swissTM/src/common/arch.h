#ifndef WLPDSTM_ARCH_H_
#define WLPDSTM_ARCH_H_

#include <cstdint>
#include <cstddef>

#if defined(__GNUC__) || defined(__clang__)
#include <x86intrin.h>
#endif

namespace wlpdstm {

inline std::size_t read_sp() {
#ifdef WLPDSTM_X86
    std::size_t ret;
    #if defined(__GNUC__) || defined(__clang__)
    __asm__ volatile ("mov %%rsp, %0" : "=r" (ret));
    #else
    ret = 0;
    #endif
    return ret;
#else
    return 0;
#endif
}

inline std::size_t read_bp() {
#ifdef WLPDSTM_X86
    std::size_t ret;
    #if defined(__GNUC__) || defined(__clang__)
    __asm__ volatile ("mov %%rbp, %0" : "=r" (ret));
    #else
    ret = 0;
    #endif
    return ret;
#else
    return 0;
#endif
}

}

#endif