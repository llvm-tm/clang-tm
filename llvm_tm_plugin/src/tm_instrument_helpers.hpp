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
#include "tm_platform.hpp"
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
    const char *SetjmpFunc = tm_platform::sigsetjmpName(M);
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

// Detect whether a memory intrinsic (memcpy/memmove/memset) touches TM-tracked
// memory.  Returns true if so — the caller should defer instrumentation to
// after the instruction loop (instrumentMemoryIntrinsic splits basic blocks,
// which would invalidate the iterator).
static bool needsMemIntrinsicInstrumentation(CallBase *Call, Module &M)
{
    Function *Callee = Call->getCalledFunction();
    if (!Callee) return false;
    StringRef Name = Callee->getName();
    if (!Name.starts_with("llvm.memcpy") && !Name.starts_with("llvm.memmove")
        && !Name.starts_with("llvm.memset"))
        return false;

    // Clone functions (suffixed _tm_clone) ONLY operate on TM-tracked memory.
    // Any memory intrinsic inside a clone MUST go through the TM write set,
    // otherwise tm_read in destructors sees stale write-set values while the
    // underlying memory has been mutated by the intrinsic (e.g., memset in
    // vector<int> move constructor zeroes source pointers in memory but not
    // in the write set, causing the moved-from destructor to free the buffer).
    if (Function *F = Call->getFunction())
        if (F->getName().ends_with("_tm_clone"))
            return true;

    // Check each argument for TM-global provenance.
    for (unsigned i = 0; i < Call->arg_size(); ++i)
        if (tracesFromTMGlobal(Call->getArgOperand(i), M))
            return true;

    // If the DESTINATION (arg 0) is a stack alloca, the intrinsic writes to
    // thread-private memory and should NOT be instrumented.  This prevents
    // memset expansion in split_buffer_pointer_layout constructors from
    // creating byte-level tm_write_i1 entries at stack addresses, which would
    // then not update memory (write-set only) and cause subsequent raw loads
    // to read stale data.
    if (tracesFromAlloca(Call->getArgOperand(0), M))
        return false;

    // FALLBACK: tracesFromTMGlobal may miss inlined pointer chains (e.g.
    // old buffer loaded via tm_read_ptr stored into an alloca).
    if (Name.starts_with("llvm.memmove")) {
        // Check if src came from a tm_read_ptr call
        if (Value *Src = Call->getArgOperand(1))
            if (auto *SrcCall = dyn_cast<CallBase>(Src->stripPointerCasts()))
                if (Function *SrcCallee = SrcCall->getCalledFunction())
                    if (SrcCallee->getName() == "tm_read_ptr")
                        return true;
        // Check if dest came from tm_malloc
        if (Value *Dst = Call->getArgOperand(0))
            if (auto *DstCall = dyn_cast<CallBase>(Dst->stripPointerCasts()))
                if (Function *DstCallee = DstCall->getCalledFunction())
                    if (DstCallee->getName() == "tm_malloc")
                        return true;
    }
    return false;
}

// Legacy detection — kept for ABI compatibility, new code should use
// needsMemIntrinsicInstrumentation().
static bool handleMemoryIntrinsic(CallBase *Call, Module &M,
                                    const TMRuntimeHooks &H,
                                    SmallVectorImpl<Instruction *> *ToErase = nullptr)
{
    (void)H;
    (void)ToErase;
    return needsMemIntrinsicInstrumentation(Call, M);
}

