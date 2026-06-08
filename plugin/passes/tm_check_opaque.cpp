// tm_check_opaque.cpp
// Opaque function detection: verifies all calls in TX-reachable functions
// can be instrumented or are known-safe.

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include <fstream>

#include "opaque_safe_table.hpp"
#include "tm_annotation_utils.hpp"
#include "tm_debug.hpp"
#include "tm_instrument_helpers.hpp"
#include "tm_pipeline_opts.hpp"
#include "tm_platform.hpp"
#include "tm_runtime_hooks.hpp"
using namespace llvm;

bool checkOpaqueFunctions(Module &M,
                          SmallPtrSetImpl<Function *> &TxReachableFuncs)
{
	bool foundOpaque = false;
	StringSet<> UnresolvedSymbols;

	for (Function *F : TxReachableFuncs) {
		if (F->isDeclaration())
			continue;
		if (F->getName().starts_with("tm_"))
			continue;
		if (hasAnnotation(*F, ALLOW_OPAQUE_ANNOT))
			continue;
		for (auto &BB : *F) {
			for (auto &I : BB) {
				auto *Call = dyn_cast<CallBase>(&I);
				if (!Call)
					continue;
				if (Call->hasFnAttr(ALLOW_OPAQUE_ANNOT))
					continue;
				if (Call->isInlineAsm())
					continue;
				Function *Callee = Call->getCalledFunction();
				if (Callee && hasAnnotation(*Callee, ALLOW_OPAQUE_ANNOT))
					continue;
				// Format location prefix consistently
				auto locStr = [&](raw_ostream &OS) {
					if (auto *DIL = I.getDebugLoc().get())
						if (auto *Scope = dyn_cast_or_null<DIScope>(DIL->getScope()))
							OS << Scope->getFilename() << ":" << DIL->getLine() << ":"
							   << DIL->getColumn() << ": ";
				};
				if (!Callee) {
					foundOpaque = true;
					locStr(errs());
					errs() << "error: indirect call in TM context\n"
					       << "  Called from: " << F->getName()
					       << "\n"
					          "  Calls via function pointer or virtual method "
					          "cannot be instrumented for TM.\n";
					continue;
				}
				if (Callee->isIntrinsic())
					continue;
				if (tm_platform::isHeapAllocationCall(Call) || tm_platform::isDeallocationCall(Call))
					continue;
				if (!Callee->isDeclaration())
					continue;
				bool hasTMTracedArg = false;
				for (unsigned i = 0; i < Call->arg_size(); i++) {
					if (Call->getArgOperand(i)->getType()->isPointerTy() &&
					    tracesFromTMGlobal(Call->getArgOperand(i), M)) {
						hasTMTracedArg = true;
						break;
					}
				}
				if (!hasTMTracedArg) {
					if (isKnownSafeOpaque(Callee->getName(), StrictOpaque))
						continue;
					if (isSyscallSymbol(Callee->getName()))
						continue;
				} else {
					if (isKnownSafeWithTMArgs(Callee->getName()))
						continue;
				}
				foundOpaque = true;
				UnresolvedSymbols.insert(Callee->getName());
				locStr(errs());
				errs() << "error: call to '" << Callee->getName() << "' in TM context\n"
				       << "  Called from: " << F->getName() << "\n";
				if (hasTMTracedArg)
					errs()
					    << "  This function receives TM-shared pointer arguments "
					       "but its body is not visible (defined in external library).\n"
					       "  Its internal modifications bypass TM write-set tracking, "
					       "causing data corruption with concurrent transactions.\n";
				else
					errs() << "  This function is not visible to TM "
					          "instrumentation (no body in this translation unit).\n";
				emitOpaqueSuggestion(Callee->getName(), errs());
			}
		}
	}

	if (!OpaqueSymbolsFile.empty() && !UnresolvedSymbols.empty()) {
		std::error_code EC;
		raw_fd_ostream OS(OpaqueSymbolsFile, EC);
		if (!EC) {
			for (auto &Sym : UnresolvedSymbols)
				OS << Sym.getKey() << "\n";
			OS.close();
			errs() << "Unresolved opaque symbols written to: " << OpaqueSymbolsFile
			       << "\n";
		}
	}

	return !foundOpaque;
}

void checkOpaqueOrAbort(Module &M,
                        SmallPtrSetImpl<Function *> &TxReachableFuncs)
{
	if (TxReachableFuncs.empty())
		return;
	bool ok = checkOpaqueFunctions(M, TxReachableFuncs);
	if (!AllowOpaque && !ok) {
		errs() << "error: opaque function call(s) in TM context\n"
		       << "Use -tm-allow-opaque to disable this check.\n";
		exit(1);
	}
}
