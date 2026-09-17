# Plugin tests and local pipeline comparisons

Files in this directory are split by purpose:

- `test_*.cpp` and `test_*.sh` — correctness tests run by the plugin test suite
  or manually during debugging. These should fail when TM instrumentation is
  missing or wrong.
- `bench_*.cpp` — pipeline/performance comparisons that happen to live near
  related tests. Treat their output as measurements, not assertions.

New performance workloads should normally go under `benchmarks/plugin/`. Use a
`bench_*` source file here only when it is a small local comparison tied to an
existing correctness test.
