// tm_local_vars.hpp
// Local variable detection for TM instrumentation
//
// Default behavior: ALL loads/stores inside TM transaction functions are
// instrumented with tm_read/tm_write.  This is the safe default — the
// heuristic-based approach (isSharedPointer / originatesFromLocal) was
// removed because it had false negatives that caused data corruption.
//
// Users can OPT OUT of instrumentation for specific local variables by
// annotating them with __attribute__((annotate("tm_local"))):
//
//   void tx_func() {
//     __attribute__((annotate("tm_local"))) int counter = 0;
//     counter++;  // NOT instrumented — uses plain load/store
//   }
//
// Future improvements could re-introduce a smarter heuristic, but it must
// be conservative: only skip instrumentation when CERTAIN the variable
// cannot be shared.  Any false negative causes silent data corruption.

#ifndef TM_LOCAL_VARS_HPP
#define TM_LOCAL_VARS_HPP

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
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

// Get the base object from a pointer (follows GEP and Load chains)
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

// Like getBaseObject, but does NOT trace through LoadInst.
static const Value *getBaseObjectNoLoad(const Value *Ptr)
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

// ===========================================================================
// tm_local annotation support
//
// Users can annotate local variables with __attribute__((annotate("tm_local")))
// to exclude them from TM instrumentation.  The clang frontend emits
// @llvm.var.annotation calls for these; we collect their allocas here.
// ===========================================================================

// Collect all allocas annotated with "tm_local" in a module.
static void collectTMLocalAllocas(Module &M,
                                  SmallPtrSetImpl<const AllocaInst *> &LocalAllocas)
{
    // Look for @llvm.var.annotation calls
    for (auto &F : M) {
        if (!F.getName().starts_with("llvm.var.annotation")) continue;
        for (auto *U : F.users()) {
            auto *Call = dyn_cast<CallInst>(U);
            if (!Call || Call->arg_size() < 2) continue;
            Value *AnnotatedPtr = Call->getArgOperand(0)->stripPointerCasts();
            Value *AnnotStrGV = Call->getArgOperand(1)->stripPointerCasts();
            auto *StrGV = dyn_cast<GlobalVariable>(AnnotStrGV);
            if (!StrGV || !StrGV->hasInitializer()) continue;
            auto *Init = dyn_cast<ConstantDataArray>(StrGV->getInitializer());
            if (!Init) continue;
            if (Init->getAsCString() != "tm_local") continue;
            // Found a tm_local annotation — record the alloca
            if (auto *AI = dyn_cast<AllocaInst>(AnnotatedPtr)) {
                LocalAllocas.insert(AI);
                TM_DEBUG("Found tm_local annotation on alloca: %s",
                         AI->getName().str().c_str());
            }
        }
    }
}

// Module-level cache of tm_local allocas, populated once.
static SmallPtrSet<const AllocaInst *, 16> TMLocalAllocas;

static void ensureTMLocalAllocasCollected(Module &M) {
    if (TMLocalAllocas.empty())
        collectTMLocalAllocas(M, TMLocalAllocas);
}

// Check if a pointer's base alloca is annotated with tm_local.
// Returns true if the pointer targets a user-declared local variable
// that should NOT be instrumented with tm_read/tm_write.
static bool isTMLocalVar(const Value *Ptr, Module &M) {
    ensureTMLocalAllocasCollected(M);
    const Value *Base = getBaseObjectNoLoad(Ptr);
    if (auto *AI = dyn_cast<AllocaInst>(Base))
        return TMLocalAllocas.count(AI);
    // Also check the direct pointer (in case it IS an alloca)
    Value *Direct = const_cast<Value *>(Ptr->stripPointerCasts());
    if (auto *AI = dyn_cast<AllocaInst>(Direct))
        return TMLocalAllocas.count(AI);
    return false;
}

// ===========================================================================
// tracesFromTMGlobal — used by checkOpaqueFunctions and
// needsMemIntrinsicInstrumentation to detect TM-global provenance.
// Not used for load/store instrumentation decisions.
// ===========================================================================

