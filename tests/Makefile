# Unified test orchestrator
# Sub-makefiles:
#   backends/  — backend unit tests (counter, validation, region allocator)
#   plugin/    — LLVM plugin integration tests (37 tests)
#   expli-api/ — Explicit C++ API tests

.PHONY: all run clean

all:
	$(MAKE) -C backends/tm_impl all
	$(MAKE) -C expli-api all

run:
	$(MAKE) -C backends/tm_impl run
	@echo "Plugin tests: run 'make -C ../plugin run' from plugin/"
	@echo "(Plugin test files live here, but build via plugin/ Makefile)"

clean:
	$(MAKE) -C backends/tm_impl clean