// ===========================================================================
// Load/store audit (enabled with -tm-audit)
// ===========================================================================
//
// When -tm-audit is passed to opt, this function enumerates every load and
// store instruction in a TX function (after inlining) and reports which
// are INSTRUMENTED (non-tm_local) vs NOT INSTRUMENTED (tm_local).
// Non-instrumented loads/stores are marked with "* " in the output, so
// you can `grep "^* "` to verify your tm_local annotations are correct.
//
// Output shows:
//   * LOAD/STORE — tm_local=Y/N  type/val
//     ptr=   <LLVM pointer operand>
//     base=  <LLVM base object>
//
static void auditTXFunctionLoadsStores(Function &F, Module &M) {
    if (!TMAudit) return;

    int totalLoads = 0, instrumentedLoads = 0;
    int totalStores = 0, instrumentedStores = 0;

    errs() << "\n[AUDIT] === ALL loads in " << F.getName() << " ===\n";
    for (auto &BB : F) {
        for (auto &I : BB) {
            auto *Load = dyn_cast<LoadInst>(&I);
            if (!Load) continue;
            totalLoads++;
            Value *Ptr = Load->getPointerOperand();
            bool local = isTMLocalVar(Ptr, M);
            const Value *Base = getBaseObject(Ptr);
            errs() << "[AUDIT] " << (local ? "* " : "  ")
                   << "LOAD"
                   << " tm_local=" << (local ? "Y" : "N")
                   << " type=" << *Load->getType()
                   << "\n    ptr=" << *Ptr
                   << "\n    base=" << *Base
                   << "\n";
            if (!local) instrumentedLoads++;
        }
    }

    errs() << "[AUDIT] === ALL stores in " << F.getName() << " ===\n";
    for (auto &BB : F) {
        for (auto &I : BB) {
            auto *Store = dyn_cast<StoreInst>(&I);
            if (!Store) continue;
            totalStores++;
            Value *Ptr = Store->getPointerOperand();
            bool local = isTMLocalVar(Ptr, M);
            const Value *Base = getBaseObject(Ptr);
            errs() << "[AUDIT] " << (local ? "* " : "  ")
                   << "STORE"
                   << " tm_local=" << (local ? "Y" : "N")
                   << " val=" << *Store->getValueOperand()
                   << "\n    ptr=" << *Ptr
                   << "\n    base=" << *Base
                   << "\n";
            if (!local) instrumentedStores++;
        }
    }

    errs() << "[AUDIT] Summary for " << F.getName() << ": "
           << "LOAD " << instrumentedLoads << "/" << totalLoads
           << " STORE " << instrumentedStores << "/" << totalStores
           << " INSTRUMENTED; NOT instrumented (tm_local): "
           << (totalLoads - instrumentedLoads) << " loads, "
           << (totalStores - instrumentedStores) << " stores"
           << "\n";
}

// Check if a function (possibly cloned with _tm_clone suffix) originates from
// an STL container whose internal new/delete must not go through tm_malloc.
// STL container functions have mangled names starting with _ZNSt (std::) or
// _ZNKSt (const std::).  Their internal buffer allocations (vector realloc,
// string _M_create, etc.) must use the regular heap to avoid spec_alloc being
// freed on TX abort while the container's in-memory pointer still references it.
static bool isSTLContainerAllocSite(CallBase *Call)
{
    Function *Parent = Call->getFunction();
    if (!Parent) return false;
    StringRef Name = Parent->getName();
    // Strip _tm_clone suffix to check the original function name
    if (Name.ends_with("_tm_clone"))
        Name = Name.drop_back(StringRef("_tm_clone").size());
    // Itanium ABI: _ZNSt = std::, _ZNKSt = const std::, _ZN9__gnu_cxx = __gnu_cxx::
    return Name.starts_with("_ZNSt") || Name.starts_with("_ZNKSt")
        || Name.starts_with("_ZN9__gnu_cxx");
}

