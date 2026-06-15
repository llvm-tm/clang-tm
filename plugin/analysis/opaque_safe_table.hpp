#ifndef OPAQUE_SAFE_TABLE_HPP
#define OPAQUE_SAFE_TABLE_HPP

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

struct OpaqueSafeEntry {
	StringRef Name;
	bool IsPrefix;
};

static const OpaqueSafeEntry KnownSafeOpaqueTable[] = {
    {"tm_", true},
    {"sigsetjmp", false},
    {"siglongjmp", false},
    {"longjmp", false},
    {"malloc", false},
    {"free", false},
    {"calloc", false},
    {"realloc", false},
    {"aligned_alloc", false},
    {"_Znw", true},
    {"_Zna", true},
    {"_Zdl", true},
    {"_Zda", true},
    {"llvm.", true},
    {"__cxa_", true},
    {"printf", false},
    {"fprintf", false},
    {"fflush", false},
    {"puts", false},
    {"putchar", false},
    {"putc", false},
    {"snprintf", false},
    {"sprintf", false},
    {"_ZSt17__throw_bad_allocv", false},
    {"_ZSt20__throw_length_errorPKc", false},
    {"exit", false},
    {"abort", false},
    {"_ZSt9terminatev", false},
    {"__throw_", true},
    {"_ZSt28__throw_bad_array_new_length", true},
    {"_ZNSt20bad_array_new_length", true},
    {"_ZNSt11logic_error", true},
    {"_ZNSt12length_error", true},
    {"strlen", false},
    {"strcmp", false},
    {"strncmp", false},
    {"strtol", false},
    {"strtoul", false},
    {"strtoll", false},
    {"strtoull", false},
    {"strtof", false},
    {"strtod", false},
    {"strtold", false},
    {"atoi", false},
    {"atol", false},
    {"atoll", false},
    {"atof", false},
    {"abs", false},
    {"labs", false},
    {"llabs", false},
    {"bzero", false},
    {"memcpy", false},
    {"memset", false},
    {"memmove", false},
    {"memcmp", false},
    {"wmemchr", false},
    {"posix_memalign", false},
    {"drand48", false},
    {"srand48", false},
};

static constexpr size_t KnownSafeOpaqueTableSize = sizeof(KnownSafeOpaqueTable) /
                                                   sizeof(KnownSafeOpaqueTable[0]);

// Pure functions that are safe even when called with TM-traced pointer args.
// These are read-only (or stateless) operations that don't modify TM-shared
// memory — their internal loads go through TM reads if needed, and they have
// no stores to TM-shared data.  Separated from KnownSafeOpaqueTable because
// the default check rejects ALL functions with TM-traced args as unsafe.
// NOTE: STL container functions (std::vector, std::map, std::unordered_set, etc.)
// are intentionally NOT listed here.  Previously STL entries were added as
// "exemptions" to let STL containers work inside transactions, but this
// approach is fragile and costly to maintain:
//   - Per-function exemptions never cover all STL variants (debug/checked iterators,
//     different allocators, platform-specific ABI quirks), so gaps remain.
//   - Opaque STL functions bypass TM conflict detection on their internal reads,
//     making silent data corruption possible (e.g., std::lower_bound reading
//     another thread's uncommitted write).
//   - STL container buffer allocations must go through tm_malloc / tm_free
//     like any other TM allocation, not the system heap.  Exempting STL
//     functions from malloc/free interception caused system-heap corruption
//     from TM-instrumented writes past the buffer.
// Instead, use TM-safe data structures (TMSafeMap, TMSafeQueue, SimpleVec)
// inside transactions.
static const OpaqueSafeEntry KnownSafeWithTMArgsTable[] = {
    {"memcmp", false},
    {"strlen", false},
};

// Syscall-related symbols — these are safe inside transactions because
// the kernel handles races for syscalls.  Entries are split into:
//   - ExactMatchSyscallSymbols: exact-name match (prevents false positives
//     like matching user function "read_config" against prefix "read")
//   - SyscallPrefixes: prefix match for obviously syscall-specific prefixes
//     like "syscall", "sys_", "__NR_", "__sys_"
static const char *ExactMatchSyscallSymbols[] = {
    "read",    "write",  "open",      "close",         "stat",         "fstat",  "lstat",
    "mmap",    "munmap", "mprotect",  "brk",           "sbrk",         "clone",  "fork",
    "vfork",   "execve", "wait",      "waitpid",       "getpid",       "gettid", "getuid",
    "geteuid", "getgid", "nanosleep", "clock_gettime", "gettimeofday", "time",   "socket",
    "connect", "bind",   "listen",    "accept",        "send",         "recv",
};

static const char *SyscallPrefixes[] = {
    "syscall",
    "sys_",
    "__NR_",
    "__sys_",
};

static bool isSyscallSymbol(StringRef Name)
{
	for (auto *S : ExactMatchSyscallSymbols)
		if (Name == S)
			return true;
	for (auto *Prefix : SyscallPrefixes)
		if (Name.starts_with(Prefix))
			return true;
	return false;
}

static bool isKnownSafeOpaque(const StringRef &Name, bool StrictOpaque = false)
{
	if (StrictOpaque)
		return false;
	for (size_t i = 0; i < KnownSafeOpaqueTableSize; i++)
		if (KnownSafeOpaqueTable[i].IsPrefix
		        ? Name.starts_with(KnownSafeOpaqueTable[i].Name)
		        : Name == KnownSafeOpaqueTable[i].Name)
			return true;
	return false;
}

// Like isKnownSafeOpaque, but for functions that are safe even with
// TM-traced pointer args (pure/read-only functions with no side effects).
// These are checked separately — when a function has TM-traced args but is
// known to be pure (e.g. memcmp, Prime_rehash_policy::_M_need_rehash),
// it is still allowed as safe.
static bool isKnownSafeWithTMArgs(const StringRef &Name)
{
	for (size_t i = 0;
	     i < sizeof(KnownSafeWithTMArgsTable) / sizeof(KnownSafeWithTMArgsTable[0]);
	     i++)
		if (KnownSafeWithTMArgsTable[i].IsPrefix
		        ? Name.starts_with(KnownSafeWithTMArgsTable[i].Name)
		        : Name == KnownSafeWithTMArgsTable[i].Name)
			return true;
	return false;
}

// Emit suggestion flag for a given opaque symbol
static void emitOpaqueSuggestion(StringRef Name, raw_ostream &OS)
{
	OS << "  Use one of the following to resolve:\n"
	   << "    - Add '" << Name << "' to KnownSafeOpaqueTable in opaque_safe_table.hpp\n"
	   << "    - Add __attribute__((annotate(\"tm_allow_opaque\"))) to the call site\n"
	   << "    - Pass -tm-allow-opaque to opt to disable opaque checks globally "
	      "(DANGEROUS)\n";
}

#endif // OPAQUE_SAFE_TABLE_HPP
