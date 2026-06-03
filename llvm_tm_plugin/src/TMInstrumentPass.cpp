// TMInstrumentPass.cpp
// Main plugin file for Transactional Memory instrumentation
//
// Two pipelines:
//
//   tm-instrument (legacy, debug-friendly):
//     TMGlobalInitPass + TMInstrumentPass
//     - Clones and instruments non-TX callees in one shot
//     - No aggressive inlining — works with -O0 -g
//     - Clones get NoInline + OptimizeNone attributes
//
//   tm-instrument-inline:
//     TMInitInjectPass + AlwaysInlinerPass + TMInstrumentInlinePass
//     - Clones callees with alwaysinline, then inlines them into TX body
//     - Enables load/store visibility for inlined callee internals
//     - Requires -O1+ for AlwaysInlinerPass to work
//
// This two-pass-per-pipeline design is REQUIRED because:
//   - Module-level changes (symbol tables, main init) must happen once
//   - Function-level changes modify each function's IR independently
//   - LLVM runs function passes repeatedly, so module work can't be there

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Type.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Plugins/PassPlugin.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/IPO/AlwaysInliner.h>

#include <fstream>

#include "opaque_safe_table.hpp"
#include "tm_annotation_utils.hpp"
#include "tm_call_graph.hpp"
#include "tm_debug.hpp"
#include "tm_instrument_helpers.hpp"
#include "tm_local_vars.hpp"
#include "tm_method_instrumentation.hpp"
#include "tm_platform.hpp"
#include "tm_runtime_hooks.hpp"
#include "tm_thread_guard.hpp"
using namespace llvm;

cl::opt<bool> AllowOpaque(
    "tm-allow-opaque",
    cl::desc("Allow opaque (uninstrumentable) function calls inside transactions"),
    cl::init(false));

cl::opt<bool> StrictOpaque(
    "tm-strict-opaque",
    cl::desc("Reject even known-safe opaque function calls (strict mode)"),
    cl::init(false));

cl::opt<std::string> OpaqueSymbolsFile(
    "tm-opaque-symbols-file",
    cl::desc("Write unresolved opaque symbols to this file for external resolution"),
    cl::init(""));

cl::opt<bool> TMAudit(
    "tm-audit",
    cl::desc("Print every load/store in TX functions with instrumentation analysis"),
    cl::init(false));

static bool checkOpaqueFunctions(Module &M, SmallPtrSetImpl<Function *> &TxReachableFuncs)
{
	bool foundOpaque = false;
	StringSet<> UnresolvedSymbols;

	for (Function *F : TxReachableFuncs) {
		if (F->isDeclaration())
			continue;
		if (F->getName().starts_with("tm_"))
			continue;
		for (auto &BB : *F) {
			for (auto &I : BB) {
				auto *Call = dyn_cast<CallBase>(&I);
				if (!Call)
					continue;
				if (Call->hasFnAttr(ALLOW_OPAQUE_ANNOT))
					continue;
				if (Call->isInlineAsm())
					continue;
				Function *Callee = Call->getCalledFunction();
				if (Callee && hasAnnotation(*Callee, ALLOW_OPAQUE_ANNOT))
					continue;
				if (hasAnnotation(*F, ALLOW_OPAQUE_ANNOT))
					continue;
				// Format location prefix consistently
				auto locStr = [&](raw_ostream &OS) {
					if (auto *DIL = I.getDebugLoc().get())
						if (auto *Scope = dyn_cast_or_null<DIScope>(DIL->getScope()))
							OS << Scope->getFilename() << ":" << DIL->getLine() << ":"
							   << DIL->getColumn() << ": ";
				};
				if (!Callee) {
					foundOpaque = true;
					locStr(errs());
					errs() << "error: indirect call in TM context\n"
					       << "  Called from: " << F->getName()
					       << "\n"
					          "  Calls via function pointer or virtual method "
					          "cannot be instrumented for TM.\n";
					continue;
				}
				// LLVM intrinsics (llvm.memcpy, llvm.lifetime.start, etc.) are
				// always safe — they are handled by the memory intrinsic
				// instrumentation or are no-ops for TM purposes.
				if (Callee->isIntrinsic())
					continue;
				// Heap allocation/deallocation functions are handled by
				// handleMallocFree during instrumentation — skip them here.
				if (tm_platform::isHeapAllocationCall(Call) || tm_platform::isDeallocationCall(Call))
					continue;
				if (!Callee->isDeclaration())
					continue;
				// Check if any pointer argument traces to a TM global.
				// Even known-safe opaque functions (e.g., _ZSt prefix) are
				// UNSAFE if they receive TM-shared pointers — they modify
				// shared data without TM write-set tracking.
				bool hasTMTracedArg = false;
				for (unsigned i = 0; i < Call->arg_size(); i++) {
					if (Call->getArgOperand(i)->getType()->isPointerTy() &&
					    tracesFromTMGlobal(Call->getArgOperand(i), M)) {
						hasTMTracedArg = true;
						break;
					}
				}
				if (!hasTMTracedArg) {
					if (isKnownSafeOpaque(Callee->getName(), StrictOpaque))
						continue;
					if (isSyscallSymbol(Callee->getName()))
						continue;
				} else {
					// Even with TM-traced args, some pure/read-only functions
					// are safe (they don't modify shared memory).
					if (isKnownSafeWithTMArgs(Callee->getName()))
						continue;
				}
				foundOpaque = true;
				UnresolvedSymbols.insert(Callee->getName());
				locStr(errs());
				errs() << "error: call to '" << Callee->getName() << "' in TM context\n"
				       << "  Called from: " << F->getName() << "\n";
				if (hasTMTracedArg)
					errs()
					    << "  This function receives TM-shared pointer arguments "
					       "but its body is not visible (defined in external library).\n"
					       "  Its internal modifications bypass TM write-set tracking, "
					       "causing data corruption with concurrent transactions.\n";
				else
					errs() << "  This function is not visible to TM "
					          "instrumentation (no body in this translation unit).\n";
				emitOpaqueSuggestion(Callee->getName(), errs());
			}
		}
	}

	if (!OpaqueSymbolsFile.empty() && !UnresolvedSymbols.empty()) {
		std::error_code EC;
		raw_fd_ostream OS(OpaqueSymbolsFile, EC);
		if (!EC) {
			for (auto &Sym : UnresolvedSymbols)
				OS << Sym.getKey() << "\n";
			OS.close();
			errs() << "Unresolved opaque symbols written to: " << OpaqueSymbolsFile
			       << "\n";
		}
	}

	return !foundOpaque;
}

