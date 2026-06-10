// TMInstrumentPass.cpp
// Pipeline registration and backward-compatibility passes.
//
// Primary pipeline (5-step Honorio-style decomposition):
//   tm-instrument:
//     TMCollectPass → TMClonePass → TMRedirectPass → TMInstrumentFnPass → TMCleanupPass
//
// Other pipelines (non-default variants):
//   tm-instrument-inline:
//     TMInitInjectPass + AlwaysInlinerPass + TMInstrumentInlinePass
//   tm-instrument-then-inline:
//     TMGlobalInitThenInlinePass + AlwaysInlinerPass + TMInstrumentPass
//   tm-instrument-queue:
//     TMQueueGlobalInitPass + TMInstrumentInlinePass

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
#include "tm_check_opaque.hpp"
#include "tm_debug.hpp"
#include "tm_instrument_helpers.hpp"
#include "tm_local_vars.hpp"
#include "tm_method_instrumentation.hpp"
#include "tm_platform.hpp"
#include "tm_runtime_hooks.hpp"
#include "tm_thread_guard.hpp"
using namespace llvm;

// ===========================================================================
// Command-line options (single definition — declared extern in tm_pipeline_opts.hpp)
// ===========================================================================

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
    cl::desc("Print detailed instrumentation audit to stderr"),
    cl::init(false));

cl::opt<bool> EmitTrace(
    "emit-tm-trace",
    cl::desc("Emit tm_trace before each instrumented access"),
    cl::init(false));

// ===========================================================================
// Backward-compatibility pass aliases — delegate to the 5-step state
// ===========================================================================

// The 5 pass classes are defined in their own files:
//   TMCollectPass       → TMCollectPass.cpp
//   TMClonePass         → TMClonePass.cpp
//   TMRedirectPass      → TMRedirectPass.cpp
//   TMInstrumentFnPass  → TMInstrumentFnPass.cpp
//   TMCleanupPass       → TMCleanupPass.cpp

// ===========================================================================
// PASS: Inline pipeline — clone with alwaysinline, inject tx_begin/tm_end
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
			tm_method_instrumentation::redirectTXFunctionsToClones(
			    M, Ctx.TxReachableFuncs, *Ctx.ClonedMap,
			    tm_method_instrumentation::CloneMode::AlwaysInline);
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
// PASS: Inline pipeline — load/store instrument (after inlining)
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
// PASS: Legacy pipeline — tx_begin/end + load/store instrument (standalone)
// ===========================================================================

