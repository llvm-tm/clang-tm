// tm_runtime_hooks.hpp
// Shared type for all TM runtime hook function-pointer variables.
// Each hook is an extern "C" function-pointer variable (e.g. void (*tm_begin)())
// that the runtime can swap at runtime for phase-based TM, thread-count gating, etc.
// The plugin emits indirect calls (load pointer + call through it) so that hook
// swaps take effect immediately for both instrumented and explicit API code.
#ifndef TM_RUNTIME_HOOKS_HPP
#define TM_RUNTIME_HOOKS_HPP

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

using namespace llvm;

// ── TMRuntimeHook ─────────────────────────────────────────────────
// A single TM runtime function-pointer variable.
// Stores the GlobalVariable* (the extern hook symbol) and the
// FunctionType* (for constructing indirect call IR).
struct TMRuntimeHook {
	GlobalVariable *gv = nullptr;
	FunctionType  *fnTy = nullptr;

	explicit operator bool() const { return gv && fnTy; }

	// Convenience accessors (mimic FunctionCallee interface)
	FunctionType *getFunctionType() const { return fnTy; }
	GlobalVariable *getGlobalVariable() const { return gv; }
};

// ── Helpers ───────────────────────────────────────────────────────
// Get or create a GlobalVariable for the given hook name.
// If the user's bitcode already declares `extern void (*tm_begin)()`,
// we find that existing GlobalVariable. Otherwise we create one.
static GlobalVariable *getOrCreateHookGV(Module &M, StringRef Name) {
	if (auto *GV = M.getGlobalVariable(Name))
		return GV;
	return new GlobalVariable(M, PointerType::getUnqual(M.getContext()), false,
	                          GlobalValue::ExternalLinkage, nullptr, Name);
}

// Declare a single hook variable and return the TMRuntimeHook wrapper.
static TMRuntimeHook declareHook(Module &M, StringRef Name,
                                 Type *RetTy, ArrayRef<Type *> ParamTys) {
	TMRuntimeHook H;
	H.fnTy = FunctionType::get(RetTy, ParamTys, false);
	H.gv   = getOrCreateHookGV(M, Name);
	return H;
}

// Emit an indirect call through a TMRuntimeHook.
// Generates: %ptr = load ptr, ptr @hook_name
//            call <fnTy> %ptr(args...)
static CallInst *emitHookCall(IRBuilder<> &B, const TMRuntimeHook &Hook,
                              ArrayRef<Value *> Args = {}, const Twine &Name = "") {
	Value *FnPtr = B.CreateLoad(PointerType::getUnqual(B.getContext()),
	                            Hook.gv, "hook." + Hook.gv->getName());
	return B.CreateCall(Hook.fnTy, FnPtr, Args, Name);
}

// Emit an indirect invoke through a TMRuntimeHook.
static InvokeInst *emitHookInvoke(IRBuilder<> &B, const TMRuntimeHook &Hook,
                                  ArrayRef<Value *> Args,
                                  BasicBlock *Normal, BasicBlock *Unwind,
                                  const Twine &Name = "") {
	Value *FnPtr = B.CreateLoad(PointerType::getUnqual(B.getContext()),
	                            Hook.gv, "hook." + Hook.gv->getName());
	return B.CreateInvoke(Hook.fnTy, FnPtr, Normal, Unwind, Args, Name);
}

// Create an InvokeInst directly (when no IRBuilder is available, e.g. in
// helper lambdas that create new instructions).
static InvokeInst *createHookInvoke(LLVMContext &Ctx, const TMRuntimeHook &Hook,
                                    ArrayRef<Value *> Args,
                                    BasicBlock *Normal, BasicBlock *Unwind,
                                    const Twine &Name = "") {
	auto *FnPtrTy = PointerType::getUnqual(Ctx);
	auto *FnPtr   = new LoadInst(FnPtrTy, Hook.gv, "hook." + Hook.gv->getName(),
	                             false, Unwind->getFirstNonPHI());
	return InvokeInst::Create(Hook.fnTy, FnPtr, Normal, Unwind, Args, {}, Name);
}