static void checkOpaqueOrAbort(Module &M, SmallPtrSetImpl<Function *> &TxReachableFuncs)
{
	if (TxReachableFuncs.empty())
		return;
	bool ok = checkOpaqueFunctions(M, TxReachableFuncs);
	if (!AllowOpaque && !ok) {
		errs() << "error: opaque function call(s) in TM context\n"
		       << "Use -tm-allow-opaque to disable this check.\n";
		exit(1);
	}
}

// Validate that no function is annotated with both "thread"/"main" and
// "transaction".  A function marked as both is a programming error:
// "thread" functions are entry points that may call transaction functions,
// but must not themselves be transactional.  "main" is also an entry point
// and must not be transactional.  If a function has both annotations, the
// compiler emits an error and exits.
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

static void redirectTXFunctionsToClones(
    Module &M,
    SmallPtrSetImpl<Function *> &TxReachableFuncs,
    SmallVectorImpl<std::pair<Function *, Function *>> &ClonedMap,
    tm_method_instrumentation::CloneMode Mode =
        tm_method_instrumentation::CloneMode::Instrument)
{
	for (auto &F : M) {
		if (F.isDeclaration())
			continue;
		if (!hasAnnotation(F, TX_ANNOT) && !hasAnnotation(F, ASYNC_TX_ANNOT))
			continue;
		tm_method_instrumentation::redirectCallsToClones(F,
		                                                 M,
		                                                 TxReachableFuncs,
		                                                 ClonedMap,
		                                                 Mode);
	}
	// Re-redirect clones now that they have callers
	for (int _r = 0; _r < 3; _r++)
		for (auto &pair : ClonedMap)
			tm_method_instrumentation::redirectCallsToClones(*pair.second,
			                                                 M,
			                                                 TxReachableFuncs,
			                                                 ClonedMap,
			                                                 Mode);
}

// ===========================================================================
// PASS 1 (inline pipeline): clone-with-alwaysinline + tx_begin/end injection
// ===========================================================================

class TMInitInjectPass : public PassInfoMixin<TMInitInjectPass>
{
public:
	PreservedAnalyses run(Module &M, ModuleAnalysisManager &)
	{
		TM_DEBUG("TMInitInjectPass: processing module %s", M.getName().str().c_str());
		checkAnnotationConsistency(M);
		auto Ctx = setupModulePass(M);
		bool &modified = Ctx.modified;

		if (!Ctx.TxReachableFuncs.empty()) {
			TM_DEBUG("Tx-reachable call graph has %d functions",
			         (int)Ctx.TxReachableFuncs.size());
			Ctx.ClonedMap = &tm_method_instrumentation::
			                    cloneTxReachableGraph(M,
			                                          Ctx.TxReachableFuncs,
			                                          Ctx.H,
			                                          tm_method_instrumentation::
			                                              CloneMode::AlwaysInline);
			redirectTXFunctionsToClones(M,
			                            Ctx.TxReachableFuncs,
			                            *Ctx.ClonedMap,
			                            tm_method_instrumentation::CloneMode::
			                                AlwaysInline);
			modified = true;
		}

		checkOpaqueOrAbort(M, Ctx.TxReachableFuncs);

		if (Function *MainFn = M.getFunction("main"))
			instrumentMainInitExit(MainFn, Ctx);

		SmallPtrSet<Function *, 32> EmptyEntries;
		instrumentThreadEntries(M, EmptyEntries, Ctx);

		for (auto &F : M) {
			if (F.isDeclaration())
				continue;
			if (!hasAnnotation(F, TX_ANNOT) && !hasAnnotation(F, ASYNC_TX_ANNOT))
				continue;
			injectTransactionBeginEnd(F, M, Ctx.H);
			modified = true;
		}

		TM_DEBUG("TMInitInjectPass: %s", modified ? "modified module" : "no changes");
		if (modified) {
			int n = 0;
			for (auto &F : M)
				if (!F.isDeclaration() && F.getName().contains("_tm_clone"))
					++n;
			if (TMAudit)
				errs() << "[BEFORE_INLINE] _tm_clone function definitions: " << n << "\n";
		}

		// When a clone inherits noinline from its original AND has
		// alwaysinline (from the pass), keep alwaysinline and drop
		// noinline.  The AlwaysInlinerPass will then succeed in inlining
		// the clone.  This is essential for RAII helpers like
		// _ConstructTransactionD2 whose plain store to g_vec.__end_
		// must be inlined and instrumented.
		SmallVector<Function *, 8> DeadClones;
		for (auto &F : M) {
			if (F.isDeclaration() || !F.getName().contains("_tm_clone"))
				continue;
			if (F.hasFnAttribute(llvm::Attribute::NoInline) &&
			    F.hasFnAttribute(llvm::Attribute::AlwaysInline)) {
				F.removeFnAttr(llvm::Attribute::NoInline);
				if (F.use_empty())
					DeadClones.push_back(&F);
			}
		}
		for (auto *F : DeadClones)
			F->eraseFromParent();

		return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
	}
	static bool isRequired() { return true; }
};

