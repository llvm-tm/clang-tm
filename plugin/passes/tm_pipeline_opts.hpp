#ifndef TM_PIPELINE_OPTS_HPP
#define TM_PIPELINE_OPTS_HPP

#include <llvm/Support/CommandLine.h>

extern llvm::cl::opt<bool> AllowOpaque;
extern llvm::cl::opt<bool> StrictOpaque;
extern llvm::cl::opt<std::string> OpaqueSymbolsFile;
extern llvm::cl::opt<bool> TMAudit;
extern llvm::cl::opt<bool> EmitTrace;

#endif
