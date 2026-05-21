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

#include "tm_annotation_utils.hpp"
#include "tm_debug.hpp"

// Collect all return instructions in a function (avoids iterator-invalidation bugs).
static SmallVector<ReturnInst *, 4> collectReturns(Function &F) {
    SmallVector<ReturnInst *, 4> Ret;
    for (auto &BB : F)
        if (auto *R = dyn_cast<ReturnInst>(BB.getTerminator()))
            Ret.push_back(R);
    return Ret;
}

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
    } else if (const auto *Load = dyn_cast<const LoadInst>(Result)) {
      Result = Load->getPointerOperand();
    } else {
      break;
    }
  }
  return Result ? Result : Ptr;
}

// Check if a call instruction is a heap allocation function
static bool isHeapAllocationCall(const Value *V) {
  const auto *Call = dyn_cast<CallBase>(V);
  if (!Call) return false;
  const Function *F = Call->getCalledFunction();
  if (!F) return false;
  StringRef Name = F->getName();
  return Name == "_Znwm" || Name == "_Znam" ||
         Name == "_Znwj" || Name == "_Znaj" ||
         Name == "malloc" || Name == "calloc" ||
         Name == "realloc" || Name == "strdup" ||
         Name == "tm_malloc" || Name == "tm_calloc" ||
         Name == "tm_realloc";
}