// ===========================================================================
// PASS 2 (inline pipeline): load/store instrument (after inlining)
// ===========================================================================

class TMInstrumentInlinePass : public PassInfoMixin<TMInstrumentInlinePass>
{
public:
	PreservedAnalyses run(Function &F, FunctionAnalysisManager &)
	{
		if (!hasAnnotation(F, TX_ANNOT) && !hasAnnotation(F, ASYNC_TX_ANNOT)
		    && !F.getName().ends_with(TM_CLONE_SUFFIX)) {
			return PreservedAnalyses::all();
		}
		// Skip TX-annotated originals that have a _tm_clone version
		// (handled by TMQueueGlobalInitPass — already instrumented in clone).
		Module *M = F.getParent();
		if (hasAnnotation(F, TX_ANNOT) || hasAnnotation(F, ASYNC_TX_ANNOT)) {
			std::string CloneName = (F.getName() + TM_CLONE_SUFFIX).str();
			if (M->getFunction(CloneName)) {
				TM_DEBUG("%s has queue clone %s, skipping re-instrumentation",
				         F.getName().str().c_str(), CloneName.c_str());
				return PreservedAnalyses::all();
			}
		}
		LLVMContext &Ctx = M->getContext();
		auto H = TMRuntimeHooks::declareAll(*M, Ctx, tm_platform::sigsetjmpName(*M));

		if (TMAudit)
			auditTXFunctionLoadsStores(F, *M);

		SmallVector<Instruction *, 16> ToErase;
		SmallVector<CallBase *, 8> MemIntrinsics;
		for (auto &BB : F) {
			for (auto InstIt = BB.begin(); InstIt != BB.end();) {
				Instruction *I = &*InstIt++;
				IRBuilder<> B(I->getParent(), I->getIterator());
#ifndef DISABLE_TM_READ_WRITE
				if (auto *Call = dyn_cast<CallBase>(I)) {
					if (needsMemIntrinsicInstrumentation(Call, *M)) {
						MemIntrinsics.push_back(Call);
						continue;
					}
				}
#endif
#ifndef DISABLE_MALLOC_FREE
				if (auto *Call = dyn_cast<CallBase>(I))
					if (handleMallocFree(Call, B, H, ToErase))
						continue;
#endif
#ifndef DISABLE_TM_READ_WRITE
				handleLoadStore(I, F, *M, H, ToErase);
#endif
			}
		}
		// Instrument memory intrinsics AFTER all loops (they split basic blocks,
		// which would invalidate the instruction iterators above).
		for (auto *Call : MemIntrinsics) {
			tm_method_instrumentation::instrumentMemoryIntrinsic(Call, *M, H);
			ToErase.push_back(Call);
		}
		for (Instruction *I : ToErase)
			I->eraseFromParent();
		return PreservedAnalyses::none();
	}
	static bool isRequired() { return true; }
};

// ===========================================================================
// PASS 1 (legacy pipeline): clone-with-instrumentation + thread init
// ===========================================================================

class TMGlobalInitPass : public PassInfoMixin<TMGlobalInitPass>
{
public:
	PreservedAnalyses run(Module &M, ModuleAnalysisManager &)
	{
		TM_DEBUG("TMGlobalInitPass: processing module %s", M.getName().str().c_str());
		checkAnnotationConsistency(M);
		auto Ctx = setupModulePass(M);
		bool &modified = Ctx.modified;

		if (!Ctx.TxReachableFuncs.empty()) {
			TM_DEBUG("Tx-reachable call graph has %d functions",
			         (int)Ctx.TxReachableFuncs.size());
			// CloneOnly: clones without instrumentation, so tracesFromTMGlobal
			// can find actual callers and trace arguments through allocas to TM
			// globals.  Then instrument ALL clones after call redirection.
			Ctx.ClonedMap = &tm_method_instrumentation::
			                    cloneTxReachableGraph(M,
			                                          Ctx.TxReachableFuncs,
			                                          Ctx.H,
			                                          tm_method_instrumentation::
			                                              CloneMode::CloneOnly);
			redirectTXFunctionsToClones(M,
			                            Ctx.TxReachableFuncs,
			                            *Ctx.ClonedMap,
			                            tm_method_instrumentation::CloneMode::CloneOnly);
			tm_method_instrumentation::instrumentAllClones(*Ctx.ClonedMap, M, Ctx.H);
			modified = true;
		}

		checkOpaqueOrAbort(M, Ctx.TxReachableFuncs);

		SmallPtrSet<Function *, 32> EmptyEntries;

		if (Function *MainFn = M.getFunction(MAIN_ANNOT))
			instrumentMainInitExit(MainFn, Ctx);

		instrumentThreadEntries(M, EmptyEntries, Ctx);

		TM_DEBUG("TMGlobalInitPass: %s", modified ? "modified module" : "no changes");
		return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
	}
	static bool isRequired() { return true; }
};

