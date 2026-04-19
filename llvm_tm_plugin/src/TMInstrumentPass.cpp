#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Plugins/PassPlugin.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>

using namespace llvm;

namespace {

class TMGlobalInitPass : public PassInfoMixin<TMGlobalInitPass> {
public:
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
        LLVMContext &Ctx = M.getContext();
        Type *voidTy = Type::getVoidTy(Ctx);

        auto declareHook = [&](StringRef Name, Type *Ret, ArrayRef<Type*> Args) {
            FunctionType *FT = FunctionType::get(Ret, Args, false);
            return M.getOrInsertFunction(Name, FT);
        };

        FunctionCallee tm_init = declareHook("tm_init", voidTy, {});
        FunctionCallee tm_exit = declareHook("tm_exit", voidTy, {});
        FunctionCallee tm_init_thread = declareHook("tm_init_thread", voidTy, {});
        FunctionCallee tm_exit_thread = declareHook("tm_exit_thread", voidTy, {});

        bool modified = false;

        if (Function *MainFn = M.getFunction("main")) {
            BasicBlock &Entry = MainFn->getEntryBlock();
            IRBuilder<> Builder(&Entry, Entry.begin());
            Builder.CreateCall(tm_init, {});
        }

        for (auto &F : M) {
            if (F.isDeclaration()) continue;
            if (hasTransactionAnnotation(F) || hasTMGlobals(F)) {
                BasicBlock &Entry = F.getEntryBlock();
                IRBuilder<> Builder(&Entry, Entry.begin());
                Builder.CreateCall(tm_init_thread, {});

                for (auto &BB : F) {
                    if (auto *Ret = dyn_cast<ReturnInst>(BB.getTerminator())) {
                        IRBuilder<> RetBuilder(Ret);
                        RetBuilder.CreateCall(tm_exit_thread, {});
                    }
                }
                modified = true;
            }
        }

        return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
    }

    static bool isRequired() { return true; }

private:
    bool hasTransactionAnnotation(Function &F) const {
        Module *M = F.getParent();
        GlobalVariable *GA = M->getNamedGlobal("llvm.global.annotations");
        if (!GA)
            return false;

        auto *CA = dyn_cast<ConstantArray>(GA->getInitializer());
        if (!CA)
            return false;

        for (auto &Op : CA->operands()) {
            auto *CS = dyn_cast<ConstantStruct>(Op);
            if (!CS || CS->getNumOperands() < 2)
                continue;

            auto *AnnotatedVal = CS->getOperand(0)->stripPointerCasts();
            auto *AnnotatedFunc = dyn_cast<Function>(AnnotatedVal);
            if (!AnnotatedFunc || AnnotatedFunc != &F)
                continue;

            auto *AnnoVal = CS->getOperand(1)->stripPointerCasts();
            auto *GV = dyn_cast<GlobalVariable>(AnnoVal);
            if (!GV)
                continue;

            auto *Init = dyn_cast<ConstantDataSequential>(GV->getInitializer());
            if (!Init || !Init->isCString())
                continue;

            if (Init->getAsCString() == "transaction")
                return true;
        }
        return false;
    }

    bool hasTMGlobals(Function &F) const {
        Module *M = F.getParent();
        GlobalVariable *GA = M->getNamedGlobal("llvm.global.annotations");
        if (!GA)
            return false;

        auto *CA = dyn_cast<ConstantArray>(GA->getInitializer());
        if (!CA)
            return false;

        for (auto &Op : CA->operands()) {
            auto *CS = dyn_cast<ConstantStruct>(Op);
            if (!CS || CS->getNumOperands() < 2)
                continue;

            Value *Val = CS->getOperand(0)->stripPointerCasts();
            auto *AnnoVal = CS->getOperand(1)->stripPointerCasts();
            auto *GV = dyn_cast<GlobalVariable>(AnnoVal);
            if (!GV)
                continue;

            auto *Init = dyn_cast<ConstantDataSequential>(GV->getInitializer());
            if (!Init || !Init->isCString())
                continue;

            if (Init->getAsCString() == "tm") {
                if (auto *Arg = dyn_cast<Argument>(Val)) {
                    if (Arg->getParent() == &F)
                        return true;
                }
            }
        }
        return false;
    }
};

