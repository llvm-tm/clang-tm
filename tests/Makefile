# Unified test orchestrator
# Sub-makefiles:
#   backends/  — backend unit tests (counter, validation, region allocator)
#   plugin/    — LLVM plugin integration tests (37 tests)
#   expli-api/ — Explicit C++ API tests

.PHONY: all run clean

all:
	$(MAKE) -C backends all
	$(MAKE) -C expli-api all

run:
	$(MAKE) -C backends run
	@echo "Plugin tests: run 'make -C ../llvm_tm_plugin run' from llvm_tm_plugin/"
	@echo "(Plugin test files live here, but build via llvm_tm_plugin Makefile)"

clean:
	$(MAKE) -C backends clean
