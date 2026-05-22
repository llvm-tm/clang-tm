// TMInstrumentPass.cpp
// Main plugin file for Transactional Memory instrumentation
//
// Two pipelines:
//
//   tm-instrument (legacy, debug-friendly):
//     TMGlobalInitPass + TMInstrumentPass
//     - Clones and instruments non-TX callees in one shot
//     - No aggressive inlining — works with -O0 -g
//     - Clones get NoInline + OptimizeNone attributes
//
//   tm-instrument-inline:
//     TMInitInjectPass + AlwaysInlinerPass + TMInstrumentInlinePass
//     - Clones callees with alwaysinline, then inlines them into TX body
//     - Enables load/store visibility for inlined callee internals
//     - Requires -O1+ for AlwaysInlinerPass to work
//
// This two-pass-per-pipeline design is REQUIRED because:
//   - Module-level changes (symbol tables, main init) must happen once
//   - Function-level changes modify each function's IR independently
//   - LLVM runs function passes repeatedly, so module work can't be there

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Type.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Plugins/PassPlugin.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/IPO/AlwaysInliner.h>

#include <fstream>

#include "tm_annotation_utils.hpp"
#include "tm_call_graph.hpp"
#include "tm_debug.hpp"
#include "tm_instrument_helpers.hpp"
#include "tm_local_vars.hpp"
#include "tm_method_instrumentation.hpp"
#include "tm_runtime_hooks.hpp"
#include "tm_thread_guard.hpp"
#include "tm_thread_symbols.hpp"
#include "opaque_safe_table.hpp"

using namespace llvm;

cl::opt<bool> AllowOpaque("tm-allow-opaque",
    cl::desc("Allow opaque (uninstrumentable) function calls inside transactions"),
    cl::init(false));

cl::opt<bool> StrictOpaque("tm-strict-opaque",
    cl::desc("Reject even known-safe opaque function calls (strict mode)"),
    cl::init(false));

cl::opt<std::string> OpaqueSymbolsFile("tm-opaque-symbols-file",
    cl::desc("Write unresolved opaque symbols to this file for external resolution"),
    cl::init(""));

cl::opt<bool> TMAudit("tm-audit",
    cl::desc("Print every load/store in TX functions with instrumentation analysis"),
    cl::init(false));

static bool checkOpaqueFunctions(
    Module &M, SmallPtrSetImpl<Function *> &TxReachableFuncs)
{
    bool foundOpaque = false;
    StringSet<> UnresolvedSymbols;

    for (Function *F : TxReachableFuncs) {
        if (F->isDeclaration()) continue;
        if (F->getName().starts_with("tm_")) continue;
        for (auto &BB : *F) {
            for (auto &I : BB) {
                auto *Call = dyn_cast<CallBase>(&I);
                if (!Call) continue;
                if (Call->hasFnAttr("tm_allow_opaque")) continue;
                if (Call->isInlineAsm()) continue;
                Function *Callee = Call->getCalledFunction();
                if (Callee && hasAnnotation(*Callee, "tm_allow_opaque")) continue;
                if (hasAnnotation(*F, "tm_allow_opaque")) continue;
                if (!Callee) {
                    foundOpaque = true;
                    errs() << "error: indirect call in TM context";
                    if (auto *DIL = I.getDebugLoc().get())
                        if (auto *Scope = dyn_cast_or_null<DIScope>(DIL->getScope()))
                            errs() << " at " << Scope->getFilename() << ":"
                                   << DIL->getLine() << ":" << DIL->getColumn();
                    errs() << "\n  Called from: " << F->getName() << "\n"
                              "  Calls via function pointer or virtual method "
                              "cannot be instrumented for TM.\n";
                    continue;
                }
                // LLVM intrinsics (llvm.memcpy, llvm.lifetime.start, etc.) are
                // always safe — they are handled by the memory intrinsic
                // instrumentation or are no-ops for TM purposes.
                if (Callee->isIntrinsic()) continue;
                // Heap allocation/deallocation functions are handled by
                // handleMallocFree during instrumentation — skip them here.
                if (isHeapAllocationCall(Call) || isDeallocationCall(Call))
                    continue;
                if (!Callee->isDeclaration()) continue;
                // Check if any pointer argument traces to a TM global.
                // Even known-safe opaque functions (e.g., _ZSt prefix) are
                // UNSAFE if they receive TM-shared pointers — they modify
                // shared data without TM write-set tracking.
                bool hasTMTracedArg = false;
                for (unsigned i = 0; i < Call->arg_size(); i++) {
                    if (Call->getArgOperand(i)->getType()->isPointerTy() &&
                        tracesFromTMGlobal(Call->getArgOperand(i), M)) {
                        hasTMTracedArg = true;
                        break;
                    }
                }
                if (!hasTMTracedArg) {
                    if (isKnownSafeOpaque(Callee->getName(), StrictOpaque)) continue;
                    if (isSyscallSymbol(Callee->getName())) continue;
                }
                foundOpaque = true;
                UnresolvedSymbols.insert(Callee->getName());
                errs() << "error: call to '" << Callee->getName()
                       << "' in TM context";
                if (auto *DIL = I.getDebugLoc().get())
                    if (auto *Scope = dyn_cast_or_null<DIScope>(DIL->getScope()))
                        errs() << " at " << Scope->getFilename() << ":"
                               << DIL->getLine() << ":" << DIL->getColumn();
                errs() << "\n  Called from: " << F->getName() << "\n";
                if (hasTMTracedArg)
                    errs() << "  This function receives TM-shared pointer arguments "
                              "but its body is not visible (defined in external library).\n"
                              "  Its internal modifications bypass TM write-set tracking, "
                              "causing data corruption with concurrent transactions.\n";
                else
                    errs() << "  This function is not visible to TM "
                              "instrumentation (no body in this translation unit).\n";
                emitOpaqueSuggestion(Callee->getName(), errs());
            }
        }
    }

    if (!OpaqueSymbolsFile.empty() && !UnresolvedSymbols.empty()) {
        std::error_code EC;
        raw_fd_ostream OS(OpaqueSymbolsFile, EC);
        if (!EC) {
            for (auto &Sym : UnresolvedSymbols)
                OS << Sym.getKey() << "\n";
            OS.close();
            errs() << "Unresolved opaque symbols written to: "
                   << OpaqueSymbolsFile << "\n";
        }
    }

    return !foundOpaque;
}

