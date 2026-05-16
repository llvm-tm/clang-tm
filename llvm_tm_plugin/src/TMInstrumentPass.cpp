// TMInstrumentPass.cpp
// Main plugin file for Transactional Memory instrumentation
//
// WHY TWO PASSES:
// ================
// We use a two-pass approach because some operations are module-level (Pass 1)
// while others are function-level (Pass 2):
//
// PASS 1 (TMGlobalInitPass - Module-level):
//   - Runs ONCE per module before function passes
//   - Collects all "tm" and "transaction" annotated globals/functions
//   - Creates symbol tables for TM-annotated globals (module-level data)
//   - Instruments main() and thread entry points with init/exit calls
//   - These need to be done once, not per-function
//
// PASS 2 (TMInstrumentPass - Function-level):
//   - Runs on EACH function marked with "transaction" annotation
//   - Instruments transaction entry/exit (sigsetjmp, tm_begin, tm_end)
//   - Instruments loads/stores within transactions (tm_read/tm_write)
//   - Function-level pass allows us to analyze each transaction function
//     independently and modify its IR
//
// This separation is REQUIRED because:
//   - Module-level changes (symbol tables, main instrumentation) should
//     happen once, not be repeated for each function
//   - Function-level changes need to modify the function's own IR
//   - LLVM's pass manager runs function passes multiple times, so we can't
//     do module-level work there

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Type.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Plugins/PassPlugin.h>

#include "tm_annotation_utils.hpp"
#include "tm_call_graph.hpp"
#include "tm_debug.hpp"
#include "tm_local_vars.hpp"
#include "tm_method_instrumentation.hpp"
#include "tm_runtime_hooks.hpp"
#include "tm_thread_guard.hpp"
#include "tm_thread_symbols.hpp"

using namespace llvm;

namespace
{

class TMGlobalInitPass : public PassInfoMixin<TMGlobalInitPass>
{
// PASS 1: Module-level instrumentation
//
// This pass runs ONCE per module and handles:
//   1. Collect all variables and functions annotated with "tm" and "transaction"
//   2. Look for thread entry point functions (e.g., pthread_create, std::thread)
//   3. Create symbol tables for TM-annotated globals (module-level data)
//   4. Instrument main() with tm_init at entry and tm_exit at return
//   5. Instrument thread entry points with tm_init_thread/tm_exit_thread
//   6. Use thread-local guard variable (tm_thread_ready) to prevent multiple calls
//
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &)
  {
    TM_DEBUG("TMGlobalInitPass: processing module %s", M.getName().str().c_str());
    
    LLVMContext &Ctx = M.getContext();
    auto H = TMRuntimeHooks::declareAll(M, Ctx);
    Type *i8Ty = Type::getInt8Ty(Ctx);
    Type *i32Ty = Type::getInt32Ty(Ctx);

    // Create thread-ready guard variable (thread-local)
    GlobalVariable *ThreadReadyGV = M.getGlobalVariable("tm_thread_ready");
    if (!ThreadReadyGV) {
      ThreadReadyGV = new GlobalVariable(M, i8Ty, false,
                                       GlobalValue::ExternalLinkage,
                                       ConstantInt::get(i8Ty, 0),
                                       "tm_thread_ready");
      ThreadReadyGV->setThreadLocal(true);
    }

    // ---- Collect "tm"-annotated globals and create symbol tables ----
    SmallVector<std::pair<GlobalVariable *, StringRef>, 16> TMSymbols;
    collectTMSymbols(M, TMSymbols);
    TM_DEBUG("Found %d TM-annotated symbols", (int)TMSymbols.size());
    createTMSymbolTables(M, TMSymbols);

    // ---- Build TX-reachable call graph and clone non-TX callees ----
    bool modified = false;
    SmallPtrSet<Function *, 32> TxReachableFuncs;
    for (auto &F : M) {
        if (!F.isDeclaration() && hasAnnotation(F, "transaction"))
            collectTransactionCallGraph(F, M, TxReachableFuncs);
    }
    if (!TxReachableFuncs.empty()) {
        TM_DEBUG("Tx-reachable call graph has %d functions", (int)TxReachableFuncs.size());
        auto &ClonedMap = tm_method_instrumentation::cloneTxReachableGraph(M, TxReachableFuncs, H);

        for (auto &F : M) {
            if (F.isDeclaration()) continue;
            if (!hasAnnotation(F, "transaction")) continue;
            tm_method_instrumentation::redirectCallsToClones(F, TxReachableFuncs, ClonedMap);
        }
        modified = true;
    }

    // ---- Explicit thread entry point detection ----
    SmallPtrSet<Function *, 32> ExplicitThreadEntries;
    for (auto &F : M) {
        if (F.isDeclaration()) continue;
        for (size_t i = 0; ThreadEntrySymbols[i] != nullptr; ++i) {
            if (F.getName() == ThreadEntrySymbols[i]) {
                ExplicitThreadEntries.insert(&F);
                TM_DEBUG("Found explicit thread entry: %s", F.getName().str().c_str());
                break;
            }
        }
    }

    // ---- Instrument main() ----
    if (Function *MainFn = M.getFunction("main")) {
      TM_DEBUG("Instrumenting main()");
      BasicBlock &Entry = MainFn->getEntryBlock();
      IRBuilder<> Builder(&Entry, Entry.begin());
      Builder.CreateCall(H.init, {});
      insertThreadInitWithGuard(Builder, H.init_thread, ThreadReadyGV);

      // ---- Call pstatic_rebuild functions after tm_init ----
      // These user-defined functions reconstruct pointer-based data structures
      // (e.g. std::map) from TM-annotated arrays after the persistent state
      // has been restored by tm_init().
      for (auto &F : M) {
        if (F.isDeclaration()) continue;
        if (hasAnnotation(F, "pstatic_rebuild")) {
          Builder.CreateCall(&F, {});
          TM_DEBUG("Calling pstatic_rebuild: %s", F.getName().str().c_str());
        }
      }

      auto MainReturns = collectReturns(*MainFn);
      for (auto *Ret : MainReturns) {
        IRBuilder<> RetBuilder(Ret);
        insertThreadExitWithGuard(RetBuilder, H.exit_thread, ThreadReadyGV);
        RetBuilder.CreateCall(H.exit_fn, {});
      }
      modified = true;
    }

    // ---- Instrument thread entry points ----
    for (auto &F : M) {
      if (F.isDeclaration() || F.getName() == "main") continue;
      if (hasAnnotation(F, "transaction")) continue;
      if (!hasAnnotation(F, "thread") && !ExplicitThreadEntries.count(&F)) continue;

      TM_DEBUG("Instrumenting thread entry point: %s", F.getName().str().c_str());
      BasicBlock &Entry = F.getEntryBlock();
      IRBuilder<> Builder(&Entry, Entry.begin());
      insertThreadInitWithGuard(Builder, H.init_thread, ThreadReadyGV);

      for (auto *Ret : collectReturns(F)) {
        if (!Ret) continue;
        IRBuilder<> RetBuilder(Ret);
        insertThreadExitWithGuard(RetBuilder, H.exit_thread, ThreadReadyGV);
      }
      modified = true;
    }

    TM_DEBUG("TMGlobalInitPass: %s", modified ? "modified module" : "no changes");
    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }

