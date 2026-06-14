// TMRaceCheckerPass.cpp
// Standalone LLVM pass plugin: detects accesses to TM-annotated globals
// outside transaction-annotated functions.
//
// Usage:
//   opt -load-pass-plugin=libTMRaceChecker.so -passes="tm-race-checker" input.bc
//
// This pass can be used independently of the TM instrumentation pipeline to
// identify code locations where shared TM state is accessed without
// transactional protection.
//
// The pass reuses core detection logic from the analysis/ headers that are
// also used by the instrumentation pipeline (tm_instrument_helpers.hpp uses
// tracesFromTMGlobal from tm_local_vars.hpp), avoiding code duplication.
//
// Call graph analysis: the checker builds a call graph starting from all
// TX/THREAD/MAIN-annotated functions and marks every reachable function as
// "safe".  Only functions NOT reachable from any TX function are checked for
// races.  This avoids false positives on helper functions (e.g. A::inc(),
// insert(), std::vector::push_back) that are called exclusively from within
// transaction contexts — the instrumentation pipeline clones and instruments
// them via the same call-graph reachability.

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Plugins/PassPlugin.h>
#include <llvm/Support/raw_ostream.h>

#include "tm_annotation_utils.hpp"
#include "tm_debug.hpp"
#include "tm_local_vars.hpp"

using namespace llvm;

namespace {

// ── Shared: inline the tracesFromTMGlobal check here ──
// (tm_local_vars.hpp already defines it; we just call it.)

// Build a set of all functions reachable from TX-annotated functions via the
// call graph.  The instrumentation pipeline clones & instruments all functions
// transitively reachable from TX functions, so the race checker should not
// flag code that will be instrumented.
//
// Only "transaction"-annotated functions are used as seeds.  THREAD, MAIN,
// pstatic_rebuild, and tm_allow_opaque functions are NOT used as seeds — they
// are checked individually (they call TX functions, but the TX function calls
// are what provide safety, not the annotation on the caller).
static void collectReachableFromTX(Module &M,
                                   SmallPtrSetImpl<Function *> &SafeSet) {
    SmallVector<Function *, 32> Worklist;

    // Seed: only functions annotated with "transaction" or "async_transaction"
    for (auto &F : M) {
        if (F.isDeclaration())
            continue;
        if (hasAnnotation(F, TX_ANNOT) ||
            hasAnnotation(F, ASYNC_TX_ANNOT))
            Worklist.push_back(&F);
    }

    // BFS through the call graph
    while (!Worklist.empty()) {
        Function *F = Worklist.pop_back_val();
        if (!SafeSet.insert(F).second)
            continue; // already visited

        for (auto &BB : *F) {
            for (auto &I : BB) {
                auto *CB = dyn_cast<CallBase>(&I);
                if (!CB)
                    continue;
                Function *Callee = CB->getCalledFunction();
                if (Callee && !Callee->isDeclaration() &&
                    !SafeSet.count(Callee))
                    Worklist.push_back(Callee);
            }
        }
    }

    TM_DEBUG("Call graph: %u functions reachable from TX/thread/main",
             (unsigned)SafeSet.size());
}

// Emit a structured warning with source location
static void emitWarning(Function &F, Instruction &I, StringRef GlobalName) {
    DebugLoc DL = I.getDebugLoc();
    std::string LocStr;
    if (DL) {
        auto *Scope = dyn_cast<DIScope>(DL.getScope());
        StringRef File = Scope ? Scope->getFilename() : "";
        unsigned Line = DL.getLine();
        unsigned Col = DL.getCol();
        LocStr = (File.str() + ":" + std::to_string(Line) + ":" + std::to_string(Col));
    } else {
        LocStr = "<unknown location>";
    }

    errs() << "TM-RACE-CHECKER: " << LocStr << ": "
           << (isa<StoreInst>(I) ? "write" : "read")
           << " to TM-annotated global '" << GlobalName
           << "' in function '" << F.getName()
           << "' without transaction annotation\n";
    errs() << "  note: add [[tx::transaction]] to function '"
           << F.getName()
           << "' or use peek()/poke() for intentional non-transactional access\n";
}

class TMRaceCheckerPass : public PassInfoMixin<TMRaceCheckerPass> {
public:
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
        TM_DEBUG("TMRaceCheckerPass: scanning module %s",
                 M.getName().str().c_str());

        // Collect TM-annotated globals once
        SmallPtrSet<const Value *, 16> TMGlobals;
        collectTMGlobals(M, TMGlobals);

        if (TMGlobals.empty()) {
            TM_DEBUG("No TM-annotated globals found, skipping");
            return PreservedAnalyses::all();
        }

        // Build call-graph reachable set: functions transitively called from
        // TX/THREAD/MAIN are safe (the pipeline instruments them).
        SmallPtrSet<Function *, 32> SafeSet;
        collectReachableFromTX(M, SafeSet);

        bool Found = false;
        for (auto &F : M) {
            // Skip declarations
            if (F.isDeclaration())
                continue;
            // Skip annotation-marked functions (they are transaction boundaries,
            // thread entry points, or have other special semantics)
            if (hasAnnotation(F, TX_ANNOT) ||
                hasAnnotation(F, ASYNC_TX_ANNOT) ||
                hasAnnotation(F, THREAD_ANNOT) ||
                hasAnnotation(F, PSTATIC_REBUILD_ANNOT) ||
                hasAnnotation(F, ALLOW_OPAQUE_ANNOT))
                continue;
            // Skip main (entry point, may do setup/teardown outside TX)
            if (F.getName() == "main")
                continue;
            // Skip functions reachable from TX via call graph — the
            // instrumentation pipeline clones+instruments these.
            if (SafeSet.count(&F))
                continue;

            TM_DEBUG("Checking function: %s", F.getName().str().c_str());
            for (auto &BB : F) {
                for (auto &I : BB) {
                    Value *Ptr = nullptr;
                    if (auto *Load = dyn_cast<LoadInst>(&I)) {
                        Ptr = Load->getPointerOperand();
                    } else if (auto *Store = dyn_cast<StoreInst>(&I)) {
                        Ptr = Store->getPointerOperand();
                    } else {
                        continue;
                    }

                    if (!Ptr)
                        continue;

                    // Reuse the same tracesFromTMGlobal analysis
                    // that the instrumentation pipeline uses
                    if (tracesFromTMGlobal(Ptr, M)) {
                        // Extract the global variable name for the diagnostic
                        StringRef GlobalName = "<TM global>";
                        if (auto *GV = dyn_cast<GlobalVariable>(
                                Ptr->stripPointerCasts()))
                            GlobalName = GV->getName();
                        else if (auto *Base = getBaseObject(Ptr))
                            if (auto *GV = dyn_cast<GlobalVariable>(Base))
                                GlobalName = GV->getName();

                        emitWarning(F, I, GlobalName);
                        Found = true;
                    }
                }
            }
        }

        if (!Found)
            TM_DEBUG("No TM-race candidates found");

        return PreservedAnalyses::all();
    }
    static bool isRequired() { return true; }
};

} // anonymous namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return {
        LLVM_PLUGIN_API_VERSION,
        "TMRaceCheckerPass",
        LLVM_VERSION_STRING,
        [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name,
                   ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                    if (Name == "tm-race-checker") {
                        MPM.addPass(TMRaceCheckerPass());
                        return true;
                    }
                    return false;
                });
        }};
}
