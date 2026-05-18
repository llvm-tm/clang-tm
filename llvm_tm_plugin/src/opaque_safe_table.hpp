#ifndef OPAQUE_SAFE_TABLE_HPP
#define OPAQUE_SAFE_TABLE_HPP

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

struct OpaqueSafeEntry {
    StringRef Name;
    bool     IsPrefix;
};

static const OpaqueSafeEntry KnownSafeOpaqueTable[] = {
    {"tm_",                      true},
    {"sigsetjmp",                false}, {"siglongjmp", false}, {"longjmp", false},
    {"malloc", false}, {"free", false}, {"calloc", false}, {"realloc", false}, {"aligned_alloc", false},
    {"_Znw", true}, {"_Zna", true}, {"_Zdl", true}, {"_Zda", true},
    {"llvm.", true},
    {"__cxa_", true}, {"_ZSt", true}, {"_ZNSt", true}, {"_ZNKSt", true}, {"_Unwind", true},
    {"pthread_", true},
    {"printf", false}, {"fprintf", false}, {"fflush", false}, {"puts", false},
    {"putchar", false}, {"putc", false}, {"snprintf", false}, {"sprintf", false},
    {"exit", false}, {"abort", false},
    {"strlen", false}, {"strcmp", false}, {"strncmp", false},
    {"memcmp", false}, {"memcpy", false}, {"memset", false}, {"memmove", false},
};

static constexpr size_t KnownSafeOpaqueTableSize =
    sizeof(KnownSafeOpaqueTable) / sizeof(KnownSafeOpaqueTable[0]);

// Syscall patterns — symbols matching these patterns are safe (kernel handles races)
static const char *SyscallPrefixes[] = {
    "syscall", "__NR_", "sys_", "__sys_",
    "read", "write", "open", "close", "stat", "fstat", "lstat",
    "mmap", "munmap", "mprotect", "brk", "sbrk",
    "clone", "fork", "vfork", "execve", "wait", "waitpid",
    "getpid", "gettid", "gettid", "getuid", "geteuid", "getgid",
    "nanosleep", "clock_gettime", "gettimeofday", "time",
    "socket", "connect", "bind", "listen", "accept", "send", "recv",
};

static bool isSyscallSymbol(StringRef Name) {
    for (auto *Prefix : SyscallPrefixes)
        if (Name == Prefix || Name.starts_with(Prefix))
            return true;
    return false;
}

static bool isKnownSafeOpaque(const StringRef &Name, bool StrictOpaque = false)
{
    if (StrictOpaque) return false;
    for (size_t i = 0; i < KnownSafeOpaqueTableSize; i++)
        if (KnownSafeOpaqueTable[i].IsPrefix ? Name.starts_with(KnownSafeOpaqueTable[i].Name)
                                              : Name == KnownSafeOpaqueTable[i].Name)
            return true;
    return false;
}

// Emit suggestion flag for a given opaque symbol
static void emitOpaqueSuggestion(StringRef Name, raw_ostream &OS) {
    OS << "  Use one of the following to resolve:\n"
       << "    - Add '" << Name << "' to KnownSafeOpaqueTable in opaque_safe_table.hpp\n"
       << "    - Add __attribute__((annotate(\"tm_allow_opaque\"))) to the call site\n"
       << "    - Pass -tm-allow-opaque to opt to disable opaque checks globally (DANGEROUS)\n";
}

#endif // OPAQUE_SAFE_TABLE_HPP
