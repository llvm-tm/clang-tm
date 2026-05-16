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
        } else {
            break;
        }
    }
    return false;
}

// Replace llvm.memcpy/memmove/memset on TM globals with per-byte instrumented loops.
#ifndef DISABLE_TM_READ_WRITE
static void instrumentMemoryIntrinsic(CallInst *Call, Module &M,
                                      const TMRuntimeHooks &H) {
    LLVMContext &Ctx = M.getContext();
    auto *i8Ty = Type::getInt8Ty(Ctx);
    auto *i64Ty = Type::getInt64Ty(Ctx);
    Function *F = Call->getFunction();
    StringRef Name = Call->getCalledFunction()->getName();
    bool isMemset = (Name == "llvm.memset");

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
    Value *DG = BB.CreateGEP(i8Ty, Dst, Idx);
    if (isMemset) {
        BB.CreateCall(H.write_i1, {DG, SrcOrVal});
    } else {
        Value *SG = BB.CreateGEP(i8Ty, SrcOrVal, Idx);
        BB.CreateCall(H.write_i1, {DG, BB.CreateCall(H.read_i1, {SG})});
    }
    Idx->addIncoming(BB.CreateAdd(Idx, ConstantInt::get(i64Ty, 1)), LoopBody);
    BB.CreateBr(LoopEntry);

    Call->eraseFromParent();
}
#else
static void instrumentMemoryIntrinsic(CallInst *, Module &,
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

#ifndef DISABLE_TM_READ_WRITE
static void instrumentLoadsStoresInFunction(Function *F, Module *M,
                                             const TMRuntimeHooks &H)
{
    SmallVector<Instruction *, 16> ToErase;

    for (auto &BB : *F) {
        for (auto &I : BB) {
            if (auto *Load = dyn_cast<LoadInst>(&I)) {
                Value *Ptr = Load->getPointerOperand();
                if (!isSharedPointer(Ptr, {}, *F, *M)) continue;
                IRBuilder<> Builder(Load);
                if (auto *Call = emitTMRead(Builder, Ptr, Load->getType(), H)) {
                    Load->replaceAllUsesWith(Call);
                    ToErase.push_back(Load);
                }
            } else if (auto *Store = dyn_cast<StoreInst>(&I)) {
                Value *Ptr = Store->getPointerOperand();
                if (!isSharedPointer(Ptr, {}, *F, *M)) continue;
                IRBuilder<> Builder(Store);
                emitTMWrite(Builder, Ptr, Store->getValueOperand(), H);
                ToErase.push_back(Store);
            }
        }
    }
    for (auto *I : ToErase) I->eraseFromParent();
}
#else
static void instrumentLoadsStoresInFunction(Function *, Module *,
                                             const TMRuntimeHooks &) {}
#endif

static Function *cloneMethodWithSuffix(Function *Original, const Twine &Suffix,
                                        Module *M, LLVMContext &Ctx,
                                        SmallPtrSetImpl<const GlobalVariable *> &TMG,
                                        const TMRuntimeHooks &H)
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
    NewFunc->addFnAttr(llvm::Attribute::NoInline);
    NewFunc->addFnAttr(llvm::Attribute::OptimizeNone);

    instrumentLoadsStoresInFunction(NewFunc, M, H);

    TM_DEBUG("Cloned method %s -> %s",
            Original->getName().str().c_str(),
            NewFunc->getName().str().c_str());

    return NewFunc;
}

static SmallVector<std::pair<Function *, Function *>, 32> &
getClonedMethodsMap()
{
    static SmallVector<std::pair<Function *, Function *>, 32> Map;
    return Map;
}

// Check if a function has any stores to TM-annotated globals.
// Read-only functions don't need cloning (no buffered writes to undo).
static bool hasDirectTMWrites(Function &F, Module &M)
{
    for (auto &BB : F)
        for (auto &I : BB)
            if (auto *Store = dyn_cast<StoreInst>(&I))
                if (tracesToTMGlobal(Store->getPointerOperand(), M))
                    return true;
    return false;
}

// Check if a function has any loads/stores that trace directly to a TM global.
static bool hasDirectTMGlobal(Function &F, Module &M)
{
    for (auto &BB : F)
        for (auto &I : BB) {
            if (auto *Load = dyn_cast<LoadInst>(&I))
                if (tracesToTMGlobal(Load->getPointerOperand(), M))
                    return true;
            if (auto *Store = dyn_cast<StoreInst>(&I))
                if (tracesToTMGlobal(Store->getPointerOperand(), M))
                    return true;
        }
    return false;
}

