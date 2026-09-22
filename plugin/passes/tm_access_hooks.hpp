// tm_access_hooks.hpp
// Shared, header-only classification of an LLVM IR call as a TM transactional
// access hook (tm_read_* / tm_write_*). Used by the tm-access-trace pass and
// reusable by other passes (race checker, instrumentation) to avoid duplicating
// the hook-name -> {kind,width} switch.
//
// In instrumented IR a tracked access is an *indirect* call through a runtime
// function-pointer global:
//     %p = load ptr, ptr @tm_read_i4        ; @tm_read_i4 is `void(*)(void*)`
//     %v = call i32 %p(ptr %addr)
// (In some builds it may be a direct call to a function of the same name.)
#ifndef TM_ACCESS_HOOKS_HPP
#define TM_ACCESS_HOOKS_HPP

#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>

#include <optional>

#include "tm_runtime_hooks.hpp"

namespace tmacc
{

enum class Kind { Read, Write };

struct Info {
	Kind kind;
	llvm::CallBase *call;
	llvm::Value *addr;  // first operand (the accessed pointer)
	llvm::Value *value; // write value (nullptr for reads)
	unsigned
	    widthBytes; // access width in bytes (pointer => sizeof(ptr), filled by caller if 0)
};

// Width in bytes from the hook-name suffix (i1/i2/i4/i8/f4/f8/ptr/i16/i32/i64).
// Returns 0 for pointer-sized (caller resolves via DataLayout).
inline unsigned widthFromHookName(llvm::StringRef N)
{
	if (N.ends_with("_i1"))
		return 1;
	if (N.ends_with("_i2"))
		return 2;
	if (N.ends_with("_i4"))
		return 4;
	if (N.ends_with("_i8"))
		return 8;
	if (N.ends_with("_f4"))
		return 4;
	if (N.ends_with("_f8"))
		return 8;
	if (N.ends_with("_i16"))
		return 16;
	if (N.ends_with("_i32"))
		return 32;
	if (N.ends_with("_i64"))
		return 64;
	if (N.ends_with("_ptr"))
		return 0; // pointer-sized
	return 0;
}

// Resolve the callee name for a direct or hook-pointer (load+call) call.
// Returns e.g. "tm_read_i4" or "" if not resolvable.
inline llvm::StringRef calleeHookName(const llvm::CallBase &CB)
{
	if (const llvm::Function *F = CB.getCalledFunction())
		return F->getName();
	// indirect through a global function pointer: load of @tm_read_i4
	if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(CB.getCalledOperand()))
		if (const auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(
		        LI->getPointerOperand()))
			return GV->getName();
	return "";
}

// Classify a call as a TM access. Returns std::nullopt for non-access calls.
// isPtrSizedWidth: if true, width==0 means pointer width (caller fixes up).
inline std::optional<Info> classify(const llvm::CallBase &CB)
{
	llvm::StringRef N = calleeHookName(CB);
	if (N.empty())
		return std::nullopt;
	bool isRead = N.starts_with("tm_read_");
	bool isWrite = N.starts_with("tm_write_");
	if (!isRead && !isWrite)
		return std::nullopt;
	if (CB.arg_empty())
		return std::nullopt;

	Info I;
	I.kind = isRead ? Kind::Read : Kind::Write;
	I.call = const_cast<llvm::CallBase *>(&CB);
	I.addr = const_cast<llvm::Value *>(CB.getArgOperand(0));
	I.value = (!isRead && CB.arg_size() >= 2)
	              ? const_cast<llvm::Value *>(CB.getArgOperand(1))
	              : nullptr;
	I.widthBytes = widthFromHookName(N);
	return I;
}

} // namespace tmacc

#endif // TM_ACCESS_HOOKS_HPP