// ── TMRuntimeHooks ────────────────────────────────────────────────
// Collection of all TM runtime hook variables.
struct TMRuntimeHooks {
	TMRuntimeHook read_i1, read_i2, read_i4, read_i8;
	TMRuntimeHook read_i16, read_i32, read_i64;
	TMRuntimeHook read_f4, read_f8, read_ptr;
	TMRuntimeHook write_i1, write_i2, write_i4, write_i8;
	TMRuntimeHook write_i16, write_i32, write_i64;
	TMRuntimeHook write_f4, write_f8, write_ptr;
	TMRuntimeHook begin, end;
	TMRuntimeHook set_jmpbuf, get_env, sigsetjmp;
	TMRuntimeHook init, exit_fn;
	TMRuntimeHook init_thread, exit_thread;
	TMRuntimeHook serialize_lock, serialize_unlock;
	TMRuntimeHook malloc_fn, calloc_fn, realloc_fn, free_fn;
	TMRuntimeHook memset_fn;
	TMRuntimeHook enqueue_fn, wait_prev_tx_fn;
	TMRuntimeHook get_thread_state;
	TMRuntimeHook trace_fn;

	bool valid() { return read_i4 && write_i4; }

	static TMRuntimeHooks declareAll(Module &M,
	                                 LLVMContext &Ctx,
	                                 StringRef SetjmpFunc = "sigsetjmp")
	{
		TMRuntimeHooks h;
		auto *voidTy = Type::getVoidTy(Ctx);
		auto *i8Ty = Type::getInt8Ty(Ctx);
		auto *i16Ty = Type::getInt16Ty(Ctx);
		auto *i32Ty = Type::getInt32Ty(Ctx);
		auto *i64Ty = Type::getInt64Ty(Ctx);
		auto *f32Ty = Type::getFloatTy(Ctx);
		auto *f64Ty = Type::getDoubleTy(Ctx);
		auto *i8PtrTy = PointerType::getUnqual(Ctx);

		auto hook = [&](StringRef N, Type *R, ArrayRef<Type *> A) {
			return declareHook(M, N, R, A);
		};

		h.init = hook("tm_init", voidTy, {});
		h.exit_fn = hook("tm_exit", voidTy, {});
		h.init_thread = hook("tm_init_thread", voidTy, {});
		h.exit_thread = hook("tm_exit_thread", voidTy, {});
		h.begin = hook("tm_begin", voidTy, {});
		h.end = hook("tm_end", voidTy, {});
		h.set_jmpbuf = hook("tm_set_jmpbuf", voidTy, {i8PtrTy});
		h.get_env = hook("tm_get_env", i8PtrTy, {});

		// sigsetjmp needs ReturnsTwice attribute set on the CallInst
		h.sigsetjmp = hook(SetjmpFunc, i32Ty, {i8PtrTy, i32Ty});

		h.serialize_lock = hook("tm_serialize_lock", voidTy, {});
		h.serialize_unlock = hook("tm_serialize_unlock", voidTy, {});
		h.malloc_fn = hook("tm_malloc", i8PtrTy, {i64Ty});
		h.calloc_fn = hook("tm_calloc", i8PtrTy, {i64Ty, i64Ty});
		h.realloc_fn = hook("tm_realloc", i8PtrTy, {i8PtrTy, i64Ty});
		h.free_fn = hook("tm_free", voidTy, {i8PtrTy});
		h.memset_fn = hook("tm_memset", voidTy, {i8PtrTy, i8Ty, i64Ty});

		h.read_i1 = hook("tm_read_i1", i8Ty, {i8PtrTy});
		h.read_i2 = hook("tm_read_i2", i16Ty, {i8PtrTy});
		h.read_i4 = hook("tm_read_i4", i32Ty, {i8PtrTy});
		h.read_i8 = hook("tm_read_i8", i64Ty, {i8PtrTy});
		h.read_i16 = hook("tm_read_i16", voidTy, {i8PtrTy, i8PtrTy});
		h.read_i32 = hook("tm_read_i32", voidTy, {i8PtrTy, i8PtrTy});
		h.read_i64 = hook("tm_read_i64", voidTy, {i8PtrTy, i8PtrTy});
		h.read_f4 = hook("tm_read_f4", f32Ty, {i8PtrTy});
		h.read_f8 = hook("tm_read_f8", f64Ty, {i8PtrTy});
		h.read_ptr = hook("tm_read_ptr", i8PtrTy, {i8PtrTy});

		h.enqueue_fn = hook("tm_enqueue", voidTy, {i8PtrTy, i8PtrTy});
		h.wait_prev_tx_fn = hook("tm_wait_prev_tx", voidTy, {});
		h.get_thread_state = hook("tm_get_thread_state", i8PtrTy, {});

		h.write_i1 = hook("tm_write_i1", voidTy, {i8PtrTy, i8Ty});
		h.write_i2 = hook("tm_write_i2", voidTy, {i8PtrTy, i16Ty});
		h.write_i4 = hook("tm_write_i4", voidTy, {i8PtrTy, i32Ty});
		h.write_i8 = hook("tm_write_i8", voidTy, {i8PtrTy, i64Ty});
		h.write_i16 = hook("tm_write_i16", voidTy, {i8PtrTy, i8PtrTy});
		h.write_i32 = hook("tm_write_i32", voidTy, {i8PtrTy, i8PtrTy});
		h.write_i64 = hook("tm_write_i64", voidTy, {i8PtrTy, i8PtrTy});
		h.write_f4 = hook("tm_write_f4", voidTy, {i8PtrTy, f32Ty});
		h.write_f8 = hook("tm_write_f8", voidTy, {i8PtrTy, f64Ty});
		h.write_ptr = hook("tm_write_ptr", voidTy, {i8PtrTy, i8PtrTy});

		// Optional trace hook (only injected if --emit-tm-trace is set)
		h.trace_fn = hook("tm_trace", voidTy, {i32Ty, i8PtrTy, i64Ty, i64Ty});
		// Params: (type_code: R=0/W=1, addr, width, value)

		return h;
	}
};

