---
name: Bug report
about: Report a correctness bug, hang, crash, or misbehavior
title: "[bug] "
labels: bug
---

## Summary

<!-- One or two sentences: what is wrong and what you expected. -->

## Backend / pipeline

<!-- e.g. NOREC / TINYSTM / XTM; explicit API or LLVM plugin; C++ or Rust. -->

## Reproduction

<!-- Exact commands + flags that reproduce the issue. -->

```sh
# e.g. make -C benchmarks/cpp bin/test_tx BACKEND=NOREC
# e.g. ./benchmarks/cpp/bin/bank -a 128 -t 4
```

## Observed vs expected

- Observed:
- Expected:

## Environment

- OS / arch:
- LLVM version (if plugin):
- Rust version (if Rust):
- Commit / branch:

## Notes

<!-- Any diagnostics: TM_TRACE_PATH jsonl, sanitizer output, m5out/, etc. -->
