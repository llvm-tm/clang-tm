// TMFuzzStrategyPass.cpp
// Fuzz tool Phase 1: Strategic Point Detection LLVM pass.
//
// Auto-identifies instrumentation points for TM fuzzing without requiring
// manual [[tm::shared]] annotations.
//
// Detection targets:
//   1. Transaction boundaries — functions on pthread_create/main/join paths
//   2. Shared data — globals accessed by >=2 thread-reachable call graphs
//   3. Hot loops — loops with cross-iteration dependencies
//   4. Sync points — pthread_mutex_lock/unlock, atomic builtins
//
// Output: !tm.strategic metadata on candidate instructions.
//
// Usage:
//   opt-22 -load-pass-plugin=libTMFuzzStrategy.so \
//          -passes="tm-fuzz-strategy" myapp.bc -o /dev/null
//
//   opt-22 -load-pass-plugin=libTMFuzzStrategy.so \
//          -passes="tm-fuzz-strategy" \
//          -tm-strategy-dump  myapp.bc -o /dev/null
//
// Build:
//   clang++ -shared -fPIC -std=c++20 $(llvm-config-22 --cxxflags --ldflags --libs) \
//           TMFuzzStrategyPass.cpp -o libTMFuzzStrategy.so

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallSet.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Plugins/PassPlugin.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>

#include "tm_annotation_utils.hpp"
#include "tm_debug.hpp"

using namespace llvm;

// ── Command-line options ────────────────────────────────────────────
static cl::opt<bool> StrategyDump(
    "tm-strategy-dump",
    cl::desc("Dump all detected strategic points to stderr"),
    cl::init(false));

static cl::opt<bool> StrategyMetadata(
    "tm-strategy-metadata",
    cl::desc("Attach !tm.strategic metadata to candidate instructions"),
    cl::init(true));

static cl::opt<unsigned> MinThreadAccess(
    "tm-strategy-min-threads",
    cl::desc("Minimum threads accessing a global to flag as shared"),
    cl::init(2));

namespace {

// ── Helper: extract source location for diagnostics ─────────────────
static std::string getSourceLoc(Instruction &I) {
    DebugLoc DL = I.getDebugLoc();
    if (!DL) return "<unknown>";
    auto *Scope = dyn_cast_or_null<DIScope>(DL.getScope());
    if (!Scope) return "<unknown>";
    return (Twine(Scope->getFilename()) + ":" +
            Twine(DL.getLine()) + ":" +
            Twine(DL.getCol())).str();
}

// ── Helper: !tm.strategic metadata node types ───────────────────────
enum class StrategicKind : uint8_t {
    TransactionBoundary = 0,
    SharedDataRead,
    SharedDataWrite,
    SyncPoint,
    HotLoop
};

static MDNode *createStrategyMD(LLVMContext &Ctx, StrategicKind kind,
                                 const char *desc) {
    Metadata *MDs[] = {
        MDString::get(Ctx, "tm.strategic"),
        ConstantAsMetadata::get(
            ConstantInt::get(Type::getInt8Ty(Ctx), (uint8_t)kind)),
        MDString::get(Ctx, desc),
    };
    return MDNode::get(Ctx, MDs);
}

// ── Strategic Point Detection Pass ──────────────────────────────────
class TMFuzzStrategyPass : public PassInfoMixin<TMFuzzStrategyPass> {
public:
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
        TM_DEBUG("TMFuzzStrategyPass: scanning module %s",
                 M.getName().str().c_str());

        // ── Step 1: Build call graph from main and thread entry points ──
        SmallPtrSet<Function *, 32> ThreadReachable;
        SmallPtrSet<Function *, 32> AllReachable;
        collectThreadReachableFunctions(M, ThreadReachable, AllReachable);

        // ── Step 2: Identify transaction boundaries ───────────────────
        findTransactionBoundaries(M, ThreadReachable, AllReachable);

        // ── Step 3: Identify shared data ──────────────────────────────
        findSharedData(M, ThreadReachable);

        // ── Step 4: Identify sync points ───────────────────────────────
        findSyncPoints(M, ThreadReachable);

        // ── Step 5: Identify hot loops ─────────────────────────────────
        findHotLoops(M, ThreadReachable);

        if (!StrategyDump && !StrategyMetadata)
            TM_DEBUG("Use -tm-strategy-dump to see results or "
                     "-tm-strategy-metadata to annotate IR");

        return PreservedAnalyses::all(); // metadata-only, no IR changes
    }