// ===========================================================================
// PASS 1 (combined pipeline): clone + instrument + prepare for inlining
//   Clones ALL reachable callees (AlwaysInline mode), instruments each clone
//   individually (without NoInline/OptimizeNone), and ensures AlwaysInline
//   is set so the subsequent AlwaysInlinerPass will inline them into the TX
//   function body.  After inlining, TMInstrumentPass handles the TX body
//   (tx_begin/end, residual loads/stores, memory intrinsics).
//
//   This gives the debugging benefit of per-clone instrumentation
//   (isolated, easy to verify) with the runtime correctness of the inline
//   pipeline (all callee internals visible for TM instrumentation).
// ===========================================================================

class TMGlobalInitThenInlinePass : public PassInfoMixin<TMGlobalInitThenInlinePass>
{
public:
	PreservedAnalyses run(Module &M, ModuleAnalysisManager &)
	{
		TM_DEBUG("TMGlobalInitThenInlinePass: processing module %s",
		         M.getName().str().c_str());
		auto Ctx = setupModulePass(M);
		bool &modified = Ctx.modified;
		using namespace tm_method_instrumentation;

		if (!Ctx.TxReachableFuncs.empty()) {
			TM_DEBUG("Tx-reachable call graph has %d functions",
			         (int)Ctx.TxReachableFuncs.size());
			// Clone with AlwaysInline mode so ALL reachable functions are cloned
			Ctx.ClonedMap = &cloneTxReachableGraph(M,
			                                       Ctx.TxReachableFuncs,
			                                       Ctx.H,
			                                       CloneMode::AlwaysInline);
			redirectTXFunctionsToClones(M,
			                            Ctx.TxReachableFuncs,
			                            *Ctx.ClonedMap,
			                            CloneMode::AlwaysInline);
			// Instrument each clone individually (before inlining).
			// Unlike instrumentAllClones, we do NOT add NoInline/OptimizeNone —
			// these clones will be inlined into the TX function body afterwards.
			for (auto &pair : *Ctx.ClonedMap) {
				Function *Clone = pair.second;
				auto OrigIt = TMTracedArgs.find(pair.first);
				if (OrigIt != TMTracedArgs.end())
					TMTracedArgs[Clone] = OrigIt->second;
				instrumentLoadsStoresInFunction(Clone, &M, Ctx.H);
				TM_DEBUG("Pre-inline instrumented clone: %s",
				         Clone->getName().str().c_str());
			}
			modified = true;
		}

		checkOpaqueOrAbort(M, Ctx.TxReachableFuncs);

		SmallPtrSet<Function *, 32> EmptyEntries;

		if (Function *MainFn = M.getFunction(MAIN_ANNOT))
			instrumentMainInitExit(MainFn, Ctx);

		instrumentThreadEntries(M, EmptyEntries, Ctx);

		TM_DEBUG("TMGlobalInitThenInlinePass: %s",
		         modified ? "modified module" : "no changes");
		return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
	}
	static bool isRequired() { return true; }
};

// ===========================================================================
// PASS 2 (legacy pipeline): tx_begin/end + load/store instrument
// ===========================================================================

class TMInstrumentPass : public PassInfoMixin<TMInstrumentPass>
{
public:
	PreservedAnalyses run(Function &F, FunctionAnalysisManager &)
	{
		if (!hasAnnotation(F, TX_ANNOT) && !hasAnnotation(F, ASYNC_TX_ANNOT)) {
			TM_DEBUG("%s is not a transaction function, skipping",
			         F.getName().str().c_str());
			return PreservedAnalyses::all();
		}
		// Skip functions that have already been handled by the queue pipeline
		// (detected by the presence of a _tm_clone version of the same function).
		// The queue pass clones + instruments the clone, and the original should
		// remain un-instrumented (callers are replaced with tm_enqueue).
		Module *M = F.getParent();
		{
			std::string CloneName = (F.getName() + TM_CLONE_SUFFIX).str();
			if (M->getFunction(CloneName)) {
				TM_DEBUG("%s has queue clone %s, skipping re-instrumentation",
				         F.getName().str().c_str(), CloneName.c_str());
				return PreservedAnalyses::all();
			}
		}
		TM_DEBUG("TMInstrumentPass: processing function %s", F.getName().str().c_str());
		LLVMContext &Ctx = M->getContext();
		auto H = TMRuntimeHooks::declareAll(*M, Ctx, tm_platform::sigsetjmpName(*M));

		injectTransactionBeginEnd(F, *M, H);

		if (TMAudit)
			auditTXFunctionLoadsStores(F, *M);

		SmallVector<Instruction *, 16> ToErase;
		SmallVector<CallBase *, 8> MemIntrinsics;

		for (auto &BB : F) {
			for (auto InstIt = BB.begin(); InstIt != BB.end();) {
				Instruction *I = &*InstIt++;
				IRBuilder<> B(I->getParent(), I->getIterator());
#ifndef DISABLE_TM_READ_WRITE
				if (auto *Call = dyn_cast<CallBase>(I))
					if (needsMemIntrinsicInstrumentation(Call, *M)) {
						MemIntrinsics.push_back(Call);
						continue;
					}
#endif
#ifndef DISABLE_MALLOC_FREE
				if (auto *Call = dyn_cast<CallBase>(I))
					if (handleMallocFree(Call, B, H, ToErase))
						continue;
#endif
#ifndef DISABLE_TM_READ_WRITE
				handleLoadStore(I, F, *M, H, ToErase);
#endif
			}
		}
		// Instrument memory intrinsics AFTER all loops (they split basic blocks,
		// which would invalidate the instruction iterators above).
		for (auto *Call : MemIntrinsics) {
			tm_method_instrumentation::instrumentMemoryIntrinsic(Call, *M, H);
			ToErase.push_back(Call);
		}
		for (Instruction *I : ToErase)
			I->eraseFromParent();
		return PreservedAnalyses::none();
	}
	static bool isRequired() { return true; }
};

