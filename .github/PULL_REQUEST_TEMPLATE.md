## Summary

<!-- What does this PR do? Link the issue / review-02 S-item it addresses. -->

## Type of change

- [ ] Correctness fix (needs a `docs/CORRECTNESS_FIXES.md` section)
- [ ] New backend
- [ ] New feature / tooling
- [ ] CI / docs
- [ ] Refactor / cleanup (no behavior change)

## Verification

- [ ] `make check-fast` passes
- [ ] `make fmt-check` passes
- [ ] Backend/pipeline-specific tests run:
      - C++: `make -C benchmarks/cpp bin/test_tx BACKEND=<B>` + `bin/test_ds`
      - Plugin: `make -C plugin run`
      - Rust: `cargo test --features <name> -p tm -- --test-threads=1`
      - Simulator: `cd simulator && cargo test`

## Notes

<!-- Anything a reviewer should know: design decisions, known limitations, follow-ups. -->
