#ifndef TM_INSTRUMENT_HELPERS_HPP
#define TM_INSTRUMENT_HELPERS_HPP

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include "tm_annotation_utils.hpp"
#include "tm_call_graph.hpp"
#include "tm_debug.hpp"
#include "tm_local_vars.hpp"
#include "tm_platform.hpp"
#include "tm_runtime_hooks.hpp"
#include "tm_thread_guard.hpp"
using namespace llvm;

// ===========================================================================
// Shared module-level helpers (used by both TMInitInjectPass and TMGlobalInitPass)
// ===========================================================================

struct ModulePassContext {
	TMRuntimeHooks H;
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

	SmallVector<std::pair<GlobalVariable *, StringRef>, 16> TMSymbols;
	collectTMSymbols(M, TMSymbols);
	TM_DEBUG("Found %d TM-annotated symbols", (int)TMSymbols.size());
	createTMSymbolTables(M, TMSymbols);

	for (auto &F : M) {
		if (!F.isDeclaration() && (hasAnnotation(F, TX_ANNOT) || hasAnnotation(F, ASYNC_TX_ANNOT)))
			collectTransactionCallGraph(F, M, CtxOut.TxReachableFuncs);
	}
	return CtxOut;
}

static void checkAnnotationConsistency(Module &M)
{
	for (auto &F : M) {
		if (F.isDeclaration())
			continue;
		bool isThread = hasAnnotation(F, THREAD_ANNOT);
		bool isMain = (F.getName() == MAIN_ANNOT) || hasAnnotation(F, MAIN_ANNOT);
		bool isTx = hasAnnotation(F, TX_ANNOT);
		bool isAsyncTx = hasAnnotation(F, ASYNC_TX_ANNOT);
		if (isTx && isAsyncTx) {
			errs() << "error: function '" << F.getName()
			       << "' has both 'transaction' and 'async_transaction' annotations. "
			       << "A function cannot be both synchronous and asynchronous.\n";
			exit(1);
		}
		if (isThread && (isTx || isAsyncTx)) {
			errs() << "error: function '" << F.getName()
			       << "' has both 'thread' and transaction annotations. "
			       << "A thread entry function cannot be a transaction function.\n";
			exit(1);
		}
		if (isMain && (isTx || isAsyncTx)) {
			errs() << "error: function '" << F.getName()
			       << "' has both 'main' and transaction annotations. "
			       << "The main function cannot be a transaction function.\n";
			exit(1);
		}
	}
}

static void instrumentMainInitExit(Function *MainFn, ModulePassContext &Ctx)
{
	BasicBlock &Entry = MainFn->getEntryBlock();
	IRBuilder<> Builder(&Entry, Entry.begin());
	emitHookCall(Builder, Ctx.H.init, {});
	insertThreadInitWithGuard(Builder, Ctx.H.init_thread);

	for (auto &F : *MainFn->getParent()) {
		if (F.isDeclaration())
			continue;
		if (hasAnnotation(F, PSTATIC_REBUILD_ANNOT)) {
			Builder.CreateCall(&F, {});
			TM_DEBUG("Calling pstatic_rebuild: %s", F.getName().str().c_str());
		}
	}

	auto MainReturns = collectReturns(*MainFn);
	for (auto *Ret : MainReturns) {
		IRBuilder<> RetBuilder(Ret);
		insertThreadExitWithGuard(RetBuilder, Ctx.H.exit_thread);
		emitHookCall(RetBuilder, Ctx.H.exit_fn, {});
	}
	Ctx.modified = true;
}