// TLS variable names for queue runtime (accessed via M.getGlobalVariable)
static constexpr char TM_QUEUE_ACTIVE_TLS[] = "g_tm_queue_active";

// Check if a function has any TX annotation (transaction or async_transaction)
static bool hasAnyTXAnnotation(Function &F)
{
	return hasAnnotation(F, TX_ANNOT) || hasAnnotation(F, ASYNC_TX_ANNOT);
}

static Function *findClone(Function *Original, Module &M)
{
	std::string CloneName = (Original->getName() + TM_CLONE_SUFFIX).str();
	return M.getFunction(CloneName);
}

static StructType *createArgsStructType(Function *F, LLVMContext &Ctx)
{
	SmallVector<Type *, 8> ArgTypes;
	for (auto &Arg : F->args())
		ArgTypes.push_back(Arg.getType());
	if (ArgTypes.empty())
		return nullptr;
	std::string StructName = (F->getName() + "_args_t").str();
	return StructType::create(Ctx, ArgTypes, StructName);
}

// Generate a dispatch wrapper for a TX function.
// The wrapper receives a void* pointing to the packed arguments struct,
// loads the args, calls the _tm_clone, then frees the struct.
//
// Returns the dispatch function or nullptr on failure.
static Function *createDispatchWrapper(Function *Original, Function *Clone,
                                       Module &M, IRBuilder<> &B)
{
	LLVMContext &Ctx = M.getContext();
	auto *i8PtrTy = PointerType::getUnqual(Ctx);
	auto *voidTy  = Type::getVoidTy(Ctx);

	// Create the dispatch function type: void(i8*)
	auto *DispFT = FunctionType::get(voidTy, {i8PtrTy}, false);
	auto *DispF = Function::Create(DispFT,
	                               GlobalValue::InternalLinkage,
	                               Original->getName() + TM_DISPATCH_SUFFIX,
	                               &M);

	// Create entry block
	auto *Entry = BasicBlock::Create(Ctx, "entry", DispF);
	B.SetInsertPoint(Entry);

	// Build the packed args struct type
	StructType *ArgsTy = createArgsStructType(Original, Ctx);
	if (!ArgsTy) {
		// Function takes no arguments — dispatch just calls clone
		B.CreateCall(Clone, {});
		B.CreateRetVoid();
		return DispF;
	}

	// Bitcast raw_args to the struct pointer
	Value *RawArg = DispF->getArg(0); // i8*
	Value *ArgsPtr = B.CreateBitCast(RawArg, PointerType::getUnqual(Ctx));

	// Load each argument from the struct
	SmallVector<Value *, 8> LoadedArgs;
	unsigned Idx = 0;
	for (auto &Arg : Original->args()) {
		Value *ElemPtr = B.CreateStructGEP(ArgsTy, ArgsPtr, Idx);
		Value *Loaded = B.CreateLoad(Arg.getType(), ElemPtr);
		LoadedArgs.push_back(Loaded);
		++Idx;
	}

	// Call the clone with loaded args
	B.CreateCall(Clone, LoadedArgs);

	// Free the arg struct
	FunctionCallee FreeFn = M.getOrInsertFunction("free",
	    FunctionType::get(voidTy, {i8PtrTy}, false));
	B.CreateCall(FreeFn, {RawArg});

	B.CreateRetVoid();
	DispF->addFnAttr(llvm::Attribute::NoInline);
	TM_DEBUG("Created dispatch wrapper: %s", DispF->getName().str().c_str());
	return DispF;
}

