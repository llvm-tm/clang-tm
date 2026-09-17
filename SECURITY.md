# Security Policy

## Supported versions

| Component | Supported |
|-----------|-----------|
| `main` branch | Yes |
| LLVM plugin | LLVM 22 (required for CI); LLVM 23 is validated as a compatibility probe |
| C++ backends | The backends built by `make check-fast` and `make post-merge-check` |
| Rust workspace / simulator | Current Rust stable toolchain used by CI |

## Reporting a vulnerability

Please report security issues privately instead of opening a public issue:

1. Use GitHub's **Report a vulnerability** flow for this repository, if enabled.
2. Otherwise, open a minimal private reproduction and contact a maintainer
   through a private channel available to your organization.

Include:

- affected commit/tag and compiler/toolchain versions;
- a minimal reproduction command (`make check-fast`, a plugin benchmark, or a
  simulator test);
- sanitizer/ASan/UBSan output if available;
- impact and any suggested mitigation.

## Scope

Security reports may include memory-safety, concurrency, or sandbox-boundary
issues in:

- `plugin/passes/`, `plugin/runtime/`, `plugin/analysis/`;
- C++ backend runtimes under `backends/tm_impl/`;
- explicit C++/Rust API code under `explicit_api/`;
- the deterministic simulator under `simulator/`.

Benchmark harness bugs are usually not security vulnerabilities unless they
affect a library, CI runner, or build tool used by downstream projects.

## Automated dependency checks

GitHub Dependabot is configured for GitHub Actions and the Rust simulator
dependency lockfile. The `Nightly` workflow runs a weekly `cargo audit` job for
`simulator/Cargo.lock`.

## Disclosure

Maintainers aim to acknowledge reports within five business days, coordinate a
patch on a private branch when practical, and publish advisories or release
notes after the fix is available.

## Branch protection

`main` should require pull requests and passing `lint` and `plugin-test` CI
checks. Reviewers should pay extra attention to changes under `plugin/` and
`backends/tm_impl/common/`, because those paths affect instrumentation and
runtime hook contracts.