// Map from a Function* to the set of argument indices that are known to be
// TM-traceable.  Propagated through the call graph: if F passes its arg i
// (which is TM-traceable) to G at position j, then G's arg j is also
// TM-traceable.  This handles deep chains like:
//   TX → map::find(this=&g_apById) → tree::find(this) → __tree_left_rotate(...)
// where internal tree functions take `this` (an Argument) that originates
// from a TM global at the TX call site.
using TMTraceableArgsMap = DenseMap<Function *, SmallSet<unsigned, 4>>;

// Build the set of functions that should be cloned by propagating
// TM-traceability through the call graph from TX functions.
static SmallPtrSet<Function *, 32>
computeClonableFunctions(Module &M,
                         SmallPtrSetImpl<Function *> &TxReachableFuncs)
{
    // Step 1: seed with functions that have direct TM-global WRITES.
    // Read-only functions don't need cloning (no buffered writes to undo on abort).
    SmallPtrSet<Function *, 32> Clonable;
    TMTraceableArgsMap TraceableArgs;
    for (Function *F : TxReachableFuncs) {
        if (F->isDeclaration()) continue;
        if (F->getName().starts_with("tm_")) continue;
        if (hasAnnotation(*F, "transaction")) continue;
        if (hasDirectTMWrites(*F, M)) {
            Clonable.insert(F);
            // All arguments of a directly-writing function are potentially TM-traceable
            SmallSet<unsigned, 4> Args;
            for (unsigned i = 0; i < F->arg_size(); i++)
                Args.insert(i);
            TraceableArgs[F] = Args;
        }
    }

    // Step 2: fixed-point propagation through the call graph.
    // A function F is clonable if any caller C passes an argument to F
    // where that argument either:
    //   (a) traces directly to a TM global (e.g. &g_apById), OR
    //   (b) is a TM-traceable argument of C (propagated from a previous iteration)
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto &CallerF : M) {
            if (CallerF.isDeclaration()) continue;
            if (!TxReachableFuncs.count(&CallerF)) continue;
            for (auto &BB : CallerF) {
                for (auto &I : BB) {
                    auto *Call = dyn_cast<CallBase>(&I);
                    if (!Call) continue;
                    Function *Callee = Call->getCalledFunction();
                    if (!Callee || Callee->isDeclaration()) continue;
                    if (Callee->getName().starts_with("tm_")) continue;
                    if (hasAnnotation(*Callee, "transaction")) continue;
                    if (Clonable.count(Callee)) continue;

                    // Check each argument at this call site.
                    // Follow through GEPs, Loads, Calls, BitCasts to determine
                    // if the argument traces to a TM-traceable value.
                    // Recursive check: does Val trace to a TM-traceable value in CallerF?
                    // Uses TraceableArgs for argument propagation through the call graph.
                    std::function<bool(Value *, int)> isArgTraceable;
                    isArgTraceable = [&](Value *Val, int Depth) -> bool {
                        if (!Val || Depth > 10) return false;
                        Val = Val->stripPointerCasts();

                        if (tracesToTMGlobal(Val, M))
                            return true;

                        if (auto *ArgAsArg = dyn_cast<Argument>(Val)) {
                            auto it = TraceableArgs.find(&CallerF);
                            if (it != TraceableArgs.end() && it->second.count(ArgAsArg->getArgNo()))
                                return true;
                            return false;
                        }

                        if (auto *GEP = dyn_cast<GEPOperator>(Val))
                            return isArgTraceable(GEP->getPointerOperand(), Depth + 1);

                        if (auto *LI = dyn_cast<LoadInst>(Val))
                            return isArgTraceable(LI->getPointerOperand(), Depth + 1);

                        // Load from return of a function called on TM-traceable args
                        if (auto *CB = dyn_cast<CallBase>(Val)) {
                            for (unsigned j = 0; j < CB->arg_size(); j++)
                                if (isArgTraceable(CB->getArgOperand(j), Depth + 1))
                                    return true;
                            return false;
                        }

                        if (auto *Phi = dyn_cast<PHINode>(Val)) {
                            for (Value *Inc : Phi->incoming_values())
                                if (isArgTraceable(Inc, Depth + 1)) return true;
                            return false;
                        }

                        return false;
                    };

                    bool anyTraceable = false;
                    SmallSet<unsigned, 4> newArgs;
                    for (unsigned i = 0; i < Call->arg_size(); i++) {
                        if (isArgTraceable(Call->getArgOperand(i), 0)) {
                            anyTraceable = true;
                            newArgs.insert(i);
                        }
                    }
                    if (anyTraceable) {
                        Clonable.insert(Callee);
                        TraceableArgs[Callee] = newArgs;
                        changed = true;
                        TM_DEBUG("propagate: %s clonable via %s (args: %zu traceable)",
                                Callee->getName().str().c_str(),
                                CallerF.getName().str().c_str(),
                                newArgs.size());
                    }
                }
            }
        }
    }

    TM_DEBUG("computeClonableFunctions: %d functions clonable before filtering", (int)Clonable.size());

    // Step 3: filter out functions that don't actually access TM data.
    // These are pure pointer-computation functions (e.g. __wrap_iter constructors,
    // vector::begin/end, __tree::__root_ptr) that were propagated through
    // TM-traceable arguments but never load or store to TM globals.
    // Cloning them adds unnecessary TM instrumentation overhead without benefit.
    SmallPtrSet<Function *, 32> ToRemove;
    for (Function *F : Clonable) {
        if (!hasDirectTMGlobal(*F, M)) {
            ToRemove.insert(F);
        }
    }
    for (Function *F : ToRemove) {
        Clonable.erase(F);
        TraceableArgs.erase(F);
        TM_DEBUG("filter: %s removed (no direct TM access)", F->getName().str().c_str());
    }

    TM_DEBUG("computeClonableFunctions: %d functions clonable after filtering", (int)Clonable.size());
    return Clonable;
}