// Replace a call site to _tm_clone with tm_enqueue(dispatch, packed_args)
// and, if sync=true, tm_wait_prev_tx().
static void replaceCallWithEnqueue(CallBase *Call, Function *DispatchFn,
                                   Module &M, IRBuilder<> &B,
                                   const TMRuntimeHooks &H, bool sync)
{
	LLVMContext &Ctx = M.getContext();
	auto *i8PtrTy = PointerType::getUnqual(Ctx);
	auto *i64Ty = Type::getInt64Ty(Ctx);
	auto *voidTy = Type::getVoidTy(Ctx);

	Function *CalledFn = Call->getCalledFunction();
	if (!CalledFn)
		return;

	// Compute argument struct size
	StructType *ArgsTy = createArgsStructType(CalledFn, Ctx);
	if (!ArgsTy) {
		// No args: just enqueue with NULL
		B.SetInsertPoint(Call);
		B.CreateCall(H.enqueue_fn, {
		    B.CreateBitCast(DispatchFn, i8PtrTy),
		    ConstantPointerNull::get(i8PtrTy)});
		if (sync)
			B.CreateCall(H.wait_prev_tx_fn, {});
		return;
	}

	const DataLayout &DL = M.getDataLayout();
	uint64_t StructSize = DL.getTypeAllocSize(ArgsTy);

	// malloc the args struct
	FunctionCallee MallocFn = M.getOrInsertFunction("malloc",
	    FunctionType::get(i8PtrTy, {i64Ty}, false));
	B.SetInsertPoint(Call);
	Value *RawArgs = B.CreateCall(MallocFn, {ConstantInt::get(i64Ty, StructSize)});
	Value *ArgsPtr = B.CreateBitCast(RawArgs, PointerType::getUnqual(Ctx));

	// Store each argument into the struct
	unsigned Idx = 0;
	for (auto &Arg : Call->args()) {
		Value *ElemPtr = B.CreateStructGEP(ArgsTy, ArgsPtr, Idx);
		B.CreateStore(Arg, ElemPtr);
		++Idx;
	}

	// tm_enqueue(dispatch, raw_args)
	B.CreateCall(H.enqueue_fn, {B.CreateBitCast(DispatchFn, i8PtrTy), RawArgs});

	// If sync, tm_wait_prev_tx()
	if (sync)
		B.CreateCall(H.wait_prev_tx_fn, {});
}

// ===========================================================================
// PASS (queue pipeline): clone + instrument + dispatch wrapper + enqueue
// ===========================================================================