static void instrumentThreadEntries(Module &M,
                                    SmallPtrSetImpl<Function *> &ExplicitThreadEntries,
                                    ModulePassContext &Ctx)
{
	for (auto &F : M) {
		if (F.isDeclaration() || F.getName() == "main")
			continue;
		if (hasAnnotation(F, TX_ANNOT) || hasAnnotation(F, ASYNC_TX_ANNOT))
			continue;
		if (!hasAnnotation(F, THREAD_ANNOT) && !ExplicitThreadEntries.count(&F))
			continue;

		TM_DEBUG("Instrumenting thread entry point: %s", F.getName().str().c_str());
		BasicBlock &Entry = F.getEntryBlock();
		IRBuilder<> Builder(&Entry, Entry.begin());
		insertThreadInitWithGuard(Builder, Ctx.H.init_thread);

		for (auto *Ret : collectReturns(F)) {
			if (!Ret)
				continue;
			IRBuilder<> RetBuilder(Ret);
			insertThreadExitWithGuard(RetBuilder, Ctx.H.exit_thread);
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
	if (!Callee)
		return false;
	StringRef Name = Callee->getName();
	if (!Name.starts_with("llvm.memcpy") && !Name.starts_with("llvm.memmove") &&
	    !Name.starts_with("llvm.memset"))
		return false;

	// Clone functions (suffixed _tm_clone) ONLY operate on TM-tracked memory.
	// Any memory intrinsic inside a clone MUST go through the TM write set,
	// otherwise tm_read in destructors sees stale write-set values while the
	// underlying memory has been mutated by the intrinsic (e.g., memset in
	// vector<int> move constructor zeroes source pointers in memory but not
	// in the write set, causing the moved-from destructor to free the buffer).
	if (Function *F = Call->getFunction())
		if (F->getName().ends_with(TM_CLONE_SUFFIX))
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
static bool handleMemoryIntrinsic(CallBase *Call,
                                  Module &M,
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
static void auditTXFunctionLoadsStores(Function &F, Module &M)
{
	if (!TMAudit)
		return;

	int totalLoads = 0, instrumentedLoads = 0;
	int totalStores = 0, instrumentedStores = 0;

	errs() << "\n[AUDIT] === ALL loads in " << F.getName() << " ===\n";
	for (auto &BB : F) {
		for (auto &I : BB) {
			auto *Load = dyn_cast<LoadInst>(&I);
			if (!Load)
				continue;
			totalLoads++;
			Value *Ptr = Load->getPointerOperand();
			bool local = isTMLocalVar(Ptr, M);
			const Value *Base = getBaseObject(Ptr);
			errs() << "[AUDIT] " << (local ? "* " : "  ") << "LOAD"
			       << " tm_local=" << (local ? "Y" : "N") << " type=" << *Load->getType()
			       << "\n    ptr=" << *Ptr << "\n    base=" << *Base << "\n";
			if (!local)
				instrumentedLoads++;
		}
	}

	errs() << "[AUDIT] === ALL stores in " << F.getName() << " ===\n";
	for (auto &BB : F) {
		for (auto &I : BB) {
			auto *Store = dyn_cast<StoreInst>(&I);
			if (!Store)
				continue;
			totalStores++;
			Value *Ptr = Store->getPointerOperand();
			bool local = isTMLocalVar(Ptr, M);
			const Value *Base = getBaseObject(Ptr);
			errs() << "[AUDIT] " << (local ? "* " : "  ") << "STORE"
			       << " tm_local=" << (local ? "Y" : "N")
			       << " val=" << *Store->getValueOperand() << "\n    ptr=" << *Ptr
			       << "\n    base=" << *Base << "\n";
			if (!local)
				instrumentedStores++;
		}
	}

	errs() << "[AUDIT] Summary for " << F.getName() << ": "
	       << "LOAD " << instrumentedLoads << "/" << totalLoads << " STORE "
	       << instrumentedStores << "/" << totalStores
	       << " INSTRUMENTED; NOT instrumented (tm_local): "
	       << (totalLoads - instrumentedLoads) << " loads, "
	       << (totalStores - instrumentedStores) << " stores"
	       << "\n";
}

// Replace a malloc/free/operator-new call with the TM-aware equivalent.
// Handles both CallInst and InvokeInst. For InvokeInst, the callee is
// changed in-place (via setCalledFunction) to preserve the unwind
// destination — converting to a CallInst + branch would lose exception
// semantics (e.g., std::bad_alloc from operator new).
// NOTE: STL container functions are NOT exempt from interception.  All
// operator new / operator delete calls, including those inside STL container
// functions, are redirected to tm_malloc / tm_free.  Previously STL
// functions were exempt (to keep their buffers on the system heap), but
// this caused system-heap buffer overflow when TM-instrumented code
// wrote past the buffer — the TM runtime cannot detect system-heap
// corruption.  Maintaining per-STL exemptions is fragile and costly.
static bool handleMallocFree(CallBase *Call,
                             IRBuilder<> &B,
                             const TMRuntimeHooks &H,
                             SmallVectorImpl<Instruction *> &ToErase)
{
	Function *Callee = Call->getCalledFunction();
	if (!Callee || Callee->getName().starts_with("tm_"))
		return false;

	bool isInvoke = isa<InvokeInst>(Call);

	StringRef N = Callee->getName();

	// Helper: change the callee of the call/invoke to the TM runtime function.
	// For InvokeInst, creates a new invoke preserving the unwind destination.
	// For CallInst, creates a new call and marks the old one for erasure.
	// If NumArgs is specified, only forward the first NumArgs arguments
	// (needed for operator new with alignment which passes 2 args to
	//  tm_malloc which only expects 1).
	auto redirectTo = [&](const TMRuntimeHook &Target, unsigned NumArgs = ~0u) {
		SmallVector<Value *, 4> Args;
		unsigned N = std::min((unsigned)Call->arg_size(), NumArgs);
		for (unsigned i = 0; i < N; i++)
			Args.push_back(Call->getArgOperand(i));
		if (isInvoke) {
			auto *II = cast<InvokeInst>(Call);
			auto *NewInvoke = createHookInvoke(II->getContext(), Target, Args,
			                                   II->getNormalDest(),
			                                   II->getUnwindDest(),
			                                   Call->getName());
			NewInvoke->setAttributes(AttributeList{});
			Call->replaceAllUsesWith(NewInvoke);
			ToErase.push_back(Call);
		} else {
			auto *NewCall = emitHookCall(B, Target, Args, Call->getName());
			NewCall->setAttributes(AttributeList{});
			Call->replaceAllUsesWith(NewCall);
			ToErase.push_back(Call);
		}
	};

	bool isNew = tm_platform::isOperatorNew(N);
	bool isFree = tm_platform::isOperatorDelete(N);

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
		// For InvokeInst, replace with a new invoke that loads the hook.
		// For CallInst, create a new indirect call through the free hook
		// with a bitcast pointer, matching the existing CallInst pattern.
		if (isInvoke) {
			auto *II = cast<InvokeInst>(Call);
			auto *NewInvoke = createHookInvoke(B.getContext(), H.free_fn,
			                                   {B.CreateBitCast(Call->getArgOperand(0), B.getPtrTy())},
			                                   II->getNormalDest(),
			                                   II->getUnwindDest(),
			                                   Call->getName());
			NewInvoke->setAttributes(AttributeList{});
			Call->replaceAllUsesWith(NewInvoke);
			ToErase.push_back(Call);
		} else {
			Value *PtrArg = Call->getArgOperand(0);
			auto *BC = B.CreateBitCast(PtrArg, B.getPtrTy());
			emitHookCall(B, H.free_fn, {BC});
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
			if (isa<GEPOperator>(U) || isa<BitCastInst>(U) || isa<AddrSpaceCastInst>(U)) {
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
static bool handleLoadStore(Instruction *I,
                            Function &F,
                            Module &M,
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

	// Skip loads/stores that access TM runtime thread state fields via
	// tm_get_thread_state().  These are injected by injectTransactionBeginEnd
	// and must use raw loads/stores (routing through tm_read/tm_write would
	// fail because tx->active is not yet set when we read the counter).
	auto isThreadStateAccess = [](Value *Ptr) -> bool {
		Value *Base = Ptr->stripPointerCasts();
		if (auto *GEP = dyn_cast<GetElementPtrInst>(Base))
			Base = GEP->getPointerOperand()->stripPointerCasts();
		if (auto *Call = dyn_cast<CallInst>(Base))
			if (Call->getCalledFunction() &&
			    Call->getCalledFunction()->getName() == "tm_get_thread_state")
				return true;
		return false;
	};

	if (auto *Load = dyn_cast<LoadInst>(I)) {
		Value *Ptr = Load->getPointerOperand();
		if (isTLSGlobal(Ptr))
			return false;
		if (isThreadStateAccess(Ptr))
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
		if (isThreadStateAccess(Ptr))
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
// TM runtime state (nested_call_counter, longjmp_ret) lives in the TM
// address space, accessed via tm_get_thread_state().  No thread_local
// globals in the IR — this eliminates TLV relocations that conflict
// with runtimes that define many __thread variables (e.g. TinySTM).
static void injectTransactionBeginEnd(Function &F, Module &M, const TMRuntimeHooks &H)
{
	LLVMContext &Ctx = M.getContext();
	Type *i32Ty = Type::getInt32Ty(Ctx);
	auto *i8PtrTy = PointerType::getUnqual(Ctx);
	auto *i64Ty = Type::getInt64Ty(Ctx);

	// Field offsets within TMThreadState struct (must match tm_thread_state.hpp)
	//   struct TMThreadState {
	//       int32_t nested_call_counter;  // offset 0
	//       int32_t longjmp_ret;          // offset 4
	//   };
	constexpr int COUNTER_OFFSET = 0;
	constexpr int JMPRET_OFFSET = 4;

	TM_DEBUG("Injecting tm_begin/tm_end in transaction function: %s",
	         F.getName().str().c_str());
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

	// Load thread state pointer from TM address space
	IRBuilder<> Builder(&Entry);
	Value *StatePtr = emitHookCall(Builder, H.get_thread_state, {}, "tm_state");

	// Access nested_call_counter at offset 0
	Value *CounterPtr = Builder.CreateGEP(i32Ty, Builder.CreateBitCast(StatePtr, i8PtrTy),
	                                      {Builder.getInt64(0)},
	                                      "cnt_ptr");
	Value *CounterVal = Builder.CreateLoad(i32Ty, CounterPtr, "counter");
	Value *IsOuter = Builder.CreateICmpEQ(CounterVal,
	                                      ConstantInt::get(i32Ty, 0),
	                                      "is_outer");
#ifndef DISABLE_SETJMP
	// Access longjmp_ret at offset 4
	Value *JmpRetPtr = Builder.CreateGEP(i32Ty, Builder.CreateBitCast(StatePtr, i8PtrTy),
	                                     {Builder.getInt64(1)},
	                                     "jmpret_ptr");
	Value *JmpRetVal = Builder.CreateLoad(i32Ty, JmpRetPtr, "jmpret");
	IsOuter = Builder.CreateOr(IsOuter,
	                           Builder.CreateICmpNE(JmpRetVal,
	                                                ConstantInt::get(i32Ty, 0),
	                                                "is_retry"),
	                           "is_outer");
#endif

	BasicBlock *OuterBB = BasicBlock::Create(Ctx, "tx_outer", &F, ContBB);
	BasicBlock *NestedBB = BasicBlock::Create(Ctx, "tx_nested", &F, ContBB);
	Builder.CreateCondBr(IsOuter, OuterBB, NestedBB);

	IRBuilder<> OuterBuilder(OuterBB);
#ifndef DISABLE_SETJMP
	Value *JmpBufPtr = emitHookCall(OuterBuilder, H.get_env);
	emitHookCall(OuterBuilder, H.set_jmpbuf, {JmpBufPtr});
	auto *SigJmpRetCall = emitHookCall(OuterBuilder, H.sigsetjmp,
	                                   {JmpBufPtr,
	                                    ConstantInt::get(i32Ty, 0)});
	SigJmpRetCall->addFnAttr(Attribute::ReturnsTwice);
	// Store sigsetjmp result to longjmp_ret, then clear
	OuterBuilder.CreateStore(SigJmpRetCall, JmpRetPtr);
	OuterBuilder.CreateStore(ConstantInt::get(i32Ty, 0), JmpRetPtr);
#endif
	OuterBuilder.CreateStore(ConstantInt::get(i32Ty, 1), CounterPtr);
	emitHookCall(OuterBuilder, H.begin, {});
	OuterBuilder.CreateBr(ContBB);

	IRBuilder<> NestedBuilder(NestedBB);
	NestedBuilder.CreateStore(NestedBuilder.CreateAdd(CounterVal,
	                                                  ConstantInt::get(i32Ty, 1)),
	                          CounterPtr);
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
	emitHookCall(OuterEndBuilder, H.end, {});
#ifndef DISABLE_SETJMP
	OuterEndBuilder.CreateStore(ConstantInt::get(i32Ty, 0), JmpRetPtr);
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
		Value *CounterAtEnd = NewBBuilder.CreateLoad(i32Ty, CounterPtr, "counter_at_end");
		NewBBuilder.CreateCondBr(NewBBuilder.CreateICmpEQ(CounterAtEnd,
		                                                  ConstantInt::get(i32Ty, 1),
		                                                  "is_outer_at_end"),
		                         OuterEndBB,
		                         NestedEndBB);
		NewBB->moveAfter(CleanupBB);
	}

	IRBuilder<> CleanupBuilder(CleanupBB);
	Value *Cnt = CleanupBuilder.CreateLoad(i32Ty, CounterPtr, "counter_cleanup");
	CleanupBuilder.CreateStore(CleanupBuilder.CreateSub(Cnt, ConstantInt::get(i32Ty, 1)),
	                           CounterPtr);
	if (F.getReturnType()->isVoidTy())
		CleanupBuilder.CreateRetVoid();
	else
		CleanupBuilder.CreateRet(
		    CleanupBuilder.CreateLoad(F.getReturnType(), RetValAlloca));
}

#endif // TM_INSTRUMENT_HELPERS_HPP
