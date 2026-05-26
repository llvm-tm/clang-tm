// tm_annotation_utils.hpp
// Helper functions for reading TM annotations from LLVM metadata
//
// PURPOSE: The plugin uses LLVM annotations ("tm" and "transaction") to mark
//          variables and functions that should be managed transactionally.
//          These annotations are added by the user via __attribute__((annotate("tm")))
//          or similar mechanisms.

#ifndef TM_ANNOTATION_UTILS_HPP
#define TM_ANNOTATION_UTILS_HPP

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

#include "tm_debug.hpp"

using namespace llvm;

// Check if a function has a specific annotation
// PURPOSE: Used to identify transaction functions (annotated with "transaction")
//          and functions using TM (annotated with "tm")
// NOTE: Annotations are stored in @llvm.global.annotations, not on the function directly
static bool hasAnnotation(Function &F, StringRef annot)
{
  Module *M = F.getParent();
  
  // Look for @llvm.global.annotations in the module
  if (GlobalVariable *GVA = M->getNamedGlobal("llvm.global.annotations")) {
    if (Constant *Init = GVA->getInitializer()) {
      // Iterate through the array of annotations
      for (unsigned i = 0; i < Init->getNumOperands(); ++i) {
        Constant *Annotation = cast<Constant>(Init->getOperand(i));
        // Each annotation is a struct: {ptr to annotated, ptr to annotation string, ptr to source file, line, ptr to null}
        if (Annotation->getNumOperands() >= 2) {
          // Check if this annotation is for our function
          // operand(0) is ptr to the annotated value (function or global)
          Value *AnnotatedValue = Annotation->getOperand(0)->stripPointerCasts();
          
          if (Function *AnnotatedFunc = dyn_cast<Function>(AnnotatedValue)) {
            if (AnnotatedFunc == &F) {
              // Check the annotation string
              // operand(1) is ptr to the annotation string global
              Value *StrOperand = Annotation->getOperand(1)->stripPointerCasts();
              if (GlobalVariable *StrGV = dyn_cast<GlobalVariable>(StrOperand)) {
                if (ConstantDataArray *StrArray = dyn_cast<ConstantDataArray>(StrGV->getInitializer())) {
                  // Use getAsCString() which properly handles null terminator
                  StringRef AnnotationStr = StrArray->getAsCString();
                  if (AnnotationStr == annot) {
                    TM_DEBUG("Function %s has annotation '%s'", F.getName().str().c_str(), annot.str().c_str());
                    return true;
                  }
                }
              }
            }
          }
        }
      }
    }
  }
  return false;
}

// Collect all globals annotated with "tm"
// PURPOSE: Build a list of TM-annotated globals for the symbol tables used
//          by persistent runtimes (PersistentSGL_runtime.cpp, persistent.cpp).
//          These tables let persistence code save/restore global state across
//          program restarts.  NOT related to the (now-removed) symbol_id
//          parameter in read/write function signatures.
static void collectTMSymbols(Module &M,
                             SmallVectorImpl<std::pair<GlobalVariable *, StringRef>> &Symbols)
{
  if (GlobalVariable *GVA = M.getNamedGlobal("llvm.global.annotations")) {
    if (Constant *Init = GVA->getInitializer()) {
      for (unsigned i = 0; i < Init->getNumOperands(); ++i) {
        Constant *Annotation = cast<Constant>(Init->getOperand(i));
        if (Annotation->getNumOperands() >= 2) {
          Value *AnnotatedValue = Annotation->getOperand(0)->stripPointerCasts();
          if (GlobalVariable *AnnotatedGV = dyn_cast<GlobalVariable>(AnnotatedValue)) {
            Value *StrOperand = Annotation->getOperand(1)->stripPointerCasts();
            if (GlobalVariable *StrGV = dyn_cast<GlobalVariable>(StrOperand)) {
              if (ConstantDataArray *StrArray = dyn_cast<ConstantDataArray>(StrGV->getInitializer())) {
                StringRef AnnotationStr = StrArray->getAsCString();
                if (AnnotationStr == "tm") {
                  StringRef name = AnnotatedGV->getName();
                  Symbols.push_back({AnnotatedGV, name});
                  TM_DEBUG("Found TM-annotated global: %s", name.str().c_str());
                }
              }
            }
          }
        }
      }
    }
  }
}