class TMQueueGlobalInitPass : public PassInfoMixin<TMQueueGlobalInitPass>
{
public:
	PreservedAnalyses run(Module &M, ModuleAnalysisManager &)
	{
		TM_DEBUG("TMQueueGlobalInitPass: processing module %s", M.getName().str().c_str());
		checkAnnotationConsistency(M);
		auto Ctx = setupModulePass(M);
		bool &modified = Ctx.modified;
		using namespace tm_method_instrumentation;

		// 0. Clone TX-annotated functions FIRST, since cloneTxReachableGraph
		//    skips them (hasAnnotation TX_ANNOT/ASYNC_TX_ANNOT → continue).
		//    The clones serve as the runnable TX body on worker threads.
		{
			auto &AllClones = tm_method_instrumentation::getClonedMethodsMap();
			SmallPtrSet<const GlobalVariable *, 16> TMG;
			tm_method_instrumentation::collectTMGlobalsCached(M, TMG);
			for (auto &F : M) {
				if (F.isDeclaration())
					continue;
				if (!hasAnyTXAnnotation(F))
					continue;
				// Check if already cloned
				bool already = false;
				for (auto &pair : AllClones)
					if (pair.first == &F) { already = true; break; }
				if (already)
					continue;

				Function *Clone = tm_method_instrumentation::cloneMethod(
				    &F, TM_CLONE_SUFFIX, &M, M.getContext(), TMG, Ctx.H,
				    CloneMode::CloneOnly);
				AllClones.push_back({&F, Clone});
				TM_DEBUG("Queue: cloned TX function %s -> %s",
				         F.getName().str().c_str(),
				         Clone->getName().str().c_str());
			}
		}

		if (!Ctx.TxReachableFuncs.empty()) {
			// 1. Clone the reachable call graph (CloneOnly mode)
			//    This also appends to the same getClonedMethodsMap() used above,
			//    but skips already-cloned and TX-annotated functions.
			Ctx.ClonedMap = &cloneTxReachableGraph(M,
			                                        Ctx.TxReachableFuncs,
			                                        Ctx.H,
			                                        CloneMode::CloneOnly);
			// 2. Redirect all calls to _tm_clone versions.
			//    For TX functions this moves external callers (e.g., main)
			//    to the clone, letting us later replace with tm_enqueue.
			redirectTXFunctionsToClones(M,
			                            Ctx.TxReachableFuncs,
			                            *Ctx.ClonedMap,
			                            CloneMode::CloneOnly);
			// 3. Instrument all clones (load/store + malloc instrumentation)
			instrumentAllClones(*Ctx.ClonedMap, M, Ctx.H);
			// 4. Inject tm_begin/tm_end + sigsetjmp retry into each clone
			//    (clones run as self-contained TXs on worker threads)
			for (auto &pair : *Ctx.ClonedMap) {
				Function *Clone = pair.second;
				injectTransactionBeginEnd(*Clone, M, Ctx.H);
			}
			modified = true;
		}

		checkOpaqueOrAbort(M, Ctx.TxReachableFuncs);

		// 4. Create dispatch wrappers for each TX function
		SmallVector<std::pair<Function *, Function *>, 8> DispatchWrappers;
		{
			IRBuilder<> B(M.getContext());
			for (auto &F : M) {
				if (F.isDeclaration())
					continue;
				if (!hasAnyTXAnnotation(F))
					continue;
				Function *Clone = findClone(&F, M);
				if (!Clone) {
					TM_DEBUG("No clone found for TX function %s, skipping dispatch",
					         F.getName().str().c_str());
					continue;
				}
				// Only handle void-returning functions for now
				if (!F.getReturnType()->isVoidTy()) {
					TM_DEBUG("TX function %s returns non-void, skipping dispatch",
					         F.getName().str().c_str());
					continue;
				}
				Function *DispF = createDispatchWrapper(&F, Clone, M, B);
				if (DispF)
					DispatchWrappers.push_back({&F, DispF});
			}
		}

		// 5. Replace calls to ORIGINAL TX functions with tm_enqueue + optional
		//    tm_wait_prev_tx.  We use Orig (the original function) because
		//    external callers (main) still call the original, not the clone.
		LLVMContext &CtxRef = M.getContext();
		for (auto &[Orig, DispF] : DispatchWrappers) {
			bool isSync = hasAnnotation(*Orig, TX_ANNOT);

			// Collect call sites to replace — look for calls to Orig
			SmallVector<CallBase *, 16> CallSites;
			for (auto *U : Orig->users()) {
				auto *Call = dyn_cast<CallBase>(U);
				if (!Call)
					continue;
				Function *Caller = Call->getFunction();
				if (!Caller)
					continue;
				// Skip calls from within _tm_clone functions (internal recursion)
				if (Caller->getName().contains(TM_CLONE_SUFFIX))
					continue;
				CallSites.push_back(Call);
			}

			for (auto *Call : CallSites) {
				IRBuilder<> B(Call);
				replaceCallWithEnqueue(Call, DispF, M, B, Ctx.H, isSync);
				// Erase original call
				Call->eraseFromParent();
				modified = true;
			}
		}

		// 6. Declare TLS globals for queue mode
		auto getOrCreateTLS = [&](StringRef Name, Type *Ty) -> GlobalVariable * {
			if (auto *GV = M.getGlobalVariable(Name))
				return GV;
			auto *GV = new GlobalVariable(M,
			                              Ty,
			                              false,
			                              GlobalValue::ExternalLinkage,
			                              nullptr,
			                              Name);
			GV->setThreadLocal(true);
			return GV;
		};
		getOrCreateTLS(TM_QUEUE_ACTIVE_TLS, Type::getInt32Ty(CtxRef));

		// 7. Inject tm_init/tm_exit + tm_queue_init/tm_queue_shutdown in main.
		//    Also inject tm_init_thread/tm_exit_thread into THREAD functions.
		{
			SmallPtrSet<Function *, 32> EmptyEntries;
			instrumentThreadEntries(M, EmptyEntries, Ctx);
		}

		if (Function *MainFn = M.getFunction(MAIN_ANNOT)) {
			// Insert tm_queue_init at beginning of entry block.
			// instrumentMainInitExit will insert tm_init/tm_init_thread
			// after this (both use Entry.begin(), pushing ours down).
			auto *i32Ty2 = Type::getInt32Ty(CtxRef);
			FunctionCallee QueueInit =
			    M.getOrInsertFunction("tm_queue_init",
			        FunctionType::get(Type::getVoidTy(CtxRef),
			                          {i32Ty2, i32Ty2}, false));
			{
				IRBuilder<> Builder(&MainFn->getEntryBlock(),
				                    MainFn->getEntryBlock().begin());
				Builder.CreateCall(QueueInit,
				                   {ConstantInt::get(i32Ty2, 4),
				                    ConstantInt::get(i32Ty2, 4)});
			}

			// Insert tm_queue_shutdown before returns.
			// instrumentMainInitExit will insert tm_exit_thread/tm_exit
			// before the return too, but after our tm_queue_shutdown
			// (both use IRBuilder<>(Ret), inserting before Ret).
			auto MainReturns = collectReturns(*MainFn);
			FunctionCallee QueueShutdown =
			    M.getOrInsertFunction("tm_queue_shutdown",
			        FunctionType::get(Type::getVoidTy(CtxRef), {}, false));
			for (auto *Ret : MainReturns) {
				IRBuilder<> RetBuilder(Ret);
				RetBuilder.CreateCall(QueueShutdown, {});
			}

			// Inject tm_init/tm_exit + tm_init_thread/tm_exit_thread in
			// main.  This inserts AFTER tm_queue_init at entry and AFTER
			// tm_queue_shutdown before returns (see comments above).
			instrumentMainInitExit(MainFn, Ctx);
		}

		TM_DEBUG("TMQueueGlobalInitPass: %s", modified ? "modified module" : "no changes");
		return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
	}
	static bool isRequired() { return true; }
};

// ===========================================================================
// Utility pass: Strip llvm.lifetime.start/end intrinsics for LLVM version
// portability. LLVM 21 -> 22 changed these from (i64, ptr) to (ptr), causing
// "Intrinsic has incorrect argument type!" when linking with older LLVM.
// Lifetime intrinsics are optimization hints only — removing them is always
// safe.
// ===========================================================================
class TMStripLifetimePass : public PassInfoMixin<TMStripLifetimePass>
{
public:
	PreservedAnalyses run(Module &M, ModuleAnalysisManager &)
	{
		bool changed = false;
		for (auto &F : M) {
			for (auto &BB : F) {
				for (auto InstIt = BB.begin(); InstIt != BB.end();) {
					Instruction *I = &*InstIt++;
					if (auto *CI = dyn_cast<CallInst>(I)) {
						if (CI->getIntrinsicID() == Intrinsic::lifetime_start ||
						    CI->getIntrinsicID() == Intrinsic::lifetime_end) {
							I->eraseFromParent();
							changed = true;
						}
					}
				}
			}
		}
		return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
	}
	static bool isRequired() { return true; }
};