static bool tracesFromTMGlobal(Value *V, Module &M,
                               SmallPtrSetImpl<const AllocaInst *> *VisitedAllocas = nullptr,
                               int Depth = 0)
{
  if (Depth > 15) return false;
  if (!V) return false;

  V = V->stripPointerCasts();

  if (auto *GV = dyn_cast<GlobalVariable>(V))
    return isTMAnnotatedGlobal(GV, M);

  if (auto *GEP = dyn_cast<GetElementPtrInst>(V))
    return tracesFromTMGlobal(const_cast<Value *>(GEP->getPointerOperand()),
                              M, VisitedAllocas, Depth + 1);
  if (auto *GEPOp = dyn_cast<GEPOperator>(V))
    return tracesFromTMGlobal(const_cast<Value *>(GEPOp->getPointerOperand()),
                              M, VisitedAllocas, Depth + 1);

  if (auto *Load = dyn_cast<LoadInst>(V))
    return tracesFromTMGlobal(const_cast<Value *>(Load->getPointerOperand()),
                              M, VisitedAllocas, Depth + 1);

  if (auto *Call = dyn_cast<CallBase>(V)) {
    if (Function *Callee = Call->getCalledFunction()) {
      if (Callee->getName().starts_with("tm_")) {
        if (Callee->getName() == "tm_read_ptr" ||
            Callee->getName() == "tm_malloc" ||
            Callee->getName() == "tm_calloc" ||
            Callee->getName() == "tm_realloc")
          return true;
        return false;
      }
    }
    for (unsigned i = 0; i < Call->arg_size(); i++)
      if (tracesFromTMGlobal(Call->getArgOperand(i), M, VisitedAllocas, Depth + 1))
        return true;
    return false;
  }

  if (auto *Phi = dyn_cast<PHINode>(V)) {
    for (Value *Inc : Phi->incoming_values())
      if (tracesFromTMGlobal(Inc, M, VisitedAllocas, Depth + 1))
        return true;
    return false;
  }

  if (auto *Sel = dyn_cast<SelectInst>(V))
    return tracesFromTMGlobal(Sel->getTrueValue(), M, VisitedAllocas, Depth + 1) ||
           tracesFromTMGlobal(Sel->getFalseValue(), M, VisitedAllocas, Depth + 1);

  if (auto *PTI = dyn_cast<PtrToIntInst>(V))
    return tracesFromTMGlobal(PTI->getPointerOperand(), M, VisitedAllocas, Depth + 1);

  if (auto *ITP = dyn_cast<IntToPtrInst>(V)) {
    Value *Src = ITP->getOperand(0);
    if (auto *PTI = dyn_cast<PtrToIntInst>(Src))
      return tracesFromTMGlobal(PTI->getPointerOperand(), M, VisitedAllocas, Depth + 1);
    return false;
  }

  if (auto *EV = dyn_cast<ExtractValueInst>(V))
    return tracesFromTMGlobal(EV->getAggregateOperand(), M, VisitedAllocas, Depth + 1);

  if (auto *IV = dyn_cast<InsertValueInst>(V)) {
    if (tracesFromTMGlobal(IV->getAggregateOperand(), M, VisitedAllocas, Depth + 1))
      return true;
    return tracesFromTMGlobal(IV->getInsertedValueOperand(), M, VisitedAllocas, Depth + 1);
  }

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
            Value *StorePtr = Store->getPointerOperand()->stripPointerCasts();
            if (StorePtr == AI || getBaseObject(StorePtr) == AI) {
              if (isa<AllocaInst>(getBaseObjectNoLoad(Store->getValueOperand())))
                continue;
            if (tracesFromTMGlobal(Store->getValueOperand(), M, VisitedAllocas, Depth + 1))
              return true;
          }
          continue;
        }
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

// ===========================================================================
// Legacy heuristic functions — kept for reference but NOT used for
// instrumentation decisions.  All loads/stores are instrumented by
// default; only tm_local-annotated variables are skipped.
//
// The heuristic (isSharedPointer / originatesFromLocal / isTMTracedPtr)
// was removed because it had false negatives: pointers that appeared
// "local" (originating from an alloca) but actually pointed to shared
// memory (e.g., through reference members like ConstructTransaction::v_)
// would have their stores silently skipped, causing data corruption.
//
// Future work could re-introduce a conservative heuristic that only
// skips instrumentation when it is CERTAIN the access is thread-private.
// The litmus test: any pointer whose address can possibly escape or be
// stored into a TM-annotated global must be instrumented.
// ===========================================================================

// Collect all local (stack) variables in a function — used for audit only.
static void collectLocalVariables(Function &F,
                                SmallPtrSet<const Value *, 32> &LocalVars)
{
  for (auto &BB : F) {
    for (auto &I : BB) {
      if (auto *Alloca = dyn_cast<AllocaInst>(&I)) {
        LocalVars.insert(Alloca);
      }
    }
  }
}

#endif // TM_LOCAL_VARS_HPP
