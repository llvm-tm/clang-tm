#pragma once

#include <cstddef>

// ============================================================================
// TM Opaque Safe Function Table
//
// This file defines the default set of functions that are safe to call from
// TM contexts even though their bodies are not visible to the TM plugin
// (defined in external shared libraries).  The TM plugin uses this list to
// suppress opaque-function errors.
//
// To customize for your project:
//   1. Copy this file into your project's include path.
//   2. Modify TM_OPAQUE_SAFE_TABLE to add/remove entries.
//   3. Build the plugin with your include path (-I).
//
// Each entry: (symbol_name, is_prefix)
//   symbol_name — function name (mangled C++ or plain C)
//   is_prefix   — 1 if the entry is a prefix match, 0 for exact match
// ============================================================================

struct TMOpaqueSafeEntry {
	const char *Name;
	bool IsPrefix;
};

#ifndef TM_OPAQUE_SAFE_TABLE
#define TM_OPAQUE_SAFE_TABLE(X)                                                          \
	X("tm_", 1)                                                                          \
	X("sigsetjmp", 0)                                                                    \
	X("siglongjmp", 0) X("longjmp", 0) X("malloc", 0) X("free", 0) X("calloc", 0)        \
	    X("realloc", 0) X("aligned_alloc", 0) X("_Znw", 1) X("_Zna", 1) X("_Zdl", 1)     \
	        X("_Zda", 1) X("llvm.", 1) X("__cxa_", 1) X("_ZSt", 1) X("_ZNSt", 1)         \
	            X("_ZNKSt", 1) X("_Unwind", 1) X("pthread_", 1) X("printf", 0)           \
	                X("fprintf", 0) X("fflush", 0) X("puts", 0) X("putchar", 0)          \
	                    X("putc", 0) X("snprintf", 0) X("sprintf", 0) X("exit", 0)       \
	                        X("abort", 0) X("strlen", 0) X("strcmp", 0) X("strncmp", 0)  \
	                            X("strtol", 0) X("strtoul", 0) X("strtoll", 0)           \
	                                X("strtoull", 0) X("strtof", 0) X("strtod", 0)       \
	                                    X("strtold", 0) X("atoi", 0) X("atol", 0)        \
	                                        X("atoll", 0) X("atof", 0) X("abs", 0)       \
	                                            X("labs", 0) X("llabs", 0) X("bzero", 0) \
	                                                X("memcmp", 0) X("memcpy", 0)        \
	                                                    X("memset", 0) X("memmove", 0)   \
	                                                        X("wmemchr", 0)              \
	                                                            X("posix_memalign", 0)   \
	                                                                X("drand48", 0)      \
	                                                                    X("srand48", 0)
#endif

#define TM_OPAQUE_SAFE_ENTRY(name, isprefix) {name, isprefix != 0},
static constexpr TMOpaqueSafeEntry TM_KNOWN_SAFE_TABLE[] = {
    TM_OPAQUE_SAFE_TABLE(TM_OPAQUE_SAFE_ENTRY)};
#undef TM_OPAQUE_SAFE_ENTRY

static constexpr size_t TM_KNOWN_SAFE_TABLE_SIZE = sizeof(TM_KNOWN_SAFE_TABLE) /
                                                   sizeof(TM_KNOWN_SAFE_TABLE[0]);