// ===========================================================================
// Verification pass: detect missed load/store instrumentation
// PURPOSE: Run AFTER TM instrumentation to verify that all loads and stores
//          in TX functions (and _tm_clone functions) have been wrapped in
//          tm_read/tm_write calls. Reports any remaining raw LoadInst/StoreInst
//          whose base pointer is not an alloca (local variable) or tm_local
//          annotation.
// USAGE:  Append "tm-instrument-check" to any pipeline, e.g.:
//           -passes="tm-instrument,tm-instrument-check"
// ===========================================================================

class TMInstrumentCheckPass : public PassInfoMixin<TMInstrumentCheckPass>
{
public:
	PreservedAnalyses run(Function &F, FunctionAnalysisManager &)
	{
		if (!hasAnnotation(F, TX_ANNOT) && !hasAnnotation(F, ASYNC_TX_ANNOT)
		    && !F.getName().contains(TM_CLONE_SUFFIX)) {
			return PreservedAnalyses::all();
		}
		Module *M = F.getParent();
		bool found = false;

		for (auto &BB : F) {
			for (auto &I : BB) {
				// AtomicRMW/CmpXchg are intentionally skipped by the instrumenter
				if (isa<AtomicRMWInst>(I) || isa<AtomicCmpXchgInst>(I))
					continue;

				Value *Addr = nullptr;
				bool isLoad = false;
				bool isStore = false;

				if (auto *LI = dyn_cast<LoadInst>(&I)) {
					Addr = LI->getPointerOperand();
					isLoad = true;
				} else if (auto *SI = dyn_cast<StoreInst>(&I)) {
					Addr = SI->getPointerOperand();
					isStore = true;
				}

				if (!Addr)
					continue;

				// Check if this is an alloca-based address (local stack variable)
				if (isa<AllocaInst>(Addr->stripInBoundsConstantOffsets()))
					continue;

				// Check for tm_local annotation
				if (isTMLocalVar(Addr, *M))
					continue;

				// Uninstrumented load/store found
				found = true;
				errs() << "TM-CHECK: UNINSTRUMENTED " << (isLoad ? "LOAD" : "STORE")
				       << " in " << F.getName() << "\n";
				errs() << "  ";
				I.print(errs());
				errs() << "\n";
				if (auto *DIL = I.getDebugLoc().get()) {
					if (auto *Scope = dyn_cast_or_null<DIScope>(DIL->getScope()))
						errs() << "  at " << Scope->getFilename() << ":" << DIL->getLine()
						       << ":" << DIL->getColumn() << "\n";
				}
			}
		}

		if (found) {
			errs() << "TM-CHECK: FAIL - " << F.getName()
			       << " has uninstrumented loads/stores\n";
		}

		if (found && !AllowOpaque) {
			errs() << "error: TM instrumentation check failed. "
			          "Use -tm-allow-opaque to suppress.\n";
			exit(1);
		}

		return PreservedAnalyses::all();
	}
	static bool isRequired() { return true; }
};

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
			            // ---- Debug-friendly pipeline (no inlining, -O0 compatible) ----
			            if (Name == "tm-instrument" || Name == "tm-instrument-debug") {
				            TM_DEBUG("Registering %s pass pipeline", Name.str().c_str());
				            MPM.addPass(TMGlobalInitPass());
				            MPM.addPass(
				                createModuleToFunctionPassAdaptor(TMInstrumentPass()));
				            MPM.addPass(TMStripLifetimePass());
				            return true;
			            }
			            // ---- Inline pipeline (inline callees first, then instrument) ----
			            if (Name == "tm-instrument-inline") {
				            TM_DEBUG("Registering tm-instrument-inline pass pipeline");
				            MPM.addPass(TMInitInjectPass());
				            MPM.addPass(AlwaysInlinerPass());
				            MPM.addPass(createModuleToFunctionPassAdaptor(
				                TMInstrumentInlinePass()));
				            MPM.addPass(TMStripLifetimePass());
				            return true;
			            }
			            // ---- Combined pipeline (instrument clones, then inline) ----
			            if (Name == "tm-instrument-then-inline") {
				            TM_DEBUG(
				                "Registering tm-instrument-then-inline pass pipeline");
				            MPM.addPass(TMGlobalInitThenInlinePass());
				            MPM.addPass(AlwaysInlinerPass());
				            MPM.addPass(
				                createModuleToFunctionPassAdaptor(TMInstrumentPass()));
				            MPM.addPass(TMStripLifetimePass());
				            return true;
			            }
			            // ---- Queue pipeline (clone + dispatch wrapper + enqueue) ----
			            if (Name == "tm-instrument-queue") {
				            TM_DEBUG("Registering tm-instrument-queue pass pipeline");
				            MPM.addPass(TMQueueGlobalInitPass());
				            MPM.addPass(createModuleToFunctionPassAdaptor(
				                TMInstrumentInlinePass()));
				            MPM.addPass(TMStripLifetimePass());
				            return true;
			            }
			            // ---- Standalone check pass (append to any pipeline) ----
			            if (Name == "tm-instrument-check") {
				            TM_DEBUG("Registering tm-instrument-check pass");
				            MPM.addPass(createModuleToFunctionPassAdaptor(
				                TMInstrumentCheckPass()));
				            return true;
			            }
			            return false;
		            });
	        }};
}
