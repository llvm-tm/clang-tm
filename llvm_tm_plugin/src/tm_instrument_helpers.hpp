#ifndef TM_INSTRUMENT_HELPERS_HPP
#define TM_INSTRUMENT_HELPERS_HPP

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include "tm_annotation_utils.hpp"
#include "tm_debug.hpp"
#include "tm_local_vars.hpp"
#include "tm_method_instrumentation.hpp"
#include "tm_runtime_hooks.hpp"
#include "tm_thread_guard.hpp"
#include "tm_thread_symbols.hpp"

using namespace llvm;

// ===========================================================================
// Shared module-level helpers (used by both TMInitInjectPass and TMGlobalInitPass)
// ===========================================================================

struct ModulePassContext {
    TMRuntimeHooks H;
    GlobalVariable *ThreadReadyGV;
    SmallPtrSet<Function *, 32> TxReachableFuncs;
    SmallVector<std::pair<Function *, Function *>, 32> *ClonedMap = nullptr;
    bool modified = false;
};

static ModulePassContext setupModulePass(Module &M)
{
    LLVMContext &Ctx = M.getContext();
    const char *SetjmpFunc = M.getTargetTriple().str().find("linux") != std::string::npos
                               ? "__sigsetjmp" : "sigsetjmp";
    ModulePassContext CtxOut;
    CtxOut.H = TMRuntimeHooks::declareAll(M, Ctx, SetjmpFunc);
    Type *i8Ty = Type::getInt8Ty(Ctx);

    GlobalVariable *ThreadReadyGV = M.getGlobalVariable("tm_thread_ready");
    if (!ThreadReadyGV) {
        ThreadReadyGV = new GlobalVariable(M, i8Ty, false,
                                         GlobalValue::ExternalLinkage,
                                         ConstantInt::get(i8Ty, 0),
                                         "tm_thread_ready");
        ThreadReadyGV->setThreadLocal(true);
    }
    CtxOut.ThreadReadyGV = ThreadReadyGV;

    SmallVector<std::pair<GlobalVariable *, StringRef>, 16> TMSymbols;
    collectTMSymbols(M, TMSymbols);
    TM_DEBUG("Found %d TM-annotated symbols", (int)TMSymbols.size());
    createTMSymbolTables(M, TMSymbols);

    for (auto &F : M) {
        if (!F.isDeclaration() && hasAnnotation(F, "transaction"))
            collectTransactionCallGraph(F, M, CtxOut.TxReachableFuncs);
    }
    return CtxOut;
}

static void redirectTXFunctionsToClones(Module &M,
                                         SmallPtrSetImpl<Function *> &TxReachableFuncs,
                                         SmallVectorImpl<std::pair<Function *, Function *>> &ClonedMap)
{
    for (auto &F : M) {
        if (F.isDeclaration()) continue;
        if (!hasAnnotation(F, "transaction")) continue;
        tm_method_instrumentation::redirectCallsToClones(F, M, TxReachableFuncs, ClonedMap);
    }
    // Re-redirect clones now that they have callers
    for (int _r = 0; _r < 3; _r++)
        for (auto &pair : ClonedMap)
            tm_method_instrumentation::redirectCallsToClones(*pair.second, M, TxReachableFuncs, ClonedMap);
}

// checkOpaqueOrAbort is defined in TMInstrumentPass.cpp alongside checkOpaqueFunctions

static SmallPtrSet<Function *, 32>
detectExplicitThreadEntries(Module &M)
{
    SmallPtrSet<Function *, 32> Entries;
    for (auto &F : M) {
        if (F.isDeclaration()) continue;
        for (size_t i = 0; ThreadEntrySymbols[i] != nullptr; ++i) {
            if (F.getName() == ThreadEntrySymbols[i]) {
                Entries.insert(&F);
                TM_DEBUG("Found explicit thread entry: %s", F.getName().str().c_str());
                break;
            }
        }
    }
    return Entries;
}