static void checkOpaqueOrAbort(Module &M,
                                SmallPtrSetImpl<Function *> &TxReachableFuncs)
{
    if (TxReachableFuncs.empty())
        return;
    bool ok = checkOpaqueFunctions(M, TxReachableFuncs);
    if (!AllowOpaque && !ok) {
        errs() << "Aborting due to opaque function call(s) in TM context.\n"
               << "Use -tm-allow-opaque to disable this check.\n";
        report_fatal_error("TM instrumentation: opaque function calls detected");
    }
}

static void redirectTXFunctionsToClones(Module &M,
                                         SmallPtrSetImpl<Function *> &TxReachableFuncs,
                                         SmallVectorImpl<std::pair<Function *, Function *>> &ClonedMap,
                                         tm_method_instrumentation::CloneMode Mode = tm_method_instrumentation::CloneMode::Instrument)
{
    for (auto &F : M) {
        if (F.isDeclaration()) continue;
        if (!hasAnnotation(F, "transaction")) continue;
        tm_method_instrumentation::redirectCallsToClones(F, M, TxReachableFuncs, ClonedMap, Mode);
    }
    // Re-redirect clones now that they have callers
    for (int _r = 0; _r < 3; _r++)
        for (auto &pair : ClonedMap)
            tm_method_instrumentation::redirectCallsToClones(*pair.second, M, TxReachableFuncs, ClonedMap, Mode);
}

// ===========================================================================
// PASS 1 (inline pipeline): clone-with-alwaysinline + tx_begin/end injection
// ===========================================================================

