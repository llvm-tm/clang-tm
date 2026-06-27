// TMInstrumentHelpers.cpp
// Shared helper function definitions that need tm_method_instrumentation.hpp
// (breaks the circular include dependency if defined in the header).

#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include "tm_instrument_helpers.hpp"
#include "tm_method_instrumentation.hpp"
#include "tm_runtime_hooks.hpp"

using namespace llvm;

void instrumentFunctionBody(Function &F, Module &M, TMRuntimeHooks &H)
{
	SmallVector<Instruction *, 16> ToErase;
	SmallVector<CallBase *, 8> MemIntrinsics;

	for (auto &BB : F) {
		for (auto InstIt = BB.begin(); InstIt != BB.end();) {
			Instruction *I = &*InstIt++;
			IRBuilder<> B(I->getParent(), I->getIterator());
#ifndef DISABLE_TM_READ_WRITE
			if (auto *Call = dyn_cast<CallBase>(I))
				if (needsMemIntrinsicInstrumentation(Call, M)) {
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
			handleLoadStore(I, F, M, H, ToErase);
#endif
		}
	}
	for (auto *Call : MemIntrinsics) {
		tm_method_instrumentation::instrumentMemoryIntrinsic(Call, M, H);
		ToErase.push_back(Call);
	}
	for (Instruction *I : ToErase)
		I->eraseFromParent();
}
