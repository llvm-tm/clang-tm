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

// =========================================================================
// Annotation name constants
//
// Changes to these strings require corresponding changes in user source
// code that uses __attribute__((annotate("..."))) with the same values.
// =========================================================================
constexpr char ANNOTATION_GLOBAL[] = "llvm.global.annotations";
constexpr unsigned ANNOTATION_MIN_OPERANDS = 2;
constexpr char TM_ANNOT[] = "tm";
constexpr char TX_ANNOT[] = "transaction";
constexpr char THREAD_ANNOT[] = "thread";
constexpr char MAIN_ANNOT[] = "main";
constexpr char TM_LOCAL_ANNOT[] = "tm_local";
constexpr char ALLOW_OPAQUE_ANNOT[] = "tm_allow_opaque";
constexpr char PSTATIC_REBUILD_ANNOT[] = "pstatic_rebuild";
constexpr char TM_CLONE_SUFFIX[] = "_tm_clone";

// Symbol table name constants (persistent runtimes)
constexpr char TM_SYMBOL_COUNT[] = "tm_symbol_count";
constexpr char TM_SYMBOL_NAMES[] = "tm_symbol_names";
constexpr char TM_SYMBOL_ADDRESSES[] = "tm_symbol_addresses";
constexpr char TM_SYMBOL_SIZES[] = "tm_symbol_sizes";

// =========================================================================
// Annotation iteration helpers
// =========================================================================

// Iterate all entries in @llvm.global.annotations, calling Fn for each.
// Fn receives (Constant *Annotation, Value *AnnotatedValue, StringRef AnnotationStr).
// Return false from Fn to stop iteration early (useful for hasAnnotation).
template <typename Fn>
static void forEachAnnotation(Module &M, Fn Callback)
{
	if (GlobalVariable *GVA = M.getNamedGlobal(ANNOTATION_GLOBAL)) {
		if (Constant *Init = GVA->getInitializer()) {
			for (unsigned i = 0; i < Init->getNumOperands(); ++i) {
				Constant *Annotation = cast<Constant>(Init->getOperand(i));
				if (Annotation->getNumOperands() < ANNOTATION_MIN_OPERANDS)
					continue;
				Value *AnnotatedValue = Annotation->getOperand(0)
				                            ->stripPointerCasts();
				Value *StrOperand = Annotation->getOperand(1)
				                        ->stripPointerCasts();
				if (auto *StrGV = dyn_cast<GlobalVariable>(StrOperand)) {
					if (auto *StrArray = dyn_cast<ConstantDataArray>(
					        StrGV->getInitializer())) {
						if (!Callback(Annotation,
						              AnnotatedValue,
						              StrArray->getAsCString()))
							return;
					}
				}
			}
		}
	}
}

// Check if a function has a specific annotation
// PURPOSE: Used to identify transaction functions (annotated with "transaction")
//          and functions using TM (annotated with "tm")
// NOTE: Annotations are stored in @llvm.global.annotations, not on the function directly
static bool hasAnnotation(Function &F, StringRef annot)
{
	Module *M = F.getParent();
	bool found = false;
	forEachAnnotation(*M, [&](Constant *, Value *V, StringRef S) -> bool {
		if (found)
			return false;
		if (S != annot)
			return true;
		if (auto *AnnotatedFunc = dyn_cast<Function>(V)) {
			if (AnnotatedFunc == &F) {
				TM_DEBUG("Function %s has annotation '%s'",
				         F.getName().str().c_str(),
				         annot.str().c_str());
				found = true;
			}
		}
		return !found;
	});
	return found;
}

// Collect all globals annotated with "tm"
// PURPOSE: Build a list of TM-annotated globals for the symbol tables used
//          by persistent runtimes (PersistentSGL_runtime.cpp, persistent.cpp).
//          These tables let persistence code save/restore global state across
//          program restarts.  NOT related to the (now-removed) symbol_id
//          parameter in read/write function signatures.
static void collectTMSymbols(
    Module &M,
    SmallVectorImpl<std::pair<GlobalVariable *, StringRef>> &Symbols)
{
	forEachAnnotation(M, [&](Constant *, Value *V, StringRef S) -> bool {
		if (S != TM_ANNOT)
			return true;
		if (auto *AnnotatedGV = dyn_cast<GlobalVariable>(V)) {
			StringRef name = AnnotatedGV->getName();
			Symbols.push_back({AnnotatedGV, name});
			TM_DEBUG("Found TM-annotated global: %s", name.str().c_str());
		}
		return true;
	});
}