class TMInstrumentPass : public PassInfoMixin<TMInstrumentPass> {
public:
    PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) {
        if (!hasAnnotation(F, "transaction"))
            return PreservedAnalyses::all();

        // Override optnone - we need to instrument transaction functions
        // even if they're marked optnone
        Module *M = F.getParent();
        LLVMContext &Ctx = M->getContext();

        SmallPtrSet<const Value*, 8> TMValues;
        collectTMGlobals(*M, TMValues);

        Type *voidTy = Type::getVoidTy(Ctx);
        Type *i8Ty = Type::getInt8Ty(Ctx);
        Type *i16Ty = Type::getInt16Ty(Ctx);
        Type *i32Ty = Type::getInt32Ty(Ctx);
        Type *i64Ty = Type::getInt64Ty(Ctx);
        Type *f32Ty = Type::getFloatTy(Ctx);
        Type *f64Ty = Type::getDoubleTy(Ctx);
        Type *i8PtrTy = PointerType::getUnqual(Ctx);

        auto declareHook = [&](StringRef Name, Type *Ret, ArrayRef<Type*> Args) {
            FunctionType *FT = FunctionType::get(Ret, Args, false);
            return M->getOrInsertFunction(Name, FT);
        };

        FunctionCallee tm_begin = declareHook("tm_begin", voidTy, {});
        FunctionCallee tm_end = declareHook("tm_end", voidTy, {});
        FunctionCallee tm_read_i1 = declareHook("tm_read_i1", i8Ty, {i8PtrTy});
        FunctionCallee tm_read_i2 = declareHook("tm_read_i2", i16Ty, {i8PtrTy});
        FunctionCallee tm_read_i4 = declareHook("tm_read_i4", i32Ty, {i8PtrTy});
        FunctionCallee tm_read_i8 = declareHook("tm_read_i8", i64Ty, {i8PtrTy});
        FunctionCallee tm_read_f4 = declareHook("tm_read_f4", f32Ty, {i8PtrTy});
        FunctionCallee tm_read_f8 = declareHook("tm_read_f8", f64Ty, {i8PtrTy});
        FunctionCallee tm_read_ptr = declareHook("tm_read_ptr", i8PtrTy, {i8PtrTy});
        FunctionCallee tm_read_z = declareHook("tm_read_z", i8PtrTy, {i8PtrTy, i64Ty});
        FunctionCallee tm_write_i1 = declareHook("tm_write_i1", voidTy, {i8PtrTy, i8Ty});
        FunctionCallee tm_write_i2 = declareHook("tm_write_i2", voidTy, {i8PtrTy, i16Ty});
        FunctionCallee tm_write_i4 = declareHook("tm_write_i4", voidTy, {i8PtrTy, i32Ty});
        FunctionCallee tm_write_i8 = declareHook("tm_write_i8", voidTy, {i8PtrTy, i64Ty});
        FunctionCallee tm_write_f4 = declareHook("tm_write_f4", voidTy, {i8PtrTy, f32Ty});
        FunctionCallee tm_write_f8 = declareHook("tm_write_f8", voidTy, {i8PtrTy, f64Ty});
        FunctionCallee tm_write_ptr = declareHook("tm_write_ptr", voidTy, {i8PtrTy, i8PtrTy});
        FunctionCallee tm_write_z = declareHook("tm_write_z", voidTy, {i8PtrTy, i8PtrTy, i64Ty});
        FunctionCallee tm_memset = declareHook("tm_memset", voidTy, {i8PtrTy, i8Ty, i64Ty});

#ifdef INCLUDE_NESTED_COUNTER
        GlobalVariable *CounterGV = M->getGlobalVariable("tm_nested_call_counter");
        if (!CounterGV) {
            CounterGV = new GlobalVariable(
                *M,
                i32Ty,
                false,
                GlobalValue::ExternalLinkage,
                nullptr,
                "tm_nested_call_counter"
            );
            CounterGV->setThreadLocal(true);
        }
#else
        GlobalVariable *CounterGV = nullptr;
#endif

#ifdef INCLUDE_SETJMP
        GlobalVariable *JmpBufGV = M->getGlobalVariable("tm_jmpbuf");
        if (!JmpBufGV) {
            ArrayType *BufTy = ArrayType::get(i8Ty, 256);
            JmpBufGV = new GlobalVariable(
                *M,
                BufTy,
                false,
                GlobalValue::ExternalLinkage,
                nullptr,
                "tm_jmpbuf"
            );
            JmpBufGV->setThreadLocal(true);
        }
#endif

        BasicBlock &Entry = F.getEntryBlock();
        Instruction *SplitPt = &*Entry.getFirstInsertionPt();
        BasicBlock *ContBB = Entry.splitBasicBlock(SplitPt, "cont");

        Entry.getTerminator()->eraseFromParent();

        IRBuilder<> Builder(&Entry);
        Builder.CreateCall(tm_begin, {});
        Builder.CreateBr(ContBB);

        IRBuilder<> ContBuilder(&*ContBB->begin());

        SmallVector<Instruction*, 16> ToErase;
        for (auto &BB : F) {
            for (auto InstIt = BB.begin(); InstIt != BB.end(); ) {
                Instruction *I = &*InstIt++;
                IRBuilder<> Builder(I);

                if (auto *Call = dyn_cast<CallInst>(I)) {
                    if (Function *Callee = Call->getCalledFunction()) {
                        StringRef Name = Callee->getName();

                        if (Name.find("llvm.memset") == 0) {
                            Value *Dest = Call->getArgOperand(0);
                            Value *Val = Call->getArgOperand(1);
                            Value *Len = Call->getArgOperand(2);
                            Value *Len64 = Builder.CreateZExtOrTrunc(Len, i64Ty);
                            Builder.CreateCall(tm_memset, {Dest, Val, Len64});
                            ToErase.push_back(Call);
                            continue;
                        }

                        if (Name.find("llvm.memcpy") == 0 || Name.find("llvm.memmove") == 0) {
                            Value *Dst = Call->getArgOperand(0);
                            Value *Src = Call->getArgOperand(1);
                            Value *Len = Call->getArgOperand(2);
                            Value *Len64 = Builder.CreateZExtOrTrunc(Len, i64Ty);
                            const Value *DstBase = getBaseObject(Dst);
                            const Value *SrcBase = getBaseObject(Src);
                            Value *DstCast = Builder.CreateBitCast(Dst, i8PtrTy);
                            Value *SrcCast = Builder.CreateBitCast(Src, i8PtrTy);

                            if (TMValues.count(DstBase)) {
                                Builder.CreateCall(tm_write_z, {DstCast, SrcCast, Len64});
                                ToErase.push_back(Call);
                                continue;
                            }
                            if (TMValues.count(SrcBase)) {
                                Value *Buffer = Builder.CreateCall(tm_read_z, {SrcCast, Len64});
                                Builder.CreateMemCpy(Dst, MaybeAlign(1), Buffer, MaybeAlign(1), Len);
                                ToErase.push_back(Call);
                                continue;
                            }
                        }
                    }
                }

                if (auto *Load = dyn_cast<LoadInst>(I)) {
                    const Value *Base = getBaseObject(Load->getPointerOperand());
                    if (!TMValues.count(Base))
                        continue;

                    Value *PtrCast = Builder.CreateBitCast(Load->getPointerOperand(), i8PtrTy);
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
                } else if (auto *Store = dyn_cast<StoreInst>(I)) {
                    const Value *Base = getBaseObject(Store->getPointerOperand());
                    if (!TMValues.count(Base))
                        continue;

                    Value *PtrCast = Builder.CreateBitCast(Store->getPointerOperand(), i8PtrTy);
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
                else if (auto *MI = dyn_cast<MemSetInst>(I)) {
                    const Value *Base = getBaseObject(MI->getDest());
                    if (!TMValues.count(Base))
                        continue;

                    Value *Dest = Builder.CreateBitCast(MI->getDest(), i8PtrTy);
                    Value *Len = Builder.CreateZExtOrTrunc(MI->getLength(), i64Ty);
                    Value *Val = Builder.CreateIntCast(MI->getValue(), i8Ty, false);
                    Builder.CreateCall(tm_memset, {Dest, Val, Len});
                    ToErase.push_back(MI);
                }
                else if (auto *MT = dyn_cast<MemTransferInst>(I)) {
                    const Value *DstBase = getBaseObject(MT->getDest());
                    const Value *SrcBase = getBaseObject(MT->getSource());
                    Value *Len = Builder.CreateZExtOrTrunc(MT->getLength(), i64Ty);
                    Value *DstCast = Builder.CreateBitCast(MT->getDest(), i8PtrTy);
                    Value *SrcCast = Builder.CreateBitCast(MT->getSource(), i8PtrTy);

                    if (TMValues.count(DstBase)) {
                        Builder.CreateCall(tm_write_z, {DstCast, SrcCast, Len});
                        ToErase.push_back(MT);
                    } else if (TMValues.count(SrcBase)) {
                        Value *Buffer = Builder.CreateCall(tm_read_z, {SrcCast, Len});
                        Builder.CreateMemCpy(MT->getDest(), MaybeAlign(1), Buffer, MaybeAlign(1), MT->getLength());
                        ToErase.push_back(MT);
                    }
                }
            }
        }

        for (auto &BB : F) {
            if (auto *Ret = dyn_cast<ReturnInst>(BB.getTerminator())) {
                IRBuilder<> RetBuilder(Ret);
                RetBuilder.CreateCall(tm_end, {});
            }
        }

        for (Instruction *I : ToErase) {
            I->eraseFromParent();
        }

        return PreservedAnalyses::none();
    }

private:
    bool hasAnnotation(Function &F, StringRef Annotation) const {
        Module *M = F.getParent();
        GlobalVariable *GA = M->getNamedGlobal("llvm.global.annotations");
        if (!GA)
            return false;

        auto *CA = dyn_cast<ConstantArray>(GA->getInitializer());
        if (!CA)
            return false;

        for (auto &Op : CA->operands()) {
            auto *CS = dyn_cast<ConstantStruct>(Op);
            if (!CS || CS->getNumOperands() < 2)
                continue;

            auto *AnnotatedVal = CS->getOperand(0)->stripPointerCasts();
            auto *AnnotatedFunc = dyn_cast<Function>(AnnotatedVal);
            if (!AnnotatedFunc || AnnotatedFunc != &F)
                continue;

            auto *AnnoVal = CS->getOperand(1)->stripPointerCasts();
            auto *GV = dyn_cast<GlobalVariable>(AnnoVal);
            if (!GV)
                continue;

            auto *Init = dyn_cast<ConstantDataSequential>(GV->getInitializer());
            if (!Init || !Init->isCString())
                continue;

            if (Init->getAsCString() == Annotation)
                return true;
        }
        return false;
    }

    void collectTMGlobals(Module &M, SmallPtrSetImpl<const Value*> &Out) const {
        GlobalVariable *GA = M.getNamedGlobal("llvm.global.annotations");
        if (!GA)
            return;

        auto *CA = dyn_cast<ConstantArray>(GA->getInitializer());
        if (!CA)
            return;

        for (auto &Op : CA->operands()) {
            auto *CS = dyn_cast<ConstantStruct>(Op);
            if (!CS || CS->getNumOperands() < 2)
                continue;

            Value *Val = CS->getOperand(0)->stripPointerCasts();
            auto *AnnoVal = CS->getOperand(1)->stripPointerCasts();
            auto *GV = dyn_cast<GlobalVariable>(AnnoVal);
            if (!GV)
                continue;

            auto *Init = dyn_cast<ConstantDataSequential>(GV->getInitializer());
            if (!Init || !Init->isCString())
                continue;

            if (Init->getAsCString() == "tm")
                Out.insert(Val);
        }
    }

const Value *getBaseObject(const Value *Ptr) const {
        const Value *Result = Ptr;
        for (int i = 0; i < 10 && Result; i++) {
            Result = Result->stripPointerCasts();
            if (const auto *GEP = dyn_cast<const GetElementPtrInst>(Result)) {
                Result = GEP->getPointerOperand();
            } else if (const auto *GEP = dyn_cast<const GEPOperator>(Result)) {
                Result = GEP->getPointerOperand();
            } else {
                break;
            }
        }
        return Result ? Result : Ptr;
    }

 	static bool isRequired() { return true; } // forces the pass to execute
};

} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return {LLVM_PLUGIN_API_VERSION, "TMInstrumentPass", LLVM_VERSION_STRING,
            [](PassBuilder &PB) {
                // Register module-level pass that also runs function instrumentation
                PB.registerPipelineParsingCallback(
                    [](StringRef Name, ModulePassManager &MPM,
                       ArrayRef<PassBuilder::PipelineElement>) {
                        if (Name == "tm-instrument") {
                            MPM.addPass(TMGlobalInitPass());
                            MPM.addPass(createModuleToFunctionPassAdaptor(TMInstrumentPass()));
                            return true;
                        }
                        return false;
                    });
            }};
}