static void instrumentMainInitExit(Function *MainFn,
                                    ModulePassContext &Ctx)
{
    BasicBlock &Entry = MainFn->getEntryBlock();
    IRBuilder<> Builder(&Entry, Entry.begin());
    Builder.CreateCall(Ctx.H.init, {});
    insertThreadInitWithGuard(Builder, Ctx.H.init_thread, Ctx.ThreadReadyGV);

    for (auto &F : *MainFn->getParent()) {
        if (F.isDeclaration()) continue;
        if (hasAnnotation(F, "pstatic_rebuild")) {
            Builder.CreateCall(&F, {});
            TM_DEBUG("Calling pstatic_rebuild: %s", F.getName().str().c_str());
        }
    }

    auto MainReturns = collectReturns(*MainFn);
    for (auto *Ret : MainReturns) {
        IRBuilder<> RetBuilder(Ret);
        insertThreadExitWithGuard(RetBuilder, Ctx.H.exit_thread, Ctx.ThreadReadyGV);
        RetBuilder.CreateCall(Ctx.H.exit_fn, {});
    }
    Ctx.modified = true;
}

static void instrumentThreadEntries(Module &M,
                                      SmallPtrSetImpl<Function *> &ExplicitThreadEntries,
                                      ModulePassContext &Ctx)
{
    for (auto &F : M) {
        if (F.isDeclaration() || F.getName() == "main") continue;
        if (hasAnnotation(F, "transaction")) continue;
        if (!hasAnnotation(F, "thread") && !ExplicitThreadEntries.count(&F)) continue;

        TM_DEBUG("Instrumenting thread entry point: %s", F.getName().str().c_str());
        BasicBlock &Entry = F.getEntryBlock();
        IRBuilder<> Builder(&Entry, Entry.begin());
        insertThreadInitWithGuard(Builder, Ctx.H.init_thread, Ctx.ThreadReadyGV);

        for (auto *Ret : collectReturns(F)) {
            if (!Ret) continue;
            IRBuilder<> RetBuilder(Ret);
            insertThreadExitWithGuard(RetBuilder, Ctx.H.exit_thread, Ctx.ThreadReadyGV);
        }
        Ctx.modified = true;
    }
}

// ===========================================================================
// Shared function-level helpers (used by both TMInstrumentPass and TMInstrumentInlinePass)
// ===========================================================================

static bool handleMemoryIntrinsic(CallInst *Call, Module &M,
                                   const TMRuntimeHooks &H,
                                   SmallVectorImpl<Instruction *> *ToErase = nullptr)
{
    Function *Callee = Call->getCalledFunction();
    if (!Callee) return false;
    StringRef Name = Callee->getName();
    if (Name != "llvm.memcpy" && Name != "llvm.memmove" && Name != "llvm.memset")
        return false;
    bool touchesTM = false;
    for (unsigned i = 0; i < Call->arg_size(); ++i)
        if (tm_method_instrumentation::tracesToTMGlobal(Call->getArgOperand(i), M))
            { touchesTM = true; break; }
    if (touchesTM) {
        tm_method_instrumentation::instrumentMemoryIntrinsic(Call, M, H);
        if (ToErase) ToErase->push_back(Call);
    }
    return true;
}

static bool handleMallocFree(CallInst *Call, IRBuilder<> &B,
                              const TMRuntimeHooks &H,
                              SmallVectorImpl<Instruction *> &ToErase)
{
    Function *Callee = Call->getCalledFunction();
    if (!Callee || Callee->getName().starts_with("tm_")) return false;

    StringRef N = Callee->getName();
    if (N == "malloc") {
        Value *SizeArg = Call->getArgOperand(0);
        auto *NewCall = B.CreateCall(H.malloc_fn, {SizeArg});
        NewCall->setAttributes(AttributeList{});
        Call->replaceAllUsesWith(NewCall);
        ToErase.push_back(Call);
        return true;
    }
    if (N == "calloc") {
        Value *Nmemb = Call->getArgOperand(0);
        Value *Size  = Call->getArgOperand(1);
        auto *NewCall = B.CreateCall(H.calloc_fn, {Nmemb, Size});
        NewCall->setAttributes(AttributeList{});
        Call->replaceAllUsesWith(NewCall);
        ToErase.push_back(Call);
        return true;
    }
    if (N == "realloc") {
        Value *Ptr  = Call->getArgOperand(0);
        Value *Size = Call->getArgOperand(1);
        auto *NewCall = B.CreateCall(H.realloc_fn, {Ptr, Size});
        NewCall->setAttributes(AttributeList{});
        Call->replaceAllUsesWith(NewCall);
        ToErase.push_back(Call);
        return true;
    }
    if (N == "free") {
        Value *PtrArg = Call->getArgOperand(0);
        auto *BC = B.CreateBitCast(PtrArg, B.getPtrTy());
        B.CreateCall(H.free_fn, {BC});
        ToErase.push_back(Call);
        return true;
    }
    return false;
}

