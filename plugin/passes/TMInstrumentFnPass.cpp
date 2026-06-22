// TMInstrumentFnPass.cpp
// Step 4: LoadStoreBarrierInsertion
// Replaces load and store instructions inside transactions with calls
// to TM runtime read/write barriers.  Also instruments malloc/free and
// memory intrinsics (memcpy/memmove/memset) on TM-tracked memory.

#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/IRBuilder.h>

#include "tm_annotation_utils.hpp"
#include "tm_debug.hpp"
#include "tm_instrument_helpers.hpp"
#include "tm_method_instrumentation.hpp"
#include "tm_pipeline_opts.hpp"
#include "tm_pipeline_state.hpp"
#include "tm_platform.hpp"
#include "tm_runtime_hooks.hpp"

using namespace llvm;

class TMInstrumentFnPass : public PassInfoMixin<TMInstrumentFnPass>
{
public:
	PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM)
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

		TM_DEBUG("TMInstrumentFnPass: instrumenting function %s",
		         F.getName().str().c_str());

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

		// Emit computation events for non-TM instruction cost estimation
		auto &TTI = AM.getResult<TargetIRAnalysis>(F);
		emitComputationEvents(F, TTI, H, *M);

		return PreservedAnalyses::none();
	}
	static bool isRequired() { return true; }
};

bool registerTMInstrumentFnPass(ModulePassManager &MPM)
{
	MPM.addPass(createModuleToFunctionPassAdaptor(TMInstrumentFnPass()));
	return true;
}