// Forward declaration for --emit-tm-trace (defined in TMInstrumentPass.cpp)
extern llvm::cl::opt<bool> EmitTrace;

// Helpers to emit tm_read/tm_write calls through hook variables.
// These replace the duplicated switch-on-type chains.
static Value *emitTMRead(IRBuilder<> &B, Value *Ptr, Type *Ty, const TMRuntimeHooks &H)
{
	auto &Ctx = B.getContext();
	auto *i32Ty = Type::getInt32Ty(Ctx);
	auto *i64Ty = Type::getInt64Ty(Ctx);
	auto *i8PtrTy = PointerType::getUnqual(Ctx);
	Value *PC = B.CreateBitCast(Ptr, i8PtrTy);

	// Optional trace emission
	if (EmitTrace) {
		Value *TypeCode = ConstantInt::get(i32Ty, 0); // 0 = read
		Value *WidthVal = ConstantInt::get(i64Ty, Ty->getScalarSizeInBits() / 8);
		Value *ValPlaceholder = ConstantInt::get(i64Ty, 0);
		emitHookCall(B, H.trace_fn, {TypeCode, PC, WidthVal, ValPlaceholder});
	}

	if (Ty->isIntegerTy(1)) {
		Value *V = emitHookCall(B, H.read_i1, {PC});
		return B.CreateTrunc(V, Ty);
	}
	if (Ty->isIntegerTy(8))
		return emitHookCall(B, H.read_i1, {PC});
	if (Ty->isIntegerTy(16))
		return emitHookCall(B, H.read_i2, {PC});
	if (Ty->isIntegerTy(32))
		return emitHookCall(B, H.read_i4, {PC});
	if (Ty->isIntegerTy(64))
		return emitHookCall(B, H.read_i8, {PC});

	// Integer types wider than 64 bits (i128, i256, i512): delegate to
	// wide runtime hooks that handle multiple tm_read_i8 calls internally.
	if (Ty->isIntegerTy()) {
		unsigned BitWidth = Ty->getIntegerBitWidth();
		TMRuntimeHook Hook;
		if (BitWidth <= 128)
			Hook = H.read_i16;
		else if (BitWidth <= 256)
			Hook = H.read_i32;
		else
			Hook = H.read_i64;
		AllocaInst *Alloca = B.CreateAlloca(Ty);
		Value *OutBuf = B.CreateBitCast(Alloca, i8PtrTy);
		emitHookCall(B, Hook, {PC, OutBuf});
		return B.CreateLoad(Ty, Alloca);
	}

	if (Ty->isFloatTy())
		return emitHookCall(B, H.read_f4, {PC});
	if (Ty->isDoubleTy())
		return emitHookCall(B, H.read_f8, {PC});
	if (Ty->isPointerTy()) {
		Value *V = emitHookCall(B, H.read_ptr, {PC});
		return B.CreateBitCast(V, Ty);
	}

	// Fixed-length vector types: decompose into per-element reads.
	if (auto *FVT = dyn_cast<FixedVectorType>(Ty)) {
		Type *ElemTy = FVT->getElementType();
		unsigned NumElems = FVT->getNumElements();
		Value *Result = PoisonValue::get(FVT);
		unsigned ElemSizeBytes = ElemTy->getScalarSizeInBits() / 8;
		if (ElemSizeBytes == 0)
			ElemSizeBytes = 1;
		for (unsigned i = 0; i < NumElems; i++) {
			Value *ElemPtr = B.CreateGEP(Type::getInt8Ty(Ctx),
			                             PC,
			                             ConstantInt::get(i64Ty, i * ElemSizeBytes));
			Value *ElemVal = emitTMRead(B, ElemPtr, ElemTy, H);
			if (!ElemVal)
				return nullptr;
			Result = B.CreateInsertElement(Result, ElemVal, i);
		}
		return Result;
	}

	// Generic fallback for any other type (x86_fp80, fp128, half, bfloat, etc.):
	// decompose into byte-level i1 reads and combine via bitcast.
	Module *M = B.GetInsertBlock()->getModule();
	const DataLayout &DL = M->getDataLayout();
	unsigned NumBytes = DL.getTypeStoreSize(Ty);
	unsigned BitWidth = NumBytes * 8;
	Type *IntTy = Type::getIntNTy(Ctx, BitWidth);
	Value *IntResult = ConstantInt::get(IntTy, 0);
	for (unsigned i = 0; i < NumBytes; i++) {
		Value *BytePtr = B.CreateGEP(Type::getInt8Ty(Ctx),
		                             PC,
		                             ConstantInt::get(i64Ty, i));
		Value *ByteVal = emitHookCall(B, H.read_i1, {BytePtr});
		Value *ExtByte = B.CreateZExt(ByteVal, IntTy);
		if (i > 0)
			ExtByte = B.CreateShl(ExtByte, ConstantInt::get(IntTy, i * 8));
		IntResult = B.CreateOr(IntResult, ExtByte);
	}
	return B.CreateBitCast(IntResult, Ty);
}

