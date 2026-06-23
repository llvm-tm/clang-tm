# TLA+ Proofs Makefile
#
# Targets:
#   make check              — TLC on all backends
#   make check-<BACKEND>    — TLC on a single backend (e.g., make check-TL2)
#   make check-all          — TLC + coverage on all backends
#   make tla                — run pcal.trans on all PlusCal sources
#   make clean              — remove *.old, *TTrace*, *.bin, states/
#   make proof-<BACKEND>    — TLAPS proof check (SGL only)
#
# Configuration:
#   TLA2TOOLS_JAR — path to tla2tools.jar (default: /tmp/tla2tools.jar)
#   TLC_OPTS     — extra flags to TLC (e.g. -coverage 1)
#   TLAPM        — path to tlapm (default: tlapm)

TLA2TOOLS_JAR ?= /tmp/tla2tools.jar
TLAPM         ?= tlapm
TLC_OPTS      ?=

JAVA := /tmp/jdk-11.0.10+9-jre/bin/java

# All backends with .tla and .cfg files
BACKENDS := $(patsubst %.cfg,%,$(wildcard *.cfg))
# Backends that use PlusCal (have $${$}--algorithm comment)
PLUSCAL_BACKENDS := $(shell grep -l '\-\-algorithm' *.tla 2>/dev/null | sed 's/\.tla//')

.PHONY: check-all tla clean

# Default: TLC on all backends
check:
	@for b in $(BACKENDS); do \
	    echo "=== TLC: $$b ==="; \
	    $(JAVA) -cp $(TLA2TOOLS_JAR) tlc2.TLC -deadlock $(TLC_OPTS) $$b.tla -config $$b.cfg 2>&1; \
	    echo; \
	done

# TLC with coverage
check-all: TLC_OPTS += -coverage 1
check-all: check

# TLC on a single backend: make BACKEND=TL2 check-one
check-one:
	@echo "=== TLC: $(BACKEND) ==="
	$(JAVA) -cp $(TLA2TOOLS_JAR) tlc2.TLC -deadlock $(TLC_OPTS) $(BACKEND).tla -config $(BACKEND).cfg 2>&1

# TLAPS proof check on a single backend
proof-one:
	@echo "=== TLAPS: $(BACKEND) ==="
	$(TLAPM) $(BACKEND).tla 2>&1

# Translate PlusCal to TLA+ for all PlusCal backends
tla:
	@for b in $(PLUSCAL_BACKENDS); do \
	    echo "=== pcal.trans: $$b ==="; \
	    $(JAVA) -cp $(TLA2TOOLS_JAR) pcal.trans -nocfg $$b 2>&1; \
	done

# Clean generated artifacts
clean:
	rm -rf *.old *_TTrace_* *.bin states/ states.* 2>/dev/null; true
