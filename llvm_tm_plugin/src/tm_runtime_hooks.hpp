// tm_runtime_hooks.hpp
// Shared type for all TM runtime hook function declarations.
// Eliminates the 14-parameter explosion through every instrumentation function.
#ifndef TM_RUNTIME_HOOKS_HPP
#define TM_RUNTIME_HOOKS_HPP

#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>

using namespace llvm;

struct TMRuntimeHooks {
    FunctionCallee read_i1, read_i2, read_i4, read_i8;
    FunctionCallee read_f4, read_f8, read_ptr;
    FunctionCallee write_i1, write_i2, write_i4, write_i8;
    FunctionCallee write_f4, write_f8, write_ptr;
    FunctionCallee begin, end;
    FunctionCallee set_jmpbuf, sigsetjmp;
    FunctionCallee init, exit_fn;
    FunctionCallee init_thread, exit_thread;
    FunctionCallee serialize_lock, serialize_unlock;
    FunctionCallee malloc_fn, free_fn;

    bool valid() { return read_i4.getCallee() && write_i4.getCallee(); }

    static TMRuntimeHooks declareAll(Module &M, LLVMContext &Ctx) {
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
            return M.getOrInsertFunction(N, FunctionType::get(R, A, false));
        };

        h.init         = hook("tm_init", voidTy, {});
        h.exit_fn      = hook("tm_exit", voidTy, {});
        h.init_thread  = hook("tm_init_thread", voidTy, {});
        h.exit_thread  = hook("tm_exit_thread", voidTy, {});
        h.begin        = hook("tm_begin", voidTy, {});
        h.end          = hook("tm_end", voidTy, {});
        h.set_jmpbuf   = hook("tm_set_jmpbuf", voidTy, {i8PtrTy});
        h.sigsetjmp    = hook("sigsetjmp", i32Ty, {i8PtrTy, i32Ty});
        h.serialize_lock   = hook("tm_serialize_lock", voidTy, {});
        h.serialize_unlock = hook("tm_serialize_unlock", voidTy, {});
        h.malloc_fn        = hook("tm_malloc", i8PtrTy, {i64Ty});
        h.free_fn          = hook("tm_free", voidTy, {i8PtrTy});

        h.read_i1  = hook("tm_read_i1", i8Ty, {i8PtrTy});
        h.read_i2  = hook("tm_read_i2", i16Ty, {i8PtrTy});
        h.read_i4  = hook("tm_read_i4", i32Ty, {i8PtrTy});
        h.read_i8  = hook("tm_read_i8", i64Ty, {i8PtrTy});
        h.read_f4  = hook("tm_read_f4", f32Ty, {i8PtrTy});
        h.read_f8  = hook("tm_read_f8", f64Ty, {i8PtrTy});
        h.read_ptr = hook("tm_read_ptr", i8PtrTy, {i8PtrTy});

        h.write_i1  = hook("tm_write_i1", voidTy, {i8PtrTy, i8Ty});
        h.write_i2  = hook("tm_write_i2", voidTy, {i8PtrTy, i16Ty});
        h.write_i4  = hook("tm_write_i4", voidTy, {i8PtrTy, i32Ty});
        h.write_i8  = hook("tm_write_i8", voidTy, {i8PtrTy, i64Ty});
        h.write_f4  = hook("tm_write_f4", voidTy, {i8PtrTy, f32Ty});
        h.write_f8  = hook("tm_write_f8", voidTy, {i8PtrTy, f64Ty});
        h.write_ptr = hook("tm_write_ptr", voidTy, {i8PtrTy, i8PtrTy});

        return h;
    }
};

// Helpers to emit tm_read/tm_write calls based on LLVM types.
// These replace the duplicated switch-on-type chains.
static CallInst *emitTMRead(IRBuilder<> &B, Value *Ptr, Type *Ty,
                            const TMRuntimeHooks &H) {
    auto *i8PtrTy = PointerType::getUnqual(B.getContext());
    Value *PC = B.CreateBitCast(Ptr, i8PtrTy);
    if (Ty->isIntegerTy(8))  return B.CreateCall(H.read_i1, {PC});
    if (Ty->isIntegerTy(16)) return B.CreateCall(H.read_i2, {PC});
    if (Ty->isIntegerTy(32)) return B.CreateCall(H.read_i4, {PC});
    if (Ty->isIntegerTy(64)) return B.CreateCall(H.read_i8, {PC});
    if (Ty->isFloatTy())     return B.CreateCall(H.read_f4, {PC});
    if (Ty->isDoubleTy())    return B.CreateCall(H.read_f8, {PC});
    if (Ty->isPointerTy()) {
        Value *V = B.CreateCall(H.read_ptr, {PC});
        return cast<CallInst>(B.CreateBitCast(V, Ty));
    }
    return nullptr;
}

static void emitTMWrite(IRBuilder<> &B, Value *Ptr, Value *Val,
                        const TMRuntimeHooks &H) {
    auto *i8PtrTy = PointerType::getUnqual(B.getContext());
    Value *PC = B.CreateBitCast(Ptr, i8PtrTy);
    Type *Ty = Val->getType();
    if (Ty->isIntegerTy(8))  { B.CreateCall(H.write_i1, {PC, Val}); return; }
    if (Ty->isIntegerTy(16)) { B.CreateCall(H.write_i2, {PC, Val}); return; }
    if (Ty->isIntegerTy(32)) { B.CreateCall(H.write_i4, {PC, Val}); return; }
    if (Ty->isIntegerTy(64)) { B.CreateCall(H.write_i8, {PC, Val}); return; }
    if (Ty->isFloatTy())     { B.CreateCall(H.write_f4, {PC, Val}); return; }
    if (Ty->isDoubleTy())    { B.CreateCall(H.write_f8, {PC, Val}); return; }
    if (Ty->isPointerTy()) {
        auto *i8PtrTy2 = PointerType::getUnqual(B.getContext());
        Value *VC = B.CreateBitCast(Val, i8PtrTy2);
        B.CreateCall(H.write_ptr, {PC, VC});
    }
}

#endif // TM_RUNTIME_HOOKS_HPP
