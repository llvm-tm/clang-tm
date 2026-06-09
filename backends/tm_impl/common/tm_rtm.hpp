#pragma once

#include <cstdint>
#include <cstdio>

#if defined(__x86_64__) || defined(__i386__)
  #include <cpuid.h>
  #include <immintrin.h>
#endif

namespace tm_rtm {

/// Returns true if RTM transactions are both CPUID-reported AND functional
/// (verified by a live _xbegin() probe).  Result is cached after the first
/// call so this is safe to invoke early in tm_init() or tm_init_thread().
inline bool available()
{
#if defined(__x86_64__) || defined(__i386__)
    static int cached = -1;
    if (cached < 0) {
        unsigned int a = 0, b = 0, c = 0, d = 0;
        __cpuid_count(7, 0, a, b, c, d);
        if (b & (1 << 11)) {
            unsigned status = _xbegin();
            if (status == _XBEGIN_STARTED) {
                _xend();
                cached = 1;
            } else {
                fprintf(stderr,
                    "[tm_rtm] CPUID RTM bit set but _xbegin() fails "
                    "(disabled by microcode/BIOS) -- using SGL fallback\n");
                cached = 0;
            }
        } else {
            fprintf(stderr, "[tm_rtm] RTM not available -- using SGL fallback\n");
            cached = 0;
        }
    }
    return cached > 0;
#else
    return false;
#endif
}

} // namespace tm_rtm
