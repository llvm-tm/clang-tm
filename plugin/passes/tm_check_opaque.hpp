#ifndef TM_CHECK_OPAQUE_HPP
#define TM_CHECK_OPAQUE_HPP

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

// Check all TX-reachable functions for opaque (uninstrumentable) calls.
// Returns true if no opaque calls found, false otherwise.
bool checkOpaqueFunctions(llvm::Module &M,
                          SmallPtrSetImpl<llvm::Function *> &TxReachableFuncs);

// Call checkOpaqueFunctions and abort on failure (unless -tm-allow-opaque).
void checkOpaqueOrAbort(llvm::Module &M,
                        SmallPtrSetImpl<llvm::Function *> &TxReachableFuncs);

#endif
