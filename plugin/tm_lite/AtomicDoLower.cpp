// TM-lite pre-processing pass
// Lowers `atomic do { ... }` blocks into TX-annotated helper functions,
// then feeds into the standard TM instrumentation pipeline.

#include "llvm/Pass.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

using namespace llvm;

namespace {

struct AtomicDoLowerPass : public ModulePass {
    static char ID;
    AtomicDoLowerPass() : ModulePass(ID) {}

    bool runOnModule(Module &M) override {
        bool Changed = false;
        // TODO: Recognize __tm_atomic_begin() / __tm_atomic_end() pairs,
        // extract each block into a TX-annotated helper function,
        // and replace the block with a call to that helper.
        return Changed;
    }
};

} // anonymous namespace

char AtomicDoLowerPass::ID = 0;
static RegisterPass<AtomicDoLowerPass> X("tm-atomic-do-lower",
    "Lower atomic do blocks to TX-annotated functions");

static void registerAtomicDoLower(const PassManagerBuilder &Builder,
                                   legacy::PassManagerBase &PM) {
    PM.add(new AtomicDoLowerPass());
}
static RegisterStandardPasses
    RegisterAtomicDoLower(PassManagerBuilder::EP_EnabledOnOptLevel,
                           registerAtomicDoLower);
