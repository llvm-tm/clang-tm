#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#include <immintrin.h>
#endif

namespace tm_rtm
{

namespace detail
{

// Cache of the probe result: -1 unknown, 0 no, 1 yes.
inline std::atomic<int> &cached()
{
	static std::atomic<int> v{-1};
	return v;
}

#if defined(__x86_64__) || defined(__i386__)
// The one-time probe: CPUID plus a live _xbegin()/_xend() round trip.
inline bool probe()
{
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
	if (!try_probe) {
#if !defined(GEM5_M5OPS) || defined(TM_RTM_DEBUG)
		fprintf(stderr,
		        "[tm_rtm] RTM not available (CPUID.07H:EBX=0x%x) -- using SGL "
		        "fallback\n",
		        b);
#endif
		return false;
	}
	unsigned status = _xbegin();
	if (status != _XBEGIN_STARTED) {
#if !defined(GEM5_M5OPS) || defined(TM_RTM_DEBUG)
		fprintf(stderr,
		        "[tm_rtm] RTM probe _xbegin() fails (CPUID.07H:EBX=0x%x "
		        "status=0x%x) "
		        "-- using SGL fallback\n",
		        b,
		        status);
#endif
		return false;
	}
	_xend();
	return true;
}
#endif // x86

} // namespace detail

/// Returns true if RTM transactions are both CPUID-reported AND functional
/// (verified by a live _xbegin() probe).  Result is cached after the first
/// call so this is safe to invoke early in tm_init() or tm_init_thread().
///
/// The cache used to be a plain `static int` with an unsynchronised
/// `if (cached < 0)` guard: several threads reaching it at once re-ran the
/// probe (and re-printed the fallback warning), and — the part that matters —
/// they read and wrote the cache concurrently, which is a data race on the C++
/// memory model (ThreadSanitizer reported it as a race on
/// `tm_rtm::available()::cached` from every backend that probes RTM).  The
/// probe now runs exactly once under call_once, so the fast path is a single
/// relaxed atomic load and a thread that arrives during the probe waits for
/// its result instead of starting its own nested _xbegin().
inline bool available()
{
#if defined(__x86_64__) || defined(__i386__)
	static std::once_flag once;
	std::atomic<int> &v = detail::cached();
	if (v.load(std::memory_order_relaxed) < 0) {
		std::call_once(once, []() {
			detail::cached().store(detail::probe() ? 1 : 0, std::memory_order_relaxed);
		});
	}
	return v.load(std::memory_order_relaxed) > 0;
#else
	return false;
#endif
}

} // namespace tm_rtm