static bool handleLoadStore(Instruction *I, Function &F, Module &M,
                             const TMRuntimeHooks &H,
                             SmallVectorImpl<Instruction *> &ToErase)
{
    SmallPtrSet<const Value *, 32> LocalVars;
    collectLocalVariables(F, LocalVars);

    if (auto *Load = dyn_cast<LoadInst>(I)) {
        if (isSharedPointer(Load->getPointerOperand(), LocalVars, F, M)) {
            IRBuilder<> B(Load);
            if (auto *Call = emitTMRead(B, Load->getPointerOperand(), Load->getType(), H)) {
                Load->replaceAllUsesWith(Call);
                ToErase.push_back(Load);
                return true;
            }
        }
    } else if (auto *Store = dyn_cast<StoreInst>(I)) {
        if (isSharedPointer(Store->getPointerOperand(), LocalVars, F, M)) {
            IRBuilder<> B(Store);
            if (emitTMWrite(B, Store->getPointerOperand(), Store->getValueOperand(), H))
                ToErase.push_back(Store);
            return true;
        }
    }
    return false;
}

// Inject tm_begin/tm_end + sigsetjmp nesting into a TX function.
static void injectTransactionBeginEnd(Function &F, Module &M,
                                       const TMRuntimeHooks &H)
{
    LLVMContext &Ctx = M.getContext();
    Type *i32Ty = Type::getInt32Ty(Ctx);

    auto getOrCreateTLS = [&](StringRef Name, Type *Ty) -> GlobalVariable * {
        if (auto *GV = M.getGlobalVariable(Name)) return GV;
        auto *GV = new GlobalVariable(M, Ty, false, GlobalValue::ExternalLinkage, nullptr, Name);
        GV->setThreadLocal(true);
        return GV;
    };
    auto *CounterGV = getOrCreateTLS("tm_nested_call_counter", i32Ty);
#ifndef DISABLE_SETJMP
    auto *JmpRetGV  = getOrCreateTLS("tm_longjmp_ret", i32Ty);
    auto *JmpBufGV  = getOrCreateTLS("tm_jmpbuf", ArrayType::get(Type::getInt8Ty(Ctx), 256));
#endif

    TM_DEBUG("Injecting tm_begin/tm_end in transaction function: %s", F.getName().str().c_str());
    F.addFnAttr(llvm::Attribute::NoInline);

    BasicBlock &Entry = F.getEntryBlock();
    Instruction *SplitPt = &*Entry.getFirstNonPHIIt();
    TM_ASSERT(SplitPt != nullptr, "Entry block has no non-PHI instruction");
    BasicBlock *ContBB = Entry.splitBasicBlock(SplitPt, "tx_cont");

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

    BasicBlock *OuterBB = BasicBlock::Create(Ctx, "tx_outer", &F, ContBB);
    BasicBlock *NestedBB = BasicBlock::Create(Ctx, "tx_nested", &F, ContBB);
    Builder.CreateCondBr(IsOuter, OuterBB, NestedBB);

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

    IRBuilder<> NestedBuilder(NestedBB);
    NestedBuilder.CreateStore(
        NestedBuilder.CreateAdd(CounterVal, ConstantInt::get(i32Ty, 1)), CounterGV);
    NestedBuilder.CreateBr(ContBB);

    // Transaction exit
    SmallVector<ReturnInst *, 4> Returns;
    for (auto &BB : F)
        if (auto *Ret = dyn_cast<ReturnInst>(BB.getTerminator()))
            Returns.push_back(Ret);

    BasicBlock *OuterEndBB = BasicBlock::Create(Ctx, "tx_outer_end", &F);
    BasicBlock *NestedEndBB = BasicBlock::Create(Ctx, "tx_nested_end", &F);
    BasicBlock *CleanupBB = BasicBlock::Create(Ctx, "tx_cleanup", &F);

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
        BasicBlock *NewBB = RetBB->splitBasicBlock(Ret, "tx_ret_check");
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
}

#endif // TM_INSTRUMENT_HELPERS_HPP