class TMInitInjectPass : public PassInfoMixin<TMInitInjectPass>
{
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &)
  {
    TM_DEBUG("TMInitInjectPass: processing module %s", M.getName().str().c_str());
    auto Ctx = setupModulePass(M);
    bool &modified = Ctx.modified;

    if (!Ctx.TxReachableFuncs.empty()) {
        TM_DEBUG("Tx-reachable call graph has %d functions", (int)Ctx.TxReachableFuncs.size());
        Ctx.ClonedMap = &tm_method_instrumentation::cloneTxReachableGraph(
            M, Ctx.TxReachableFuncs, Ctx.H, tm_method_instrumentation::CloneMode::AlwaysInline);
        redirectTXFunctionsToClones(M, Ctx.TxReachableFuncs, *Ctx.ClonedMap,
                                      tm_method_instrumentation::CloneMode::AlwaysInline);
        modified = true;
    }

    checkOpaqueOrAbort(M, Ctx.TxReachableFuncs);

    auto ExplicitThreadEntries = detectExplicitThreadEntries(M);

    if (Function *MainFn = M.getFunction("main"))
        instrumentMainInitExit(MainFn, Ctx);

    instrumentThreadEntries(M, ExplicitThreadEntries, Ctx);

    for (auto &F : M) {
        if (F.isDeclaration()) continue;
        if (!hasAnnotation(F, "transaction")) continue;
        injectTransactionBeginEnd(F, M, Ctx.H);
        modified = true;
    }

    TM_DEBUG("TMInitInjectPass: %s", modified ? "modified module" : "no changes");
    if (modified) {
      int n = 0;
      for (auto &F : M)
        if (!F.isDeclaration() && F.getName().contains("_tm_clone")) ++n;
      errs() << "[BEFORE_INLINE] _tm_clone function definitions: " << n << "\n";
    }

    // When a clone inherits noinline from its original AND has
    // alwaysinline (from the pass), keep alwaysinline and drop
    // noinline.  The AlwaysInlinerPass will then succeed in inlining
    // the clone.  This is essential for RAII helpers like
    // _ConstructTransactionD2 whose plain store to g_vec.__end_
    // must be inlined and instrumented.
    SmallVector<Function *, 8> DeadClones;
    for (auto &F : M) {
      if (F.isDeclaration() || !F.getName().contains("_tm_clone"))
        continue;
      if (F.hasFnAttribute(llvm::Attribute::NoInline) &&
          F.hasFnAttribute(llvm::Attribute::AlwaysInline)) {
        F.removeFnAttr(llvm::Attribute::NoInline);
        if (F.use_empty())
          DeadClones.push_back(&F);
      }
    }
    for (auto *F : DeadClones)
      F->eraseFromParent();

    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
  static bool isRequired() { return true; }
};

// ===========================================================================
// PASS 2 (inline pipeline): load/store instrument (after inlining)
// ===========================================================================

class TMInstrumentInlinePass : public PassInfoMixin<TMInstrumentInlinePass>
{
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &)
  {
    if (!hasAnnotation(F, "transaction") && !F.getName().contains("_tm_clone")) {
        return PreservedAnalyses::all();
    }
    Module *M = F.getParent();
    LLVMContext &Ctx = M->getContext();
    const char *SetjmpFunc = M->getTargetTriple().str().find("linux") != std::string::npos
                               ? "__sigsetjmp" : "sigsetjmp";
    auto H = TMRuntimeHooks::declareAll(*M, Ctx, SetjmpFunc);

    if (TMAudit) auditTXFunctionLoadsStores(F, *M);

    SmallVector<Instruction *, 16> ToErase;
    SmallVector<CallBase *, 8> MemIntrinsics;
    for (auto &BB : F) {
      for (auto InstIt = BB.begin(); InstIt != BB.end();) {
        Instruction *I = &*InstIt++;
        IRBuilder<> B(I->getParent(), I->getIterator());
#ifndef DISABLE_TM_READ_WRITE
        if (auto *Call = dyn_cast<CallBase>(I)) {
            if (needsMemIntrinsicInstrumentation(Call, *M)) {
                MemIntrinsics.push_back(Call);
                continue;
            }
        }
#endif
#ifndef DISABLE_MALLOC_FREE
        if (auto *Call = dyn_cast<CallBase>(I))
            if (handleMallocFree(Call, B, H, ToErase))
                continue;
#endif
#ifndef DISABLE_TM_READ_WRITE
        handleLoadStore(I, F, *M, H, ToErase);
#endif
      }
    }
    // Instrument memory intrinsics AFTER all loops (they split basic blocks,
    // which would invalidate the instruction iterators above).
    for (auto *Call : MemIntrinsics) {
        tm_method_instrumentation::instrumentMemoryIntrinsic(Call, *M, H);
        ToErase.push_back(Call);
    }
    for (Instruction *I : ToErase) I->eraseFromParent();
    return PreservedAnalyses::none();
  }
  static bool isRequired() { return true; }
};

// ===========================================================================
// PASS 1 (legacy pipeline): clone-with-instrumentation + thread init
// ===========================================================================

