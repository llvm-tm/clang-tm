# `eager_broken` backend status

**Status:** `teaching` (intentionally broken — never sweep/benchmark with it)

Trivially eager STM for the book's non-opacity worked example (Ch. 2,
`bin/opaquedemo`) and the `docs/proofs/EagerSTM` TLA+ model. Writes
publish immediately; commit validates values and undoes on abort.
Provides NO opacity by construction. Excluded from `check-fast` /
`check-all` on purpose; `test_tx` failures against it are expected and
are themselves the lesson.
