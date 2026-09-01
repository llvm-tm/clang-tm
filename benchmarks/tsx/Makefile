CC ?= gcc
CFLAGS := -O2 -mrtm -pthread -Wall -Wextra
TARGETS := tsx_conflict_matrix tsx_spurious

all: $(TARGETS)

tsx_conflict_matrix: tsx_conflict_matrix.c
	$(CC) $(CFLAGS) -o $@ $<

tsx_spurious: tsx_spurious.c
	$(CC) $(CFLAGS) -o $@ $<

# Run on a TSX-capable host; fails gracefully elsewhere.
check: all
	@echo "== RR =="; ./tsx_conflict_matrix RR 50000 || echo "no TSX (expected on non-RTM hosts)"
	@echo "== spurious =="; ./tsx_spurious 200000 0 || true

clean:
	rm -f $(TARGETS)

.PHONY: all check clean