static bool emitTMWrite(IRBuilder<> &B, Value *Ptr, Value *Val, const TMRuntimeHooks &H)
{
	auto &Ctx = B.getContext();
	auto *i32Ty = Type::getInt32Ty(Ctx);
	auto *i64Ty = Type::getInt64Ty(Ctx);
	auto *i8PtrTy = PointerType::getUnqual(Ctx);
	Value *PC = B.CreateBitCast(Ptr, i8PtrTy);
	Type *Ty = Val->getType();

	// Optional trace emission before write
	if (EmitTrace) {
		Value *TypeCode = ConstantInt::get(i32Ty, 1); // 1 = write
		Value *WidthVal = ConstantInt::get(i64Ty, Ty->getScalarSizeInBits() / 8);
		Value *ValWide = B.CreateZExtOrBitCast(Val, i64Ty);
		emitHookCall(B, H.trace_fn, {TypeCode, PC, WidthVal, ValWide});
	}

	if (Ty->isIntegerTy(1)) {
		emitHookCall(B, H.write_i1, {PC, B.CreateZExt(Val, Type::getInt8Ty(Ctx))});
		return true;
	}
	if (Ty->isIntegerTy(8)) {
		emitHookCall(B, H.write_i1, {PC, Val});
		return true;
	}
	if (Ty->isIntegerTy(16)) {
		emitHookCall(B, H.write_i2, {PC, Val});
		return true;
	}
	if (Ty->isIntegerTy(32)) {
		emitHookCall(B, H.write_i4, {PC, Val});
		return true;
	}
	if (Ty->isIntegerTy(64)) {
		emitHookCall(B, H.write_i8, {PC, Val});
		return true;
	}

	// Integer types wider than 64 bits (i128, i256, i512): delegate to
	// wide runtime hooks that handle multiple tm_write_i8 calls internally.
	if (Ty->isIntegerTy()) {
		unsigned BitWidth = Ty->getIntegerBitWidth();
		TMRuntimeHook Hook;
		if (BitWidth <= 128)
			Hook = H.write_i16;
		else if (BitWidth <= 256)
			Hook = H.write_i32;
		else
			Hook = H.write_i64;
		AllocaInst *Alloca = B.CreateAlloca(Ty);
		B.CreateStore(Val, Alloca);
		Value *ValBuf = B.CreateBitCast(Alloca, i8PtrTy);
		emitHookCall(B, Hook, {PC, ValBuf});
		return true;
	}

	if (Ty->isFloatTy()) {
		emitHookCall(B, H.write_f4, {PC, Val});
		return true;
	}
	if (Ty->isDoubleTy()) {
		emitHookCall(B, H.write_f8, {PC, Val});
		return true;
	}
	if (Ty->isPointerTy()) {
		auto *i8PtrTy2 = PointerType::getUnqual(Ctx);
		Value *VC = B.CreateBitCast(Val, i8PtrTy2);
		emitHookCall(B, H.write_ptr, {PC, VC});
		return true;
	}

	// Fixed-length vector types: decompose into per-element writes.
	if (auto *FVT = dyn_cast<FixedVectorType>(Ty)) {
		Type *ElemTy = FVT->getElementType();
		unsigned NumElems = FVT->getNumElements();
		unsigned ElemSizeBytes = ElemTy->getScalarSizeInBits() / 8;
		if (ElemSizeBytes == 0)
			ElemSizeBytes = 1;
		for (unsigned i = 0; i < NumElems; i++) {
			Value *ElemVal = B.CreateExtractElement(Val, i);
			Value *ElemPtr = B.CreateGEP(Type::getInt8Ty(Ctx),
			                             PC,
			                             ConstantInt::get(i64Ty, i * ElemSizeBytes));
			if (!emitTMWrite(B, ElemPtr, ElemVal, H))
				return false;
		}
		return true;
	}

	// Generic fallback for any other type (x86_fp80, fp128, half, bfloat, etc.):
	// bitcast to integer, then decompose into byte-level i1 writes.
	Module *M = B.GetInsertBlock()->getModule();
	const DataLayout &DL = M->getDataLayout();
	unsigned NumBytes = DL.getTypeStoreSize(Ty);
	unsigned BitWidth = NumBytes * 8;
	Type *IntTy = Type::getIntNTy(Ctx, BitWidth);
	Value *IntVal = B.CreateBitCast(Val, IntTy);
	for (unsigned i = 0; i < NumBytes; i++) {
		Value *ByteVal = B.CreateTrunc(B.CreateLShr(IntVal,
		                                            ConstantInt::get(IntTy, i * 8)),
		                               Type::getInt8Ty(Ctx));
		Value *BytePtr = B.CreateGEP(Type::getInt8Ty(Ctx),
		                             PC,
		                             ConstantInt::get(i64Ty, i));
		emitHookCall(B, H.write_i1, {BytePtr, ByteVal});
	}
	return true;
}

#endif // TM_RUNTIME_HOOKS_HPP
