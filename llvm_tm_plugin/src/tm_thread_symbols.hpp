// tm_thread_symbols.hpp
// Configurable list of thread entry point symbols
//
// PURPOSE: Explicitly detect thread creation functions to properly
//          instrument thread entry points with tm_init_thread/tm_exit_thread.
//          This replaces the heuristic-based detection with explicit symbols.

#ifndef TM_THREAD_SYMBOLS_HPP
#define TM_THREAD_SYMBOLS_HPP

#include <cstddef>

// Configurable global list of thread entry point symbols
// Add future thread creation functions here as needed
// Includes both libc++ (LLVM) and libstdc++ (GCC) mangled names.
static const char *const ThreadEntrySymbols[] = {
    "pthread_create",
    // libc++ (LLVM) std::thread constructors (namespace std::__1::thread)
    "_ZNSt3__16threadC1Em",
    "_ZNSt3__16threadC1ERKNS_6threadE",
    "_ZNSt3__16threadC1EOS0_",
    // libstdc++ (GCC) std::thread constructors (namespace std::thread)
    "_ZNSt6threadC1Em",
    "_ZNSt6threadC1ERKNS_6threadE",
    "_ZNSt6threadC1EOS0_",
    nullptr  // Sentinel value
};

// Check if a function name matches any thread entry symbol
static bool isThreadEntrySymbol(const char *funcName) {
    for (size_t i = 0; ThreadEntrySymbols[i] != nullptr; ++i) {
        if (strcmp(funcName, ThreadEntrySymbols[i]) == 0) {
            return true;
        }
    }
    return false;
}

#endif // TM_THREAD_SYMBOLS_HPP
