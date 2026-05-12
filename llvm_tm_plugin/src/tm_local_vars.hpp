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
static const Value *getBaseObject(const Value *Ptr)
{
  const Value *Result = Ptr;
  for (int i = 0; i < 10 && Result; i++) {
    Result = Result->stripPointerCasts();
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

// Check if a call instruction is a heap allocation function
static bool isHeapAllocationCall(const Value *V) {
  const auto *Call = dyn_cast<CallInst>(V);
  if (!Call) return false;
  const Function *F = Call->getCalledFunction();
  if (!F) return false;
  StringRef Name = F->getName();
  return Name == "_Znwm" || Name == "_Znam" ||
         Name == "_Znwj" || Name == "_Znaj" ||
         Name == "malloc" || Name == "calloc" ||
         Name == "realloc" || Name == "strdup";
}

// Check if a call instruction is a heap deallocation function
static bool isDeallocationCall(const Value *V) {
  const auto *Call = dyn_cast<CallInst>(V);
  if (!Call) return false;
  const Function *F = Call->getCalledFunction();
  if (!F) return false;
  StringRef Name = F->getName();
  return Name == "_ZdlPv" || Name == "_ZdlPvm" ||
         Name == "_ZdaPv" || Name == "_ZdaPvm" ||
         Name == "free";
}

// Collect all local (stack) variables in a function
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

// Check if an allocation result's uses escape to non-local memory
static bool escapesToNonLocal(Value *Alloc, Function &F) {
  SmallPtrSet<Value *, 32> Visited;
  SmallVector<Value *, 32> Worklist;
  Worklist.push_back(Alloc);
  Visited.insert(Alloc);

  while (!Worklist.empty()) {
    Value *V = Worklist.pop_back_val();
    for (User *U : V->users()) {
      if (!Visited.insert(U).second) continue;
      if (auto *Store = dyn_cast<StoreInst>(U)) {
        if (Store->getValueOperand() == V) {
          const Value *Target = getBaseObject(Store->getPointerOperand());
          if (isa<GlobalVariable>(Target) || isa<Argument>(Target))
            return true;
        }
        continue;
      }
      if (auto *Call = dyn_cast<CallBase>(U)) {
        if (isHeapAllocationCall(Call)) continue;
        if (isDeallocationCall(Call)) continue;
        Function *Callee = Call->getCalledFunction();
        if (Callee && Callee->isIntrinsic()) continue;
        if (Callee != &F)
          return true;
        continue;
      }
      if (isa<PHINode>(U) || isa<GEPOperator>(U) ||
          isa<BitCastInst>(U) || isa<SelectInst>(U) ||
          isa<PtrToIntInst>(U) || isa<IntToPtrInst>(U)) {
        Worklist.push_back(U);
      }
      if (isa<ReturnInst>(U)) return true;
    }
  }
  return false;
}

// Check if a pointer originates from a local variable or in-function heap allocation
static bool originatesFromLocal(Value *Ptr,
                              const SmallPtrSet<const Value *, 32> &LocalVars,
                              Function *CurrentFunc = nullptr,
                              int Depth = 0)
{
  if (Depth > 15) {
    TM_DEBUG("Max recursion depth reached in originatesFromLocal");
    return false;
  }

  Ptr = Ptr->stripPointerCasts();

  if (LocalVars.count(Ptr)) return true;

  if (auto *GEP = dyn_cast<GEPOperator>(Ptr)) {
    return originatesFromLocal(const_cast<Value *>(GEP->getPointerOperand()),
                             LocalVars, CurrentFunc, Depth + 1);
  }

  if (auto *Load = dyn_cast<LoadInst>(Ptr)) {
    return originatesFromLocal(Load->getPointerOperand(), LocalVars, CurrentFunc, Depth + 1);
  }

  // Handle phi nodes: all incoming values must be local
  if (auto *Phi = dyn_cast<PHINode>(Ptr)) {
    for (Value *Incoming : Phi->incoming_values()) {
      Incoming = Incoming->stripPointerCasts();
      if (isa<ConstantPointerNull>(Incoming) || isa<UndefValue>(Incoming))
        continue;
      if (!originatesFromLocal(Incoming, LocalVars, CurrentFunc, Depth + 1))
        return false;
    }
    return true;
  }

  // Handle select: both operands must be local
  if (auto *Select = dyn_cast<SelectInst>(Ptr)) {
    return originatesFromLocal(Select->getTrueValue(), LocalVars, CurrentFunc, Depth + 1) &&
           originatesFromLocal(Select->getFalseValue(), LocalVars, CurrentFunc, Depth + 1);
  }

  // Handle inttoptr
  if (auto *IToP = dyn_cast<IntToPtrInst>(Ptr)) {
    return originatesFromLocal(IToP->getOperand(0), LocalVars, CurrentFunc, Depth + 1);
  }

  // Handle function calls (CallInst and InvokeInst)
  if (auto *Call = dyn_cast<CallBase>(Ptr)) {
    // In-function heap allocation that doesn't escape → local
    if (CurrentFunc && isHeapAllocationCall(Call) &&
        Call->getFunction() == CurrentFunc) {
      if (!escapesToNonLocal(Call, *CurrentFunc)) {
        TM_DEBUG("In-function heap allocation that doesn't escape → local");
        return true;
      }
      TM_DEBUG("In-function heap allocation that escapes → not local");
      return false;
    }
    // Check if any pointer argument traces to a local variable
    for (auto &Arg : Call->args()) {
      if (Arg->getType()->isPointerTy()) {
        if (originatesFromLocal(const_cast<Value *>(&*Arg), LocalVars, CurrentFunc, Depth + 1))
          return true;
      }
    }
    return false;
  }

  return false;
}

// Overload without CurrentFunc for backward compatibility
static bool originatesFromLocal(Value *Ptr,
                              const SmallPtrSet<const Value *, 32> &LocalVars,
                              int Depth = 0)
{
  return originatesFromLocal(Ptr, LocalVars, nullptr, Depth);
}

// Determine if a pointer accesses shared (non-local) data
static bool isSharedPointer(Value *Ptr,
                           const SmallPtrSet<const Value *, 32> &LocalVars,
                           Function &F,
                           Module &M,
                           int DepthLimit = 15)
{
  if (originatesFromLocal(Ptr, LocalVars, &F)) {
    TM_DEBUG("Pointer is local (from alloca or in-function allocation)");
    return false;
  }

  const Value *Base = getBaseObject(Ptr);
  if (auto *GV = dyn_cast<GlobalVariable>(Base)) {
    if (GV->isThreadLocal()) {
      TM_DEBUG("Pointer is thread-local global: %s", GV->getName().str().c_str());
      return false;
    }
    TM_DEBUG("Pointer is shared global: %s", GV->getName().str().c_str());
    return true;
  }

  // Handle phi nodes as base objects
  if (auto *Phi = dyn_cast<PHINode>(Base)) {
    for (const Value *Incoming : Phi->incoming_values()) {
      const Value *Stripped = Incoming->stripPointerCasts();
      if (auto *GV = dyn_cast<GlobalVariable>(Stripped)) {
        if (!GV->isThreadLocal()) {
          TM_DEBUG("PHI has global incoming value → shared");
          return true;
        }
      }
      if (isa<Argument>(Stripped)) {
        TM_DEBUG("PHI has argument incoming value → shared");
        return true;
      }
    }
    TM_DEBUG("PHI has only local/null incoming values → not shared");
    return false;
  }

  TM_DEBUG("Pointer assumed shared (heap/argument)");
  return true;
}

#endif // TM_LOCAL_VARS_HPP