// Replace a malloc/free/operator-new call with the TM-aware equivalent.
// Handles both CallInst and InvokeInst. For InvokeInst, the callee is
// changed in-place (via setCalledFunction) to preserve the unwind
// destination — converting to a CallInst + branch would lose exception
// semantics (e.g., std::bad_alloc from operator new).
// SKIPS interception for calls inside STL container functions (vector, string,
// deque, etc.) whose internal buffer allocations must use the regular heap.
static bool handleMallocFree(CallBase *Call, IRBuilder<> &B,
                              const TMRuntimeHooks &H,
                              SmallVectorImpl<Instruction *> &ToErase)
{
    Function *Callee = Call->getCalledFunction();
    if (!Callee || Callee->getName().starts_with("tm_")) return false;

    bool isInvoke = isa<InvokeInst>(Call);

    StringRef N = Callee->getName();

    // Helper: change the callee of the call/invoke to the TM runtime function.
    // For InvokeInst, creates a new invoke preserving the unwind destination.
    // For CallInst, creates a new call and marks the old one for erasure.
    // If NumArgs is specified, only forward the first NumArgs arguments
    // (needed for operator new with alignment which passes 2 args to
    //  tm_malloc which only expects 1).
    auto redirectTo = [&](FunctionCallee Target, unsigned NumArgs = ~0u) {
        SmallVector<Value *, 4> Args;
        unsigned N = std::min((unsigned)Call->arg_size(), NumArgs);
        for (unsigned i = 0; i < N; i++)
            Args.push_back(Call->getArgOperand(i));
        if (isInvoke) {
            auto *II = cast<InvokeInst>(Call);
            auto *FTy = Target.getFunctionType();
            auto *NewInvoke = InvokeInst::Create(
                FTy, Target.getCallee(), II->getNormalDest(),
                II->getUnwindDest(), Args, {}, Call->getName(),
                Call->getParent());
            NewInvoke->setAttributes(AttributeList{});
            Call->replaceAllUsesWith(NewInvoke);
            ToErase.push_back(Call);
        } else {
            auto *NewCall = B.CreateCall(Target, Args);
            NewCall->setAttributes(AttributeList{});
            Call->replaceAllUsesWith(NewCall);
            ToErase.push_back(Call);
        }
    };

    // STL container internal allocations (vector realloc, string _M_create, etc.)
    // must NOT go through tm_malloc — their buffers persist across TX boundaries
    // and must not be freed on TX abort while the container's in-memory pointer
    // still references them.  HOWEVER, deallocation (operator delete/free) must
    // STILL go through tm_free so the deallocation is deferred to commit time,
    // preventing concurrent readers from accessing freed memory during the TX.
    bool isSTL = isSTLContainerAllocSite(Call);

    // For STL container allocation sites: skip new/malloc/calloc/realloc
    // interception (use regular heap allocator), but still intercept
    // delete/free to defer deallocation to commit time.
    bool isNew = N == "malloc" || N == "_Znwm" || N == "_Znam" || N == "_Znwj" || N == "_Znaj"
        || N == "_ZnwmSt11align_val_t" || N == "_ZnamSt11align_val_t";
    bool isFree = N == "free" || N == "_ZdlPv" || N == "_ZdlPvm" || N == "_ZdaPv" || N == "_ZdaPvm"
        || N == "_ZdlPvSt11align_val_t" || N == "_ZdlPvmSt11align_val_t"
        || N == "_ZdaPvSt11align_val_t" || N == "_ZdaPvmSt11align_val_t";

    // STL container new/malloc: use regular heap (skip tm_malloc).
    // STL container delete/free: still redirect to tm_free for deferred free.
    if ((isNew || N == "calloc" || N == "realloc") && isSTL)
        return false;

    if (isNew) {
        // operator new with alignment (e.g. _ZnwmSt11align_val_t) has
        // 2 args (size + alignment). tm_malloc only expects size (1 arg).
        redirectTo(H.malloc_fn, 1);
        return true;
    }
    if (N == "calloc") {
        redirectTo(H.calloc_fn);
        return true;
    }
    if (N == "realloc") {
        redirectTo(H.realloc_fn);
        return true;
    }
    if (isFree) {
        // For InvokeInst, change the callee to tm_free in-place.
        // For CallInst, create a new tm_free call with a bitcast pointer,
        // matching the existing CallInst pattern.
        if (isInvoke) {
            Call->setCalledFunction(H.free_fn);
            Call->setAttributes(AttributeList{});
        } else {
            Value *PtrArg = Call->getArgOperand(0);
            auto *BC = B.CreateBitCast(PtrArg, B.getPtrTy());
            B.CreateCall(H.free_fn, {BC});
            ToErase.push_back(Call);
        }
        return true;
    }
    return false;
}