// Collect all TM-annotated globals into a set for quick lookup
static void collectTMGlobals(Module &M, SmallPtrSetImpl<const Value *> &TMValues)
{
  // Look for @llvm.global.annotations in the module
  if (GlobalVariable *GVA = M.getNamedGlobal("llvm.global.annotations")) {
    if (Constant *Init = GVA->getInitializer()) {
      // Iterate through the array of annotations
      for (unsigned i = 0; i < Init->getNumOperands(); ++i) {
        Constant *Annotation = cast<Constant>(Init->getOperand(i));
        // Each annotation is a struct: {ptr to annotated, ptr to annotation string, ptr to source file, line, ptr to null}
        if (Annotation->getNumOperands() >= 2) {
          // Get the annotated global
          Value *AnnotatedValue = Annotation->getOperand(0)->stripPointerCasts();
          if (GlobalVariable *AnnotatedGV = dyn_cast<GlobalVariable>(AnnotatedValue)) {
            // Check the annotation string
            Value *StrOperand = Annotation->getOperand(1)->stripPointerCasts();
            if (GlobalVariable *StrGV = dyn_cast<GlobalVariable>(StrOperand)) {
              if (ConstantDataArray *StrArray = dyn_cast<ConstantDataArray>(StrGV->getInitializer())) {
                // Use getAsCString() which properly handles null terminator
                StringRef AnnotationStr = StrArray->getAsCString();
                if (AnnotationStr == "tm") {
                  TMValues.insert(AnnotatedGV);
                  TM_DEBUG("Added TM global to set: %s", AnnotatedGV->getName().str().c_str());
                }
              }
            }
          }
        }
      }
    }
  }
}

// Check if a function uses any TM-annotated globals
// PURPOSE: Determine if a function uses TM globals (needs thread init/exit)
//          Used to identify thread entry points
// NOTE: This checks if the function accesses any global that has "tm" annotation
static bool hasTMGlobals(Function &F)
{
  Module *M = F.getParent();
  
  // First, collect all TM-annotated globals
  SmallPtrSet<const Value *, 8> TMGlobals;
  collectTMGlobals(*M, TMGlobals);
   
  // Check if the function accesses any of these globals
  for (auto &BB : F) {
    for (auto &I : BB) {
      // Check loads
      if (auto *Load = dyn_cast<LoadInst>(&I)) {
        if (TMGlobals.count(Load->getPointerOperand())) {
          TM_DEBUG("Function %s uses TM global", F.getName().str().c_str());
          return true;
        }
      }
      // Check stores
      if (auto *Store = dyn_cast<StoreInst>(&I)) {
        if (TMGlobals.count(Store->getPointerOperand())) {
          TM_DEBUG("Function %s uses TM global", F.getName().str().c_str());
          return true;
        }
      }
    }
  }
  return false;
}

// Check if a specific GlobalVariable has the "tm" annotation.
// Uses a cached set for O(1) lookups.
static bool isTMAnnotatedGlobal(const GlobalVariable *GV, Module &M)
{
  static SmallPtrSet<const GlobalVariable *, 16> *Cache = nullptr;
  if (!Cache) {
    Cache = new SmallPtrSet<const GlobalVariable *, 16>();
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
                    Cache->insert(AnnotatedGV);
                  }
                }
              }
            }
          }
        }
      }
    }
  }
  return Cache->count(GV);
}

// Create symbol table globals in the module for persistent runtimes.
// Generates: tm_symbol_count, tm_symbol_names[], tm_symbol_addresses[], tm_symbol_sizes[].
// Used by PersistentSGL_runtime.cpp and persistent.cpp to save/restore TM-annotated
// globals across program restarts.  NOT related to the (now-removed) symbol_id
// parameter in read/write functions.
static void createTMSymbolTables(Module &M,
    SmallVectorImpl<std::pair<GlobalVariable *, StringRef>> &Symbols)
{
  LLVMContext &Ctx = M.getContext();
  auto *Int32Ty = Type::getInt32Ty(Ctx);
  auto *Int64Ty = Type::getInt64Ty(Ctx);
  auto *CharPtrTy = PointerType::getUnqual(Ctx);
  auto *VoidPtrTy = PointerType::getUnqual(Ctx);
  const DataLayout &DL = M.getDataLayout();

  SmallVector<Constant *, 16> namePtrs, addrPtrs, sizesVals;

  for (auto &Sym : Symbols) {
    GlobalVariable *GV = Sym.first;
    StringRef name = Sym.second;

    auto *nameGV = new GlobalVariable(M,
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
    sizesVals.push_back(ConstantInt::get(Int64Ty, DL.getTypeAllocSize(GV->getValueType())));
  }

  new GlobalVariable(M, Int32Ty, true, GlobalValue::ExternalLinkage,
                     ConstantInt::get(Int32Ty, Symbols.size()), "tm_symbol_count");

  auto mkArr = [&](Type *ElemTy, auto &Vals, StringRef Name) {
    if (Vals.empty()) return;
    auto *ArrTy = ArrayType::get(ElemTy, Vals.size());
    new GlobalVariable(M, ArrTy, false, GlobalValue::ExternalLinkage,
                       ConstantArray::get(ArrTy, Vals), Name);
  };
  mkArr(CharPtrTy, namePtrs,  "tm_symbol_names");
  mkArr(VoidPtrTy, addrPtrs,  "tm_symbol_addresses");
  mkArr(Int64Ty,   sizesVals, "tm_symbol_sizes");
}

#endif // TM_ANNOTATION_UTILS_HPP
