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
//   3. Stack-address detection          (isStackAddress)
//   4. Backtrace / crash diagnostics    (tm_backtrace_print)
//   5. CPU relax / spin-loop hint       (tm_cpu_relax)
//   6. Cycle-accurate timestamp         (tm_timestamp)
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
  #define TM_PLATFORM_HAS_EXECINFO 1
#elif defined(__APPLE__)
  // macOS provides backtrace/backtrace_symbols_fd from <execinfo.h>
  // (Apple's own implementation, not glibc).
  #define TM_PLATFORM_HAS_EXECINFO 1
#else
  #define TM_PLATFORM_HAS_EXECINFO 0
#endif

// ── 2. System includes (selected per platform) ───────────────────

// POSIX threads (for isStackAddress)
#if TM_PLATFORM_POSIX
  #include <pthread.h>
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

// ── 3. Stack-address detection ───────────────────────────────────
//
// Stack-allocated data is thread-private and must never go through TM
// read/write — write-back to a popped stack frame would corrupt active
// stack data, causing jump-to-garbage crashes.
//
// Used as the FIRST check in every backend's read_word_XX / write_word_XX.
// When the address is on the calling thread's stack, the function
// bypasses all TM protocol (no lock, no write-set/read-set entry).
//
// Adding a new platform:
//   1. Add a new #elif block below.
//   2. Use the platform's thread API to get stack base + size.
//   3. Return true if addr is within [base, base+size).
//   4. Do NOT call malloc/new / do I/O inside (signal-unsafe callers).

namespace stm
{

inline bool isStackAddress(void const *addr)
{
#if defined(__APPLE__)
	// macOS / iOS: pthread_get_stackaddr_np returns the HIGH address
	// of the stack (stack grows downward).
	pthread_t self = pthread_self();
	void *stack_addr = pthread_get_stackaddr_np(self);
	size_t stack_size = pthread_get_stacksize_np(self);
	void *stack_bottom = (char *)stack_addr - stack_size;
	return (addr >= stack_bottom && addr < stack_addr);

#elif defined(__linux__) || defined(__linux)
	// Linux: pthread_getattr_np + pthread_attr_getstack.
	pthread_attr_t attr;
	void *stack_addr;
	size_t stack_size;
	pthread_getattr_np(pthread_self(), &attr);
	pthread_attr_getstack(&attr, &stack_addr, &stack_size);
	pthread_attr_destroy(&attr);
	void *stack_end = (char *)stack_addr + stack_size;
	return (addr >= stack_addr && addr < stack_end);

#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
	// BSD: pthread_attr_get_np (different name from Linux).
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_get_np(pthread_self(), &attr);
	void *stack_addr;
	size_t stack_size;
	pthread_attr_getstack(&attr, &stack_addr, &stack_size);
	pthread_attr_destroy(&attr);
	void *stack_end = (char *)stack_addr + stack_size;
	return (addr >= stack_addr && addr < stack_end);

#elif defined(_WIN32) || defined(_WIN64)
	// Windows: GetCurrentThreadStackLimits (Vista+).
	ULONG_PTR low, high;
	GetCurrentThreadStackLimits(&low, &high);
	uintptr_t a = (uintptr_t)addr;
	return (a >= (uintptr_t)low && a < (uintptr_t)high);

#elif defined(__sun)
	// Solaris / illumos: thr_stksegment.
	stack_t stk;
	thr_stksegment(&stk);
	uintptr_t a = (uintptr_t)addr;
	uintptr_t base = (uintptr_t)stk.ss_sp;
	uintptr_t end = base + stk.ss_size;
	return (a >= base && a < end);

#else
	// Unsupported platform: return false (stack writes go through TM).
	// If you see "jump to garbage" crashes, add a detection block above.
	(void)addr;
	return false;
#endif
}


// ── 4. Portable backtrace (crash diagnostics) ────────────────────
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
	__builtin_arm_yield();
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

} // namespace stm