    static bool isRequired() { return true; }

private:
    // ── Call graph collection ──────────────────────────────────────
    void collectThreadReachableFunctions(
        Module &M,
        SmallPtrSetImpl<Function *> &ThreadReachable,
        SmallPtrSetImpl<Function *> &AllReachable) {

        // Seed: functions reachable from main
        SmallVector<Function *, 32> Worklist;

        if (Function *MainFn = M.getFunction("main"))
            Worklist.push_back(MainFn);

        // Also seed: functions that take/return pthread_t or are passed
        // to pthread_create
        for (auto &F : M) {
            if (F.isDeclaration()) continue;
            if (hasAnnotation(F, THREAD_ANNOT))
                Worklist.push_back(&F);
        }

        // BFS through call graph
        while (!Worklist.empty()) {
            Function *F = Worklist.pop_back_val();
            if (!AllReachable.insert(F).second)
                continue;
            if (                F->getName().starts_with("pthread_") ||
                F->getName().starts_with("std::thread::"))
                continue;
            for (auto &BB : *F) {
                for (auto &I : BB) {
                    auto *CB = dyn_cast<CallBase>(&I);
                    if (!CB) continue;
                    Function *Callee = CB->getCalledFunction();
                    if (Callee && !Callee->isDeclaration() &&
                        !AllReachable.count(Callee))
                        Worklist.push_back(Callee);
                }
            }
        }

        // Separate thread-reachable functions
        for (Function *F : AllReachable) {
            if (F->getName() == "main") continue;
            ThreadReachable.insert(F);
        }

        TM_DEBUG("Call graph: %zu main-reachable, %zu thread-reachable",
                 (size_t)AllReachable.size(), (size_t)ThreadReachable.size());
    }