// Check if a call instruction is a heap deallocation function
static bool isDeallocationCall(const Value *V) {
  const auto *Call = dyn_cast<CallBase>(V);
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

// Follow the definition chain of V backwards to check if it ultimately
// comes from a TM-annotated global variable.  Handles iterators that
// are local allocas initialized from TM globals via begin().
static bool tracesFromTMGlobal(Value *V, Module &M,
                               SmallPtrSetImpl<const AllocaInst *> *VisitedAllocas = nullptr,
                               int Depth = 0)
{
  if (Depth > 15) return false;
  if (!V) return false;

  V = V->stripPointerCasts();

  // Direct global access
  if (auto *GV = dyn_cast<GlobalVariable>(V))
    return isTMAnnotatedGlobal(GV, M);

  // GEP: follow the pointer operand
  if (auto *GEP = dyn_cast<GetElementPtrInst>(V))
    return tracesFromTMGlobal(const_cast<Value *>(GEP->getPointerOperand()),
                              M, VisitedAllocas, Depth + 1);
  if (auto *GEPOp = dyn_cast<GEPOperator>(V))
    return tracesFromTMGlobal(const_cast<Value *>(GEPOp->getPointerOperand()),
                              M, VisitedAllocas, Depth + 1);

  // Load: follow to the address being loaded from
  if (auto *Load = dyn_cast<LoadInst>(V))
    return tracesFromTMGlobal(const_cast<Value *>(Load->getPointerOperand()),
                              M, VisitedAllocas, Depth + 1);

  // Call/Invoke returning a pointer (e.g. begin()). If any argument traces to
  // a TM global, the return value inherits that.
  if (auto *Call = dyn_cast<CallBase>(V)) {
    if (Function *Callee = Call->getCalledFunction()) {
      if (Callee->getName().starts_with("tm_")) {
        // tm_read_ptr loads a pointer from shared memory — the returned value
        // IS a shared pointer, so subsequent loads/stores through it must be
        // instrumented. All other tm_* (read_i4, write_ptr, etc.) return
        // integer/void and should stop the trace.
        if (Callee->getName() == "tm_read_ptr")
          return true;
        return false;
      }
    }
    for (unsigned i = 0; i < Call->arg_size(); i++)
      if (tracesFromTMGlobal(Call->getArgOperand(i), M, VisitedAllocas, Depth + 1))
        return true;
    return false;
  }

  // PHI: any incoming value traces to a TM global
  if (auto *Phi = dyn_cast<PHINode>(V)) {
    for (Value *Inc : Phi->incoming_values())
      if (tracesFromTMGlobal(Inc, M, VisitedAllocas, Depth + 1))
        return true;
    return false;
  }

  // Select: either operand traces to a TM global
  if (auto *Sel = dyn_cast<SelectInst>(V))
    return tracesFromTMGlobal(Sel->getTrueValue(), M, VisitedAllocas, Depth + 1) ||
           tracesFromTMGlobal(Sel->getFalseValue(), M, VisitedAllocas, Depth + 1);

  // IntToPtr: the integer operand might come from PtrToInt of a pointer
  // (common for aliasing barriers inserted by LLVM).  Strip through to the
  // original pointer.  Without this, ptrtoint→inttoptr chains inserted
  // during inlining break the trace from allocas to TM globals.
  if (auto *ITP = dyn_cast<IntToPtrInst>(V)) {
    Value *Src = ITP->getOperand(0);
    if (auto *PTI = dyn_cast<PtrToIntInst>(Src))
      return tracesFromTMGlobal(PTI->getPointerOperand(), M, VisitedAllocas, Depth + 1);
    return false;
  }

  // ExtractValueInst: the extracted sub-value might contain a pointer that
  // was embedded via insertvalue.  Trace back through the aggregate.
  //     %ptr = extractvalue %struct %agg, 0
  //     load i32, ptr %ptr
  if (auto *EV = dyn_cast<ExtractValueInst>(V))
    return tracesFromTMGlobal(EV->getAggregateOperand(), M, VisitedAllocas, Depth + 1);

  // InsertValueInst: the aggregate value might contain a TM-traced pointer
  // that was inserted into a struct field.  Check both the base aggregate
  // and the inserted value.
  //     %agg = insertvalue %struct undef, ptr %tm_ptr, 0
  //     store %struct %agg, ptr %alloca
  if (auto *IV = dyn_cast<InsertValueInst>(V)) {
    if (tracesFromTMGlobal(IV->getAggregateOperand(), M, VisitedAllocas, Depth + 1))
      return true;
    return tracesFromTMGlobal(IV->getInsertedValueOperand(), M, VisitedAllocas, Depth + 1);
  }

  // Function argument: follow through call sites to check what's actually
  // passed at this argument position.  This enables inter-procedural tracing:
  // if a TX function takes a pointer param that was loaded from a TM global,
  // accesses through that param inside callees can be traced back.
  if (auto *Arg = dyn_cast<Argument>(V)) {
    Function *Parent = Arg->getParent();
    if (!Parent) return false;
    for (User *U : Parent->users()) {
      auto *Call = dyn_cast<CallBase>(U);
      if (!Call) continue;
      if (Call->getCalledFunction() != Parent) continue;
      Value *ActualArg = Call->getArgOperand(Arg->getArgNo());
      if (!ActualArg) continue;
      if (tracesFromTMGlobal(ActualArg, M, VisitedAllocas, Depth + 1))
        return true;
    }
    return false;
  }

  // Alloca: check if any store to this alloca stores a value that traces
  // to a TM global.  This is the key case for iterators:
  //   %__begin = alloca ptr                    (or %__iter = alloca %struct)
  //   store ptr %begin_result, ptr %__begin    (or %f = GEP %__iter, 0, 0;
  //                                             store ptr %result, ptr %f)
  // where %begin_result traces to begin() on a TM global, or %result was
  // loaded from a TM global during tree traversal.
  //
  // We walk ALL expressions rooted at the alloca (GEPs, bitcasts) to catch
  // stores through struct field GEPs — this covers iterators whose internal
  // pointer field is written via GEP+store rather than a direct store to the
  // alloca itself.
  if (auto *AI = dyn_cast<AllocaInst>(V)) {
    if (VisitedAllocas && VisitedAllocas->count(AI))
      return false;
    SmallPtrSet<const AllocaInst *, 4> LocalVisited;
    if (!VisitedAllocas)
      VisitedAllocas = &LocalVisited;
    VisitedAllocas->insert(AI);

    SmallVector<Value *, 8> AllocaExpr;
    SmallPtrSet<Value *, 8> VisitedExpr;
    AllocaExpr.push_back(const_cast<AllocaInst *>(AI));
    VisitedExpr.insert(const_cast<AllocaInst *>(AI));
    while (!AllocaExpr.empty()) {
      Value *Cur = AllocaExpr.pop_back_val();
      for (User *U : Cur->users()) {
        if (!VisitedExpr.insert(U).second) continue;
        if (auto *Store = dyn_cast<StoreInst>(U)) {
          // The store writes to this alloca (directly or via GEP): trace the
          // stored value back to see if it originates from a TM global.
          Value *StorePtr = Store->getPointerOperand()->stripPointerCasts();
          if (StorePtr == AI || getBaseObject(StorePtr) == AI)
            if (tracesFromTMGlobal(Store->getValueOperand(), M, VisitedAllocas, Depth + 1))
              return true;
          continue;
        }
        // Follow pointer-typed expressions (GEPs, bitcasts) rooted at the
        // alloca to find indirect stores.
        if (isa<GetElementPtrInst>(U) || isa<BitCastInst>(U) ||
            isa<AddrSpaceCastInst>(U)) {
          AllocaExpr.push_back(U);
        }
      }
    }
    return false;
  }

  return false;
}

// Determine if a pointer accesses shared (non-local) data
static bool isSharedPointer(Value *Ptr,
                           const SmallPtrSet<const Value *, 32> &LocalVars,
                           Function &F,
                           Module &M,
                           int DepthLimit = 15)
{
  // Alloca instructions are always local (stack-allocated).  Returning false
  // here prevents tm_write_ptr/tm_read_iN on local variables.  The VALUES
  // loaded from allocas later still trace to TM globals through the normal
  // tracesFromTMGlobal chain — see the GEP/Load handling there.
  // Also handle GEPs/bitcasts of allocas (e.g. iterator field access) —
  // the address within a stack allocation is always thread-private.
  if (isa<AllocaInst>(Ptr))
    return false;
  if (isa<AllocaInst>(getBaseObject(Ptr)))
    return false;

  // Check if the pointer ultimately traces to a TM-annotated global.
  if (tracesFromTMGlobal(Ptr, M)) {
    TM_DEBUG("Pointer traces to TM global → shared");
    return true;
  }

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

  // Pointer neither traces to a TM global nor originates from a local alloca.
  // Assume shared — this is correct for cloned functions where the
  // hasSharedAccesses gatekeeper ensures `this` points to a TM global or
  // heap reachable from one.  For TX functions (not cloned), direct access
  // patterns like `g_connections[i]` pass through here and need instrumentation.
  // The only false positive is heap pointers that aren't TM-tracked (e.g. tree
  // nodes), but those are harmless — TM read/write on them adds correct entries
  // to the read/write-sets without causing correctness issues.
  TM_DEBUG("Pointer assumed shared (heap/argument)");
  return true;
}

#endif // TM_LOCAL_VARS_HPP