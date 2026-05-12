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

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include "tm_annotation_utils.hpp"
#include "tm_debug.hpp"
#include "tm_local_vars.hpp"

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

static void instrumentLoadsStoresInFunction(Function *F, Module *M,
                                             FunctionCallee tm_read_i1,
                                             FunctionCallee tm_read_i2,
                                             FunctionCallee tm_read_i4,
                                             FunctionCallee tm_read_i8,
                                             FunctionCallee tm_read_f4,
                                             FunctionCallee tm_read_f8,
                                             FunctionCallee tm_read_ptr,
                                             FunctionCallee tm_write_i1,
                                             FunctionCallee tm_write_i2,
                                             FunctionCallee tm_write_i4,
                                             FunctionCallee tm_write_i8,
                                             FunctionCallee tm_write_f4,
                                             FunctionCallee tm_write_f8,
                                             FunctionCallee tm_write_ptr)
{
    SmallVector<Instruction *, 16> ToErase;
    Type *i8PtrTy = PointerType::getUnqual(M->getContext());

    for (auto &BB : *F) {
        for (auto &I : BB) {
            if (auto *Load = dyn_cast<LoadInst>(&I)) {
                Value *Ptr = Load->getPointerOperand();
                if (!isSharedPointer(Ptr, {}, *F, *M)) continue;

                IRBuilder<> Builder(Load);
                Value *PtrCast = Builder.CreateBitCast(Ptr, i8PtrTy);
                Value *ReadValue = nullptr;
                Type *LoadTy = Load->getType();

                if (LoadTy->isIntegerTy(8)) {
                    ReadValue = Builder.CreateCall(tm_read_i1, {PtrCast});
                } else if (LoadTy->isIntegerTy(16)) {
                    ReadValue = Builder.CreateCall(tm_read_i2, {PtrCast});
                } else if (LoadTy->isIntegerTy(32)) {
                    ReadValue = Builder.CreateCall(tm_read_i4, {PtrCast});
                } else if (LoadTy->isIntegerTy(64)) {
                    ReadValue = Builder.CreateCall(tm_read_i8, {PtrCast});
                } else if (LoadTy->isFloatTy()) {
                    ReadValue = Builder.CreateCall(tm_read_f4, {PtrCast});
                } else if (LoadTy->isDoubleTy()) {
                    ReadValue = Builder.CreateCall(tm_read_f8, {PtrCast});
                } else if (LoadTy->isPointerTy()) {
                    Value *PtrVal = Builder.CreateCall(tm_read_ptr, {PtrCast});
                    ReadValue = Builder.CreateBitCast(PtrVal, LoadTy);
                }

                if (ReadValue) {
                    Load->replaceAllUsesWith(ReadValue);
                    ToErase.push_back(Load);
                }
            } else if (auto *Store = dyn_cast<StoreInst>(&I)) {
                Value *Ptr = Store->getPointerOperand();
                if (!isSharedPointer(Ptr, {}, *F, *M)) continue;

                IRBuilder<> Builder(Store);
                Value *PtrCast = Builder.CreateBitCast(Ptr, i8PtrTy);
                Value *Val = Store->getValueOperand();
                Type *ValTy = Val->getType();

                if (ValTy->isIntegerTy(8)) {
                    Builder.CreateCall(tm_write_i1, {PtrCast, Val});
                    ToErase.push_back(Store);
                } else if (ValTy->isIntegerTy(16)) {
                    Builder.CreateCall(tm_write_i2, {PtrCast, Val});
                    ToErase.push_back(Store);
                } else if (ValTy->isIntegerTy(32)) {
                    Builder.CreateCall(tm_write_i4, {PtrCast, Val});
                    ToErase.push_back(Store);
                } else if (ValTy->isIntegerTy(64)) {
                    Builder.CreateCall(tm_write_i8, {PtrCast, Val});
                    ToErase.push_back(Store);
                } else if (ValTy->isFloatTy()) {
                    Builder.CreateCall(tm_write_f4, {PtrCast, Val});
                    ToErase.push_back(Store);
                } else if (ValTy->isDoubleTy()) {
                    Builder.CreateCall(tm_write_f8, {PtrCast, Val});
                    ToErase.push_back(Store);
                } else if (ValTy->isPointerTy()) {
                    Value *ValCast = Builder.CreateBitCast(Val, i8PtrTy);
                    Builder.CreateCall(tm_write_ptr, {PtrCast, ValCast});
                    ToErase.push_back(Store);
                }
            }
        }
    }

    for (Instruction *I : ToErase) {
        I->eraseFromParent();
    }
}

