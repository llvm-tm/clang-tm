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
enum class CloneMode { Instrument, AlwaysInline };

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
// Per-function map of which pointer arguments trace to TM globals.
// Used by isTMTracedPtr to decide whether an Argument-based pointer
// should be instrumented in a cloned function.
static DenseMap<const Function *, SmallSet<unsigned, 4>> TMTracedArgs;

// Check if a pointer's base argument is TM-traced.
// In cloned functions, an Argument base that is NOT TM-traced
// indicates a local/stack pointer (e.g. `this` of a local container)
// that should not be instrumented.
static bool isTMTracedPtr(const Value *Ptr)
{
    const Value *Base = getBaseObject(const_cast<Value *>(Ptr));
    if (auto *Arg = dyn_cast<Argument>(const_cast<Value *>(Base))) {
        auto it = TMTracedArgs.find(Arg->getParent());
        if (it == TMTracedArgs.end() || !it->second.count(Arg->getArgNo()))
            return false;
    }
    return true;
}

static void instrumentLoadsStoresInFunction(Function *F, Module *M,
                                             const TMRuntimeHooks &H)
{
    SmallPtrSet<const Value *, 32> LocalVars;
    collectLocalVariables(*F, LocalVars);
    SmallVector<Instruction *, 16> ToErase;

    for (auto &BB : *F) {
        for (auto &I : BB) {
            if (auto *Load = dyn_cast<LoadInst>(&I)) {
                Value *Ptr = Load->getPointerOperand();
                if (!isSharedPointer(Ptr, LocalVars, *F, *M)) continue;
                if (!isTMTracedPtr(Ptr)) continue;
                IRBuilder<> Builder(Load);
                if (auto *Call = emitTMRead(Builder, Ptr, Load->getType(), H)) {
                    Load->replaceAllUsesWith(Call);
                    ToErase.push_back(Load);
                }
            } else if (auto *Store = dyn_cast<StoreInst>(&I)) {
                Value *Ptr = Store->getPointerOperand();
                if (!isSharedPointer(Ptr, LocalVars, *F, *M)) continue;
                if (!isTMTracedPtr(Ptr)) continue;
                IRBuilder<> Builder(Store);
                if (emitTMWrite(Builder, Ptr, Store->getValueOperand(), H))
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
        NewFunc->addFnAttr(llvm::Attribute::AlwaysInline);
    } else {
        NewFunc->addFnAttr(llvm::Attribute::NoInline);
        NewFunc->addFnAttr(llvm::Attribute::OptimizeNone);
        instrumentLoadsStoresInFunction(NewFunc, M, H);
    }

    TM_DEBUG("Cloned method %s -> %s (%s)",
            Original->getName().str().c_str(),
            NewFunc->getName().str().c_str(),
            Mode == CloneMode::AlwaysInline ? "alwaysinline" : "instrumented");

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
}

static SmallPtrSet<Function *, 32>
computeClonableFunctions(Module &M,
                         SmallPtrSetImpl<Function *> &TxReachableFuncs)
{
    SmallPtrSet<Function *, 32> Clonable;
    for (Function *F : TxReachableFuncs) {
        if (F->isDeclaration()) continue;
        if (F->getName().starts_with("tm_")) continue;
        if (hasAnnotation(*F, "transaction")) continue;
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
    TM_DEBUG("computeClonableFunctions: %d functions clonable (TM-traced args)",
             (int)Clonable.size());
    return Clonable;
}

// Redirect all direct function calls within F to their clones (if a clone exists).
// Also detects unclonable callees for diagnostic purposes.
static void redirectCallsToClones(Function &F, Module &M,
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

            // Only redirect this call site if at least one pointer argument
            // traces to a TM global (i.e. this is a TM-data access, not a
            // local-container operation within the same transaction).
            bool hasTMArg = false;
            for (unsigned i = 0; i < Call->arg_size(); i++)
                if (callArgTracesToTMGlobal(Call, i, M))
                    { hasTMArg = true; break; }
            if (!hasTMArg) continue;

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
    if (!ToRedirect.empty()) {
        errs() << "[VERIFY] redirectCallsToClones: " << ToRedirect.size()
               << " calls redirected in " << F.getName() << "\n";
        for (auto &P : ToRedirect) {
            CallBase *CB = P.first;
            Function *Callee = CB->getCalledFunction();
            errs() << "  -> now calls: " << (Callee ? Callee->getName() : "null") << "\n";
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

        Function *Cloned = cloneMethod(F, "_tm_clone", &M, M.getContext(), TMG, H, Mode);
        ClonedMap.push_back({F, Cloned});
    }

    // Pass 2: redirect all cloned functions (ClonedMap is complete, so all
    // intra-clone calls can be redirected regardless of processing order).
    for (auto &pair : ClonedMap)
        redirectCallsToClones(*pair.second, M, TxReachableFuncs, ClonedMap);

    return ClonedMap;
}

} // namespace tm_method_instrumentation

#endif // TM_METHOD_INSTRUMENTATION_HPP