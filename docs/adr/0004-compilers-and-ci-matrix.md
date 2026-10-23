# ADR-0004: Compilers, warnings, sanitizers and CI matrix

- **Status:** Proposed
- **Date:** 2026-10-19
- **Deciders:** Sivakannan, Claude

## Context

We develop on macOS ARM64 but target Linux x86-64, and no Linux machine is available
for about a month. Portability bugs (compiler differences, standard-library
differences, architecture differences) should be caught on every push, not at
deployment time. Concurrency and memory bugs should be caught by tools, not by luck.

Architecture differences that matter to us:

| Difference | ARM64 (Mac) | x86-64 (Linux) |
|---|---|---|
| Memory model | Weak — reorders loads/stores | Strong (TSO) — hides many ordering bugs |
| `char` signedness | unsigned | signed |
| Cache line | 128 B effective on Apple M-series | 64 B |

A lock-free bug may only appear on ARM; a `char` bug may only appear on x86.
Testing on both is a real advantage, not a cost.

## Options considered

- **Apple Clang** — ships with Xcode, matches the system libc++; supports ASan, UBSan, TSan (not LSan/MSan); no `clang-tidy`/`clang-format`.
- **Homebrew LLVM on macOS** — latest upstream Clang and all tools, but building with it
  mixes its libc++ headers with the system libc++ runtime unless extra link flags are used.
- **GCC on Linux** — the target compiler in most trading firms; different warnings and libstdc++.
- **Clang on Linux** — catches Clang-specific issues on the target OS.

## Decision

**Local development (macOS):** **Apple Clang** (Xcode command-line tools) builds the
code. It is current (Clang 21), supports ASan/UBSan/TSan, and avoids the libc++
header/runtime mismatch that Homebrew LLVM causes on macOS. **Homebrew LLVM** is
installed only for its tools: `clang-format`, `clang-tidy`, `clangd`.

**CI (GitHub Actions)** on every push and pull request:

| Job | OS | Compiler | Build | Purpose |
|---|---|---|---|---|
| macos-clang | macOS ARM64 | Apple Clang | Debug + ASan/UBSan | Dev platform parity |
| linux-gcc | Ubuntu x86-64 | GCC 14 | Debug + ASan/UBSan | Target compiler |
| linux-clang-tsan | Ubuntu x86-64 | Clang 19 | TSan | Data races |
| linux-gcc-release | Ubuntu x86-64 | GCC 14 | Release | Optimized build (+ benchmark smoke run once benchmarks exist) |
| lint | Ubuntu | clang-format, gitleaks (clang-tidy later) | — | Style, secrets, static analysis |

**Warnings:** `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
-Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference`;
**`-Werror` in CI only** (so local experiments aren't blocked). Third-party headers
are included as `SYSTEM` so they don't trigger our warnings.

**Release flags:** `-O2` by default; `-O3`, LTO and `-march` are decided per
benchmark, never by assumption. CI never uses `-march=native`.

**Formatting:** `.clang-format` checked in; formatting is enforced in CI.

## Rationale

Two OSes × two compilers × two architectures catches most portability bugs early,
and sanitizers in CI make memory and race bugs visible without a Linux machine.
`-Werror` only in CI keeps local iteration fast while ensuring nothing warning-laden
is merged.

## Consequences

- Positive: portability and correctness checked continuously; free for public repos.
- Negative: CI takes several minutes; some sanitizer/platform combinations are flaky and may need suppressions.
- Revisit: add a self-hosted runner on the HP Linux laptop for real performance benchmarks (shared CI runners are too noisy for latency numbers).

## Learning notes

- x86 TSO vs ARM weak memory model, and why `memory_order_relaxed` code can pass on x86 and fail on ARM.
- What ASan, UBSan and TSan instrument, and their runtime cost.
- Why benchmark numbers from shared CI runners are not trustworthy.
- Interview: "How would you catch a data race that only shows up in production?"
