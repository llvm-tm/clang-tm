// TMClonePass.cpp
// Step 2: TransactionSafeCreation
// Creates a transaction-safe clone of every function called inside
// transactional blocks or other transaction-safe functions.  Also
// creates a global map associating transaction-safe clones to their
// non-safe counterparts for call redirection.

#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>

#include "tm_annotation_utils.hpp"
#include "tm_debug.hpp"
#include "tm_method_instrumentation.hpp"
#include "tm_pipeline_state.hpp"

using namespace llvm;

class TMClonePass : public PassInfoMixin<TMClonePass>
{
public:
	PreservedAnalyses run(Module &M, ModuleAnalysisManager &)
	{
		auto *S = getPipelineState();
		if (S->TxReachableFuncs.empty()) {
			TM_DEBUG("TMClonePass: no tx-reachable functions, skipping");
			return PreservedAnalyses::all();
		}

		TM_DEBUG("TMClonePass: cloning %d tx-reachable functions",
		         (int)S->TxReachableFuncs.size());

		auto &ClonedMap = tm_method_instrumentation::cloneTxReachableGraph(
		    M, S->TxReachableFuncs, S->H,
		    tm_method_instrumentation::CloneMode::CloneOnly);

		S->ClonedMap.clear();
		for (auto &pair : ClonedMap)
			S->ClonedMap.push_back(pair);
		S->modified = true;

		TM_DEBUG("TMClonePass: created %d clones", (int)S->ClonedMap.size());
		return PreservedAnalyses::none();
	}
	static bool isRequired() { return true; }
};

bool registerTMClonePass(ModulePassManager &MPM)
{
	MPM.addPass(TMClonePass());
	return true;
}
