// TMInstrumentFnPass.cpp
// Step 4: LoadStoreBarrierInsertion
// Replaces load and store instructions inside transactions with calls
// to TM runtime read/write barriers.  Also instruments malloc/free and
// memory intrinsics (memcpy/memmove/memset) on TM-tracked memory.

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
	PreservedAnalyses run(Function &F, FunctionAnalysisManager &)
	{
		if (!hasAnnotation(F, TX_ANNOT) && !hasAnnotation(F, ASYNC_TX_ANNOT)) {
			return PreservedAnalyses::all();
		}

		Module *M = F.getParent();
		if (Function *Clone = findClone(&F, *M)) {
			TM_DEBUG("%s has queue clone %s, skipping re-instrumentation",
			         F.getName().str().c_str(), Clone->getName().str().c_str());
			return PreservedAnalyses::all();
		}

		TM_DEBUG("TMInstrumentFnPass: instrumenting function %s",
		         F.getName().str().c_str());

		LLVMContext &Ctx = M->getContext();
		auto H = TMRuntimeHooks::declareAll(*M, Ctx, tm_platform::sigsetjmpName(*M));

		injectTransactionBeginEnd(F, *M, H);

		if (TMAudit)
			auditTXFunctionLoadsStores(F, *M);

		instrumentFunctionBody(F, *M, H);

		return PreservedAnalyses::none();
	}
	static bool isRequired() { return true; }
};

bool registerTMInstrumentFnPass(ModulePassManager &MPM)
{
	MPM.addPass(createModuleToFunctionPassAdaptor(TMInstrumentFnPass()));
	return true;
}