class TMInstrumentPass : public PassInfoMixin<TMInstrumentPass>
{
public:
	PreservedAnalyses run(Function &F, FunctionAnalysisManager &)
	{
		if (!hasAnnotation(F, TX_ANNOT) && !hasAnnotation(F, ASYNC_TX_ANNOT)) {
			return PreservedAnalyses::all();
		}
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
// PASS (combined pipeline): clone + pre-instrument + prepare for inlining
// ===========================================================================

class TMGlobalInitThenInlinePass : public PassInfoMixin<TMGlobalInitThenInlinePass>
{
public:
	PreservedAnalyses run(Module &M, ModuleAnalysisManager &)
	{
		TM_DEBUG("TMGlobalInitThenInlinePass: processing module %s",
		         M.getName().str().c_str());
		checkAnnotationConsistency(M);
		auto Ctx = setupModulePass(M);
		bool &modified = Ctx.modified;
		using namespace tm_method_instrumentation;

		if (!Ctx.TxReachableFuncs.empty()) {
			Ctx.ClonedMap = &cloneTxReachableGraph(M,
			                                        Ctx.TxReachableFuncs,
			                                        Ctx.H,
			                                        CloneMode::AlwaysInline);
			redirectTXFunctionsToClones(M,
			                            Ctx.TxReachableFuncs,
			                            *Ctx.ClonedMap,
			                            CloneMode::AlwaysInline);
			for (auto &pair : *Ctx.ClonedMap) {
				Function *Clone = pair.second;
				auto OrigIt = TMTracedArgs.find(pair.first);
				if (OrigIt != TMTracedArgs.end())
					TMTracedArgs[Clone] = OrigIt->second;
				instrumentLoadsStoresInFunction(Clone, &M, Ctx.H);
			}
			modified = true;
		}

		checkOpaqueOrAbort(M, Ctx.TxReachableFuncs);

		SmallPtrSet<Function *, 32> EmptyEntries;

		if (Function *MainFn = M.getFunction(MAIN_ANNOT))
			instrumentMainInitExit(MainFn, Ctx);

		instrumentThreadEntries(M, EmptyEntries, Ctx);

		return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
	}
	static bool isRequired() { return true; }
};

// ===========================================================================
// Queue pipeline helper functions
// ===========================================================================

static constexpr char TM_QUEUE_ACTIVE_TLS[] = "g_tm_queue_active";

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

static Function *createDispatchWrapper(Function *Original, Function *Clone,
                                       Module &M, IRBuilder<> &B)
{
	LLVMContext &Ctx = M.getContext();
	auto *i8PtrTy = PointerType::getUnqual(Ctx);
	auto *voidTy  = Type::getVoidTy(Ctx);

	auto *DispFT = FunctionType::get(voidTy, {i8PtrTy}, false);
	auto *DispF = Function::Create(DispFT,
	                               GlobalValue::InternalLinkage,
	                               Original->getName() + TM_DISPATCH_SUFFIX,
	                               &M);

	auto *Entry = BasicBlock::Create(Ctx, "entry", DispF);
	B.SetInsertPoint(Entry);

	StructType *ArgsTy = createArgsStructType(Original, Ctx);
	if (!ArgsTy) {
		B.CreateCall(Clone, {});
		B.CreateRetVoid();
		return DispF;
	}

	Value *RawArg = DispF->getArg(0);
	Value *ArgsPtr = B.CreateBitCast(RawArg, PointerType::getUnqual(Ctx));

	SmallVector<Value *, 8> LoadedArgs;
	unsigned Idx = 0;
	for (auto &Arg : Original->args()) {
		Value *ElemPtr = B.CreateStructGEP(ArgsTy, ArgsPtr, Idx);
		Value *Loaded = B.CreateLoad(Arg.getType(), ElemPtr);
		LoadedArgs.push_back(Loaded);
		++Idx;
	}

	B.CreateCall(Clone, LoadedArgs);

	FunctionCallee FreeFn = M.getOrInsertFunction("free",
	    FunctionType::get(voidTy, {i8PtrTy}, false));
	B.CreateCall(FreeFn, {RawArg});

	B.CreateRetVoid();
	DispF->addFnAttr(llvm::Attribute::NoInline);
	TM_DEBUG("Created dispatch wrapper: %s", DispF->getName().str().c_str());
	return DispF;
}

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

	StructType *ArgsTy = createArgsStructType(CalledFn, Ctx);
	if (!ArgsTy) {
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

	FunctionCallee MallocFn = M.getOrInsertFunction("malloc",
	    FunctionType::get(i8PtrTy, {i64Ty}, false));
	B.SetInsertPoint(Call);
	Value *RawArgs = B.CreateCall(MallocFn, {ConstantInt::get(i64Ty, StructSize)});
	Value *ArgsPtr = B.CreateBitCast(RawArgs, PointerType::getUnqual(Ctx));

	unsigned Idx = 0;
	for (auto &Arg : Call->args()) {
		Value *ElemPtr = B.CreateStructGEP(ArgsTy, ArgsPtr, Idx);
		B.CreateStore(Arg, ElemPtr);
		++Idx;
	}

	B.CreateCall(H.enqueue_fn, {B.CreateBitCast(DispatchFn, i8PtrTy), RawArgs});

	if (sync)
		B.CreateCall(H.wait_prev_tx_fn, {});
}

// ===========================================================================
// PASS: Queue pipeline — clone + dispatch wrapper + enqueue
// ===========================================================================

class TMQueueGlobalInitPass : public PassInfoMixin<TMQueueGlobalInitPass>
{
public:
	TMQueueGlobalInitPass() : injectWait_(true) {}
	TMQueueGlobalInitPass(bool injectWait) : injectWait_(injectWait) {}

	PreservedAnalyses run(Module &M, ModuleAnalysisManager &)
	{
		TM_DEBUG("TMQueueGlobalInitPass: processing module %s", M.getName().str().c_str());
		checkAnnotationConsistency(M);
		auto Ctx = setupModulePass(M);
		bool &modified = Ctx.modified;
		using namespace tm_method_instrumentation;

		{
			auto &AllClones = tm_method_instrumentation::getClonedMethodsMap();
			SmallPtrSet<const GlobalVariable *, 16> TMG;
			tm_method_instrumentation::collectTMGlobalsCached(M, TMG);
			for (auto &F : M) {
				if (F.isDeclaration())
					continue;
				if (!hasAnnotation(F, TX_ANNOT) && !hasAnnotation(F, ASYNC_TX_ANNOT))
					continue;
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
			Ctx.ClonedMap = &cloneTxReachableGraph(M,
			                                        Ctx.TxReachableFuncs,
			                                        Ctx.H,
			                                        CloneMode::CloneOnly);
			redirectTXFunctionsToClones(M,
			                            Ctx.TxReachableFuncs,
			                            *Ctx.ClonedMap,
			                            CloneMode::CloneOnly);
			instrumentAllClones(*Ctx.ClonedMap, M, Ctx.H);
			for (auto &pair : *Ctx.ClonedMap) {
				Function *Clone = pair.second;
				injectTransactionBeginEnd(*Clone, M, Ctx.H);
			}
			modified = true;
		}

		checkOpaqueOrAbort(M, Ctx.TxReachableFuncs);

		SmallVector<std::pair<Function *, Function *>, 8> DispatchWrappers;
		{
			IRBuilder<> B(M.getContext());
			for (auto &F : M) {
				if (F.isDeclaration())
					continue;
				if (!hasAnnotation(F, TX_ANNOT) && !hasAnnotation(F, ASYNC_TX_ANNOT))
					continue;
				Function *Clone = findClone(&F, M);
				if (!Clone) {
					TM_DEBUG("No clone found for TX function %s, skipping dispatch",
					         F.getName().str().c_str());
					continue;
				}
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

		LLVMContext &CtxRef = M.getContext();
		for (auto &[Orig, DispF] : DispatchWrappers) {
			bool isSync = hasAnnotation(*Orig, TX_ANNOT);

			SmallVector<CallBase *, 16> CallSites;
			for (auto *U : Orig->users()) {
				auto *Call = dyn_cast<CallBase>(U);
				if (!Call)
					continue;
				Function *Caller = Call->getFunction();
				if (!Caller)
					continue;
				if (Caller->getName().contains(TM_CLONE_SUFFIX))
					continue;
				CallSites.push_back(Call);
			}

			for (auto *Call : CallSites) {
				IRBuilder<> B(Call);
				replaceCallWithEnqueue(Call, DispF, M, B, Ctx.H, injectWait_ && isSync);
				Call->eraseFromParent();
				modified = true;
			}
		}

		auto getOrCreateTLS = [&](StringRef Name, Type *Ty) -> GlobalVariable * {
			if (auto *GV = M.getGlobalVariable(Name))
				return GV;
			auto *GV = new GlobalVariable(M, Ty, false,
			                              GlobalValue::ExternalLinkage,
			                              nullptr, Name);
			GV->setThreadLocal(true);
			return GV;
		};
		getOrCreateTLS(TM_QUEUE_ACTIVE_TLS, Type::getInt32Ty(CtxRef));

		{
			SmallPtrSet<Function *, 32> EmptyEntries;
			instrumentThreadEntries(M, EmptyEntries, Ctx);
		}

		if (Function *MainFn = M.getFunction(MAIN_ANNOT)) {
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

			auto MainReturns = collectReturns(*MainFn);
			FunctionCallee QueueShutdown =
			    M.getOrInsertFunction("tm_queue_shutdown",
			        FunctionType::get(Type::getVoidTy(CtxRef), {}, false));
			for (auto *Ret : MainReturns) {
				IRBuilder<> RetBuilder(Ret);
				RetBuilder.CreateCall(QueueShutdown, {});
			}

			instrumentMainInitExit(MainFn, Ctx);
		}

		TM_DEBUG("TMQueueGlobalInitPass: %s", modified ? "modified module" : "no changes");
		return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
	}
	static bool isRequired() { return true; }

private:
	bool injectWait_ = true;
};

// ===========================================================================
// 5-step pipeline chaining
// ===========================================================================

bool registerTMCollectPass(ModulePassManager &);
bool registerTMClonePass(ModulePassManager &);
bool registerTMRedirectPass(ModulePassManager &);
bool registerTMInstrumentFnPass(ModulePassManager &);
bool registerTMCleanupPass(ModulePassManager &);

bool registerTM5StepPipeline(ModulePassManager &MPM)
{
	registerTMCollectPass(MPM);
	registerTMClonePass(MPM);
	registerTMRedirectPass(MPM);
	registerTMInstrumentFnPass(MPM);
	registerTMCleanupPass(MPM);
	return true;
}

// ===========================================================================
// Pipeline Registration
// ===========================================================================

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
			            // ---- 5-step Honorio-style decomposed pipeline ----
			            if (Name == "tm-instrument" || Name == "tm-instrument-debug") {
				            TM_DEBUG("Registering %s (5-step) pass pipeline",
				                     Name.str().c_str());
				            return registerTM5StepPipeline(MPM);
			            }
			            // ---- Inline pipeline ----
			            if (Name == "tm-instrument-inline") {
				            TM_DEBUG("Registering tm-instrument-inline pass pipeline");
				            MPM.addPass(TMInitInjectPass());
				            MPM.addPass(AlwaysInlinerPass());
				            MPM.addPass(createModuleToFunctionPassAdaptor(
				                TMInstrumentInlinePass()));
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
				            return true;
			            }
			    // ---- Queue pipeline (auto-wait) ----
			    if (Name == "tm-instrument-queue") {
				    TM_DEBUG("Registering tm-instrument-queue pass pipeline");
				    MPM.addPass(TMQueueGlobalInitPass(true));
				    MPM.addPass(createModuleToFunctionPassAdaptor(
				        TMInstrumentInlinePass()));
				    return true;
			    }
			    // ---- Queue pipeline (manual-wait) ----
			    if (Name == "tm-instrument-queue-manual") {
				    TM_DEBUG("Registering tm-instrument-queue-manual pass pipeline");
				    MPM.addPass(TMQueueGlobalInitPass(false));
				    MPM.addPass(createModuleToFunctionPassAdaptor(
				        TMInstrumentInlinePass()));
				    return true;
			    }
			            // ---- Individual 5-step passes ----
			            if (Name == "tm-collect")
				            return registerTMCollectPass(MPM);
			            if (Name == "tm-clone")
				            return registerTMClonePass(MPM);
			            if (Name == "tm-redirect")
				            return registerTMRedirectPass(MPM);
			            if (Name == "tm-instrument-fn")
				            return registerTMInstrumentFnPass(MPM);
			            if (Name == "tm-cleanup")
				            return registerTMCleanupPass(MPM);
			            return false;
		            });
	        }};
}
