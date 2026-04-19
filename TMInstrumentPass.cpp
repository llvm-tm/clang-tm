// TMInstrumentPass.cpp

#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"

#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"

using namespace llvm;

namespace {

class TMInstrumentPass : public PassInfoMixin<TMInstrumentPass>
{
public:
	PreservedAnalyses run(Function &F, FunctionAnalysisManager &)
	{
		if (!hasAnnotation(F, "transaction"))
        	return PreservedAnalyses::all();

		SmallPtrSet<const Value*, 8> TMvars;
		Module *M = F.getParent();
		LLVMContext &Ctx = M->getContext();
		std::vector<Instruction*> toErase;

		collectTMvars(*M, TMvars);

		// Common types
		Type *voidTy = Type::getVoidTy(Ctx);
		Type *i8Ty   = Type::getInt8Ty(Ctx);
		Type *i16Ty  = Type::getInt16Ty(Ctx);
		Type *i32Ty  = Type::getInt32Ty(Ctx);
		Type *i64Ty  = Type::getInt64Ty(Ctx);
		Type *f32Ty  = Type::getFloatTy(Ctx);
		Type *f64Ty  = Type::getDoubleTy(Ctx);

		Type *i8PtrTy = PointerType::get(Ctx, 0);

		// Declare runtime hooks:
		// void tm_begin()
		// void tm_end()
		FunctionCallee tm_begin = M->getOrInsertFunction(
			"tm_begin",
			FunctionType::get(voidTy, {}, false)
		);
		
		FunctionCallee tm_end = M->getOrInsertFunction(
			"tm_end",
			FunctionType::get(voidTy, {}, false)
		);

		errs() << "Running TMInstrumentPass on function: " << F.getName() << "\n";
		
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

#ifndef DISABLE_STM_SETJMP
		GlobalVariable *JmpBufGV = M->getGlobalVariable("tm_jmpbuf");
		if (!JmpBufGV) {
			ArrayType *BufTy = ArrayType::get(Type::getInt8Ty(Ctx), 256); // placeholder size
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
#endif /* DISABLE_STM_SETJMP */

		BasicBlock &Entry = F.getEntryBlock();
		Instruction *SplitPt = &*Entry.getFirstInsertionPt();
		BasicBlock *ContBB = Entry.splitBasicBlock(SplitPt, "cont");

		// Remove the auto-generated branch from split
		Entry.getTerminator()->eraseFromParent();

		// Insert logic into Entry
		IRBuilder<> Builder(&Entry);

		// counter++
		Value *Counter = Builder.CreateLoad(i32Ty, CounterGV);
		Value *Inc = Builder.CreateAdd(Counter, Builder.getInt32(1));
		Builder.CreateStore(Inc, CounterGV);

		// if (counter == 1)
		Value *IsOuterTX = Builder.CreateICmpEQ(Inc, Builder.getInt32(1));

		// Create setjmp block
		BasicBlock *SetJmpBB = BasicBlock::Create(Ctx, "setjmp", &F);

		// Proper conditional branch
		Builder.CreateCondBr(IsOuterTX, SetJmpBB, ContBB);

		// Build setjmp block
		IRBuilder<> SJBuilder(SetJmpBB);

#ifndef DISABLE_STM_SETJMP
		SJBuilder.CreateBitCast(JmpBufGV, i8PtrTy);
		Value *BufPtr = SJBuilder.CreateBitCast(JmpBufGV, i8PtrTy);
		Function *SetJmp = Intrinsic::getDeclarationIfExists(M, Intrinsic::eh_sjlj_setjmp);
		if (!SetJmp) {
			FunctionType *SetJmpTy = FunctionType::get(i32Ty, {i8PtrTy}, false );
			FunctionCallee SetJmpCallee = M->getOrInsertFunction("setjmp", SetJmpTy);
			SetJmp = cast<Function>(SetJmpCallee.getCallee());
		}
		Value *Res = SJBuilder.CreateCall(SetJmp, {BufPtr});
		// reset counter to 1 (as flow can jump back from anywhere)
		SJBuilder.CreateStore( SJBuilder.getInt32(1), CounterGV);
#endif /* DISABLE_STM_SETJMP */

		SJBuilder.CreateCall(tm_begin, {});

		// Jump to continuation
		SJBuilder.CreateBr(ContBB);
		
		// Declare runtime hooks:
		// type tm_read(type* addr)
		// void tm_write(type* addr, type val)
		FunctionCallee tm_read_i1 = M->getOrInsertFunction(
			"tm_read_i1",
			FunctionType::get(i8Ty, {i8PtrTy}, false)
		);

		FunctionCallee tm_read_i2 = M->getOrInsertFunction(
			"tm_read_i2",
			FunctionType::get(i16Ty, {i8PtrTy}, false)
		);

		FunctionCallee tm_read_i4 = M->getOrInsertFunction(
			"tm_read_i4",
			FunctionType::get(i32Ty, {i8PtrTy}, false)
		);

		FunctionCallee tm_read_i8 = M->getOrInsertFunction(
			"tm_read_i8",
			FunctionType::get(i64Ty, {i8PtrTy}, false)
		);

		FunctionCallee tm_read_f4 = M->getOrInsertFunction(
			"tm_read_f4",
			FunctionType::get(f32Ty, {i8PtrTy}, false)
		);

		FunctionCallee tm_read_f8 = M->getOrInsertFunction(
			"tm_read_f8",
			FunctionType::get(f64Ty, {i8PtrTy}, false)
		);

		FunctionCallee tm_read_ptr = M->getOrInsertFunction(
			"tm_read_ptr",
			FunctionType::get(i8PtrTy, {i8PtrTy}, false)
		);

		FunctionCallee tm_read_z = M->getOrInsertFunction(
			"tm_read_z",
			FunctionType::get(i8PtrTy, {i8PtrTy, i32Ty}, false)
		);

		FunctionCallee tm_write_i1 = M->getOrInsertFunction(
			"tm_write_i1",
			FunctionType::get(voidTy, {i8PtrTy, i32Ty}, false)
		);

		FunctionCallee tm_write_i2 = M->getOrInsertFunction(
			"tm_write_i2",
			FunctionType::get(voidTy, {i8PtrTy, i16Ty}, false)
		);

		FunctionCallee tm_write_i4 = M->getOrInsertFunction(
			"tm_write_i4",
			FunctionType::get(voidTy, {i8PtrTy, i32Ty}, false)
		);

		FunctionCallee tm_write_i8 = M->getOrInsertFunction(
			"tm_write_i8",
			FunctionType::get(voidTy, {i8PtrTy, i64Ty}, false)
		);

		FunctionCallee tm_write_f4 = M->getOrInsertFunction(
			"tm_write_f4",
			FunctionType::get(voidTy, {i8PtrTy, f32Ty}, false)
		);

		FunctionCallee tm_write_f8 = M->getOrInsertFunction(
			"tm_write_f8",
			FunctionType::get(voidTy, {i8PtrTy, f64Ty}, false)
		);

		FunctionCallee tm_write_ptr = M->getOrInsertFunction(
			"tm_write_ptr",
			FunctionType::get(voidTy, {i8PtrTy, i8PtrTy}, false)
		);

		FunctionCallee tm_write_z = M->getOrInsertFunction(
			"tm_write_z",
			FunctionType::get(voidTy, {i8PtrTy, i8PtrTy, i32Ty}, false)
		);


		for (auto &BB : F) {
			for (auto &I : BB)
			{
				IRBuilder<> builder(&I);

				// errs() << "Instruction: " << I << "\n";

				// Checks if the transactional function is calling another function
				if (auto *call = dyn_cast<CallInst>(&I)) {
					Value *calleeVal = call->getCalledOperand()->stripPointerCasts();
				
					if (auto *callee = dyn_cast<Function>(calleeVal)) {
						errs() << "Call to: " << callee->getName() << "\n";
						if (callee->getName() == "tm_begin")
							continue;
						// if (callee->) // TODO: is memset or memcpy
						if (!hasAnnotation(*callee, "transaction"))
							errs() << "WARNING: " << F.getName() << " is calling non-transaction function " << callee->getName() << "\n";
					}
				}
				else if (auto *load = dyn_cast<LoadInst>(&I)) // --- LOAD instrumentation ---
				{
					const Value *base = getBaseObject(load->getPointerOperand());
					errs() << "Variable: " << getVarName(base) << "\n";
					if (!TMvars.count(base)) continue;

					Value *ptr = load->getPointerOperand();

					// Cast pointer to i8*
					Value *ptrCast = builder.CreateBitCast(ptr, i8PtrTy);

					// Call tm_read_i4
					Value *val;
					if (load->getType()->isIntegerTy(8)) {
						val = builder.CreateCall(tm_read_i1, {ptrCast});
					} else if (load->getType()->isIntegerTy(16)) {
						val = builder.CreateCall(tm_read_i2, {ptrCast});
					} else if (load->getType()->isIntegerTy(32)) {
						val = builder.CreateCall(tm_read_i4, {ptrCast});
					} else if (load->getType()->isIntegerTy(64)) {
						val = builder.CreateCall(tm_read_i8, {ptrCast});
					} else if (load->getType()->isFloatTy()) {
						val = builder.CreateCall(tm_read_f4, {ptrCast});
					} else if (load->getType()->isDoubleTy()) {
						val = builder.CreateCall(tm_read_f8, {ptrCast});
					} else if (load->getType()->isPointerTy()) {
						val = builder.CreateCall(tm_read_ptr, {ptrCast});
					}

					load->replaceAllUsesWith(val);
					// Cast result back to original type if needed
					// if (load->getType()->isIntegerTy(32)) {
					// 	load->replaceAllUsesWith(val);
					// } else {
					// 	// For non-i32, skip for now (safe minimal behavior)
					// 	continue;
					// }

					toErase.push_back(load);
				} 
				else if (auto *store = dyn_cast<StoreInst>(&I)) // --- STORE instrumentation ---
				{
					const Value *base = getBaseObject(store->getPointerOperand());
					errs() << "Variable: " << getVarName(base) << "\n";
					if (!TMvars.count(base)) continue;

					Value *ptr = store->getPointerOperand();
					Value *val = store->getValueOperand();

					// Only handle i32 for now
					// if (!val->getType()->isIntegerTy(32)) continue;
					// builder.CreateCall(tm_write, {ptrCast, val});
					Value *ptrCast = builder.CreateBitCast(ptr, i8PtrTy);
					if (val->getType()->isIntegerTy(8)) {
						builder.CreateCall(tm_write_i1, {ptrCast, val});
					} else if (val->getType()->isIntegerTy(16)) {
						builder.CreateCall(tm_write_i2, {ptrCast, val});
					} else if (val->getType()->isIntegerTy(32)) {
						builder.CreateCall(tm_write_i4, {ptrCast, val});
					} else if (val->getType()->isIntegerTy(64)) {
						builder.CreateCall(tm_write_i8, {ptrCast, val});
					} else if (val->getType()->isFloatTy()) {
						builder.CreateCall(tm_write_f4, {ptrCast, val});
					} else if (val->getType()->isDoubleTy()) {
						builder.CreateCall(tm_write_f8, {ptrCast, val});
					} else if (val->getType()->isPointerTy()) {
						builder.CreateCall(tm_write_ptr, {ptrCast, val});
					}

					toErase.push_back(store);
				}
				else if (auto *MI = dyn_cast<MemSetInst>(&I)) // memset
				{
					const Value *base = getBaseObject(MI->getDest());
					errs() << "Variable: " << getVarName(base) << "\n";
					if (!TMvars.count(base)) continue;

					Value *ptr = store->getPointerOperand();
					Value *val = store->getValueOperand();
					auto *len = MI->getLength();
					Value *constLen = dyn_cast<ConstantInt>(len);
					// Value *sz = ConstLen->getZExtValue();

					// Only handle i32 for now
					Value *ptrCast = builder.CreateBitCast(ptr, i8PtrTy);
					builder.CreateCall(tm_write_z, {ptrCast, val, constLen});
					toErase.push_back(MI);
				}
				else if (auto *MT = dyn_cast<MemTransferInst>(&I))
				{
					Value *dst = (Value*)getBaseObject(MT->getDest());
					Value *src = (Value*)getBaseObject(MT->getSource());
					errs() << "Variable: " << getVarName(dst) << "\n";
					errs() << "Variable: " << getVarName(src) << "\n";
					Value *len = MT->getLength();
					// auto *constLen = dyn_cast<ConstantInt>(len);
					// size_t sz = ConstLen->getZExtValue();
					Value *val;

					Value *dstPtrCast = builder.CreateBitCast(dst, i8PtrTy);
					Value *srcPtrCast = builder.CreateBitCast(src, i8PtrTy);
					
					if (!TMvars.count(src)) {
						val = builder.CreateCall(tm_read_z, {srcPtrCast, len
						});
						load->replaceAllUsesWith(val);
					}
					if (!TMvars.count(dst)) {
						if (!TMvars.count(src)) {
							builder.CreateCall(tm_write_z, {dstPtrCast, val, len});
						} else {
							builder.CreateCall(tm_write_z, {dstPtrCast, srcPtrCast, len});
						}
					}

					toErase.push_back(MT);
				}
			}
			errs() << "Test1 " << BB.getTerminator() << " \n";
			if (auto term = BB.getTerminator()) {
				if (auto *RI = dyn_cast<ReturnInst>(term)) {
					errs() << "Return of function: " << F.getName() << "\n";
					IRBuilder<> Builder(RI);
					Builder.CreateCall(tm_end, {});
					Value *Counter = Builder.CreateLoad(
						CounterGV->getValueType(), CounterGV);
	
					Value *Dec = Builder.CreateSub(Counter, Builder.getInt32(1));
					Builder.CreateStore(Dec, CounterGV);
			   }
			}
		   errs() << "Test2\n";
		}
		errs() << "Test3\n";
		// Remove original instructions
		for (Instruction *I : toErase) {
			I->eraseFromParent();
		}
		errs() << "Test4\n";

		return PreservedAnalyses::none();
	}

	bool hasAnnotation(Function &F, StringRef Annotation)
	{
		Module *M = F.getParent();

		GlobalVariable *GA = M->getNamedGlobal("llvm.global.annotations");
		if (!GA) return false;

		auto *CA = dyn_cast<ConstantArray>(GA->getInitializer());
		if (!CA) return false;

		for (auto &Op : CA->operands()) {
			auto *CS = dyn_cast<ConstantStruct>(Op);
			if (!CS || CS->getNumOperands() < 2)
				continue;

			// Get annotated function safely
			auto *AnnotatedVal = CS->getOperand(0)->stripPointerCasts();
			auto *AnnotatedFunc = dyn_cast<Function>(AnnotatedVal);
			if (!AnnotatedFunc || AnnotatedFunc != &F)
				continue;

			// Get annotation string safely
			auto *AnnoVal = CS->getOperand(1)->stripPointerCasts();
			auto *GV = dyn_cast<GlobalVariable>(AnnoVal);
			if (!GV) continue;

			auto *Init = dyn_cast<ConstantDataSequential>(GV->getInitializer());
			if (!Init || !Init->isCString()) continue;

			if (Init->getAsCString() == Annotation)
				return true;
		}

		return false;
	}

	void collectTMvars(Module &M, SmallPtrSetImpl<const Value*> &Out)
	{
		auto *GA = M.getNamedGlobal("llvm.global.annotations");
		if (!GA) return;

		auto *CA = dyn_cast<ConstantArray>(GA->getInitializer());
		if (!CA) return;

		for (auto &Op : CA->operands())
		{
			auto *CS = dyn_cast<ConstantStruct>(Op);
			if (!CS || CS->getNumOperands() < 2)
				continue;

			// Annotated value
			Value *Val = CS->getOperand(0)->stripPointerCasts();

			// Annotation string
			auto *AnnoVal = CS->getOperand(1)->stripPointerCasts();
			auto *GV = dyn_cast<GlobalVariable>(AnnoVal);
			if (!GV) continue;

			auto *Init = dyn_cast<ConstantDataSequential>(GV->getInitializer());
			if (!Init || !Init->isCString()) continue;

			if (Init->getAsCString() == "tm") {
				Out.insert(Val);
			}
		}
	}

	const Value *getBaseObject(const Value *Ptr) {
		Ptr = Ptr->stripPointerCasts();

		while (auto *GEP = dyn_cast<GetElementPtrInst>(Ptr)) {
			Ptr = GEP->getPointerOperand()->stripPointerCasts();
		}

		return Ptr;
	}

	std::string getVarName(const Value *V) {
		if (V->hasName())
			return V->getName().str();

		// Look for dbg.declare
		for (const User *U : V->users()) {
			if (const auto *DDI = dyn_cast<DbgDeclareInst>(U)) {
				if (auto *Var = DDI->getVariable()) {
					return Var->getName().str();
				}
			}
		}

    return "<unnamed>";
}

	static bool isRequired() { return true; } // forces the pass to execute
};

} // namespace

// --- Pass registration (LLVM 22 compatible) ---
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
	return {
		LLVM_PLUGIN_API_VERSION,
		"TMInstrumentPass",
		LLVM_VERSION_STRING,
		[](PassBuilder &PB) {
			PB.registerPipelineParsingCallback(
				[](StringRef Name,
				   FunctionPassManager &FPM,
				   ArrayRef<PassBuilder::PipelineElement>) {

					errs() << Name << "\n";
					if (Name == "tm-instrument") {
						FPM.addPass(TMInstrumentPass());
						return true;
					}
					return false;
				}
			);
		}
	};
}
