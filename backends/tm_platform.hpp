#pragma once

// =========================================================================
// Platform detection for the TM runtimes (backends/).
//
// Centralizes OS/libc/compiler feature detection so that adding support for
// a new platform only requires editing this file.  Backend code should never
// use `#if defined(__linux__)` or similar directly — use the helpers below.
//
// To add a new platform:
//   1. Add a new `#elif defined(__NEW_PLATFORM__)` block to the appropriate
//      section below.
//   2. If the new platform needs a custom stack-bound detection method,
//      provide it via TM_GET_STACK_BOUNDS.
//   3. Verify with `make -C backends/tests run`.
// =========================================================================

#include <cstddef>
#include <cstdint>

// =========================================================================
// Feature-detection macros
//
// Each macro is set to 1 if the feature is available, 0 otherwise.
// Platform-specific blocks below define the right values.
//
// To add a new platform, add an `#elif` block to each section below.
// =========================================================================

// --- Stack-bound detection ---
// TM_HAVE_PTHREAD_GETATTR_NP:        Linux glibc (pthread_getattr_np)
// TM_HAVE_PTHREAD_GET_STACKADDR_NP:  macOS (pthread_get_stackaddr_np)
#ifndef TM_HAVE_PTHREAD_GETATTR_NP
#  if defined(__linux__) && !defined(__ANDROID__)
#    define TM_HAVE_PTHREAD_GETATTR_NP 1
#  else
#    define TM_HAVE_PTHREAD_GETATTR_NP 0
#  endif
#endif

#ifndef TM_HAVE_PTHREAD_GET_STACKADDR_NP
#  if defined(__APPLE__)
#    define TM_HAVE_PTHREAD_GET_STACKADDR_NP 1
#  else
#    define TM_HAVE_PTHREAD_GET_STACKADDR_NP 0
#  endif
#endif

// --- Thread-local storage ---
// C++11 thread_local is preferred; fall back to GNU __thread for old toolchains.
#ifndef TM_THREAD_LOCAL
#  if __cplusplus >= 201103L && !defined(TM_FORCE_THREAD)
#    define TM_THREAD_LOCAL thread_local
#  else
#    define TM_THREAD_LOCAL __thread
#  endif
#endif

// --- execinfo / backtrace support ---
#ifndef TM_HAVE_EXECINFO
#  if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
#    define TM_HAVE_EXECINFO 1
#  else
#    define TM_HAVE_EXECINFO 0
#  endif
#endif

// --- Intel TSX (x86 transactional memory extensions) ---
#ifndef TM_HAVE_INTEL_TSX
#  if (defined(__x86_64__) || defined(__i386__)) && !defined(__ANDROID__)
#    define TM_HAVE_INTEL_TSX 1
#  else
#    define TM_HAVE_INTEL_TSX 0
#  endif
#endif

// --- Conditional system includes (based on feature macros above) ---
#if TM_HAVE_PTHREAD_GETATTR_NP || TM_HAVE_PTHREAD_GET_STACKADDR_NP
#include <pthread.h>
#endif

// =========================================================================
// Stack-bound initialization
//
// Fills *out_low (lowest valid stack address) and *out_high (highest valid
// stack address + 1).  Returns true on success.  On failure, out_low and
// out_high are left unchanged and the caller should use fallback heuristics.
//
// To add a new platform, add an `#elif` block below that sets *out_low and
// *out_high using the platform's native API.
// =========================================================================
inline bool tm_get_stack_bounds(void **out_low, void **out_high) {
#if TM_HAVE_PTHREAD_GET_STACKADDR_NP
  // macOS: pthread_get_stackaddr_np returns the HIGH end of the stack.
  // Stack grows downward from high to low.  Valid range:
  //   [high - stack_size, high)
  void *high = pthread_get_stackaddr_np(pthread_self());
  size_t sz = pthread_get_stacksize_np(pthread_self());
  *out_high = high;
  *out_low  = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(high) - sz);
  return true;

#elif TM_HAVE_PTHREAD_GETATTR_NP
  // Linux glibc: pthread_attr_getstack gives base (low end) and size.
  // Valid range: [base, base + size)
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) == 0) {
    void *base;
    size_t sz;
    pthread_attr_getstack(&attr, &base, &sz);
    pthread_attr_destroy(&attr);
    *out_low  = base;
    *out_high = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(base) + sz);
    return true;
  }
  return false;

#else
  // Unsupported platform — rely on frame-pointer heuristics in isStackAddress.
  (void)out_low;
  (void)out_high;
  return false;
#endif
}
