// TMAccessTracePass.cpp
// Standalone LLVM pass plugin: injects a per-access trace event
// (tm_trace(type, addr, width, value)) before every TM transactional access
// hook call (tm_read_* / tm_write_*) in already-instrumented IR.
//
// Usage:
//   opt -load-pass-plugin=libTMAccessTrace.so -passes="tm-access-trace" in.bc -o out.bc
//
// Decoupled from libTMInstrument: it traces any instrumented module without
// re-running instrumentation. It reuses:
//   - tm_access_hooks.hpp  : classify tm_read_*/tm_write_* calls (shared)
//   - tm_runtime_hooks.hpp : declareHook/emitHookCall + the tm_trace hook
// The emitted trace matches the existing --emit-tm-trace format, consumed by
// backends/tm_impl/common/tm_trace_runtime.cpp (type codes: read=0, write=1).

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Plugins/PassPlugin.h>
#include <llvm/Support/raw_ostream.h>

#include <vector>

#include "tm_access_hooks.hpp"
#include "tm_runtime_hooks.hpp"

using namespace llvm;

namespace
{

class TMAccessTracePass : public PassInfoMixin<TMAccessTracePass>
{
	// Convert a write's value operand to a 64-bit trace value.
	static Value *toI64(IRBuilder<> &B, Value *V)
	{
		Type *T = V->getType();
		auto *i64 = Type::getInt64Ty(B.getContext());
		if (T->isPointerTy())
			return B.CreatePtrToInt(V, i64);
		if (T->isFloatTy()) {
			Value *Bits = B.CreateBitCast(V, Type::getInt32Ty(B.getContext()));
			return B.CreateZExt(Bits, i64);
		}
		if (T->isDoubleTy())
			return B.CreateBitCast(V, i64);
		if (T->isIntegerTy())
			return B.CreateZExtOrTrunc(V, i64);
		return ConstantInt::get(i64, 0); // unsupported: trace addr only
	}

	// True if the instruction immediately before Acc is already a tm_trace
	// hook call (idempotency guard so re-running the pass does not double-trace).
	static bool alreadyTraced(CallBase *Acc)
	{
		BasicBlock *BB = Acc->getParent();
		if (BB->begin() == Acc->getIterator())
			return false;
		Instruction *Prev = &*std::prev(Acc->getIterator());
		auto *C = dyn_cast<CallInst>(Prev);
		if (!C)
			return false;
		auto *LI = dyn_cast<LoadInst>(C->getCalledOperand());
		if (!LI)
			return false;
		auto *GV = dyn_cast<GlobalVariable>(LI->getPointerOperand());
		return GV && GV->getName() == "tm_trace";
	}

public:
	PreservedAnalyses run(Module &M, ModuleAnalysisManager &)
	{
		LLVMContext &Ctx = M.getContext();
		const DataLayout &DL = M.getDataLayout();
		auto *i32 = Type::getInt32Ty(Ctx);
		auto *i64 = Type::getInt64Ty(Ctx);
		auto *i8Ptr = PointerType::getUnqual(Ctx);
		TMRuntimeHook traceHook = declareHook(M,
		                                      "tm_trace",
		                                      Type::getVoidTy(Ctx),
		                                      {i32, i8Ptr, i64, i64});

		unsigned traced = 0;
		for (auto &F : M) {
			if (F.isDeclaration())
				continue;
			SmallVector<CallBase *, 32> accesses;
			for (auto &BB : F)
				for (auto &I : BB)
					if (auto *CB = dyn_cast<CallBase>(&I))
						if (tmacc::classify(*CB))
							accesses.push_back(CB);
			for (CallBase *CB : accesses) {
				if (alreadyTraced(CB))
					continue;
				tmacc::Info A = *tmacc::classify(*CB);
				unsigned W = A.widthBytes ? A.widthBytes : (unsigned)DL.getPointerSize();
				IRBuilder<> B(CB);
				Value *typeC = ConstantInt::get(i32,
				                                A.kind == tmacc::Kind::Read ? 0u : 1u);
				Value *addr = B.CreateBitCast(A.addr, i8Ptr);
				Value *width = ConstantInt::get(i64, W);
				Value *val = A.value ? toI64(B, A.value) : ConstantInt::get(i64, 0);
				emitHookCall(B, traceHook, {typeC, addr, width, val});
				++traced;
			}
		}
		if (traced)
			outs() << "tm-access-trace: instrumented " << traced << " accesses\n";
		return traced ? PreservedAnalyses::none() : PreservedAnalyses::all();
	}
	static bool isRequired() { return true; }
};

} // anonymous namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo()
{
	return {LLVM_PLUGIN_API_VERSION,
	        "TMAccessTracePass",
	        LLVM_VERSION_STRING,
	        [](PassBuilder &PB) {
		        PB.registerPipelineParsingCallback(
		            [](StringRef Name,
		               ModulePassManager &MPM,
		               ArrayRef<PassBuilder::PipelineElement>) {
			            if (Name == "tm-access-trace") {
				            MPM.addPass(TMAccessTracePass());
				            return true;
			            }
			            return false;
		            });
	        }};
}
