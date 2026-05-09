// tm_local_vars.hpp
// Local variable detection and shared pointer analysis
//
// PURPOSE: In TM instrumentation, we need to distinguish between:
//   - LOCAL variables (stack-allocated, alloca): NOT instrumented
//   - SHARED variables (globals, heap, function args): Instrumented with tm_read/tm_write
//
// This file provides utilities to:
//   1. Detect local (stack) variables in a function
//   2. Check if a pointer originates from a local variable
//   3. Determine if a pointer accesses shared (non-local) data

#ifndef TM_LOCAL_VARS_HPP
#define TM_LOCAL_VARS_HPP

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>

#include "tm_debug.hpp"

using namespace llvm;

// Get the base object from a pointer (follows GEP chains)
// PURPOSE: Given a pointer like &arr[3].field, find the original base object
//          (e.g., the alloca or global variable it came from)
static const Value *getBaseObject(const Value *Ptr)
{
  const Value *Result = Ptr;
  for (int i = 0; i < 10 && Result; i++) {
    Result = Result->stripPointerCasts();
    // Follow GEP (GetElementPtr) operators to find the base pointer
    if (const auto *GEP = dyn_cast<const GetElementPtrInst>(Result)) {
      Result = GEP->getPointerOperand();
    } else if (const auto *GEP = dyn_cast<const GEPOperator>(Result)) {
      Result = GEP->getPointerOperand();
    } else {
      break;
    }
  }
  return Result ? Result : Ptr;
}

// Collect all local (stack) variables in a function
// PURPOSE: Build a set of all alloca instructions in the function.
//          These are stack-allocated variables that are private to the
//          function and don't need TM instrumentation.
static void collectLocalVariables(Function &F,
                                SmallPtrSet<const Value *, 32> &LocalVars)
{
  for (auto &BB : F) {
    for (auto &I : BB) {
      if (auto *Alloca = dyn_cast<AllocaInst>(&I)) {
        LocalVars.insert(Alloca);
        TM_DEBUG("Found local variable: %s", Alloca->getName().str().c_str());
      }
    }
  }
  TM_DEBUG("Total local variables in %s: %d", F.getName().str().c_str(), (int)LocalVars.size());
}

// Check if a pointer originates from a local variable
// PURPOSE: Determine if a pointer refers to a local (stack) variable.
//          We trace back through pointer operations (GEP, load) to see
//          if we eventually reach an alloca instruction.
// DEPTH: Prevents infinite recursion from cycles in the IR.
static bool originatesFromLocal(Value *Ptr,
                              const SmallPtrSet<const Value *, 32> &LocalVars,
                              int Depth = 0)
{
  if (Depth > 15) {
    TM_DEBUG("Max recursion depth reached in originatesFromLocal");
    return false; // Prevent infinite loops
  }

  Ptr = Ptr->stripPointerCasts();

  // If the pointer itself is a local variable, return true
  if (LocalVars.count(Ptr)) return true;

  // If it's a GEP, check the pointer operand (follow the chain)
  if (auto *GEP = dyn_cast<GEPOperator>(Ptr)) {
    return originatesFromLocal(const_cast<Value *>(GEP->getPointerOperand()),
                             LocalVars, Depth + 1);
  }
  // If it's loaded from somewhere, check what was loaded
  if (auto *Load = dyn_cast<LoadInst>(Ptr)) {
    return originatesFromLocal(Load->getPointerOperand(), LocalVars, Depth + 1);
  }
  // If it's from a function call, assume it's NOT local (heap or global)
  if (auto *Call = dyn_cast<CallInst>(Ptr)) {
    return false; // Function call: could return heap/global → not local
  }

  return false;
}

// Determine if a pointer accesses shared (non-local) data
// PURPOSE: Main entry point for deciding whether to instrument a load/store.
//          Returns true if the pointer accesses shared data (needs tm_read/tm_write).
//
// Decision logic:
//   1. If pointer originates from local variable → NOT shared (return false)
//   2. If pointer is a thread-local global → NOT shared (return false)
//   3. If pointer is a regular global → SHARED (return true)
//   4. Otherwise (heap, function args) → conservative: assume SHARED
static bool isSharedPointer(Value *Ptr,
                           const SmallPtrSet<const Value *, 32> &LocalVars,
                           Function &F,
                           Module &M,
                           int DepthLimit = 15)
{
  // Check if pointer originates from local variable
  if (originatesFromLocal(Ptr, LocalVars)) {
    TM_DEBUG("Pointer is local (from alloca)");
    return false; // NOT shared (local to this function)
  }

  // Check if pointer is a thread-local global
  const Value *Base = getBaseObject(Ptr);
  if (auto *GV = dyn_cast<GlobalVariable>(Base)) {
    if (GV->isThreadLocal()) {
      TM_DEBUG("Pointer is thread-local global: %s", GV->getName().str().c_str());
      return false; // NOT shared (thread-local)
    }
    TM_DEBUG("Pointer is shared global: %s", GV->getName().str().c_str());
    return true; // Regular global → shared
  }

  // If we get here, it's likely heap-allocated or from a function argument
  // → conservative: assume shared
  TM_DEBUG("Pointer assumed shared (heap/argument)");
  return true;
}

#endif // TM_LOCAL_VARS_HPP