class TMGlobalInitPass : public PassInfoMixin<TMGlobalInitPass>
{
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &)
  {
    TM_DEBUG("TMGlobalInitPass: processing module %s", M.getName().str().c_str());
    auto Ctx = setupModulePass(M);
    bool &modified = Ctx.modified;

    if (!Ctx.TxReachableFuncs.empty()) {
        TM_DEBUG("Tx-reachable call graph has %d functions", (int)Ctx.TxReachableFuncs.size());
        // CloneOnly: clones without instrumentation, so tracesFromTMGlobal
        // can find actual callers and trace arguments through allocas to TM
        // globals.  Then instrument ALL clones after call redirection.
        Ctx.ClonedMap = &tm_method_instrumentation::cloneTxReachableGraph(
            M, Ctx.TxReachableFuncs, Ctx.H, tm_method_instrumentation::CloneMode::CloneOnly);
        redirectTXFunctionsToClones(M, Ctx.TxReachableFuncs, *Ctx.ClonedMap);
        tm_method_instrumentation::instrumentAllClones(*Ctx.ClonedMap, M, Ctx.H);
        modified = true;
    }

    checkOpaqueOrAbort(M, Ctx.TxReachableFuncs);

    auto ExplicitThreadEntries = detectExplicitThreadEntries(M);

    if (Function *MainFn = M.getFunction("main"))
        instrumentMainInitExit(MainFn, Ctx);

    instrumentThreadEntries(M, ExplicitThreadEntries, Ctx);

    TM_DEBUG("TMGlobalInitPass: %s", modified ? "modified module" : "no changes");
    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
  static bool isRequired() { return true; }
};

// ===========================================================================
// PASS 2 (legacy pipeline): tx_begin/end + load/store instrument
// ===========================================================================

class TMInstrumentPass : public PassInfoMixin<TMInstrumentPass>
{
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &)
  {
    if (!hasAnnotation(F, "transaction")) {
      TM_DEBUG("%s is not a transaction function, skipping", F.getName().str().c_str());
      return PreservedAnalyses::all();
    }
    TM_DEBUG("TMInstrumentPass: processing function %s", F.getName().str().c_str());
    Module *M = F.getParent();
    LLVMContext &Ctx = M->getContext();
    const char *SetjmpFunc = M->getTargetTriple().str().find("linux") != std::string::npos
                               ? "__sigsetjmp" : "sigsetjmp";
    auto H = TMRuntimeHooks::declareAll(*M, Ctx, SetjmpFunc);

    injectTransactionBeginEnd(F, *M, H);

    if (TMAudit) auditTXFunctionLoadsStores(F, *M);

    SmallVector<Instruction *, 16> ToErase;
    SmallVector<CallBase *, 8> MemIntrinsics;
    for (auto &BB : F) {
      for (auto InstIt = BB.begin(); InstIt != BB.end();) {
        Instruction *I = &*InstIt++;
        IRBuilder<> B(I->getParent(), I->getIterator());
#ifndef DISABLE_TM_READ_WRITE
        if (auto *Call = dyn_cast<CallBase>(I))
            if (handleMemoryIntrinsic(Call, *M, H, &ToErase))
                continue;
#endif
#ifndef DISABLE_MALLOC_FREE
        if (auto *Call = dyn_cast<CallBase>(I))
            if (handleMallocFree(Call, B, H, ToErase))
                continue;
#endif
#ifndef DISABLE_TM_READ_WRITE
        handleLoadStore(I, F, *M, H, ToErase);
#endif
      }
    }
    for (Instruction *I : ToErase) I->eraseFromParent();
    return PreservedAnalyses::none();
  }
  static bool isRequired() { return true; }
};

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo()
{
  return {LLVM_PLUGIN_API_VERSION,
          "TMInstrumentPass",
          LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name,
                   ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  // ---- Debug-friendly pipeline (no inlining, -O0 compatible) ----
                  if (Name == "tm-instrument" || Name == "tm-instrument-debug") {
                    TM_DEBUG("Registering %s pass pipeline", Name.str().c_str());
                    MPM.addPass(TMGlobalInitPass());
                    MPM.addPass(
                        createModuleToFunctionPassAdaptor(TMInstrumentPass()));
                    return true;
                  }
                  // ---- Inline pipeline (inline callees first, then instrument) ----
                  if (Name == "tm-instrument-inline") {
                    TM_DEBUG("Registering tm-instrument-inline pass pipeline");
                    MPM.addPass(TMInitInjectPass());
                    MPM.addPass(AlwaysInlinerPass());
                    MPM.addPass(
                        createModuleToFunctionPassAdaptor(TMInstrumentInlinePass()));
                    return true;
                  }
                  return false;
                });
          }};
}
