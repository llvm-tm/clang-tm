// TMInstrumentPass.cpp
// Main plugin file for Transactional Memory instrumentation
//
// WHY TWO PASSES:
// ================
// We use a two-pass approach because some operations are module-level (Pass 1)
// while others are function-level (Pass 2):
//
// PASS 1 (TMGlobalInitPass - Module-level):
//   - Runs ONCE per module before function passes
//   - Collects all "tm" and "transaction" annotated globals/functions
//   - Creates symbol tables for TM-annotated globals (module-level data)
//   - Instruments main() and thread entry points with init/exit calls
//   - These need to be done once, not per-function
//
// PASS 2 (TMInstrumentPass - Function-level):
//   - Runs on EACH function marked with "transaction" annotation
//   - Instruments transaction entry/exit (sigsetjmp, tm_begin, tm_end)
//   - Instruments loads/stores within transactions (tm_read/tm_write)
//   - Function-level pass allows us to analyze each transaction function
//     independently and modify its IR
//
// This separation is REQUIRED because:
//   - Module-level changes (symbol tables, main instrumentation) should
//     happen once, not be repeated for each function
//   - Function-level changes need to modify the function's own IR
//   - LLVM's pass manager runs function passes multiple times, so we can't
//     do module-level work there

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Type.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Plugins/PassPlugin.h>

#include "tm_annotation_utils.hpp"
#include "tm_call_graph.hpp"
#include "tm_debug.hpp"
#include "tm_local_vars.hpp"
#include "tm_method_instrumentation.hpp"
#include "tm_thread_guard.hpp"
#include "tm_thread_symbols.hpp"

using namespace llvm;