// Redirect all direct function calls within F to their clones (if a clone exists).
// Also detects unclonable callees for diagnostic purposes.
static void redirectCallsToClones(Function &F,
                                  SmallPtrSetImpl<Function *> &TxReachableFuncs,
                                  SmallVectorImpl<std::pair<Function *, Function *>> &ClonedMap)
{
    // Two-pass approach to avoid iterator invalidation:
    //   Pass 1: collect call sites that need redirecting (read-only)
    //   Pass 2: apply the redirects
    SmallVector<std::pair<CallBase *, Function *>, 32> ToRedirect;

    for (auto &BB : F) {
        for (auto &I : BB) {
            auto *Call = dyn_cast<CallBase>(&I);
            if (!Call) continue;
            Function *Callee = Call->getCalledFunction();
            if (!Callee || Callee->isDeclaration()) continue;
            if (Callee->getName().starts_with("tm_")) continue;
            if (!TxReachableFuncs.count(Callee)) continue;

            for (auto &pair : ClonedMap) {
                if (pair.first == Callee) {
                    ToRedirect.push_back({Call, Callee});
                    break;
                }
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
}

// Main entry point: clone all non-TX functions in the TX-reachable call graph.
// Each clone gets instrumented loads/stores, and calls within clones are
// redirected to their cloned callees.  This mirrors the paper's approach:
// the transitive call tree from a transaction is fully instrumented, while
// original functions stay clean for non-TM code.
//
// Returns the global clone map so callers can also redirect calls within
// original TX functions to cloned callees.
static SmallVector<std::pair<Function *, Function *>, 32> &
cloneTxReachableGraph(Module &M,
                      SmallPtrSetImpl<Function *> &TxReachableFuncs,
                      const TMRuntimeHooks &H)
{
    SmallPtrSet<const GlobalVariable *, 16> TMG;
    collectTMGlobalsCached(M, TMG);
    auto &ClonedMap = getClonedMethodsMap();

    SmallPtrSet<Function *, 32> ToClone = computeClonableFunctions(M, TxReachableFuncs);

    // Pass 1: clone all functions first (so ClonedMap is complete)
    for (Function *F : ToClone) {
        if (F->isDeclaration()) continue;
        if (F->getName().starts_with("tm_")) continue;
        if (hasAnnotation(*F, "transaction")) continue;

        bool alreadyCloned = false;
        for (auto &pair : ClonedMap)
            if (pair.first == F) { alreadyCloned = true; break; }
        if (alreadyCloned) continue;

        Function *Cloned = cloneMethodWithSuffix(F, "_tm_clone", &M, M.getContext(), TMG, H);
        ClonedMap.push_back({F, Cloned});
    }

    // Pass 2: redirect all cloned functions (ClonedMap is complete, so all
    // intra-clone calls can be redirected regardless of processing order).
    for (auto &pair : ClonedMap)
        redirectCallsToClones(*pair.second, TxReachableFuncs, ClonedMap);

    return ClonedMap;
}

} // namespace tm_method_instrumentation

#endif // TM_METHOD_INSTRUMENTATION_HPP