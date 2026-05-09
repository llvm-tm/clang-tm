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
// PURPOSE: Build a list of TM-annotated globals so the runtime can
//          track them for initialization and conflict detection
// NOTE: Annotations are stored in @llvm.global.annotations
static void collectTMSymbols(Module &M,
                             SmallVectorImpl<std::pair<GlobalVariable *, StringRef>> &Symbols)
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
// PURPOSE: Used to check if a pointer refers to a TM-annotated global
//          during load/store instrumentation
// NOTE: Annotations are stored in @llvm.global.annotations
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

#endif // TM_ANNOTATION_UTILS_HPP