  static bool isRequired() { return true; }
};

class TMInstrumentPass : public PassInfoMixin<TMInstrumentPass>
{
// PASS 2: Function-level instrumentation for transaction functions
//
// This pass runs on EACH function marked with "transaction" annotation.
// It handles:
//   1. Transaction entry: nested counter check, sigsetjmp + tm_begin for outer
//   2. Load/store instrumentation: inject tm_read/tm_write hooks
//   3. Transaction exit: decrement counter, tm_end for outermost
//   4. Local variable detection: don't instrument local (stack) variables
//
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM)
  {
    TM_DEBUG("TMInstrumentPass: processing function %s", F.getName().str().c_str());
    
    if (!hasAnnotation(F, "transaction")) {
      TM_DEBUG("%s is not a transaction function, skipping", F.getName().str().c_str());
      return PreservedAnalyses::all();
    }
    
    TM_DEBUG("Instrumenting transaction function: %s", F.getName().str().c_str());
    F.addFnAttr(llvm::Attribute::NoInline);

    Module *M = F.getParent();
    LLVMContext &Ctx = M->getContext();
    auto H = TMRuntimeHooks::declareAll(*M, Ctx);
    Type *i32Ty = Type::getInt32Ty(Ctx);

    // ---- Setup thread-local variables for transaction nesting ----
    auto getOrCreateTLS = [&](StringRef Name, Type *Ty) -> GlobalVariable * {
        if (auto *GV = M->getGlobalVariable(Name)) return GV;
        auto *GV = new GlobalVariable(*M, Ty, false, GlobalValue::ExternalLinkage, nullptr, Name);
        GV->setThreadLocal(true);
        return GV;
    };
    auto *CounterGV = getOrCreateTLS("tm_nested_call_counter", i32Ty);
#ifndef DISABLE_SETJMP
    auto *JmpRetGV  = getOrCreateTLS("tm_longjmp_ret", i32Ty);
    auto *JmpBufGV  = getOrCreateTLS("tm_jmpbuf", ArrayType::get(Type::getInt8Ty(Ctx), 256));
#endif

    // ---- Transaction entry ----
    BasicBlock &Entry = F.getEntryBlock();
    Instruction *SplitPt = &*Entry.getFirstNonPHIIt();
    TM_ASSERT(SplitPt != nullptr, "Entry block has no non-PHI instruction");
    BasicBlock *ContBB = Entry.splitBasicBlock(SplitPt, "cont");

    AllocaInst *RetValAlloca = nullptr;
    if (!F.getReturnType()->isVoidTy()) {
        IRBuilder<> EntryBuilder(&Entry, Entry.getFirstNonPHIIt());
        RetValAlloca = EntryBuilder.CreateAlloca(F.getReturnType(), nullptr, "tx_retval");
    }
    Entry.getTerminator()->eraseFromParent();

    IRBuilder<> Builder(&Entry);
    Value *CounterVal = Builder.CreateLoad(i32Ty, CounterGV, "counter");
    Value *IsOuter = Builder.CreateICmpEQ(CounterVal, ConstantInt::get(i32Ty, 0), "is_outer");
#ifndef DISABLE_SETJMP
    Value *JmpRetVal = Builder.CreateLoad(i32Ty, JmpRetGV, "jmpret");
    IsOuter = Builder.CreateOr(IsOuter,
        Builder.CreateICmpNE(JmpRetVal, ConstantInt::get(i32Ty, 0), "is_retry"), "is_outer");
#endif

    BasicBlock *OuterBB = BasicBlock::Create(Ctx, "outer", &F, ContBB);
    BasicBlock *NestedBB = BasicBlock::Create(Ctx, "nested", &F, ContBB);
    Builder.CreateCondBr(IsOuter, OuterBB, NestedBB);

    // Outer path: tm_begin (with optional sigsetjmp for retry support)
    IRBuilder<> OuterBuilder(OuterBB);
#ifndef DISABLE_SETJMP
    Value *JmpBufPtr = OuterBuilder.CreateBitCast(JmpBufGV, PointerType::getUnqual(Ctx));
    OuterBuilder.CreateCall(H.set_jmpbuf, {JmpBufPtr});
    OuterBuilder.CreateStore(
        OuterBuilder.CreateCall(H.sigsetjmp, {JmpBufPtr, ConstantInt::get(i32Ty, 0)}),
        JmpRetGV);
    OuterBuilder.CreateStore(ConstantInt::get(i32Ty, 0), JmpRetGV);
#endif
    OuterBuilder.CreateStore(ConstantInt::get(i32Ty, 1), CounterGV);
    OuterBuilder.CreateCall(H.begin, {});
    OuterBuilder.CreateBr(ContBB);

    // Nested path: increment counter
    IRBuilder<> NestedBuilder(NestedBB);
    NestedBuilder.CreateStore(
        NestedBuilder.CreateAdd(CounterVal, ConstantInt::get(i32Ty, 1)), CounterGV);
    NestedBuilder.CreateBr(ContBB);

    // ---- Instrument loads/stores in original body blocks ----
    SmallVector<Instruction *, 16> ToErase;

    for (auto &BB : F) {
      // Only instrument original body blocks (not our newly-created entry/outer/nested)
      if (&BB == &Entry || &BB == OuterBB || &BB == NestedBB) continue;
      for (auto InstIt = BB.begin(); InstIt != BB.end();) {
        Instruction *I = &*InstIt++;
        IRBuilder<> B(I->getParent(), I->getIterator());

#ifndef DISABLE_TM_READ_WRITE
        // Memory intrinsics (memcpy, memmove, memset)
        if (auto *Call = dyn_cast<CallInst>(I)) {
          if (Function *Callee = Call->getCalledFunction()) {
            StringRef Name = Callee->getName();
            if (Name == "llvm.memcpy" || Name == "llvm.memmove" || Name == "llvm.memset") {
              bool touchesTM = false;
              for (unsigned i = 0; i < Call->arg_size(); ++i)
                if (tm_method_instrumentation::tracesToTMGlobal(Call->getArgOperand(i), *M))
                  { touchesTM = true; break; }
              if (touchesTM) {
                tm_method_instrumentation::instrumentMemoryIntrinsic(Call, *M, H);
                ToErase.push_back(Call);
              }
              continue;
            }
          }
        }
#endif

#ifndef DISABLE_MALLOC_FREE
        // Replace malloc/calloc/realloc with tm_malloc
        if (auto *Call = dyn_cast<CallInst>(I)) {
          if (Function *Callee = Call->getCalledFunction()) {
            if (Callee && !Callee->getName().starts_with("tm_")) {
              StringRef N = Callee->getName();
              if (N == "malloc" || N == "calloc" || N == "realloc") {
                Value *SizeArg = Call->getArgOperand(0);
                auto *NewCall = B.CreateCall(H.malloc_fn, {SizeArg});
                NewCall->setAttributes(AttributeList{});
                Call->replaceAllUsesWith(NewCall);
                ToErase.push_back(Call);
                continue;
              }
            }
            // Replace free with tm_free
            if (Callee && !Callee->getName().starts_with("tm_")) {
              StringRef N = Callee->getName();
              if (N == "free") {
                Value *PtrArg = Call->getArgOperand(0);
                auto *BC = B.CreateBitCast(PtrArg, B.getPtrTy());
                B.CreateCall(H.free_fn, {BC});
                ToErase.push_back(Call);
                continue;
              }
            }
          }
        }
#endif

#ifndef DISABLE_TM_READ_WRITE
        // Loads and stores
        if (auto *Load = dyn_cast<LoadInst>(I)) {
          SmallPtrSet<const Value *, 32> LocalVars;
          collectLocalVariables(F, LocalVars);
          if (isSharedPointer(Load->getPointerOperand(), LocalVars, F, *M)) {
            if (auto *Call = emitTMRead(B, Load->getPointerOperand(), Load->getType(), H)) {
              Load->replaceAllUsesWith(Call);
              ToErase.push_back(Load);
            }
          }
        } else if (auto *Store = dyn_cast<StoreInst>(I)) {
          SmallPtrSet<const Value *, 32> LocalVars;
          collectLocalVariables(F, LocalVars);
          if (isSharedPointer(Store->getPointerOperand(), LocalVars, F, *M)) {
            emitTMWrite(B, Store->getPointerOperand(), Store->getValueOperand(), H);
            ToErase.push_back(Store);
          }
        }
#endif
      }
    }

    // ---- Transaction exit ----
    SmallVector<ReturnInst *, 4> Returns;
    for (auto &BB : F)
      if (auto *Ret = dyn_cast<ReturnInst>(BB.getTerminator()))
        Returns.push_back(Ret);

    BasicBlock *OuterEndBB = BasicBlock::Create(Ctx, "outer_end", &F);
    BasicBlock *NestedEndBB = BasicBlock::Create(Ctx, "nested_end", &F);
    BasicBlock *CleanupBB = BasicBlock::Create(Ctx, "cleanup", &F);

    IRBuilder<> OuterEndBuilder(OuterEndBB);
    OuterEndBuilder.CreateCall(H.end, {});
#ifndef DISABLE_SETJMP
    OuterEndBuilder.CreateStore(ConstantInt::get(i32Ty, 0), JmpRetGV);
#endif
    OuterEndBuilder.CreateBr(CleanupBB);

    IRBuilder<> NestedEndBuilder(NestedEndBB);
    NestedEndBuilder.CreateBr(CleanupBB);

    for (auto *Ret : Returns) {
      BasicBlock *RetBB = Ret->getParent();
      Value *RetVal = Ret->getNumOperands() > 0 ? Ret->getOperand(0) : nullptr;
      BasicBlock *NewBB = RetBB->splitBasicBlock(Ret, "ret_check");
      if (RetVal && RetValAlloca) {
          IRBuilder<> StoreBuilder(Ret);
          StoreBuilder.CreateStore(RetVal, RetValAlloca);
      }
      Ret->eraseFromParent();
      IRBuilder<> NewBBuilder(NewBB);
      Value *CounterAtEnd = NewBBuilder.CreateLoad(i32Ty, CounterGV, "counter_at_end");
      NewBBuilder.CreateCondBr(
          NewBBuilder.CreateICmpEQ(CounterAtEnd, ConstantInt::get(i32Ty, 1), "is_outer_at_end"),
          OuterEndBB, NestedEndBB);
      NewBB->moveAfter(CleanupBB);
    }

    IRBuilder<> CleanupBuilder(CleanupBB);
    Value *Cnt = CleanupBuilder.CreateLoad(i32Ty, CounterGV, "counter_cleanup");
    CleanupBuilder.CreateStore(CleanupBuilder.CreateSub(Cnt, ConstantInt::get(i32Ty, 1)), CounterGV);
    if (F.getReturnType()->isVoidTy())
      CleanupBuilder.CreateRetVoid();
    else
      CleanupBuilder.CreateRet(CleanupBuilder.CreateLoad(F.getReturnType(), RetValAlloca));

    for (Instruction *I : ToErase) I->eraseFromParent();

    return PreservedAnalyses::none();
  }

  static bool isRequired() { return true; }
};

} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo()
{
  return {LLVM_PLUGIN_API_VERSION,
          "TMInstrumentPass",
          LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name,
                   ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "tm-instrument") {
                    TM_DEBUG("Registering tm-instrument pass pipeline");
                    MPM.addPass(TMGlobalInitPass());
                    MPM.addPass(
                        createModuleToFunctionPassAdaptor(TMInstrumentPass()));
                    return true;
                  }
                  return false;
                });
          }};
}
