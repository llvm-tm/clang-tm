// TMRedirectPass.cpp
// Step 3: ReplaceCallInsideTransaction
// Replaces all calls inside transactions or transaction-safe functions
// with calls to their transaction-safe counterparts.  Also injects
// runtime init/exit hooks into main and thread functions.

#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>

#include "tm_annotation_utils.hpp"
#include "tm_check_opaque.hpp"
#include "tm_debug.hpp"
#include "tm_instrument_helpers.hpp"
#include "tm_method_instrumentation.hpp"
#include "tm_pipeline_state.hpp"

using namespace llvm;

// Bridge helper: create a ModulePassContext from pipeline state for the
// instrumentMainInitExit / instrumentThreadEntries calls (which need
// ClonedMap as a pointer and modified as a mutable reference).
static void bridgeInstrumentMainInitExit(Function *MainFn, TMPipelineState *S)
{
	ModulePassContext Ctx;
	Ctx.H = S->H;
	Ctx.ClonedMap = &S->ClonedMap;
	Ctx.modified = false;
	instrumentMainInitExit(MainFn, Ctx);
	S->modified = Ctx.modified;
}

static void bridgeInstrumentThreadEntries(Module &M, TMPipelineState *S)
{
	SmallPtrSet<Function *, 32> EmptyEntries;
	ModulePassContext Ctx;
	Ctx.H = S->H;
	Ctx.ClonedMap = &S->ClonedMap;
	Ctx.modified = false;
	instrumentThreadEntries(M, EmptyEntries, Ctx);
	if (Ctx.modified)
		S->modified = true;
}

class TMRedirectPass : public PassInfoMixin<TMRedirectPass>
{
public:
	PreservedAnalyses run(Module &M, ModuleAnalysisManager &)
	{
		auto *S = getPipelineState();
		if (S->ClonedMap.empty()) {
			TM_DEBUG("TMRedirectPass: no clones to redirect, skipping");
		} else {
			TM_DEBUG("TMRedirectPass: redirecting %d clones",
			         (int)S->ClonedMap.size());

			tm_method_instrumentation::redirectTXFunctionsToClones(
			    M, S->TxReachableFuncs, S->ClonedMap,
			    tm_method_instrumentation::CloneMode::CloneOnly);

			tm_method_instrumentation::instrumentAllClones(
			    S->ClonedMap, M, S->H);

			S->modified = true;
		}

		checkOpaqueOrAbort(M, S->TxReachableFuncs);

		if (Function *MainFn = M.getFunction(MAIN_ANNOT))
			bridgeInstrumentMainInitExit(MainFn, S);

		bridgeInstrumentThreadEntries(M, S);

		TM_DEBUG("TMRedirectPass: %s", S->modified ? "modified module" : "no changes");
		return S->modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
	}
	static bool isRequired() { return true; }
};

bool registerTMRedirectPass(ModulePassManager &MPM)
{
	MPM.addPass(TMRedirectPass());
	return true;
}
