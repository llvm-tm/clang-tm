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
        // Under gem5 (GEM5_M5OPS) the X86 CPUID may not advertise RTM
        // (gem5's X86CPUID table doesn't set leaf7 EBX bit11), but the
        // Ruby MESI_Three_Level_HTM still implements XBEGIN/XEND via
        // HTMSequencer. Probe regardless in gem5. Never touch stdio
        // here when in HTM fast path: stderr write() syscalls while
        // holding transactional state become HTM aborts.
#if defined(GEM5_M5OPS)
        bool try_probe = true;
#else
        bool try_probe = (b & (1 << 11));
#endif
        if (try_probe) {
            unsigned status = _xbegin();
            if (status == _XBEGIN_STARTED) {
                _xend();
                cached = 1;
            } else {
#if !defined(GEM5_M5OPS) || defined(TM_RTM_DEBUG)
                fprintf(stderr,
                    "[tm_rtm] RTM probe _xbegin() fails (CPUID.07H:EBX=0x%x status=0x%x) "
                    "-- using SGL fallback\n", b, status);
#endif
                cached = 0;
            }
        } else {
#if !defined(GEM5_M5OPS) || defined(TM_RTM_DEBUG)
            fprintf(stderr, "[tm_rtm] RTM not available (CPUID.07H:EBX=0x%x) -- using SGL fallback\n", b);
#endif
            cached = 0;
        }
    }
    return cached > 0;
#else
    return false;
#endif
}

} // namespace tm_rtm
