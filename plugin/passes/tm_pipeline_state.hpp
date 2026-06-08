#ifndef TM_PIPELINE_STATE_HPP
#define TM_PIPELINE_STATE_HPP

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

#include "tm_runtime_hooks.hpp"

struct TMPipelineState {
	TMRuntimeHooks H;
	SmallPtrSet<llvm::Function *, 32> TxReachableFuncs;
	SmallVector<std::pair<llvm::Function *, llvm::Function *>, 32> ClonedMap;
	llvm::Module *M = nullptr;
	bool modified = false;
};

inline TMPipelineState *getPipelineState()
{
	static TMPipelineState State;
	return &State;
}

#endif