namespace
{

class TMGlobalInitPass : public PassInfoMixin<TMGlobalInitPass>
{
// PASS 1: Module-level instrumentation
//
// This pass runs ONCE per module and handles:
//   1. Collect all variables and functions annotated with "tm" and "transaction"
//   2. Look for thread entry point functions (e.g., pthread_create, std::thread)
//   3. Create symbol tables for TM-annotated globals (module-level data)
//   4. Instrument main() with tm_init at entry and tm_exit at return
//   5. Instrument thread entry points with tm_init_thread/tm_exit_thread
//   6. Use thread-local guard variable (tm_thread_ready) to prevent multiple calls
//
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &)
  {
    TM_DEBUG("TMGlobalInitPass: processing module %s", M.getName().str().c_str());
    
    LLVMContext &Ctx = M.getContext();
    Type *voidTy = Type::getVoidTy(Ctx);
    Type *i8Ty = Type::getInt8Ty(Ctx);

    auto declareHook = [&](StringRef Name, Type *Ret, ArrayRef<Type *> Args) {
      FunctionType *FT = FunctionType::get(Ret, Args, false);
      return M.getOrInsertFunction(Name, FT);
    };

    // Declare runtime hooks needed for module-level instrumentation
    FunctionCallee tm_init = declareHook("tm_init", voidTy, {});
    FunctionCallee tm_exit = declareHook("tm_exit", voidTy, {});
    FunctionCallee tm_init_thread = declareHook("tm_init_thread", voidTy, {});
    FunctionCallee tm_exit_thread = declareHook("tm_exit_thread", voidTy, {});

    // Declare read/write hooks for method instrumentation (Fix 1)
    Type *i16Ty = Type::getInt16Ty(Ctx);
    Type *i32Ty = Type::getInt32Ty(Ctx);
    Type *i64Ty = Type::getInt64Ty(Ctx);
    Type *f32Ty = Type::getFloatTy(Ctx);
    Type *f64Ty = Type::getDoubleTy(Ctx);
    Type *i8PtrTy = PointerType::getUnqual(Ctx);

    FunctionCallee tm_read_i1 = declareHook("tm_read_i1", i8Ty, {i8PtrTy});
    FunctionCallee tm_read_i2 = declareHook("tm_read_i2", i16Ty, {i8PtrTy});
    FunctionCallee tm_read_i4 = declareHook("tm_read_i4", i32Ty, {i8PtrTy});
    FunctionCallee tm_read_i8 = declareHook("tm_read_i8", i64Ty, {i8PtrTy});
    FunctionCallee tm_read_f4 = declareHook("tm_read_f4", f32Ty, {i8PtrTy});
    FunctionCallee tm_read_f8 = declareHook("tm_read_f8", f64Ty, {i8PtrTy});
    FunctionCallee tm_read_ptr = declareHook("tm_read_ptr", i8PtrTy, {i8PtrTy});
    FunctionCallee tm_write_i1 = declareHook("tm_write_i1", voidTy, {i8PtrTy, i8Ty});
    FunctionCallee tm_write_i2 = declareHook("tm_write_i2", voidTy, {i8PtrTy, i16Ty});
    FunctionCallee tm_write_i4 = declareHook("tm_write_i4", voidTy, {i8PtrTy, i32Ty});
    FunctionCallee tm_write_i8 = declareHook("tm_write_i8", voidTy, {i8PtrTy, i64Ty});
    FunctionCallee tm_write_f4 = declareHook("tm_write_f4", voidTy, {i8PtrTy, f32Ty});
    FunctionCallee tm_write_f8 = declareHook("tm_write_f8", voidTy, {i8PtrTy, f64Ty});
    FunctionCallee tm_write_ptr = declareHook("tm_write_ptr", voidTy, {i8PtrTy, i8PtrTy});

    // Create thread-ready guard variable (thread-local)
    // PURPOSE: Prevents multiple tm_init_thread/tm_exit_thread calls
    //          in the same thread (idempotency guard)
    GlobalVariable *ThreadReadyGV = M.getGlobalVariable("tm_thread_ready");
    if (!ThreadReadyGV) {
      ThreadReadyGV = new GlobalVariable(M, i8Ty, false,
                                       GlobalValue::ExternalLinkage,
                                       ConstantInt::get(i8Ty, 0),
                                       "tm_thread_ready");
      ThreadReadyGV->setThreadLocal(true);
      TM_DEBUG("Created thread_ready guard variable");
    }

    // ---- Collect "tm"-annotated globals ----
    // PURPOSE: Build symbol table so runtime knows about TM-annotated globals
    SmallVector<std::pair<GlobalVariable *, StringRef>, 16> TMSymbols;
    collectTMSymbols(M, TMSymbols);
    TM_DEBUG("Found %d TM-annotated symbols", (int)TMSymbols.size());

    // Create symbol tables for TM-annotated globals
    // PURPOSE: Runtime needs to know which globals are TM-annotated for
    //          initialization and tracking purposes
    IntegerType *Int32Ty = Type::getInt32Ty(Ctx);
    IntegerType *Int64Ty = Type::getInt64Ty(Ctx);
    PointerType *CharPtrTy = PointerType::getUnqual(Ctx);
    PointerType *VoidPtrTy = PointerType::getUnqual(Ctx);

    SmallVector<Constant *, 16> namePtrs;
    SmallVector<Constant *, 16> addrPtrs;
    SmallVector<Constant *, 16> sizesVals;

    const DataLayout &DL = M.getDataLayout();

    for (auto &Sym : TMSymbols) {
      GlobalVariable *GV = Sym.first;
      StringRef name = Sym.second;

      // Create a global string containing the symbol name
      GlobalVariable *nameGV = new GlobalVariable(M,
          ArrayType::get(Type::getInt8Ty(Ctx), name.size() + 1),
          false, GlobalValue::PrivateLinkage,
          ConstantDataArray::getString(Ctx, name, true),
          Twine("tm_symbol_name_") + name);
      nameGV->setDSOLocal(true);
      nameGV->setUnnamedAddr(GlobalValue::UnnamedAddr::Local);

      Constant *namePtr = ConstantExpr::getInBoundsGetElementPtr(
          Type::getInt8Ty(Ctx), nameGV, ConstantInt::get(Int32Ty, 0));
      namePtrs.push_back(ConstantExpr::getBitCast(namePtr, CharPtrTy));
      addrPtrs.push_back(ConstantExpr::getBitCast(GV, VoidPtrTy));

      // Compute the size of the global variable for persistence
      uint64_t size = DL.getTypeAllocSize(GV->getValueType());
      sizesVals.push_back(ConstantInt::get(Int64Ty, size));
    }

    // Create: int tm_symbol_count
    new GlobalVariable(M, Int32Ty, true, GlobalValue::ExternalLinkage,
                      ConstantInt::get(Int32Ty, TMSymbols.size()),
                      "tm_symbol_count");

    // Create: const char* tm_symbol_names[] (empty array if no symbols)
    ArrayType *NamesArrTy = ArrayType::get(CharPtrTy, TMSymbols.size());
    Constant *namesInit = ConstantArray::get(NamesArrTy, namePtrs);
    new GlobalVariable(M, NamesArrTy, false, GlobalValue::ExternalLinkage,
                      namesInit, "tm_symbol_names");

    // Create: void* tm_symbol_addresses[] (empty array if no symbols)
    ArrayType *AddrsArrTy = ArrayType::get(VoidPtrTy, TMSymbols.size());
    Constant *addrsInit = ConstantArray::get(AddrsArrTy, addrPtrs);
    new GlobalVariable(M, AddrsArrTy, false, GlobalValue::ExternalLinkage,
                      addrsInit, "tm_symbol_addresses");

    // Create: uint64_t tm_symbol_sizes[] for persistence runtime
    ArrayType *SizesArrTy = ArrayType::get(Int64Ty, TMSymbols.size());
    Constant *sizesInit = ConstantArray::get(SizesArrTy, sizesVals);
    new GlobalVariable(M, SizesArrTy, false, GlobalValue::ExternalLinkage,
                      sizesInit, "tm_symbol_sizes");

    // ---- Fix 1: Method call instrumentation ----
    // PURPOSE: Clone methods called on TM objects and redirect calls
    //          Must happen after symbol table creation (which just finished)
    tm_method_instrumentation::processMethodCalls(M, tm_read_i1, tm_read_i2, tm_read_i4, tm_read_i8,
                                                  tm_read_f4, tm_read_f8, tm_read_ptr,
                                                  tm_write_i1, tm_write_i2, tm_write_i4, tm_write_i8,
                                                  tm_write_f4, tm_write_f8, tm_write_ptr);

    bool modified = false;

    // ---- Fix 2: Explicit thread entry point detection ----
    // PURPOSE: Use explicit symbol list to detect thread entry points,
    //          then mark their transitive call closure as thread entry points.
    //          Fall back to heuristic if no explicit matches found.

    // Collect all functions that are explicitly thread entry points
    SmallPtrSet<Function *, 32> ExplicitThreadEntries;
    bool foundExplicitThread = false;

    for (auto &F : M) {
        if (F.isDeclaration()) continue;

        // Check if this function matches any thread entry symbol
        for (size_t i = 0; ThreadEntrySymbols[i] != nullptr; ++i) {
            if (F.getName() == ThreadEntrySymbols[i]) {
                ExplicitThreadEntries.insert(&F);
                foundExplicitThread = true;
                TM_DEBUG("Found explicit thread entry: %s", F.getName().str().c_str());
                break;
            }
        }
    }

    // ---- Instrument main() ----
    // PURPOSE: main() needs tm_init at entry (to initialize TM system)
    //          and tm_exit at all return points
    if (Function *MainFn = M.getFunction("main")) {
      TM_DEBUG("Instrumenting main()");
      BasicBlock &Entry = MainFn->getEntryBlock();
      IRBuilder<> Builder(&Entry, Entry.begin());
      Builder.CreateCall(tm_init, {});
      insertThreadInitWithGuard(Builder, tm_init_thread, ThreadReadyGV);

      // Collect all return instructions FIRST before modifying function
      // PURPOSE: Avoid iterator invalidation when we modify the function
      SmallVector<ReturnInst *, 4> MainReturns;
      for (auto &BB : *MainFn) {
        Instruction *Term = BB.getTerminator();
        if (Term && isa<ReturnInst>(Term)) {
          MainReturns.push_back(cast<ReturnInst>(Term));
        }
      }
      
      // Now process each return instruction
      for (auto *Ret : MainReturns) {
        if (!Ret) continue;
        IRBuilder<> RetBuilder(Ret);
        insertThreadExitWithGuard(RetBuilder, tm_exit_thread, ThreadReadyGV);
        RetBuilder.CreateCall(tm_exit, {});
      }
      modified = true;
    }

    // ---- Instrument thread entry points ----
    // PURPOSE: Functions marked with "thread" annotation or matching explicit
    //          thread entry symbols need tm_init_thread/tm_exit_thread.
    for (auto &F : M) {
      if (F.isDeclaration() || F.getName() == "main")
        continue;

      if (hasAnnotation(F, "transaction")) continue;

      bool shouldInstrument = hasAnnotation(F, "thread");
      if (!shouldInstrument) {
        shouldInstrument = ExplicitThreadEntries.count(&F);
      }

      if (shouldInstrument) {
        TM_DEBUG("Instrumenting thread entry point: %s", F.getName().str().c_str());
        BasicBlock &Entry = F.getEntryBlock();
        IRBuilder<> Builder(&Entry, Entry.begin());
        insertThreadInitWithGuard(Builder, tm_init_thread, ThreadReadyGV);

        // Collect return instructions FIRST before modifying function
        SmallVector<ReturnInst *, 4> ThreadReturns;
        for (auto &BB : F) {
          Instruction *Term = BB.getTerminator();
          if (Term && isa<ReturnInst>(Term)) {
            ThreadReturns.push_back(cast<ReturnInst>(Term));
          }
        }

        // Now process each return
        for (auto *Ret : ThreadReturns) {
          if (!Ret) continue;
          IRBuilder<> RetBuilder(Ret);
          insertThreadExitWithGuard(RetBuilder, tm_exit_thread, ThreadReadyGV);
        }
        modified = true;
      }
    }

    TM_DEBUG("TMGlobalInitPass: %s", modified ? "modified module" : "no changes");
    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }

  static bool isRequired() { return true; }
};

class TMInstrumentPass : public PassInfoMixin<TMInstrumentPass>
{
// PASS 2: Function-level instrumentation for transaction functions
//
// This pass runs on EACH function marked with "transaction" annotation.
// It handles:
//   1. Transaction entry: nested counter check, sigsetjmp + tm_begin for outer
//   2. Load/store instrumentation: inject tm_read/tm_write hooks
//   3. Transaction exit: decrement counter, tm_end for outermost
//   4. Local variable detection: don't instrument local (stack) variables
//
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM)
  {
    TM_DEBUG("TMInstrumentPass: processing function %s", F.getName().str().c_str());
    
    // Check if this is a transaction function
    // PURPOSE: Only instrument functions explicitly marked as transactions
    if (!hasAnnotation(F, "transaction")) {
      TM_DEBUG("%s is not a transaction function, skipping", F.getName().str().c_str());
      return PreservedAnalyses::all();
    }
    
    TM_DEBUG("Instrumenting transaction function: %s", F.getName().str().c_str());

    // Prevent inlining - we need to instrument transaction functions
    // PURPOSE: Inlining would lose the transaction boundaries we're adding
    F.addFnAttr(llvm::Attribute::NoInline);

    Module *M = F.getParent();
    LLVMContext &Ctx = M->getContext();

    // Type declarations for runtime hook functions
    Type *voidTy = Type::getVoidTy(Ctx);
    Type *i8Ty = Type::getInt8Ty(Ctx);
    Type *i16Ty = Type::getInt16Ty(Ctx);
    Type *i32Ty = Type::getInt32Ty(Ctx);
    Type *i64Ty = Type::getInt64Ty(Ctx);
    Type *f32Ty = Type::getFloatTy(Ctx);
    Type *f64Ty = Type::getDoubleTy(Ctx);
    Type *i8PtrTy = PointerType::getUnqual(Ctx);

    auto declareHook = [&](StringRef Name, Type *Ret, ArrayRef<Type *> Args) {
      FunctionType *FT = FunctionType::get(Ret, Args, false);
      return M->getOrInsertFunction(Name, FT);
    };

    // Declare transaction runtime hooks
    FunctionCallee tm_begin = declareHook("tm_begin", voidTy, {});
    FunctionCallee tm_end = declareHook("tm_end", voidTy, {});
    FunctionCallee tm_set_jmpbuf = declareHook("tm_set_jmpbuf", voidTy, {i8PtrTy});
    FunctionCallee sigsetjmpFn = declareHook("sigsetjmp", i32Ty, {i8PtrTy, i32Ty});
    FunctionCallee tm_read_i1 = declareHook("tm_read_i1", i8Ty, {i8PtrTy});
    FunctionCallee tm_read_i2 = declareHook("tm_read_i2", i16Ty, {i8PtrTy});
    FunctionCallee tm_read_i4 = declareHook("tm_read_i4", i32Ty, {i8PtrTy});
    FunctionCallee tm_read_i8 = declareHook("tm_read_i8", i64Ty, {i8PtrTy});
    FunctionCallee tm_read_f4 = declareHook("tm_read_f4", f32Ty, {i8PtrTy});
    FunctionCallee tm_read_f8 = declareHook("tm_read_f8", f64Ty, {i8PtrTy});
    FunctionCallee tm_read_ptr = declareHook("tm_read_ptr", i8PtrTy, {i8PtrTy});
    FunctionCallee tm_write_i1 = declareHook("tm_write_i1", voidTy, {i8PtrTy, i8Ty});
    FunctionCallee tm_write_i2 = declareHook("tm_write_i2", voidTy, {i8PtrTy, i16Ty});
    FunctionCallee tm_write_i4 = declareHook("tm_write_i4", voidTy, {i8PtrTy, i32Ty});
    FunctionCallee tm_write_i8 = declareHook("tm_write_i8", voidTy, {i8PtrTy, i64Ty});
    FunctionCallee tm_write_f4 = declareHook("tm_write_f4", voidTy, {i8PtrTy, f32Ty});
    FunctionCallee tm_write_f8 = declareHook("tm_write_f8", voidTy, {i8PtrTy, f64Ty});
    FunctionCallee tm_write_ptr = declareHook("tm_write_ptr", voidTy, {i8PtrTy, i8PtrTy});

    // ---- Setup thread-local variables for transaction nesting ----
    // PURPOSE: Track nesting depth and store jump buffer for retries
    //          These must be thread-local because each thread has its own
    //          transaction context
    
    // tm_nested_call_counter: tracks nesting depth (0=not in transaction)
    GlobalVariable *CounterGV = M->getGlobalVariable("tm_nested_call_counter");
    if (!CounterGV) {
      CounterGV = new GlobalVariable(*M, i32Ty, false,
                                   GlobalValue::ExternalLinkage, nullptr,
                                   "tm_nested_call_counter");
      CounterGV->setThreadLocal(true);
      TM_DEBUG("Created tm_nested_call_counter");
    }

    // tm_jmpbuf: stores sigjmp_buf for retry (setjmp/longjmp)
    GlobalVariable *JmpBufGV = M->getGlobalVariable("tm_jmpbuf");
    if (!JmpBufGV) {
      ArrayType *JmpBufTy = ArrayType::get(i8Ty, 256);
      JmpBufGV = new GlobalVariable(*M, JmpBufTy, false,
                                  GlobalValue::ExternalLinkage, nullptr,
                                  "tm_jmpbuf");
      JmpBufGV->setThreadLocal(true);
      TM_DEBUG("Created tm_jmpbuf");
    }

    // tm_longjmp_ret: non-zero if we're retrying after longjmp
    GlobalVariable *JmpRetGV = M->getGlobalVariable("tm_longjmp_ret");
    if (!JmpRetGV) {
      JmpRetGV = new GlobalVariable(*M, i32Ty, false,
                                   GlobalValue::ExternalLinkage, nullptr,
                                   "tm_longjmp_ret");
      JmpRetGV->setThreadLocal(true);
      TM_DEBUG("Created tm_longjmp_ret");
    }

    // ---- Transaction entry: split block and add nesting logic ----
    // PURPOSE: At transaction entry, we need to:
    //   1. Check if this is an outer (nesting=0) or nested transaction
    //   2. For outer: call sigsetjmp (to allow retry), then tm_begin()
    //   3. For nested: just increment the nesting counter
    // We split the entry block to insert this logic at the start
    
    BasicBlock &Entry = F.getEntryBlock();
    // Find first non-PHI instruction as split point
    Instruction *SplitPt = &*Entry.getFirstNonPHIIt();
    TM_ASSERT(SplitPt != nullptr, "Entry block has no non-PHI instruction");
    
    // Split entry block: Entry -> [our logic] -> ContBB (original code)
    BasicBlock *ContBB = Entry.splitBasicBlock(SplitPt, "cont");
    TM_DEBUG("Split entry block, created cont block");
    
    // Create alloca for preserving return value across transaction exit blocks
    // PURPOSE: Non-void @transaction functions need to preserve their return value
    //          through the outer/nested exit branching
    AllocaInst *RetValAlloca = nullptr;
    if (!F.getReturnType()->isVoidTy()) {
        IRBuilder<> EntryBuilder(&Entry, Entry.getFirstNonPHIIt());
        RetValAlloca = EntryBuilder.CreateAlloca(F.getReturnType(), nullptr, "tx_retval");
    }
    
    // Remove the terminator from Entry (it was an unconditional branch to ContBB)
    Entry.getTerminator()->eraseFromParent();
    
    // Builder for inserting instructions at the end of Entry block
    IRBuilder<> Builder(&Entry);
    
    // Load current nesting counter
    Value *CounterVal = Builder.CreateLoad(i32Ty, CounterGV, "counter");
    // is_outer = (counter == 0) meaning this is the outermost transaction
    Value *IsOuter = Builder.CreateICmpEQ(CounterVal,
                                            ConstantInt::get(i32Ty, 0),
                                            "is_outer");

    // Check if this is a retry (longjmp return)
    // PURPOSE: After tm_abort, we longjmp back. If tm_longjmp_ret != 0,
    //          we're retrying, so treat as outer transaction
    Value *JmpRetVal = Builder.CreateLoad(i32Ty, JmpRetGV, "jmpret");
    Value *IsRetry = Builder.CreateICmpNE(JmpRetVal,
                                           ConstantInt::get(i32Ty, 0),
                                           "is_retry");
    IsOuter = Builder.CreateOr(IsOuter, IsRetry, "is_outer");

    // Create two paths: OuterBB (outer/nested transaction) and NestedBB (nested only)
    BasicBlock *OuterBB = BasicBlock::Create(Ctx, "outer", &F, ContBB);
    BasicBlock *NestedBB = BasicBlock::Create(Ctx, "nested", &F, ContBB);
    Builder.CreateCondBr(IsOuter, OuterBB, NestedBB);
    TM_DEBUG("Created outer/nested branches");

    // --- Outer transaction path ---
    // PURPOSE: For outermost transaction (or retry):
    //   1. Register jmpbuf with runtime via tm_set_jmpbuf (so abort_tx can
    //      siglongjmp back to the correct buffer)
    //   2. Call sigsetjmp to set jump point (allows retry via longjmp)
    //   3. Store sigsetjmp's return value to detect retry vs first-time
    //   4. Set counter to 1 (reset for retry case)
    //   5. Call tm_begin() to start the transaction
    IRBuilder<> OuterBuilder(OuterBB);
    Value *JmpBufPtr = OuterBuilder.CreateBitCast(JmpBufGV,
                                                    PointerType::getUnqual(Ctx),
                                                    "jmpbuf_ptr");
    OuterBuilder.CreateCall(tm_set_jmpbuf, {JmpBufPtr});
    Value *SetjmpRet = OuterBuilder.CreateCall(sigsetjmpFn,
                                                 {JmpBufPtr,
                                                  ConstantInt::get(i32Ty, 0)},
                                                 "setjmp_ret");
    OuterBuilder.CreateStore(SetjmpRet, JmpRetGV);
    // Set counter to 1 (in case of retry, counter needs to be reset)
    OuterBuilder.CreateStore(ConstantInt::get(i32Ty, 1), CounterGV);
    OuterBuilder.CreateCall(tm_begin, {});
    // Clear jmpret so nested @transaction functions use counter-based nesting
    // rather than incorrectly taking the outer path during retry
    OuterBuilder.CreateStore(ConstantInt::get(i32Ty, 0), JmpRetGV);
    OuterBuilder.CreateBr(ContBB);
    TM_DEBUG("Outer path: sigsetjmp + tm_begin + clear jmpret");

    // --- Nested transaction path ---
    // PURPOSE: For nested transaction, just increment counter
    //          NO sigsetjmp or tm_begin (those only happen at outermost level)
    IRBuilder<> NestedBuilder(NestedBB);
    Value *IncCounter = NestedBuilder.CreateAdd(CounterVal,
                                               ConstantInt::get(i32Ty, 1),
                                               "counter_inc");
    NestedBuilder.CreateStore(IncCounter, CounterGV);
    NestedBuilder.CreateBr(ContBB);
    TM_DEBUG("Nested path: increment counter");

    // ---- Instrument loads/stores in the function body ----
    // PURPOSE: Replace loads/stores to shared (TM-annotated) globals with
    //          tm_read/tm_write calls. Local variables are NOT instrumented.
    SmallVector<Instruction *, 16> ToErase;

    // Collect original blocks BEFORE we added transaction blocks
    // PURPOSE: Only instrument the original function body, not the entry/outer/nested blocks we created
    SmallPtrSet<BasicBlock *, 32> OriginalBlocks;
    for (auto &BB : F) {
      OriginalBlocks.insert(&BB);
    }
    // Remove the blocks we just created (they shouldn't be instrumented)
    // NOTE: ContBB is the continuation of the ORIGINAL body after splitBasicBlock,
    // it's NOT a newly created block — it MUST be instrumented.
    // Removing it would skip ALL load/store instrumentation (the root cause of
    // bank correctness failures at 4+ threads: no tm_read/tm_write calls emitted).
    OriginalBlocks.erase(&Entry);
    OriginalBlocks.erase(OuterBB);
    OriginalBlocks.erase(NestedBB);
    TM_DEBUG("Collected %d original blocks for instrumentation", (int)OriginalBlocks.size());

    for (auto *BB : OriginalBlocks) {
      for (auto InstIt = BB->begin(); InstIt != BB->end();) {
        Instruction *I = &*InstIt++;
        // Use SetInsertPoint with the instruction to insert BEFORE it
        IRBuilder<> Builder(I->getParent(), I->getIterator());

        // Fix 3: Memory intrinsic instrumentation
        // PURPOSE: Detect llvm.memcpy, llvm.memmove, llvm.memset and
        //          replace with per-byte loops using tm_read/tm_write
        if (auto *Call = dyn_cast<CallInst>(I)) {
          if (Function *Callee = Call->getCalledFunction()) {
            StringRef Name = Callee->getName();
            if (Name == "llvm.memcpy" || Name == "llvm.memmove" || Name == "llvm.memset") {
              // Check if any operand traces to a TM global
              bool touchesTM = false;
              for (unsigned i = 0; i < Call->arg_size(); ++i) {
                Value *Arg = Call->getArgOperand(i);
                if (tm_method_instrumentation::tracesToTMGlobal(Arg, *M)) {
                  touchesTM = true;
                  break;
                }
              }

              if (touchesTM) {
                TM_DEBUG("Instrumenting memory intrinsic: %s", Name.str().c_str());

                Value *Dst = Call->getArgOperand(0);
                Value *Len = Call->getArgOperand(2);
                Value *SrcOrVal = Call->getArgOperand(1);
                bool isMemset = (Name == "llvm.memset");

                // Split the current block at the call so we can insert loop blocks
                BasicBlock *OrigBB = Call->getParent();
                BasicBlock *ContBB = OrigBB->splitBasicBlock(Call, "mem_after");

                // Create loop blocks between OrigBB and ContBB
                BasicBlock *LoopEntryBB = BasicBlock::Create(Ctx, "mem_loop_entry", &F, ContBB);
                BasicBlock *LoopBodyBB = BasicBlock::Create(Ctx, "mem_loop_body", &F, ContBB);

                // Replace OrigBB's terminator (br ContBB from split) with br LoopEntryBB
                OrigBB->getTerminator()->eraseFromParent();
                IRBuilder<> PreBuilder(OrigBB);
                PreBuilder.CreateBr(LoopEntryBB);

                // Loop entry: PHI for index + exit check
                Type *i64MemTy = Type::getInt64Ty(Ctx);
                IRBuilder<> EntryBuilder(LoopEntryBB);
                PHINode *Idx = EntryBuilder.CreatePHI(i64MemTy, 2, "mem_idx");
                Idx->addIncoming(ConstantInt::get(i64MemTy, 0), OrigBB);

                Value *Done = EntryBuilder.CreateICmpEQ(Idx, Len, "mem_done");
                EntryBuilder.CreateCondBr(Done, ContBB, LoopBodyBB);

                // Loop body: per-byte tm_read/tm_write
                IRBuilder<> BodyBuilder(LoopBodyBB);
                Value *DstGEP = BodyBuilder.CreateGEP(i8Ty, Dst, Idx);

                if (isMemset) {
                  BodyBuilder.CreateCall(tm_write_i1, {DstGEP, SrcOrVal});
                } else {
                  Value *SrcGEP = BodyBuilder.CreateGEP(i8Ty, SrcOrVal, Idx);
                  Value *Byte = BodyBuilder.CreateCall(tm_read_i1, {SrcGEP});
                  BodyBuilder.CreateCall(tm_write_i1, {DstGEP, Byte});
                }

                Value *NextIdx = BodyBuilder.CreateAdd(Idx, ConstantInt::get(i64MemTy, 1), "mem_next");
                Idx->addIncoming(NextIdx, LoopBodyBB);
                BodyBuilder.CreateBr(LoopEntryBB);

                ToErase.push_back(Call);
              }
            }
          }
        }

        // Instrument loads from TM globals
        if (auto *Load = dyn_cast<LoadInst>(I)) {
          SmallPtrSet<const Value *, 32> LocalVars;
          collectLocalVariables(F, LocalVars);
          // Only instrument if pointer is shared (not local)
          if (isSharedPointer(Load->getPointerOperand(), LocalVars, F, *M)) {
            Value *PtrCast = Builder.CreateBitCast(Load->getPointerOperand(), i8PtrTy);
            Value *ReadValue = nullptr;
            Type *LoadTy = Load->getType();

            // Call appropriate tm_read_XX based on type
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
            } else {
              // Unhandled load type — skip instrumentation
            }

            if (ReadValue) {
              Load->replaceAllUsesWith(ReadValue);
              ToErase.push_back(Load);
            }
          }
        // Instrument stores to TM globals
        } else if (auto *Store = dyn_cast<StoreInst>(I)) {
          SmallPtrSet<const Value *, 32> LocalVars;
          collectLocalVariables(F, LocalVars);
          // Only instrument if pointer is shared (not local)
          if (isSharedPointer(Store->getPointerOperand(), LocalVars, F, *M)) {
            Value *PtrCast = Builder.CreateBitCast(Store->getPointerOperand(), i8PtrTy);
            Value *Val = Store->getValueOperand();
            Type *ValTy = Val->getType();

            // Call appropriate tm_write_XX based on type
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
            } else {
              // Unhandled store type — skip instrumentation
            }
          }
        }
      }
    }
    TM_DEBUG("Instrumented loads/stores, %d instructions to erase", (int)ToErase.size());

    // ---- Transaction exit: handle return instructions ----
    // PURPOSE: At return from transaction:
    //   1. Decrement nesting counter
    //   2. If outermost (counter becomes 0), call tm_end()
    //   3. Then return
    
    // Collect returns FIRST before creating new blocks
    // PURPOSE: We need to know all return points before modifying the function
    SmallVector<ReturnInst *, 4> Returns;
    for (auto &BB : F) {
      if (auto *Ret = dyn_cast<ReturnInst>(BB.getTerminator())) {
        Returns.push_back(Ret);
      }
    }
    TM_DEBUG("Found %d return instructions", (int)Returns.size());

    // Create exit blocks for handling outer vs nested exit
    // OuterEndBB: outermost transaction exit (call tm_end)
    // NestedEndBB: nested transaction exit (just continue)
    // CleanupBB: decrement counter and return (common to both paths)
    BasicBlock *OuterEndBB = BasicBlock::Create(Ctx, "outer_end", &F);
    BasicBlock *NestedEndBB = BasicBlock::Create(Ctx, "nested_end", &F);
    BasicBlock *CleanupBB = BasicBlock::Create(Ctx, "cleanup", &F);
    TM_DEBUG("Created exit blocks: outer_end, nested_end, cleanup");

    // Outer end: call tm_end, reset jmpret
    // PURPOSE: Outermost transaction is ending, so finalize it
    IRBuilder<> OuterEndBuilder(OuterEndBB);
    OuterEndBuilder.CreateCall(tm_end, {});
    OuterEndBuilder.CreateStore(ConstantInt::get(i32Ty, 0), JmpRetGV);
    OuterEndBuilder.CreateBr(CleanupBB);

    // Nested end: just continue to cleanup
    IRBuilder<> NestedEndBuilder(NestedEndBB);
    NestedEndBuilder.CreateBr(CleanupBB);

    // Process each return instruction
    // PURPOSE: Replace each return with a check: if outermost, go to OuterEndBB,
    //          else go to NestedEndBB. Both then go to CleanupBB.
    for (auto *Ret : Returns) {
      BasicBlock *RetBB = Ret->getParent();
      Value *RetVal = Ret->getNumOperands() > 0 ? Ret->getOperand(0) : nullptr;

      // Split block before return: RetBB -> NewBB (with check) -> [OuterEndBB/NestedEndBB] -> CleanupBB
      BasicBlock *NewBB = RetBB->splitBasicBlock(Ret, "ret_check");

      // Preserve return value before erasing the return instruction
      if (RetVal && RetValAlloca) {
          IRBuilder<> StoreBuilder(Ret);
          StoreBuilder.CreateStore(RetVal, RetValAlloca);
      }

      // Remove the return instruction (we'll replace it with conditional branch)
      Ret->eraseFromParent();

      // Create conditional branch based on nesting level
      IRBuilder<> NewBBuilder(NewBB);
      Value *CounterAtEnd = NewBBuilder.CreateLoad(i32Ty, CounterGV, "counter_at_end");
      // is_outer_at_end = (counter == 1) meaning this is the outermost transaction ending
      Value *IsOuterAtEnd = NewBBuilder.CreateICmpEQ(CounterAtEnd,
                                                        ConstantInt::get(i32Ty, 1),
                                                        "is_outer_at_end");
      NewBBuilder.CreateCondBr(IsOuterAtEnd, OuterEndBB, NestedEndBB);

      // Move NewBB to after the conditional blocks
      // PURPOSE: Ensure proper block ordering in the function
      NewBB->moveAfter(CleanupBB);
    }

    // Cleanup block: decrement counter and return
    // PURPOSE: Whether outer or nested, we always decrement the counter and return
    IRBuilder<> CleanupBuilder(CleanupBB);
    Value *CounterAtCleanup = CleanupBuilder.CreateLoad(i32Ty, CounterGV, "counter_cleanup");
    Value *DecCounter = CleanupBuilder.CreateSub(CounterAtCleanup,
                                                 ConstantInt::get(i32Ty, 1),
                                                 "counter_dec");
    CleanupBuilder.CreateStore(DecCounter, CounterGV);

    // Create appropriate return (void or with value)
    if (F.getReturnType()->isVoidTy()) {
      CleanupBuilder.CreateRetVoid();
    } else {
      Value *RetVal = CleanupBuilder.CreateLoad(F.getReturnType(), RetValAlloca, "tx_retval");
      CleanupBuilder.CreateRet(RetVal);
    }
    TM_DEBUG("Cleanup block: decrement counter and return");

    // Erase all replaced instructions
    for (Instruction *I : ToErase) {
      I->eraseFromParent();
    }
    TM_DEBUG("Erased %d instrumented instructions", (int)ToErase.size());

    return PreservedAnalyses::none();
  }

  static bool isRequired() { return true; }
};

} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo()
{
  return {LLVM_PLUGIN_API_VERSION,
          "TMInstrumentPass",
          LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name,
                   ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "tm-instrument") {
                    TM_DEBUG("Registering tm-instrument pass pipeline");
                    MPM.addPass(TMGlobalInitPass());
                    MPM.addPass(
                        createModuleToFunctionPassAdaptor(TMInstrumentPass()));
                    return true;
                  }
                  return false;
                });
          }};
}
