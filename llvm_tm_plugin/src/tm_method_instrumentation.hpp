// tm_method_instrumentation.hpp
// Method call instrumentation for TM-annotated objects
//
// PURPOSE: When a method is called on a "tm"-annotated object (e.g.,
//          std::vector<int> with TM annotation), the plugin needs to:
//            1. Clone the method into an uninstrumented version (_tm_uninst suffix)
//            2. Instrument all loads/stores in the clone with tm_read/tm_write
//            3. Redirect call sites on TM objects to the cloned version
//
// This is done in TMGlobalInitPass (module-level) so that method duplication
// happens once, before function-level passes instrument transaction functions.

#ifndef TM_METHOD_INSTRUMENTATION_HPP
#define TM_METHOD_INSTRUMENTATION_HPP

#include <functional>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallSet.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include "tm_annotation_utils.hpp"
#include "tm_call_graph.hpp"
#include "tm_debug.hpp"
#include "tm_local_vars.hpp"
#include "tm_runtime_hooks.hpp"

using namespace llvm;

namespace tm_method_instrumentation
{

// Clone mode: Instrument clones immediately, or mark for later inlining
enum class CloneMode { Instrument, AlwaysInline, CloneOnly };

struct TMMethodInfo {
    Function *Original;
    Function *Cloned;
    Value *TMGlobal;
};

static SmallPtrSet<const GlobalVariable *, 16> *TMGlobalsCache = nullptr;

static void collectTMGlobalsCached(Module &M, SmallPtrSetImpl<const GlobalVariable *> &TMG)
{
    if (TMGlobalsCache && TMGlobalsCache->empty() == false) {
        TMG.insert(TMGlobalsCache->begin(), TMGlobalsCache->end());
        return;
    }
    if (GlobalVariable *GVA = M.getNamedGlobal("llvm.global.annotations")) {
        if (Constant *Init = GVA->getInitializer()) {
            for (unsigned i = 0; i < Init->getNumOperands(); ++i) {
                Constant *Annotation = cast<Constant>(Init->getOperand(i));
                if (Annotation->getNumOperands() >= 2) {
                    Value *AnnotatedValue = Annotation->getOperand(0)->stripPointerCasts();
                    if (auto *AnnotatedGV = dyn_cast<GlobalVariable>(AnnotatedValue)) {
                        Value *StrOperand = Annotation->getOperand(1)->stripPointerCasts();
                        if (auto *StrGV = dyn_cast<GlobalVariable>(StrOperand)) {
                            if (auto *StrArray = dyn_cast<ConstantDataArray>(StrGV->getInitializer())) {
                                if (StrArray->getAsCString() == "tm") {
                                    TMG.insert(AnnotatedGV);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    TMGlobalsCache = new SmallPtrSet<const GlobalVariable *, 16>();
    TMGlobalsCache->insert(TMG.begin(), TMG.end());
}

static bool tracesToTMGlobal(Value *Ptr, Module &M)
{
    SmallPtrSet<const GlobalVariable *, 8> TMG;
    collectTMGlobalsCached(M, TMG);

    Value *Current = Ptr->stripPointerCasts();
    for (int depth = 0; depth < 20 && Current != nullptr; ++depth) {
        Current = Current->stripPointerCasts();

        if (const GlobalVariable *GV = dyn_cast<GlobalVariable>(Current)) {
            if (TMG.count(GV)) return true;
        }

        if (const GEPOperator *GEP = dyn_cast<GEPOperator>(Current)) {
            Current = const_cast<Value*>(GEP->getPointerOperand());
        } else if (const LoadInst *Load = dyn_cast<LoadInst>(Current)) {
            Current = const_cast<Value*>(Load->getPointerOperand());
        } else if (auto *Call = dyn_cast<CallInst>(Current)) {
            // Trace through TM clone call returns: if `this` (arg 0) of a
            // TM-cloned method traces to a TM global, the return pointer does too.
            Function *Callee = Call->getCalledFunction();
            if (Callee && Callee->getName().ends_with("_tm_clone") &&
                Call->arg_size() > 0) {
                Current = Call->getArgOperand(0);
            } else {
                break;
            }
        } else {
            break;
        }
    }
    return false;
}

// Replace llvm.memcpy/memmove/memset on TM globals with per-byte instrumented loops.
#ifndef DISABLE_TM_READ_WRITE
static void instrumentMemoryIntrinsic(CallBase *Call, Module &M,
                                      const TMRuntimeHooks &H) {
    LLVMContext &Ctx = M.getContext();
    auto *i8Ty = Type::getInt8Ty(Ctx);
    auto *i64Ty = Type::getInt64Ty(Ctx);
    auto *i64PtrTy = PointerType::get(Ctx, 0);
    Function *F = Call->getFunction();
    StringRef Name = Call->getCalledFunction()->getName();
    bool isMemset = Name.starts_with("llvm.memset");
    bool isMemmove = Name.starts_with("llvm.memmove");

    Value *Dst = Call->getArgOperand(0);
    Value *Len = Call->getArgOperand(2);
    Value *SrcOrVal = Call->getArgOperand(1);

    BasicBlock *OrigBB = Call->getParent();
    BasicBlock *ContBB = OrigBB->splitBasicBlock(Call, "mem_after");
    BasicBlock *LoopEntry = BasicBlock::Create(Ctx, "mem_loop_entry", F, ContBB);
    BasicBlock *LoopBody  = BasicBlock::Create(Ctx, "mem_loop_body", F, ContBB);

    OrigBB->getTerminator()->eraseFromParent();
    IRBuilder<>(OrigBB).CreateBr(LoopEntry);

    IRBuilder<> EB(LoopEntry);
    PHINode *Idx = EB.CreatePHI(i64Ty, 2, "mem_idx");
    Idx->addIncoming(ConstantInt::get(i64Ty, 0), OrigBB);
    EB.CreateCondBr(EB.CreateICmpEQ(Idx, Len), ContBB, LoopBody);

    IRBuilder<> BB(LoopBody);
    Value *Remaining = BB.CreateSub(Len, Idx);
    Value *CanWide = BB.CreateICmpUGE(Remaining, ConstantInt::get(i64Ty, 8));

    BasicBlock *WideBB = BasicBlock::Create(Ctx, "mem_wide", F, ContBB);
    BasicBlock *NarrowBB = BasicBlock::Create(Ctx, "mem_narrow", F, ContBB);
    BasicBlock *DoneBB = BasicBlock::Create(Ctx, "mem_done", F, ContBB);
    BB.CreateCondBr(CanWide, WideBB, NarrowBB);

    // Wide (8-byte) path
    IRBuilder<> WB(WideBB);
    Value *WideDstGEP = WB.CreateGEP(i8Ty, Dst, Idx);
    Value *WideDstPtr = WB.CreateBitCast(WideDstGEP, i64PtrTy);
    if (isMemset) {
        // Broadcast fill byte to all 8 byte positions in the i64 word
        Value *Fill = WB.CreateZExt(SrcOrVal, i64Ty);
        Fill = WB.CreateMul(Fill, ConstantInt::get(i64Ty, 0x0101010101010101ULL));
        WB.CreateStore(Fill, WideDstPtr);
    } else {
        // memcpy/memmove: tm_read_i8 from source, tm_write_i8 to dest
        Value *WideSrcGEP = WB.CreateGEP(i8Ty, SrcOrVal, Idx);
        Value *WideSrcPtr = WB.CreateBitCast(WideSrcGEP, i64PtrTy);
        Value *WideVal = WB.CreateCall(H.read_i8, {WideSrcPtr});
        WB.CreateCall(H.write_i8, {WideDstPtr, WideVal});
    }
    WB.CreateBr(DoneBB);

    // Narrow (1-byte) path
    IRBuilder<> NB(NarrowBB);
    Value *NarrowDstGEP = NB.CreateGEP(i8Ty, Dst, Idx);
    if (isMemset) {
        NB.CreateStore(SrcOrVal, NarrowDstGEP);
    } else {
        Value *NarrowSrcGEP = NB.CreateGEP(i8Ty, SrcOrVal, Idx);
        NB.CreateCall(H.write_i1, {NarrowDstGEP, NB.CreateCall(H.read_i1, {NarrowSrcGEP})});
    }
    NB.CreateBr(DoneBB);

    // Next iteration
    IRBuilder<> DB(DoneBB);
    PHINode *NextIdx = DB.CreatePHI(i64Ty, 2, "mem_next");
    NextIdx->addIncoming(ConstantInt::get(i64Ty, 8), WideBB);
    NextIdx->addIncoming(ConstantInt::get(i64Ty, 1), NarrowBB);
    Value *PhiNext = DB.CreateAdd(Idx, NextIdx);
    Idx->addIncoming(PhiNext, DoneBB);
    DB.CreateBr(LoopEntry);
}
#else
static void instrumentMemoryIntrinsic(CallBase *, Module &,
                                      const TMRuntimeHooks &) {}
#endif

static bool isCallOnTMObject(CallBase *Call, Module &M)
{
    if (Call->isIndirectCall()) return false;

    Function *Callee = Call->getCalledFunction();
    if (!Callee || Callee->isDeclaration()) return false;
    if (Callee->getName().starts_with("tm_")) return false;

    if (Call->arg_size() == 0) return false;

    Value *ThisPtr = Call->getArgOperand(0);
    if (!ThisPtr) return false;

    bool traced = tracesToTMGlobal(ThisPtr, M);
    if (traced) {
        TM_DEBUG("isCallOnTMObject: found call to %s on TM object",
                Callee->getName().str().c_str());
    }
    return traced;
}

// Per-function map of which pointer arguments trace to TM globals.
// Used by cloneMethod/computeClonableFunctions/redirectCallsToClones
// to decide which functions to clone and which calls to redirect.
// Not used for load/store instrumentation decisions (always-instrument default).
static DenseMap<const Function *, SmallSet<unsigned, 4>> TMTracedArgs;

#ifndef DISABLE_TM_READ_WRITE

// Instrument ALL loads/stores in a function, only skipping tm_local-annotated
// variables. The old heuristic (isSharedPointer + isTMTracedPtr double-guard)
// had false negatives: it skipped stores to pointers that appeared "local"
// but actually pointed to shared memory (e.g., v_.end_ = pos_ in a vector
// destructor).  Instrumenting everything is the safe default.
static void instrumentLoadStoresInFunction(Function *F, Module *M,
                                              const TMRuntimeHooks &H)
{
    if (TMAudit) {
        int tLoads = 0, iLoads = 0, tStores = 0, iStores = 0;
        errs() << "\n[AUDIT] === ALL loads in clone " << F->getName() << " ===\n";
        for (auto &BB : *F) for (auto &I : BB) {
            auto *L = dyn_cast<LoadInst>(&I); if (!L) continue;
            tLoads++;
            bool local = isTMLocalVar(L->getPointerOperand(), *M);
            errs() << "[AUDIT] " << (local ? "* " : "  ")
                   << "LOAD tm_local=" << (local ? "Y" : "N")
                   << " type=" << *L->getType()
                   << "\n    ptr=" << *L->getPointerOperand()
                   << "\n    base=" << *getBaseObject(L->getPointerOperand()) << "\n";
            if (!local) iLoads++;
        }
        errs() << "[AUDIT] === ALL stores in clone " << F->getName() << " ===\n";
        for (auto &BB : *F) for (auto &I : BB) {
            auto *S = dyn_cast<StoreInst>(&I); if (!S) continue;
            tStores++;
            bool local = isTMLocalVar(S->getPointerOperand(), *M);
            errs() << "[AUDIT] " << (local ? "* " : "  ")
                   << "STORE tm_local=" << (local ? "Y" : "N")
                   << " val=" << *S->getValueOperand()
                   << "\n    ptr=" << *S->getPointerOperand()
                   << "\n    base=" << *getBaseObject(S->getPointerOperand()) << "\n";
            if (!local) iStores++;
        }
        errs() << "[AUDIT] Summary for clone " << F->getName() << ": "
               << "LOAD " << iLoads << "/" << tLoads
               << " STORE " << iStores << "/" << tStores
               << " INSTRUMENTED; NOT instr (tm_local): LOAD "
               << (tLoads - iLoads) << " STORE " << (tStores - iStores) << "\n";
    }

    SmallVector<Instruction *, 16> ToErase;
    for (auto &BB : *F) {
        for (auto InstIt = BB.begin(); InstIt != BB.end();) {
            Instruction *I = &*InstIt++;
#ifndef DISABLE_MALLOC_FREE
            if (auto *Call = dyn_cast<CallBase>(I)) {
                IRBuilder<> B(I);
                if (handleMallocFree(Call, B, H, ToErase))
                    continue;
            }
#endif
            if (auto *Load = dyn_cast<LoadInst>(I)) {
                Value *Ptr = Load->getPointerOperand();
                if (isTMLocalVar(Ptr, *M)) continue;
                IRBuilder<> Builder(Load);
                if (auto *Call = emitTMRead(Builder, Ptr, Load->getType(), H)) {
                    Load->replaceAllUsesWith(Call);
                    ToErase.push_back(Load);
                }
            } else if (auto *Store = dyn_cast<StoreInst>(I)) {
                Value *Ptr = Store->getPointerOperand();
                if (isTMLocalVar(Ptr, *M)) continue;
                IRBuilder<> Builder(Store);
                if (emitTMWrite(Builder, Ptr, Store->getValueOperand(), H))
                    ToErase.push_back(Store);
            }
        }
    }
    for (auto *I : ToErase) I->eraseFromParent();
}
#else
static void instrumentLoadStoresInFunction(Function *, Module *,
                                              const TMRuntimeHooks &) {}
#endif

static Function *cloneMethod(Function *Original, const Twine &Suffix,
                              Module *M, LLVMContext &Ctx,
                              SmallPtrSetImpl<const GlobalVariable *> &TMG,
                              const TMRuntimeHooks &H,
                              CloneMode Mode = CloneMode::Instrument)
{
    FunctionType *FTy = Original->getFunctionType();
    Function *NewFunc = Function::Create(
        FTy, GlobalValue::PrivateLinkage, Original->getAddressSpace(),
        Original->getName() + Suffix, M);

    ValueToValueMapTy VMap;
    Function::arg_iterator DestI = NewFunc->arg_begin();
    for (const Argument &I : Original->args()) {
        DestI->setName(I.getName());
        VMap[&I] = &*DestI++;
    }

    SmallVector<ReturnInst *, 8> Returns;
    CloneFunctionInto(NewFunc, Original, VMap,
                      CloneFunctionChangeType::LocalChangesOnly, Returns, "",
                      nullptr);

    NewFunc->setDSOLocal(true);
    if (Mode == CloneMode::AlwaysInline) {
        // Strip noinline in case the original carried it (e.g. __tree_deleter
        // from libc++).  alwaysinline + noinline is incompatible.
        NewFunc->removeFnAttr(llvm::Attribute::NoInline);
        NewFunc->addFnAttr(llvm::Attribute::AlwaysInline);
    } else if (Mode == CloneMode::Instrument) {
        NewFunc->addFnAttr(llvm::Attribute::NoInline);
        NewFunc->addFnAttr(llvm::Attribute::OptimizeNone);
        // Propagate TMTracedArgs BEFORE instrumentation so isTMTracedPtr works.
        auto OrigIt = TMTracedArgs.find(Original);
        if (OrigIt != TMTracedArgs.end())
            TMTracedArgs[NewFunc] = OrigIt->second;
        instrumentLoadStoresInFunction(NewFunc, M, H);
    } else { // CloneMode::CloneOnly
        // Clone without instrumentation — instrument AFTER call redirection
        // (done in TMGlobalInitPass) so tracesFromTMGlobal can find callers.
        // Propagate TMTracedArgs now for later use by instrumentation.
        auto OrigIt = TMTracedArgs.find(Original);
        if (OrigIt != TMTracedArgs.end())
            TMTracedArgs[NewFunc] = OrigIt->second;
    }

    TM_DEBUG("Cloned method %s -> %s (%s)",
            Original->getName().str().c_str(),
            NewFunc->getName().str().c_str(),
            Mode == CloneMode::AlwaysInline ? "alwaysinline" :
            Mode == CloneMode::Instrument ? "instrumented" : "cloneonly");

    return NewFunc;
}

static SmallVector<std::pair<Function *, Function *>, 32> &
getClonedMethodsMap()
{
    static SmallVector<std::pair<Function *, Function *>, 32> Map;
    return Map;
}

// Determine if an argument at a call site traces to a TM global.
static bool callArgTracesToTMGlobal(CallBase *Call, unsigned ArgIdx, Module &M)
{
    Value *Arg = Call->getArgOperand(ArgIdx)->stripPointerCasts();
    Type *ArgTy = Arg->getType();
    if (!ArgTy->isPointerTy()) return false;
    return tracesFromTMGlobal(Arg, M);
}

// Build TMTracedArgs: for each call site in TxReachableFuncs, check
// if any pointer argument traces to a TM global.  Record traced args.
static void computeTMTracedArgs(Module &M,
                                SmallPtrSetImpl<Function *> &TxReachableFuncs)
{
    TMTracedArgs.clear();
    for (auto *Caller : TxReachableFuncs) {
        if (Caller->isDeclaration()) continue;
        for (auto &BB : *Caller) {
            for (auto &I : BB) {
                auto *Call = dyn_cast<CallBase>(&I);
                if (!Call) continue;
                Function *Callee = Call->getCalledFunction();
                if (!Callee || Callee->isDeclaration()) continue;
                for (unsigned i = 0; i < Call->arg_size(); i++)
                    if (callArgTracesToTMGlobal(Call, i, M))
                        TMTracedArgs[Callee].insert(i);
            }
        }
    }
    // Propagate: if a function has at least one TM-traced pointer arg,
    // mark ALL pointer-type args as TM-traced.  This ensures cloned
    // functions instrument accesses through ALL pointer args, not just
    // those that directly trace to a TM global.  Without this, a cloned
    // helper like __tree_balance_after_insert would skip instrumentation
    // on the new heap node (arg 1), corrupting the tree under concurrent
    // access.
    for (auto &[F, TracedSet] : TMTracedArgs) {
        if (TracedSet.empty() || F->isDeclaration()) continue;
        for (unsigned i = 0; i < F->arg_size(); i++) {
            if (F->getArg(i)->getType()->isPointerTy())
                TracedSet.insert(i);
        }
    }
}

static SmallPtrSet<Function *, 32>
computeClonableFunctions(Module &M,
                         SmallPtrSetImpl<Function *> &TxReachableFuncs,
                         CloneMode Mode = CloneMode::Instrument)
{
    SmallPtrSet<Function *, 32> Clonable;
    for (Function *F : TxReachableFuncs) {
        if (F->isDeclaration()) continue;
        if (F->getName().starts_with("tm_")) continue;
        if (hasAnnotation(*F, "transaction")) continue;
        // In AlwaysInline mode, clone ALL reachable functions so they get
        // alwaysinline and are inlined into the TX body.  Their loads/stores
        // are then instrumented by TMInstrumentInlinePass.  Without this,
        // accessor functions like vector::operator[] are never inlined and
        // their internal loads bypass TM entirely.
        if (Mode == CloneMode::AlwaysInline) {
            Clonable.insert(F);
            continue;
        }
        // Always clonable if no pointer args (value-type helpers like std::get<N>)
        bool hasPtrArg = false;
        for (auto &Arg : F->args())
            if (Arg.getType()->isPointerTy()) { hasPtrArg = true; break; }
        if (!hasPtrArg) {
            Clonable.insert(F);
            continue;
        }
        // Clone only if at least one pointer arg is TM-traced at some call site
        auto it = TMTracedArgs.find(F);
        if (it != TMTracedArgs.end() && !it->second.empty())
            Clonable.insert(F);
    }
    TM_DEBUG("computeClonableFunctions: %d functions clonable%s",
             (int)Clonable.size(),
             Mode == CloneMode::AlwaysInline ? " (AlwaysInline: ALL)" : " (TM-traced)");
    return Clonable;
}

// Redirect all direct function calls within F to their clones (if a clone exists).
// Also detects unclonable callees for diagnostic purposes.
// Mode controls redirection scope: AlwaysInline redirects all cloned calls.
static void redirectCallsToClones(Function &F, Module &M,
                                  SmallPtrSetImpl<Function *> &TxReachableFuncs,
                                  SmallVectorImpl<std::pair<Function *, Function *>> &ClonedMap,
                                  CloneMode Mode = CloneMode::Instrument)
{
    // Two-pass approach to avoid iterator invalidation:
    //   Pass 1: collect call sites that need redirecting (read-only)
    //   Pass 2: apply the redirects
    SmallVector<std::pair<CallBase *, Function *>, 32> ToRedirect;

    // In AlwaysInline mode, ALL reachable functions have clones and all
    // calls between them must be redirected so the AlwaysInlinerPass can
    // inline the full transitive closure into the TX body.  The
    // hasTMArg check below is too conservative for RAII helpers like
    // _ConstructTransactionD1/D2 whose `this` pointer is a local alloca
    // — they write to g_vec.__end_ through a stored pointer, not a
    // direct TM-traced argument.
    if (Mode == CloneMode::AlwaysInline) {
        for (auto &BB : F) {
            for (auto &I : BB) {
                auto *Call = dyn_cast<CallBase>(&I);
                if (!Call) continue;
                Function *Callee = Call->getCalledFunction();
                if (!Callee || Callee->isDeclaration()) continue;
                if (Callee->getName().starts_with("tm_")) continue;
                if (!TxReachableFuncs.count(Callee)) continue;
                for (auto &pair : ClonedMap) {
                    if (pair.first == Callee && pair.second) {
                        ToRedirect.push_back({Call, Callee});
                        break;
                    }
                }
            }
        }
    } else {
        for (auto &BB : F) {
            for (auto &I : BB) {
                auto *Call = dyn_cast<CallBase>(&I);
                if (!Call) continue;
                Function *Callee = Call->getCalledFunction();
                if (!Callee || Callee->isDeclaration()) continue;
                if (Callee->getName().starts_with("tm_")) continue;
                if (!TxReachableFuncs.count(Callee)) continue;

                // Only redirect this call site if at least one pointer argument
                // traces to a TM global (i.e. this is a TM-data access, not a
                // local-container operation within the same transaction).
                bool hasTMArg = false;
                for (unsigned i = 0; i < Call->arg_size(); i++)
                    if (callArgTracesToTMGlobal(Call, i, M))
                        { hasTMArg = true; break; }
                if (!hasTMArg) {
                    auto it = TMTracedArgs.find(Callee);
                    if (it != TMTracedArgs.end() && !it->second.empty())
                        hasTMArg = true;
                }
                if (!hasTMArg) continue;

                for (auto &pair : ClonedMap)
                    if (pair.first == Callee && pair.second)
                        { ToRedirect.push_back({Call, Callee}); break; }
            }
        }
    }

    for (auto &P : ToRedirect) {
        for (auto &pair : ClonedMap) {
            if (pair.first == P.second) {
                P.first->setCalledFunction(pair.second);
                TM_DEBUG("redirectCallsToClones: %s -> %s in %s",
                        P.second->getName().str().c_str(),
                        pair.second->getName().str().c_str(),
                        F.getName().str().c_str());
                break;
            }
        }
    }
    if (!ToRedirect.empty()) {
        TM_DEBUG("[VERIFY] redirectCallsToClones: %zu calls redirected in %s",
                 (size_t)ToRedirect.size(), F.getName().str().c_str());
        for (auto &P : ToRedirect) {
            CallBase *CB = P.first;
            Function *Callee = CB->getCalledFunction();
            TM_DEBUG("  -> now calls: %s", Callee ? Callee->getName().str().c_str() : "null");
        }
    }
}

// Clone all non-TX functions in the TX-reachable call graph.
// Mode controls whether clones get full instrumentation or alwaysinline.
// Returns the global clone map so callers can redirect TX function calls.
static SmallVector<std::pair<Function *, Function *>, 32> &
cloneTxReachableGraph(Module &M,
                      SmallPtrSetImpl<Function *> &TxReachableFuncs,
                      const TMRuntimeHooks &H,
                      CloneMode Mode = CloneMode::Instrument)
{
    SmallPtrSet<const GlobalVariable *, 16> TMG;
    collectTMGlobalsCached(M, TMG);
    auto &ClonedMap = getClonedMethodsMap();

    computeTMTracedArgs(M, TxReachableFuncs);
    SmallPtrSet<Function *, 32> ToClone = computeClonableFunctions(M, TxReachableFuncs, Mode);

    // Pass 1: clone all functions first (so ClonedMap is complete)
    for (Function *F : ToClone) {
        if (F->isDeclaration()) continue;
        if (F->getName().starts_with("tm_")) continue;
        if (hasAnnotation(*F, "transaction")) continue;

        bool alreadyCloned = false;
        for (auto &pair : ClonedMap)
            if (pair.first == F) { alreadyCloned = true; break; }
        if (alreadyCloned) continue;

        Function *Cloned = cloneMethod(F, "_tm_clone", &M, M.getContext(), TMG, H, Mode);
        ClonedMap.push_back({F, Cloned});
    }

    // Pass 1.5: propagate TMTracedArgs from originals to clones.
    // Since argument positions are preserved during cloning, the same
    // argument indices apply to both original and clone. This ensures
    // isTMTracedPtr works correctly inside cloned functions.
    for (auto &pair : ClonedMap) {
        auto OrigIt = TMTracedArgs.find(pair.first);
        if (OrigIt != TMTracedArgs.end()) {
            TMTracedArgs[pair.second] = OrigIt->second;
        }
    }

    // Pass 2: redirect all cloned functions (ClonedMap is complete, so all
    // intra-clone calls can be redirected regardless of processing order).
    for (auto &pair : ClonedMap)
        redirectCallsToClones(*pair.second, M, TxReachableFuncs, ClonedMap, Mode);

    return ClonedMap;
}

// Instrument all cloned functions AFTER call redirection, so that
// tracesFromTMGlobal can trace arguments through actual callers and
// find TM globals as base objects. This fixes the case where vector
// internal stores (push_back → __end_, __begin_) go through allocas
// but the stored value is a TM-traced argument.
static void instrumentAllClones(
    SmallVectorImpl<std::pair<Function *, Function *>> &ClonedMap,
    Module &M, const TMRuntimeHooks &H)
{
    for (auto &pair : ClonedMap) {
        Function *Clone = pair.second;
        Function *Original = pair.first;
        auto OrigIt = TMTracedArgs.find(Original);
        if (OrigIt != TMTracedArgs.end())
            TMTracedArgs[Clone] = OrigIt->second;
        Clone->addFnAttr(llvm::Attribute::NoInline);
        Clone->addFnAttr(llvm::Attribute::OptimizeNone);
        instrumentLoadStoresInFunction(Clone, &M, H);
        TM_DEBUG("After-redirect instrumented clone: %s", Clone->getName().str().c_str());
    }
}

} // namespace tm_method_instrumentation

#endif // TM_METHOD_INSTRUMENTATION_HPP