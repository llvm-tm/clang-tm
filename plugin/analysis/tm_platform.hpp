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

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include <string>

namespace tm_platform
{

// -------------------------------------------------------------------------
// Sigsetjmp function name
//
// On Linux/glibc, LLVM/clang emits calls to `__sigsetjmp` (the glibc
// internal name).  On other POSIX systems (macOS, BSD), the standard
// `sigsetjmp` is used.  We detect via the LLVM target triple string.
// -------------------------------------------------------------------------
inline const char *sigsetjmpName(llvm::Module &M)
{
	std::string triple = M.getTargetTriple().str();
	if (triple.find("linux") != std::string::npos ||
	    triple.find("-gnu") != std::string::npos)
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

// -------------------------------------------------------------------------
// Itanium ABI operator new/delete name matching
//
// These mangled names are used by clang/GCC on Linux, macOS, and BSD.
// When porting to Windows/MSVC, the mangling scheme is different and
// these tables must be updated.
// -------------------------------------------------------------------------

inline bool isOperatorNew(llvm::StringRef N)
{
	return N == "malloc" || N == "_Znwm" || N == "_Znam" || N == "_Znwj" ||
	       N == "_Znaj" || N == "_ZnwmSt11align_val_t" ||
	       N == "_ZnamSt11align_val_t";
}

inline bool isOperatorDelete(llvm::StringRef N)
{
	return N == "free" || N == "_ZdlPv" || N == "_ZdlPvm" || N == "_ZdaPv" ||
	       N == "_ZdaPvm" || N == "_ZdlPvSt11align_val_t" ||
	       N == "_ZdlPvmSt11align_val_t" || N == "_ZdaPvSt11align_val_t" ||
	       N == "_ZdaPvmSt11align_val_t";
}

// -------------------------------------------------------------------------
// Itanium ABI STL container function name matching
//
// Checks if a mangled function name belongs to a C++ standard library
// container (std::vector, std::string, etc.).  These functions' internal
// buffer allocations must NOT go through tm_malloc (spec_alloc would free
// them on TX abort while the container's in-memory pointer still references
// the buffer).
//
// Itanium ABI prefix patterns:
//   _ZNSt  = std:: (non-const method)
//   _ZNKSt = std:: (const method)
//   _ZN9__gnu_cxx = __gnu_cxx:: (libstdc++ extensions)
// -------------------------------------------------------------------------
inline bool isSTLContainerFunction(llvm::StringRef Name)
{
	return Name.starts_with("_ZNSt") || Name.starts_with("_ZNKSt") ||
	       Name.starts_with("_ZN9__gnu_cxx");
}

// -------------------------------------------------------------------------
// Heap allocation/deallocation call detection
//
// These functions check if a Value is a CallBase whose callee is a known
// heap allocator or deallocator.  They cover both the project's own TM
// runtime functions (tm_malloc, tm_free) and platform-specific names
// (malloc, operator new, etc.).
// -------------------------------------------------------------------------

inline bool isHeapAllocationCall(const llvm::Value *V)
{
	const auto *Call = llvm::dyn_cast<llvm::CallBase>(V);
	if (!Call)
		return false;
	const llvm::Function *F = Call->getCalledFunction();
	if (!F)
		return false;
	llvm::StringRef Name = F->getName();
	return isOperatorNew(Name) || Name == "calloc" || Name == "realloc" ||
	       Name == "strdup" || Name == "tm_malloc" || Name == "tm_calloc" ||
	       Name == "tm_realloc";
}

inline bool isDeallocationCall(const llvm::Value *V)
{
	const auto *Call = llvm::dyn_cast<llvm::CallBase>(V);
	if (!Call)
		return false;
	const llvm::Function *F = Call->getCalledFunction();
	if (!F)
		return false;
	return isOperatorDelete(F->getName());
}

} // namespace tm_platform