// Check if an AllocaInst's address escapes by being passed as a function argument
// (other than lifetime.start/end intrinsics).  This catches the reference-parameter
// pattern (e.g., split/merge Node*&) where the callee writes to the stack alloca via
// tm_write_ptr and the caller needs tm_read to see the value within the same TX.
static bool isEscapedAlloca(const AllocaInst *AI, const Function *F)
{
	SmallPtrSet<const Value *, 16> Visited;
	SmallVector<const Value *, 16> Worklist;
	Worklist.push_back(AI);
	Visited.insert(AI);

	while (!Worklist.empty()) {
		const Value *V = Worklist.pop_back_val();
		for (const User *U : V->users()) {
			if (!Visited.insert(U).second)
				continue;

			if (auto *CB = dyn_cast<const CallBase>(U)) {
				Function *Callee = CB->getCalledFunction();
				if (Callee && Callee->isIntrinsic() &&
				    (Callee->getName().starts_with("llvm.lifetime.start") ||
				     Callee->getName().starts_with("llvm.lifetime.end")))
					continue;
				return true;
			}

			// Follow pointer-compatible casts (GEP, bitcast, addrspacecast)
			if (isa<GEPOperator>(U) || isa<BitCastInst>(U) ||
			    isa<AddrSpaceCastInst>(U)) {
				Worklist.push_back(U);
			}
			// StoreInst is a dead-end for direct-escape detection (the stored
			// value propagates but we don't follow stores for this simple check).
		}
	}
	return false;
}

// Default: instrument ALL loads/stores in TM transaction functions.
// Skip stack-allocated variables (allocas) — they are thread-private and
// their addresses change after setjmp restart.  Also skip tm_local.
// This replaces the old heuristic-based approach (isSharedPointer +
// isTMTracedPtr) which had false negatives causing data corruption.
static bool handleLoadStore(Instruction *I, Function &F, Module &M,
                             const TMRuntimeHooks &H,
                             SmallVectorImpl<Instruction *> &ToErase)
{
    // Use getBaseObjectNoLoad (not getBaseObject) for the alloca check.
    //
    // getBaseObject traces through LoadInst, so a heap pointer stored in an
    // alloca (e.g., vector.__begin_) traces back to the alloca itself.
    // This causes element stores/loads through the pointer to be falsely
    // classified as stack-local and skipped — the modify loop's tm_write_i4
    // (write-set only) is then invisible to the verification loop's raw
    // load i32 (reads stale memory).  getBaseObjectNoLoad stops at LoadInst,
    // treating the loaded value as the base (a heap address), which is
    // correctly instrumented.
    // Skip TLS globals — they are thread-private runtime state (e.g.,
    // tm_nested_call_counter, tm_longjmp_ret) that must use raw loads/stores.
    // Routing them through tm_read/tm_write would fail in backends that
    // validate tx->active (NOrec), since reads/writes to these globals
    // happen before tm_begin() and after tm_end().
    auto isTLSGlobal = [](Value *Ptr) -> bool {
        if (auto *GV = dyn_cast<GlobalVariable>(getBaseObjectNoLoad(Ptr)))
            return GV->isThreadLocal();
        return false;
    };

    if (auto *Load = dyn_cast<LoadInst>(I)) {
        Value *Ptr = Load->getPointerOperand();
        if (isTLSGlobal(Ptr))
            return false;
        if (auto *AI = dyn_cast<AllocaInst>(getBaseObjectNoLoad(Ptr))) {
            if (!isEscapedAlloca(AI, &F))
                return false;
        }
        if (tracesFromAlloca(Ptr, M))
            return false;
        if (isTMLocalVar(Ptr, M))
            return false;
        IRBuilder<> B(Load);
        if (auto *Call = emitTMRead(B, Ptr, Load->getType(), H)) {
            Load->replaceAllUsesWith(Call);
            ToErase.push_back(Load);
            return true;
        }
    } else if (auto *Store = dyn_cast<StoreInst>(I)) {
        Value *Ptr = Store->getPointerOperand();
        if (isTLSGlobal(Ptr))
            return false;
        if (isa<AllocaInst>(getBaseObjectNoLoad(Ptr)))
            return false;
        if (tracesFromAlloca(Ptr, M))
            return false;
        if (isTMLocalVar(Ptr, M))
            return false;
        IRBuilder<> B(Store);
        if (emitTMWrite(B, Ptr, Store->getValueOperand(), H))
            ToErase.push_back(Store);
        return true;
    } else if (isa<AtomicRMWInst>(I) || isa<AtomicCmpXchgInst>(I)) {
        // Atomic instructions (fetch_add, cmpxchg, etc.) must NOT be broken
        // into non-atomic tm_read + op + tm_write — that would lose the
        // atomicity guarantee, causing lost updates under concurrent TXs.
        // The TM protocol cannot provide atomicity for compound operations;
        // only the hardware can via atomic instructions.  Skip them entirely.
        return false;
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