// Collect all TM-annotated globals into a set for quick lookup
static void collectTMGlobals(Module &M, SmallPtrSetImpl<const Value *> &TMValues)
{
	forEachAnnotation(M, [&](Constant *, Value *V, StringRef S) -> bool {
		if (S != TM_ANNOT)
			return true;
		if (auto *AnnotatedGV = dyn_cast<GlobalVariable>(V)) {
			TMValues.insert(AnnotatedGV);
			TM_DEBUG("Added TM global to set: %s",
			         AnnotatedGV->getName().str().c_str());
		}
		return true;
	});
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
			if (auto *Load = dyn_cast<LoadInst>(&I)) {
				if (TMGlobals.count(Load->getPointerOperand())) {
					TM_DEBUG("Function %s uses TM global",
					         F.getName().str().c_str());
					return true;
				}
			}
			if (auto *Store = dyn_cast<StoreInst>(&I)) {
				if (TMGlobals.count(Store->getPointerOperand())) {
					TM_DEBUG("Function %s uses TM global",
					         F.getName().str().c_str());
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
		forEachAnnotation(M, [&](Constant *, Value *V, StringRef S) -> bool {
			if (S != TM_ANNOT)
				return true;
			if (auto *AnnotatedGV = dyn_cast<GlobalVariable>(V))
				Cache->insert(AnnotatedGV);
			return true;
		});
	}
	return Cache->count(GV);
}

// Create symbol table globals in the module for persistent runtimes.
// Generates: tm_symbol_count, tm_symbol_names[], tm_symbol_addresses[], tm_symbol_sizes[].
// Used by PersistentSGL_runtime.cpp and persistent.cpp to save/restore TM-annotated
// globals across program restarts.  NOT related to the (now-removed) symbol_id
// parameter in read/write functions.
static void createTMSymbolTables(
    Module &M,
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
		                                  ArrayType::get(Type::getInt8Ty(Ctx),
		                                                 name.size() + 1),
		                                  false,
		                                  GlobalValue::PrivateLinkage,
		                                  ConstantDataArray::getString(Ctx, name, true),
		                                  Twine("tm_symbol_name_") + name);
		nameGV->setDSOLocal(true);
		nameGV->setUnnamedAddr(GlobalValue::UnnamedAddr::Local);

		Constant
		    *namePtr = ConstantExpr::getInBoundsGetElementPtr(Type::getInt8Ty(Ctx),
		                                                      nameGV,
		                                                      ConstantInt::get(Int32Ty,
		                                                                       0));
		namePtrs.push_back(ConstantExpr::getBitCast(namePtr, CharPtrTy));
		addrPtrs.push_back(ConstantExpr::getBitCast(GV, VoidPtrTy));
		sizesVals.push_back(
		    ConstantInt::get(Int64Ty, DL.getTypeAllocSize(GV->getValueType())));
	}

	new GlobalVariable(M,
	                   Int32Ty,
	                   true,
	                   GlobalValue::ExternalLinkage,
	                   ConstantInt::get(Int32Ty, Symbols.size()),
	                   TM_SYMBOL_COUNT);

	auto mkArr = [&](Type *ElemTy, auto &Vals, StringRef Name) {
		if (Vals.empty())
			return;
		auto *ArrTy = ArrayType::get(ElemTy, Vals.size());
		new GlobalVariable(M,
		                   ArrTy,
		                   false,
		                   GlobalValue::ExternalLinkage,
		                   ConstantArray::get(ArrTy, Vals),
		                   Name);
	};
	mkArr(CharPtrTy, namePtrs, TM_SYMBOL_NAMES);
	mkArr(VoidPtrTy, addrPtrs, TM_SYMBOL_ADDRESSES);
	mkArr(Int64Ty, sizesVals, TM_SYMBOL_SIZES);
}

#endif // TM_ANNOTATION_UTILS_HPP
