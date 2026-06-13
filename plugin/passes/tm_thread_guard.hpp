// tm_thread_guard.hpp
// Thread initialization/exit helpers
//
// PURPOSE: Thread entry points (functions that use TM but aren't transactions)
//          need to call tm_init_thread() and tm_exit_thread().
//
// NOTE: The runtime functions tm_init_thread() and tm_exit_thread() must be
//       idempotent (check a thread-local flag internally) — we don't emit a
//       guard variable here because LLVM IR-declared thread_local globals
//       produce broken codegen on macOS arm64 (the variable is placed in
//       __DATA,__thread_vars as a TLV descriptor, causing a null-thunk crash).

#ifndef TM_THREAD_GUARD_HPP
#define TM_THREAD_GUARD_HPP

#include <llvm/IR/IRBuilder.h>

#include "tm_debug.hpp"
#include "tm_runtime_hooks.hpp"

using namespace llvm;

// Insert thread initialization
// PURPOSE: Call tm_init_thread() at thread entry point.
//          The runtime must be idempotent (check its own flag internally).
static void insertThreadInitWithGuard(IRBuilder<> &Builder,
                                      const TMRuntimeHook &tm_init_thread)
{
	TM_DEBUG("Inserting thread init");
	emitHookCall(Builder, tm_init_thread, {});
}

// Insert thread exit
// PURPOSE: Call tm_exit_thread() at thread exit point.
//          The runtime must be idempotent (check its own flag internally).
static void insertThreadExitWithGuard(IRBuilder<> &Builder,
                                      const TMRuntimeHook &tm_exit_thread)
{
	TM_DEBUG("Inserting thread exit");
	emitHookCall(Builder, tm_exit_thread, {});
}

#endif // TM_THREAD_GUARD_HPP
