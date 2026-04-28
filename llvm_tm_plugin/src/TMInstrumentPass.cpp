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

// Helper to collect "tm"-annotated global variables
static void collectTMSymbols(Module &M, SmallVectorImpl<std::pair<GlobalVariable*, StringRef>> &Symbols) {
    GlobalVariable *GA = M.getNamedGlobal("llvm.global.annotations");
    if (!GA) return;

    auto *CA = dyn_cast<ConstantArray>(GA->getInitializer());
    if (!CA) return;

    for (auto &Op : CA->operands()) {
        auto *CS = dyn_cast<ConstantStruct>(Op);
        if (!CS || CS->getNumOperands() < 2) continue;

        Value *Val = CS->getOperand(0)->stripPointerCasts();
        auto *AnnoVal = CS->getOperand(1)->stripPointerCasts();
        auto *GV = dyn_cast<GlobalVariable>(AnnoVal);
        if (!GV) continue;

        auto *Init = dyn_cast<ConstantDataSequential>(GV->getInitializer());
        if (!Init || !Init->isCString()) continue;

        if (Init->getAsCString() == "tm") {
            auto *TMGV = dyn_cast<GlobalVariable>(Val->stripPointerCasts());
            if (TMGV) {
                Symbols.push_back({TMGV, TMGV->getName()});
            }
        }
    }
}

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

        // Collect "tm"-annotated globals for symbol table
        SmallVector<std::pair<GlobalVariable*, StringRef>, 16> TMSymbols;
        collectTMSymbols(M, TMSymbols);

        // If we have TM symbols, create symbol tables
        if (!TMSymbols.empty()) {
            IntegerType *Int32Ty = Type::getInt32Ty(Ctx);
            PointerType *CharPtrTy = PointerType::getUnqual(Ctx);
            PointerType *VoidPtrTy = PointerType::getUnqual(Ctx);

            // Create names array (array of char* pointers)
            SmallVector<Constant*, 16> namePtrs;
            SmallVector<Constant*, 16> addrPtrs;
            for (auto &Sym : TMSymbols) {
                GlobalVariable *GV = Sym.first;
                StringRef name = Sym.second;
                
                // Create name string global
                GlobalVariable *nameGV = new GlobalVariable(
                    M, 
                    ArrayType::get(Type::getInt8Ty(Ctx), name.size() + 1),
                    false,
                    GlobalValue::PrivateLinkage,
                    ConstantDataArray::getString(Ctx, name, true),
                    Twine("tm_symbol_name_") + name
                );
                nameGV->setUnnamedAddr(GlobalValue::UnnamedAddr::Local);
                
                // Get element 0 of the string array
                Constant *namePtr = ConstantExpr::getInBoundsGetElementPtr(
                    Type::getInt8Ty(Ctx), nameGV, 
                    {ConstantInt::get(Int32Ty, 0)}
                );
                namePtrs.push_back(ConstantExpr::getBitCast(namePtr, CharPtrTy));
                
                // Address pointer
                addrPtrs.push_back(ConstantExpr::getBitCast(GV, VoidPtrTy));
            }

            // Create symbol count first (so it can be used in array sizes)
            new GlobalVariable(
                M, Int32Ty, true, GlobalValue::ExternalLinkage,
                ConstantInt::get(Int32Ty, TMSymbols.size()), "tm_symbol_count"
            );

            // Create symbol names table
            ArrayType *NamesArrTy = ArrayType::get(CharPtrTy, TMSymbols.size());
            Constant *namesInit = ConstantArray::get(NamesArrTy, namePtrs);
            new GlobalVariable(
                M, NamesArrTy, false, GlobalValue::ExternalLinkage,
                namesInit, "tm_symbol_names"
            );
            
            // Create symbol addresses table  
            ArrayType *AddrsArrTy = ArrayType::get(VoidPtrTy, TMSymbols.size());
            Constant *addrsInit = ConstantArray::get(AddrsArrTy, addrPtrs);
            new GlobalVariable(
                M, AddrsArrTy, false, GlobalValue::ExternalLinkage,
                addrsInit, "tm_symbol_addresses"
            );
        }

        bool modified = false;

        // Add tm_init at the start of main
        if (Function *MainFn = M.getFunction("main")) {
            BasicBlock &Entry = MainFn->getEntryBlock();
            IRBuilder<> Builder(&Entry, Entry.begin());
            Builder.CreateCall(tm_init, {});
            // Also add tm_init_thread at start of main
            Builder.CreateCall(tm_init_thread, {});

            // Add tm_exit before each return in main
            for (auto &BB : *MainFn) {
                if (auto *Ret = dyn_cast<ReturnInst>(BB.getTerminator())) {
                    IRBuilder<> RetBuilder(Ret);
                    RetBuilder.CreateCall(tm_exit_thread, {});
                    RetBuilder.CreateCall(tm_exit, {});
                }
            }
        }

        for (auto &F : M) {
            if (F.isDeclaration()) continue;
            // Skip main - it's handled separately above
            if (F.getName() == "main") continue;

            // tm_init_thread/tm_exit_thread go to THREAD ENTRY POINTS:
            // - NOT annotated as "transaction" AND
            // - (uses TM globals OR calls transaction functions)
            bool isTxFunc = hasTransactionAnnotation(F);
            bool hasTM = hasTMGlobals(F);
            bool callsTx = callsTransactionFunctions(F, M);
            
            // Thread entry = uses TM but is NOT a transaction implementation
            if (hasTM && !isTxFunc) {
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
            // Alternative: functions that call transaction functions but aren't transactions themselves
            // (this catches worker_thread which calls transfer)
            else if (callsTx && !isTxFunc) {
                BasicBlock &Entry = F.getEntryBlock();
                // Only add if not already have it
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

    bool callsTransactionFunctions(Function &F, Module &M) {
        for (auto &BB : F) {
            for (auto &I : BB) {
                if (auto *Call = dyn_cast<CallInst>(&I)) {
                    if (Function *Callee = Call->getCalledFunction()) {
                        if (hasTransactionAnnotation(*Callee))
                            return true;
                    }
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
        FunctionCallee tm_setjmp = declareHook("tm_setjmp", i32Ty, {});
        // Declare sigsetjmp for direct use in instrumentation
        FunctionCallee sigsetjmpFn = declareHook("sigsetjmp", i32Ty, {i8PtrTy, i32Ty});
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
        // Runtime function to get symbol ID from address
        FunctionCallee tm_get_symbol_id = declareHook("tm_get_symbol_id", i32Ty, {i8PtrTy});

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

        GlobalVariable *JmpBufGV = M->getGlobalVariable("tm_jmpbuf");
        if (!JmpBufGV) {
            ArrayType *JmpBufTy = ArrayType::get(i8Ty, 256);
            JmpBufGV = new GlobalVariable(
                *M,
                JmpBufTy,
                false,
                GlobalValue::ExternalLinkage,
                nullptr,
                "tm_jmpbuf"
            );
            JmpBufGV->setThreadLocal(true);
        }

#ifndef DISABLE_SETJMP
        GlobalVariable *JmpRetGV = M->getGlobalVariable("tm_jmpbuf_ret");
        if (!JmpRetGV) {
            JmpRetGV = new GlobalVariable(
                *M,
                i32Ty,
                false,
                GlobalValue::ExternalLinkage,
                nullptr,
                "tm_jmpbuf_ret"
            );
            JmpRetGV->setThreadLocal(true);
        }
#endif

        GlobalVariable *ThreadReadyGV = M->getGlobalVariable("tm_thread_ready");
        if (!ThreadReadyGV) {
            ThreadReadyGV = new GlobalVariable(
                *M,
                i8Ty,
                false,
                GlobalValue::ExternalLinkage,
                nullptr,
                "tm_thread_ready"
            );
            ThreadReadyGV->setThreadLocal(true);
        }

        BasicBlock &Entry = F.getEntryBlock();
        Instruction *SplitPt = &*Entry.getFirstInsertionPt();
        BasicBlock *ContBB = Entry.splitBasicBlock(SplitPt, "cont");

        Entry.getTerminator()->eraseFromParent();

        IRBuilder<> Builder(&Entry);
        Value *CounterVal = Builder.CreateLoad(i32Ty, CounterGV, "counter");
        Value *IsOuter = Builder.CreateICmpEQ(CounterVal, ConstantInt::get(i32Ty, 0), "is_outer");
#ifndef DISABLE_SETJMP
        Value *JmpRetVal = Builder.CreateLoad(i32Ty, JmpRetGV, "jmpret");
        Value *IsRetry = Builder.CreateICmpNE(JmpRetVal, ConstantInt::get(i32Ty, 0), "is_retry");
        IsOuter = Builder.CreateOr(IsOuter, IsRetry, "is_outer");
#endif

        BasicBlock *OuterBB = BasicBlock::Create(Ctx, "outer", &F, ContBB);
        BasicBlock *NestedBB = BasicBlock::Create(Ctx, "nested", &F, ContBB);

        Builder.CreateCondBr(IsOuter, OuterBB, NestedBB);

IRBuilder<> OuterBuilder(OuterBB);
#ifndef DISABLE_SETJMP
        // Get pointer to tm_jmpbuf global
        Value *JmpBufPtr = OuterBuilder.CreateBitCast(JmpBufGV, PointerType::getUnqual(Ctx), "jmpbuf_ptr");
        // Call sigsetjmp(tm_jmpbuf, 0) directly - this sets up the jump buffer
        // On first call returns 0, on longjmp returns 1
        Value *SetjmpRet = OuterBuilder.CreateCall(sigsetjmpFn, {JmpBufPtr, ConstantInt::get(i32Ty, 0)}, "setjmp_ret");
        OuterBuilder.CreateStore(SetjmpRet, JmpRetGV);
        // Always call tm_begin() - both on first pass and on retry (longjmp)
        OuterBuilder.CreateStore(ConstantInt::get(i32Ty, 1), CounterGV);
        OuterBuilder.CreateCall(tm_begin, {});
        OuterBuilder.CreateBr(ContBB);
#else
        OuterBuilder.CreateStore(ConstantInt::get(i32Ty, 1), CounterGV);
        OuterBuilder.CreateCall(tm_begin, {});
        OuterBuilder.CreateBr(ContBB);
#endif

IRBuilder<> NestedBuilder(NestedBB);
        Value *IncCounter = NestedBuilder.CreateAdd(CounterVal, ConstantInt::get(i32Ty, 1), "counter_inc");
        NestedBuilder.CreateStore(IncCounter, CounterGV);
        // NESTED path: NO tm_begin() - only the outermost transaction calls tm_begin()
        NestedBuilder.CreateBr(ContBB);

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
                            const Value *DstBase = getBaseObject(Dest);
                            if (!TMValues.count(DstBase))
                                continue;
                            Value *DestCast = Builder.CreateBitCast(Dest, i8PtrTy);
                            Value *Val = Call->getArgOperand(1);
                            Value *Len = Call->getArgOperand(2);
                            Value *Len64 = Builder.CreateZExtOrTrunc(Len, i64Ty);
                            Builder.CreateCall(tm_memset, {DestCast, Val, Len64});
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
                    bool IsLoadTM = TMValues.count(Base);

                    if (!IsLoadTM) {
                        if (auto *AnnotCall = dyn_cast<CallInst>(Load->getPointerOperand())) {
                            if (Function *Callee = AnnotCall->getCalledFunction()) {
                                if (Callee->getName().find("llvm.ptr.annotation") == 0) {
                                    if (AnnotCall->getNumOperands() > 1) {
                                        const Value *AnnotPtr = AnnotCall->getArgOperand(0);
                                        const Value *AnnotBase = getBaseObject(AnnotPtr);
                                        if (TMValues.count(AnnotBase)) {
                                            IsLoadTM = true;
                                        } else {
                                            Value *Trace = const_cast<Value*>(AnnotPtr);
                                            for (int j = 0; j < 30 && Trace && !IsLoadTM; j++) {
                                                Trace = Trace->stripPointerCasts();
                                                if (auto *Call = dyn_cast<CallInst>(Trace)) {
                                                    if (Function *Fn = Call->getCalledFunction()) {
                                                        StringRef Name = Fn->getName();
                                                        if (Name.find("llvm.ptr.annotation") == 0) {
                                                            if (Call->getNumOperands() > 1) {
                                                                Trace = Call->getArgOperand(0);
                                                                continue;
                                                            }
                                                        }
                                                        if (Name.find("vector") != StringRef::npos ||
                                                            Name.find("operator[]") != StringRef::npos ||
                                                            Name.find("at()") != StringRef::npos ||
                                                            Name.find("front()") != StringRef::npos ||
                                                            Name.find("back()") != StringRef::npos ||
                                                            Name.find("data()") != StringRef::npos ||
                                                            Name == "_ZNSt3__16vector") {
                                                            IsLoadTM = true;
                                                            break;
                                                        }
                                                    }
                                                }
                                                if (auto *GEP = dyn_cast<GEPOperator>(Trace)) {
                                                    Trace = const_cast<Value*>(GEP->getPointerOperand());
                                                    continue;
                                                }
                                                if (auto *Inst = dyn_cast<Instruction>(Trace)) {
                                                    if (Inst->getNumOperands() > 0) {
                                                        Trace = Inst->getOperand(0);
                                                        continue;
                                                    }
                                                }
                                                break;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    if (!IsLoadTM) {
                        if (!Base) {
                            Value *Check = Load->getPointerOperand();
                            for (int j = 0; j < 30 && Check && !IsLoadTM; j++) {
                                Check = Check->stripPointerCasts();
                                if (isa<AllocaInst>(Check)) {
                                    IsLoadTM = true;
                                    break;
                                }
                                if (auto *Call = dyn_cast<CallInst>(Check)) {
                                    if (Function *Callee = Call->getCalledFunction()) {
                                        StringRef Name = Callee->getName();
                                        if (Name.find("malloc") != StringRef::npos ||
                                            Name.find("alloc") != StringRef::npos ||
                                            Name.find("vector") != StringRef::npos ||
                                            Name.find("new") != StringRef::npos ||
                                            Name == "malloc" ||
                                            Name == "_Znwm" ||
                                            Name == "_Znam") {
                                            IsLoadTM = true;
                                            break;
                                        }
                                    }
                                }
                                if (auto *GEP = dyn_cast<GEPOperator>(Check)) {
                                    Check = const_cast<Value*>(GEP->getPointerOperand());
                                    continue;
                                }
                                if (auto *Inst = dyn_cast<Instruction>(Check)) {
                                    if (Inst->getNumOperands() > 0) {
                                        Check = Inst->getOperand(0);
                                    } else {
                                        break;
                                    }
                                } else {
                                    break;
                                }
                            }
                        }
                    }

                    if (!IsLoadTM)
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
                    Value *StorePtrOp = Store->getPointerOperand();
                    bool ProcessedAnnotation = false;
                    bool IsTM = false;

                    // Trace pointer to find annotation calls
                    Value *TracePtr = StorePtrOp;
                    for (int trace = 0; trace < 15; trace++) {
                        if (auto *AnnotCall = dyn_cast<CallInst>(TracePtr)) {
                            if (Function *Callee = AnnotCall->getCalledFunction()) {
                                if (Callee->getName().find("llvm.ptr.annotation") == 0) {
                                    if (AnnotCall->getNumOperands() > 1) {
                                        Value *AnnotPtr = AnnotCall->getArgOperand(0);
                                        const Value *AnnotBase = getBaseObject(AnnotPtr);
                                        if (TMValues.count(AnnotBase)) {
                                            IsTM = true;
                                            ProcessedAnnotation = true;

                                            Value *PtrCast = Builder.CreateBitCast(AnnotPtr, i8PtrTy);
                                            Value *Val = Store->getValueOperand();
                                            Type *ValTy = Val->getType();
                                            if (ValTy->isIntegerTy(8)) {
                                                Builder.CreateCall(tm_write_i1, {PtrCast, Val});
                                            } else if (ValTy->isIntegerTy(16)) {
                                                Builder.CreateCall(tm_write_i2, {PtrCast, Val});
                                            } else if (ValTy->isIntegerTy(32)) {
                                                Builder.CreateCall(tm_write_i4, {PtrCast, Val});
                                            } else if (ValTy->isIntegerTy(64)) {
                                                Builder.CreateCall(tm_write_i8, {PtrCast, Val});
                                            } else if (ValTy->isFloatTy()) {
                                                Builder.CreateCall(tm_write_f4, {PtrCast, Val});
                                            } else if (ValTy->isDoubleTy()) {
                                                Builder.CreateCall(tm_write_f8, {PtrCast, Val});
                                            } else if (ValTy->isPointerTy()) {
                                                Value *ValCast = Builder.CreateBitCast(Val, i8PtrTy);
                                                Builder.CreateCall(tm_write_ptr, {PtrCast, ValCast});
                                            }
                                            ToErase.push_back(Store);
                                            break;
                                        }
                                    }
                                }
                            }
                            break;
                        }
                        TracePtr = TracePtr->stripPointerCasts();
                        if (auto *Inst = dyn_cast<Instruction>(TracePtr)) {
                            if (Inst->getNumOperands() > 0) {
                                TracePtr = Inst->getOperand(0);
                            } else {
                                break;
                            }
                        } else {
                            break;
                        }
                    }

                    if (ProcessedAnnotation)
                        continue;

                    const Value *Base = getBaseObject(StorePtrOp);

                    // In transaction functions, treat all heap/vector data as TM
                    if (TMValues.count(Base)) {
                        IsTM = true;
                    } else if (!isa<GlobalValue>(Base) && !isa<GlobalValue>(StorePtrOp)) {
                        // Not from global - trace from heap/vector/malloc
                        Value *Check = StorePtrOp;
                        for (int j = 0; j < 30 && Check && !IsTM; j++) {
                            Check = Check->stripPointerCasts();
                            if (isa<AllocaInst>(Check)) {
                                IsTM = true;
                                break;
                            }
                            if (auto *Call = dyn_cast<CallInst>(Check)) {
                                if (Function *Callee = Call->getCalledFunction()) {
                                    StringRef Name = Callee->getName();
                                    if (Name.find("new") != StringRef::npos ||
                                        Name.find("alloc") != StringRef::npos ||
                                        Name.find("vector") != StringRef::npos) {
                                        IsTM = true;
                                        break;
                                    }
                                }
                            }
                            if (LoadInst *LI = dyn_cast<LoadInst>(Check)) {
                                Check = LI->getPointerOperand();
                                continue;
                            }
                            if (auto *GEP = dyn_cast<GEPOperator>(Check)) {
                                Check = const_cast<Value*>(GEP->getPointerOperand());
                                continue;
                            }
                            if (auto *Inst = dyn_cast<Instruction>(Check)) {
                                if (Inst->getNumOperands() > 0) {
                                    Check = Inst->getOperand(0);
                                    continue;
                                }
                            }
                            break;
                        }
                    }

                    if (!IsTM)
                        continue;

                    Value *PtrCast = Builder.CreateBitCast(StorePtrOp, i8PtrTy);
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

        // Handle return instructions - add counter decrement at all return points
        for (auto &BB : F) {
            if (auto *Ret = dyn_cast<ReturnInst>(BB.getTerminator())) {
                BasicBlock *RetBB = Ret->getParent();
                
                // Get the return value if it exists
                Value *RetVal = nullptr;
                if (Ret->getNumOperands() > 0) {
                    RetVal = Ret->getOperand(0);
                }

                BasicBlock *OuterEndBB = BasicBlock::Create(Ctx, "outer_end", &F, RetBB);
                BasicBlock *NestedEndBB = BasicBlock::Create(Ctx, "nested_end", &F, RetBB);
                BasicBlock *CleanupBB = BasicBlock::Create(Ctx, "cleanup", &F, RetBB);

                Ret->eraseFromParent();

                IRBuilder<> EndCheckBuilder(&*RetBB, RetBB->end());
                Value *CounterAtEnd = EndCheckBuilder.CreateLoad(i32Ty, CounterGV, "counter_at_end");
                Value *IsOuterAtEnd = EndCheckBuilder.CreateICmpEQ(CounterAtEnd, ConstantInt::get(i32Ty, 1), "is_outer_at_end");
                EndCheckBuilder.CreateCondBr(IsOuterAtEnd, OuterEndBB, NestedEndBB);

                IRBuilder<> OuterEndBuilder(OuterEndBB);
#ifndef DISABLE_SETJMP
                OuterEndBuilder.CreateCall(tm_end, {});
#endif
                OuterEndBuilder.CreateBr(CleanupBB);

                IRBuilder<> NestedEndBuilder(NestedEndBB);
                NestedEndBuilder.CreateBr(CleanupBB);

                IRBuilder<> CleanupBuilder(CleanupBB);
                Value *DecCounter = CleanupBuilder.CreateAdd(CounterAtEnd, ConstantInt::get(i32Ty, -1), "counter_dec");
                CleanupBuilder.CreateStore(DecCounter, CounterGV);
                if (RetVal) {
                    CleanupBuilder.CreateRet(RetVal);
                } else {
                    CleanupBuilder.CreateRetVoid();
                }
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