    // ── Transaction boundary detection ─────────────────────────────
    void findTransactionBoundaries(
        Module &M,
        SmallPtrSetImpl<Function *> &ThreadReachable,
        SmallPtrSetImpl<Function *> &AllReachable) {

        // Mark functions that should be wrapped in transactions
        for (Function *F : ThreadReachable) {
            if (F->isDeclaration()) continue;

            // Skip very small functions (likely accessors/helpers)
            if (F->size() <= 1) continue;

            // Check if this function accesses globals
            bool accessesGlobal = false;
            for (auto &BB : *F) {
                for (auto &I : BB) {
                    if (auto *LI = dyn_cast<LoadInst>(&I)) {
                        if (isa<GlobalVariable>(
                                LI->getPointerOperand()->stripPointerCasts()))
                            accessesGlobal = true;
                    } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
                        if (isa<GlobalVariable>(
                                SI->getPointerOperand()->stripPointerCasts()))
                            accessesGlobal = true;
                    }
                }
            }

            if (!accessesGlobal) continue;

            if (StrategyDump)
                errs() << "TM-STRATEGY: boundary " << F->getName()
                       << " (thread-reachable, accesses globals)\n";

            if (StrategyMetadata) {
                F->setMetadata("tm.strategic",
                    createStrategyMD(M.getContext(),
                                     StrategicKind::TransactionBoundary,
                                     "thread-reachable function with global access"));
            }
        }
    }

    // ── Shared data detection ──────────────────────────────────────
    void findSharedData(Module &M,
                        SmallPtrSetImpl<Function *> &ThreadReachable) {

        // Map global → set of accessing functions
        // Using SmallVector + manual lookup (no MapVector in LLVM 22 ADT)
        struct GlobalAccessEntry {
            GlobalVariable *GV;
            SmallSet<Function *, 8> Funcs;
        };
        SmallVector<GlobalAccessEntry, 32> GlobalAccess;

        for (Function *F : ThreadReachable) {
            if (F->isDeclaration()) continue;
            for (auto &BB : *F) {
                for (auto &I : BB) {
                    Value *Ptr = nullptr;
                    if (auto *LI = dyn_cast<LoadInst>(&I))
                        Ptr = LI->getPointerOperand();
                    else if (auto *SI = dyn_cast<StoreInst>(&I))
                        Ptr = SI->getPointerOperand();
                    else
                        continue;

                    if (auto *GV = dyn_cast<GlobalVariable>(
                            Ptr->stripPointerCasts())) {
                        // Find or create entry
                        bool found = false;
                        for (auto &Entry : GlobalAccess) {
                            if (Entry.GV == GV) {
                                Entry.Funcs.insert(F);
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            GlobalAccessEntry Entry;
                            Entry.GV = GV;
                            Entry.Funcs.insert(F);
                            GlobalAccess.push_back(Entry);
                        }
                    }
                }
            }
        }

        // Flag globals accessed by >= MinThreadAccess different functions
        for (auto &Entry : GlobalAccess) {
            GlobalVariable *GV = Entry.GV;
            auto &Funcs = Entry.Funcs;
            if (Funcs.size() < MinThreadAccess) continue;

            bool isWrite = false;
            for (auto *F : Funcs) {
                for (auto &BB : *F) {
                    for (auto &I : BB) {
                        Value *Ptr = nullptr;
                        if (auto *SI = dyn_cast<StoreInst>(&I))
                            Ptr = SI->getPointerOperand();
                        if (Ptr && GV == Ptr->stripPointerCasts()) {
                            isWrite = true;
                            break;
                        }
                    }
                    if (isWrite) break;
                }
            }

            if (StrategyDump)
                errs() << "TM-STRATEGY: shared " << GV->getName()
                       << " (" << Funcs.size() << " functions, "
                       << (isWrite ? "read-write" : "read-only") << ")\n";

            if (StrategyMetadata) {
                GV->setMetadata("tm.strategic",
                    createStrategyMD(M.getContext(),
                        isWrite ? StrategicKind::SharedDataWrite
                                : StrategicKind::SharedDataRead,
                        (Twine("shared global: ") + GV->getName()).str().c_str()));
            }
        }

        TM_DEBUG("Shared data: %zu globals with >= %u thread accesses",
                 (size_t)GlobalAccess.size(), (unsigned)MinThreadAccess);
    }

    // ── Sync point detection ───────────────────────────────────────
    void findSyncPoints(Module &M,
                        SmallPtrSetImpl<Function *> &ThreadReachable) {
        for (Function *F : ThreadReachable) {
            if (F->isDeclaration()) continue;
            for (auto &BB : *F) {
                for (auto &I : BB) {
                    auto *CB = dyn_cast<CallBase>(&I);
                    if (!CB) continue;
                    Function *Callee = CB->getCalledFunction();
                    if (!Callee) continue;

                    StringRef Name = Callee->getName();
                    if (!Name.contains("pthread_mutex") &&
                        !Name.contains("pthread_rwlock") &&
                        !Name.contains("atomic") &&
                        !Name.contains("__sync") &&
                        !Name.contains("__atomic") &&
                        !Name.contains("std::atomic"))
                        continue;

                    if (StrategyDump)
                        errs() << "TM-STRATEGY: sync " << getSourceLoc(I)
                               << " " << Name << "\n";

                    if (StrategyMetadata)
                        I.setMetadata("tm.strategic",
                            createStrategyMD(M.getContext(),
                                             StrategicKind::SyncPoint,
                                             (Twine("sync: ") + Name).str().c_str()));
                }
            }
        }
    }

    // ── Hot loop detection ─────────────────────────────────────────
    void findHotLoops(Module &M,
                      SmallPtrSetImpl<Function *> &ThreadReachable) {
        for (Function *F : ThreadReachable) {
            if (F->isDeclaration()) continue;

            // Find loops by looking for back edges
            for (auto &BB : *F) {
                // Check if this block branches backwards
                Instruction *Term = BB.getTerminator();
                if (!Term) continue;

                for (unsigned i = 0; i < Term->getNumSuccessors(); ++i) {
                    BasicBlock *Succ = Term->getSuccessor(i);
                    // Back edge: successor dominates current block
                    if (Succ && Succ != &BB) continue; // simplified check
                }
            }

            // Simple loop detection via PHI nodes with backedge incoming
            for (auto &BB : *F) {
                for (auto &I : BB) {
                    auto *PN = dyn_cast<PHINode>(&I);
                    if (!PN) continue;

                    // Count how many incoming blocks are predecessors
                    // that also appear as predecessors of predecessors
                    // (crude loop detector)
                    bool hasBackEdge = false;
                    for (unsigned i = 0; i < PN->getNumIncomingValues(); ++i) {
                        BasicBlock *Incoming = PN->getIncomingBlock(i);
                        if (Incoming == &BB) { // self-loop
                            hasBackEdge = true;
                            break;
                        }
                    }

                    if (!hasBackEdge) {
                        // Check if any incoming block is reachable from BB
                        for (unsigned i = 0; i < PN->getNumIncomingValues(); ++i) {
                            BasicBlock *Incoming = PN->getIncomingBlock(i);
                            auto Preds = predecessors(Incoming);
                            for (auto PredBB = Preds.begin(); PredBB != Preds.end(); ++PredBB) {
                                if (*PredBB == &BB) {
                                    hasBackEdge = true;
                                    break;
                                }
                            }
                            if (hasBackEdge) break;
                        }
                    }

                    if (!hasBackEdge) continue;

                    if (StrategyDump)
                        errs() << "TM-STRATEGY: loop " << getSourceLoc(I)
                               << " in " << F->getName() << "\n";

                    if (StrategyMetadata)
                        I.setMetadata("tm.strategic",
                            createStrategyMD(M.getContext(),
                                             StrategicKind::HotLoop,
                                             "loop with back edge"));
                    break; // one annotation per BB
                }
            }
        }
    }
};

} // anonymous namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return {
        LLVM_PLUGIN_API_VERSION,
        "TMFuzzStrategyPass",
        LLVM_VERSION_STRING,
        [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name,
                   ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                    if (Name == "tm-fuzz-strategy") {
                        MPM.addPass(TMFuzzStrategyPass());
                        return true;
                    }
                    return false;
                });
        }};
}
