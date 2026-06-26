// TMCleanupPass.cpp
// Step 5: Cleanup
// Removes unnecessary code inserted for analysis and transformation
// (lifetime intrinsics), and verifies that all TM instrumentation was
// applied correctly.

#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>

#include "tm_annotation_utils.hpp"
#include "tm_debug.hpp"
#include "tm_instrument_helpers.hpp"
#include "tm_local_vars.hpp"
#include "tm_pipeline_opts.hpp"
#include "tm_pipeline_state.hpp"

using namespace llvm;

namespace {

static bool stripLifetimeIntrinsics(Module &M)
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
	return changed;
}

// Command-line option to enable strict instrumentation verification
static cl::opt<bool> TMStrictCheck(
    "tm-strict-check",
    cl::desc("Fail on any uninstrumented load/store in TX/clone functions"),
    cl::init(false));

static bool verifyInstrumentation(Module &M)
{
	if (!TMStrictCheck) {
		TM_DEBUG("TMCleanupPass: verification skipped (use -tm-strict-check to enable)");
		return true;
	}
	if (AllowOpaque) {
		TM_DEBUG("TMCleanupPass: -tm-allow-opaque set, skipping verification");
		return true;
	}

	bool allOk = true;
	for (auto &F : M) {
		if (F.isDeclaration())
			continue;
		if (!hasAnnotation(F, TX_ANNOT) && !hasAnnotation(F, ASYNC_TX_ANNOT)
		    && !F.getName().contains(TM_CLONE_SUFFIX)) {
			continue;
		}
		for (auto &BB : F) {
			for (auto &I : BB) {
				if (isa<AtomicRMWInst>(I) || isa<AtomicCmpXchgInst>(I))
					continue;
				Value *Addr = nullptr;
				if (auto *LI = dyn_cast<LoadInst>(&I))
					Addr = LI->getPointerOperand();
				else if (auto *SI = dyn_cast<StoreInst>(&I))
					Addr = SI->getPointerOperand();
				if (!Addr)
					continue;
				if (isTLSGlobal(Addr) || isThreadStateAccess(Addr))
					continue;
				if (isa<AllocaInst>(getBaseObjectNoLoad(Addr)))
					continue;
				if (isTMLocalVar(Addr, M))
					continue;
				allOk = false;
				errs() << "TM-CHECK: UNINSTRUMENTED "
				       << (isa<LoadInst>(I) ? "LOAD" : "STORE")
				       << " in " << F.getName() << "\n  ";
				I.print(errs());
				errs() << "\n";
				if (auto *DIL = I.getDebugLoc().get())
					if (auto *Scope = dyn_cast_or_null<DIScope>(DIL->getScope()))
						errs() << "  at " << Scope->getFilename() << ":"
						       << DIL->getLine() << ":" << DIL->getColumn() << "\n";
			}
		}
	}
	return allOk;
}

} // anonymous namespace

class TMCleanupPass : public PassInfoMixin<TMCleanupPass>
{
public:
	PreservedAnalyses run(Module &M, ModuleAnalysisManager &)
	{
		TM_DEBUG("TMCleanupPass: cleanup and verification");

		bool lifetimeStripped = stripLifetimeIntrinsics(M);

		if (!verifyInstrumentation(M)) {
			errs() << "error: TM instrumentation check failed (use -tm-strict-check to enable, "
			          "-tm-allow-opaque to suppress)\n";
			exit(1);
		}

		TM_DEBUG("TMCleanupPass: %s",
		         lifetimeStripped ? "stripped lifetime intrinsics" : "no cleanup needed");

		auto *S = getPipelineState();
		if (S)
			S->modified = false;

		return lifetimeStripped ? PreservedAnalyses::none()
		                       : PreservedAnalyses::all();
	}
	static bool isRequired() { return true; }
};

bool registerTMCleanupPass(ModulePassManager &MPM)
{
	MPM.addPass(TMCleanupPass());
	return true;
}
