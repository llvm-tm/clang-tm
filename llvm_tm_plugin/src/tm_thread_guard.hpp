// tm_thread_guard.hpp
// Thread initialization/exit with guard variable
//
// PURPOSE: Thread entry points (functions that use TM but aren't transactions)
//          need to call tm_init_thread() and tm_exit_thread(). However, if a
//          function is called multiple times from the same thread, we only want
//          to initialize once. The guard variable (tm_thread_ready) ensures:
//          - tm_init_thread() called only once per thread
//          - tm_exit_thread() called only once per thread
//
// The guard variable is thread-local, so each thread has its own copy.
//
// NOTE: The runtime functions tm_init_thread() and tm_exit_thread() should
//       check the guard variable themselves for idempotency.
//       This avoids complex IR block manipulation that can create broken IR.

#ifndef TM_THREAD_GUARD_HPP
#define TM_THREAD_GUARD_HPP

#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>

#include "tm_debug.hpp"

using namespace llvm;

// Insert thread initialization
// PURPOSE: Call tm_init_thread() at thread entry point.
//          The runtime should check guard variable for idempotency.
// NOTE: We insert the call directly without IR-level guard check to avoid
//       creating broken IR (orphan blocks without terminators).
static void insertThreadInitWithGuard(IRBuilder<> &Builder,
                                      FunctionCallee tm_init_thread,
                                      GlobalVariable *ThreadReadyGV)
{
	TM_DEBUG("Inserting thread init");
	// Store 1 to guard variable (mark as initialized)
	Builder.CreateStore(ConstantInt::get(Type::getInt8Ty(Builder.getContext()), 1),
	                    ThreadReadyGV);
	// Call tm_init_thread (runtime should be idempotent)
	Builder.CreateCall(tm_init_thread, {});
}

// Insert thread exit
// PURPOSE: Call tm_exit_thread() at thread exit point.
//          The runtime should check guard variable for idempotency.
// NOTE: We insert the call directly without IR-level guard check to avoid
//       creating broken IR.
static void insertThreadExitWithGuard(IRBuilder<> &Builder,
                                      FunctionCallee tm_exit_thread,
                                      GlobalVariable *ThreadReadyGV)
{
	TM_DEBUG("Inserting thread exit");
	// Store 0 to guard variable (mark as not initialized)
	Builder.CreateStore(ConstantInt::get(Type::getInt8Ty(Builder.getContext()), 0),
	                    ThreadReadyGV);
	// Call tm_exit_thread (runtime should be idempotent)
	Builder.CreateCall(tm_exit_thread, {});
}

#endif // TM_THREAD_GUARD_HPP
