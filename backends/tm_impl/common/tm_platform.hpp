#pragma once

// ═══════════════════════════════════════════════════════════════════
// tm_platform.hpp — Platform-abstraction header for STM backends
//
// Every platform-dependent API call, inline asm, and system header
// should be wrapped here so that adding a new OS or architecture
// requires editing ONLY this file (and its test).
//
// Sections:
//   1. Platform-detection macros
//   2. System includes (selected per platform)
//   3. (removed — address-space detection now in tm_region_allocator.hpp)
//   4. Backtrace / crash diagnostics    (tm_backtrace_print)
//   5. CPU relax / spin-loop hint       (tm_cpu_relax)
//   6. Cycle-accurate timestamp         (tm_timestamp)
//   7. Page size                        (tm_page_size)
// ═══════════════════════════════════════════════════════════════════

#include <cstddef>
#include <cstdint>
#include <cstdio>

// ── 1. Platform-detection helpers ─────────────────────────────────
// These are used throughout the section guards below.
// x86 family
#if defined(__x86_64__) || defined(__i386__) || defined(_M_IX86) || \
    defined(_M_AMD64) || defined(_M_X64)
  #define TM_PLATFORM_X86 1
#else
  #define TM_PLATFORM_X86 0
#endif

// ARM 64-bit
#if defined(__aarch64__) || defined(_M_ARM64)
  #define TM_PLATFORM_ARM64 1
#else
  #define TM_PLATFORM_ARM64 0
#endif

// POSIX family (macOS, Linux, *BSD, Solaris, etc.)
#if defined(__APPLE__) || defined(__linux__) || defined(__linux) ||  \
    defined(__FreeBSD__) || defined(__OpenBSD__) ||                   \
    defined(__NetBSD__) || defined(__sun)
  #define TM_PLATFORM_POSIX 1
#else
  #define TM_PLATFORM_POSIX 0
#endif

// glibc / Linux (where execinfo.h backtrace() is native)
#if defined(__linux__) || defined(__linux)
  // musl (e.g. Alpine) has no <execinfo.h>; probe when possible.
  #if defined(__has_include)
    #if __has_include(<execinfo.h>)
      #define TM_PLATFORM_HAS_EXECINFO 1
    #else
      #define TM_PLATFORM_HAS_EXECINFO 0
    #endif
  #else
    #define TM_PLATFORM_HAS_EXECINFO 1
  #endif
#elif defined(__APPLE__)
  // macOS provides backtrace/backtrace_symbols_fd from <execinfo.h>
  // (Apple's own implementation, not glibc).
  #define TM_PLATFORM_HAS_EXECINFO 1
#else
  #define TM_PLATFORM_HAS_EXECINFO 0
#endif

// ── 2. System includes (selected per platform) ───────────────────

// POSIX threads + sysconf
#if TM_PLATFORM_POSIX
  #include <pthread.h>
  #include <unistd.h>   // sysconf(_SC_PAGESIZE)
#endif

// Windows thread / stack API
#if defined(_WIN32) || defined(_WIN64)
  #include <windows.h>
#endif

// Solaris thr_stksegment
#if defined(__sun)
  #include <thread.h>
#endif

// execinfo.h for backtrace (glibc / macOS)
#if TM_PLATFORM_HAS_EXECINFO
  #include <execinfo.h>
#endif


namespace stm
{

// ── 3. Portable backtrace (crash diagnostics) ────────────────────
//
// tm_backtrace(buf, size)    — fill buffer with return-addresses
// tm_backtrace_print(fd)     — print one-line backtrace to file desc.
//
// When execinfo.h is unavailable, tm_backtrace returns 0 and
// tm_backtrace_print prints a single line with the PC from
// __builtin_return_address(0).

inline int tm_backtrace(void **buf, int size)
{
#if TM_PLATFORM_HAS_EXECINFO
	return ::backtrace(buf, size);
#else
	(void)size;
	if (buf && size > 0) {
		buf[0] = __builtin_return_address(0);
		return 1;
	}
	return 0;
#endif
}

/// Print a backtrace to a file descriptor (stderr = 2).
/// Falls back to "no backtrace available" on unsupported platforms.
inline void tm_backtrace_print(int fd)
{
#if TM_PLATFORM_HAS_EXECINFO
	void *frames[64];
	int n = tm_backtrace(frames, 64);
	if (n > 0)
		backtrace_symbols_fd(frames, n, fd);
	else
		dprintf(fd, "(empty backtrace)\n");
#else
	// Fallback: print the caller's return address.
	dprintf(fd, "bt: pc=%p\n", __builtin_return_address(0));
#endif
}

/// Like tm_backtrace_print but includes a labelled value.
inline void tm_backtrace_print_labeled(int fd,
                                        const char *label,
                                        uint64_t val)
{
	dprintf(fd, "%s: 0x%llx\n", label, (unsigned long long)val);
	tm_backtrace_print(fd);
}


// ── 5. CPU relax / spin-loop hint ────────────────────────────────
//
// Emits an architecture-appropriate instruction that hints the CPU
// it is in a spin-wait loop (pause on x86, yield on ARM).  This
// reduces power consumption and improves hyper-thread fairness.

inline void tm_cpu_relax()
{
#if TM_PLATFORM_X86
	__builtin_ia32_pause();
#elif TM_PLATFORM_ARM64
	__asm__ __volatile__("yield" ::: "memory");
#else
	// No hint available on this architecture.
#endif
}


// ── 6. Cycle-accurate timestamp (for event logging) ──────────────
//
// Returns a low-overhead, monotonically-non-decreasing cycle counter.
// Used by the event logger to order events across threads.
// Falls back to 0 on architectures without a user-mode cycle counter.

inline uint64_t tm_timestamp()
{
#if TM_PLATFORM_X86
	uint32_t lo, hi;
	asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
#elif TM_PLATFORM_ARM64
	uint64_t val;
	asm volatile("isb; mrs %0, cntvct_el0" : "=r"(val));
	return val;
#else
	return 0;
#endif
}


// ── 7. Page size ──────────────────────────────────────────────────
//
// Returns the system's memory page size in bytes.  Used by the TM
// region allocator to compute slab sizes that are page-aligned.
// Some TM implementations (page-protection-based conflict detection,
// hardware page-locking) rely on page granularity — slab sizes must
// be exact multiples.

inline long tm_page_size()
{
#if TM_PLATFORM_POSIX
	long ps = ::sysconf(_SC_PAGESIZE);
	return ps > 0 ? ps : 4096;
#elif defined(_WIN32) || defined(_WIN64)
	SYSTEM_INFO si;
	::GetSystemInfo(&si);
	return static_cast<long>(si.dwPageSize);
#else
	return 4096;
#endif
}


} // namespace stm
