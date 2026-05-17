// tm_call_graph.hpp
// Call graph analysis for transaction functions
//
// PURPOSE: Track which functions call transaction functions.
//          Used to identify thread entry points: functions that use TM
//          (call transactions or access TM globals) but are NOT
//          transactions themselves.

#ifndef TM_CALL_GRAPH_HPP
#define TM_CALL_GRAPH_HPP

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include "tm_annotation_utils.hpp"
#include "tm_debug.hpp"

using namespace llvm;

// Collect functions called directly from a function
// PURPOSE: Build a set of functions that are called directly from F.
//          Used as a helper for call graph traversal.
static void collectDirectCalls(Function &F, SmallPtrSet<Function *, 32> &CalledFuncs)
{
  for (auto &BB : F) {
    for (auto &I : BB) {
      if (auto *Call = dyn_cast<CallBase>(&I)) {
        if (Function *Callee = Call->getCalledFunction()) {
          if (!Callee->isDeclaration()) {
            CalledFuncs.insert(Callee);
          }
        }
      }
    }
  }
}

// Check if a function calls any transaction-annotated functions
// PURPOSE: Used to identify thread entry points. If a function calls
//          transaction functions but is not one itself, it's a thread entry.
static bool callsTransactionFunctions(Function &F, Module &M)
{
  for (auto &BB : F) {
    for (auto &I : BB) {
      if (auto *Call = dyn_cast<CallBase>(&I)) {
        if (Function *Callee = Call->getCalledFunction()) {
          if (hasAnnotation(*Callee, "transaction")) {
            TM_DEBUG("Function %s calls transaction function %s",
                     F.getName().str().c_str(), Callee->getName().str().c_str());
            return true;
          }
        }
      }
    }
  }
  return false;
}

// Check if any function directly called by F, itself directly calls a transaction function
// PURPOSE: Distinguish "thread entry points" from "transaction callers".
//          A thread entry (e.g., worker_thread) does NOT directly call a transaction
//          but calls helpers that do. A direct caller of a transaction should NOT
//          get tm_init_thread/tm_exit_thread instrumentation.
static bool transitivelyCallsTransactionFunctions(Function &F, Module &M)
{
  SmallPtrSet<Function *, 32> CalledFuncs;
  collectDirectCalls(F, CalledFuncs);
  for (Function *Callee : CalledFuncs) {
    if (callsTransactionFunctions(*Callee, M)) {
      TM_DEBUG("Function %s transitively calls a transaction function through %s",
               F.getName().str().c_str(), Callee->getName().str().c_str());
      return true;
    }
  }
  return false;
}

// Recursively collect all functions reachable from a transaction-annotated function
// PURPOSE: Build the transitive closure of functions called from transactions.
//          This helps understand the full call graph for TM optimization.
//          VISITED set prevents infinite loops from recursive calls.
static void buildTransactionCallGraph(Function &TxFunc,
                                     Module &M,
                                     SmallPtrSet<Function *, 32> &ReachableFuncs,
                                     SmallPtrSet<Function *, 32> &Visited)
{
  // Prevent infinite recursion: stop if we've already visited this function
  if (Visited.count(&TxFunc)) return;
  Visited.insert(&TxFunc);
  ReachableFuncs.insert(&TxFunc);
  TM_DEBUG("Added to call graph: %s", TxFunc.getName().str().c_str());

  // Recursively follow all direct calls
  for (auto &BB : TxFunc) {
    for (auto &I : BB) {
      if (auto *Call = dyn_cast<CallBase>(&I)) {
        if (Function *Callee = Call->getCalledFunction()) {
          // Skip external functions (declarations only - no body to analyze)
          if (!Callee->isDeclaration()) {
            buildTransactionCallGraph(*Callee, M, ReachableFuncs, Visited);
          }
        }
      }
    }
  }
}

// Wrapper to initiate the call graph traversal
// PURPOSE: Entry point for building the transaction call graph.
//          Initializes the Visited set and starts recursive traversal.
static void collectTransactionCallGraph(Function &TxFunc,
                                       Module &M,
                                       SmallPtrSet<Function *, 32> &ReachableFuncs)
{
  SmallPtrSet<Function *, 32> Visited;
  buildTransactionCallGraph(TxFunc, M, ReachableFuncs, Visited);
  TM_DEBUG("Call graph complete, %d functions reachable from %s", 
           (int)ReachableFuncs.size(), TxFunc.getName().str().c_str());
}

#endif // TM_CALL_GRAPH_HPP
