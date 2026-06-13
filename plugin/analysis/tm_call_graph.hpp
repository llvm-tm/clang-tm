// tm_call_graph.hpp
// Call graph analysis for transaction functions
//
// PURPOSE: Track which functions call transaction functions.
//          Used to identify thread entry points: functions that use TM
//          (call transactions or access TM globals) but are NOT
//          transactions themselves.

#ifndef TM_CALL_GRAPH_HPP
#define TM_CALL_GRAPH_HPP

#include "tm_annotation_utils.hpp"
#include "tm_debug.hpp"
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

using namespace llvm;

// Inline capacity hint for call graph SmallPtrSet containers
constexpr size_t CALLGRAPH_INLINE_CAPACITY = 32;



// Recursively collect all functions reachable from a transaction-annotated function
// PURPOSE: Build the transitive closure of functions called from transactions.
//          This helps understand the full call graph for TM optimization.
//          VISITED set prevents infinite loops from recursive calls.
static void buildTransactionCallGraph(Function &TxFunc,
                                      Module &M,
                                      SmallPtrSet<Function *, CALLGRAPH_INLINE_CAPACITY> &ReachableFuncs,
                                      SmallPtrSet<Function *, CALLGRAPH_INLINE_CAPACITY> &Visited)
{
	// Prevent infinite recursion: stop if we've already visited this function
	if (Visited.count(&TxFunc))
		return;
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
                                        SmallPtrSet<Function *, CALLGRAPH_INLINE_CAPACITY> &ReachableFuncs)
{
	SmallPtrSet<Function *, CALLGRAPH_INLINE_CAPACITY> Visited;
	buildTransactionCallGraph(TxFunc, M, ReachableFuncs, Visited);
	TM_DEBUG("Call graph complete, %d functions reachable from %s",
	         (int)ReachableFuncs.size(),
	         TxFunc.getName().str().c_str());
}

#endif // TM_CALL_GRAPH_HPP