static Function *cloneMethodWithSuffix(Function *Original, const Twine &Suffix,
                                        Module *M, LLVMContext &Ctx,
                                        SmallPtrSetImpl<const GlobalVariable *> &TMG,
                                        FunctionCallee tm_read_i1,
                                        FunctionCallee tm_read_i2,
                                        FunctionCallee tm_read_i4,
                                        FunctionCallee tm_read_i8,
                                        FunctionCallee tm_read_f4,
                                        FunctionCallee tm_read_f8,
                                        FunctionCallee tm_read_ptr,
                                        FunctionCallee tm_write_i1,
                                        FunctionCallee tm_write_i2,
                                        FunctionCallee tm_write_i4,
                                        FunctionCallee tm_write_i8,
                                        FunctionCallee tm_write_f4,
                                        FunctionCallee tm_write_f8,
                                        FunctionCallee tm_write_ptr)
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

    instrumentLoadsStoresInFunction(NewFunc, M,
                                     tm_read_i1, tm_read_i2, tm_read_i4, tm_read_i8,
                                     tm_read_f4, tm_read_f8, tm_read_ptr,
                                     tm_write_i1, tm_write_i2, tm_write_i4, tm_write_i8,
                                     tm_write_f4, tm_write_f8, tm_write_ptr);

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

static void processMethodCalls(Module &M,
                               FunctionCallee tm_read_i1,
                               FunctionCallee tm_read_i2,
                               FunctionCallee tm_read_i4,
                               FunctionCallee tm_read_i8,
                               FunctionCallee tm_read_f4,
                               FunctionCallee tm_read_f8,
                               FunctionCallee tm_read_ptr,
                               FunctionCallee tm_write_i1,
                               FunctionCallee tm_write_i2,
                               FunctionCallee tm_write_i4,
                               FunctionCallee tm_write_i8,
                               FunctionCallee tm_write_f4,
                               FunctionCallee tm_write_f8,
                               FunctionCallee tm_write_ptr)
{
    SmallPtrSet<const GlobalVariable *, 16> TMG;
    collectTMGlobalsCached(M, TMG);

    SmallVector<CallBase *, 32> CallsToInstrument;
    for (auto &F : M) {
        for (auto &BB : F) {
            for (auto &I : BB) {
                if (auto *Call = dyn_cast<CallBase>(&I)) {
                    if (isCallOnTMObject(Call, M)) {
                        CallsToInstrument.push_back(Call);
                    }
                }
            }
        }
    }

    if (CallsToInstrument.empty()) return;

    TM_DEBUG("Found %d method calls on TM objects", (int)CallsToInstrument.size());

    SmallPtrSet<Function *, 16> MethodsToClone;
    for (CallBase *Call : CallsToInstrument) {
        Function *Callee = Call->getCalledFunction();
        if (Callee && !Callee->isDeclaration()) {
            MethodsToClone.insert(Callee);
        }
    }

    auto &ClonedMap = getClonedMethodsMap();
    for (Function *Method : MethodsToClone) {
        bool alreadyCloned = false;
        for (auto &pair : ClonedMap) {
            if (pair.first == Method) { alreadyCloned = true; break; }
        }
        if (alreadyCloned) continue;

        Function *Cloned = cloneMethodWithSuffix(
            Method, "_tm_uninst", &M, M.getContext(), TMG,
            tm_read_i1, tm_read_i2, tm_read_i4, tm_read_i8,
            tm_read_f4, tm_read_f8, tm_read_ptr,
            tm_write_i1, tm_write_i2, tm_write_i4, tm_write_i8,
            tm_write_f4, tm_write_f8, tm_write_ptr);

        ClonedMap.push_back({Method, Cloned});
    }

    for (CallBase *Call : CallsToInstrument) {
        Function *Callee = Call->getCalledFunction();
        if (!Callee) continue;

        for (auto &pair : ClonedMap) {
            if (pair.first == Callee) {
                Call->setCalledFunction(pair.second);
                TM_DEBUG("Redirected %s -> %s",
                        Callee->getName().str().c_str(),
                        pair.second->getName().str().c_str());
                break;
            }
        }
    }
}

} // namespace tm_method_instrumentation

#endif // TM_METHOD_INSTRUMENTATION_HPP