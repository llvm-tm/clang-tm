// TMCollectPass.cpp
// Step 1: DualPathInfoCollector
// Collects transactional boundary and dominance information from all
// transactional basic blocks.  This is the analysis pass used in all
// following transformation passes.

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>

#include "tm_annotation_utils.hpp"
#include "tm_call_graph.hpp"
#include "tm_debug.hpp"
#include "tm_instrument_helpers.hpp"
#include "tm_pipeline_state.hpp"

using namespace llvm;

class TMCollectPass : public PassInfoMixin<TMCollectPass>
{
public:
	PreservedAnalyses run(Module &M, ModuleAnalysisManager &)
	{
		TM_DEBUG("TMCollectPass: collecting transactional info from %s",
		         M.getName().str().c_str());
		auto *S = getPipelineState();

		checkAnnotationConsistency(M);

		auto Ctx = setupModulePass(M);
		S->H = Ctx.H;
		S->TxReachableFuncs = std::move(Ctx.TxReachableFuncs);
		S->M = &M;
		S->modified = false;

		TM_DEBUG("TMCollectPass: %d tx-reachable functions found",
		         (int)S->TxReachableFuncs.size());

		return S->TxReachableFuncs.empty()
		           ? PreservedAnalyses::all()
		           : PreservedAnalyses::none();
	}
	static bool isRequired() { return true; }
};

bool registerTMCollectPass(ModulePassManager &MPM)
{
	MPM.addPass(TMCollectPass());
	return true;
}
