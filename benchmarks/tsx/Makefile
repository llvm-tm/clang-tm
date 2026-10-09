CC ?= gcc
CFLAGS := -O2 -mrtm -pthread -Wall -Wextra
TARGETS := tsx_conflict_matrix tsx_spurious tsx_isolate tsx_sgl_ablation
GEM5_CC ?= gcc
GEM5_TARGETS := bin/tsx_conflict_matrix_gem5

all: $(TARGETS)

bin:
	mkdir -p bin

bin/tsx_conflict_matrix_gem5: tsx_conflict_matrix_gem5.c | bin
	$(GEM5_CC) $(CFLAGS) -static -o $@ $<

tsx_conflict_matrix: tsx_conflict_matrix.c
	$(CC) $(CFLAGS) -o $@ $<

tsx_spurious: tsx_spurious.c
	$(CC) $(CFLAGS) -o $@ $<

# Run on a TSX-capable host; fails gracefully elsewhere.
check: all
	@echo "== RR =="; ./tsx_conflict_matrix RR 50000 || echo "no TSX (expected on non-RTM hosts)"
	@echo "== spurious =="; ./tsx_spurious 200000 0 || true

clean:
	rm -f $(TARGETS) bin/tsx_conflict_matrix_gem5

.PHONY: all check clean gem5
gem5: $(GEM5_TARGETS)
