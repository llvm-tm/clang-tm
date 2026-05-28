#pragma once

// =========================================================================
// Platform detection for the LLVM TM plugin.
//
// Centralizes OS/libc/compiler feature detection so that adding support for
// a new platform only requires editing this file.  Plugin code should never
// use `#if defined(__linux__)` or similar directly — use the helpers below.
//
// To add a new platform:
//   1. Add a `#elif defined(__NEW_PLATFORM__)` block to the helpers below.
//   2. Verify with `make -C llvm_tm_plugin run`.
// =========================================================================

#include "llvm/IR/Module.h"
#include <string>

namespace tm_platform {

// -------------------------------------------------------------------------
// Sigsetjmp function name
//
// On Linux/glibc, LLVM/clang emits calls to `__sigsetjmp` (the glibc
// internal name).  On other POSIX systems (macOS, BSD), the standard
// `sigsetjmp` is used.  We detect via the LLVM target triple string.
// -------------------------------------------------------------------------
inline const char *sigsetjmpName(llvm::Module &M) {
  // glibc-based systems use __sigsetjmp; all others use sigsetjmp.
  std::string triple = M.getTargetTriple().str();
  if (triple.find("linux") != std::string::npos ||
      triple.find("-gnu")  != std::string::npos)
    return "__sigsetjmp";
  return "sigsetjmp";
}

// -------------------------------------------------------------------------
// Thread-local storage keyword
//
// C++11 `thread_local` is the standard; some older toolchains may need
// the GNU `__thread` extension.  LLVM plugin code typically does not
// use TLS directly — this is provided for completeness.
// -------------------------------------------------------------------------
#if __cplusplus >= 201103L
#define TM_THREAD_LOCAL thread_local
#else
#define TM_THREAD_LOCAL __thread
#endif

} // namespace tm_platform
