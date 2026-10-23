# ADR-0002: C++20 (with selective C++23), CMake, Ninja and presets

- **Status:** Proposed
- **Date:** 2026-10-19
- **Deciders:** Sivakannan, Claude

## Context

The engine is the performance-critical core: feed decoding, normalization, snapshots,
option chains, analytics and fan-out. It must build on macOS ARM64 (development) and
Linux x86-64 (target), and it is the main vehicle for learning modern C++ for
low-latency roles.

Two decisions are bundled here because they are made together: the **language
standard** and the **build system**.

## Options considered

### Language standard

**C++17**
- Pros: universally supported; still common in older trading codebases.
- Cons: no concepts, `std::span`, `<bit>`, `std::bit_cast`, coroutines, designated
  initializers, `<=>`. Misses what interviews increasingly expect.

**C++20**
- Pros: concepts, `std::span`, `std::bit_cast`/`std::byteswap` (C++23) building blocks,
  `<bit>`, `std::atomic_ref`, `std::jthread`/`stop_token`, `consteval`, ranges, `<=>`.
  Fully usable on current Clang, GCC and libc++/libstdc++. Widely adopted in HFT shops.
- Cons: modules are still impractical across toolchains (we won't use them).

**C++23 (whole standard)**
- Pros: `std::expected`, `std::byteswap`, `std::print`, `std::flat_map`, deducing `this`.
- Cons: library support still uneven between libc++ (macOS) and libstdc++ (Linux);
  some features (`flat_map`, `mdspan`) land at different times.

### Build system

**CMake** — the de-facto standard; every library we need ships CMake config; IDEs
(CLion, VS Code/clangd) understand it; produces `compile_commands.json`.
**Bazel** — hermetic and fast at scale, but heavy for a solo project and fights
vcpkg/Conan ecosystems. **Meson** — pleasant, but smaller ecosystem and less
common in industry.

## Decision

1. Compile with **`-std=c++23`** but treat **C++20 as the baseline**. A C++23 feature
   may be used only if it compiles on *every* CI configuration (see ADR-0004).
   Pre-approved today: `std::expected`, `std::byteswap`, `std::to_underlying`,
   `std::unreachable`.
2. No C++20 **modules** and no **coroutines on the hot path** (revisit later with benchmarks).
3. **CMake ≥ 3.28** with **Ninja** as the generator, configured through
   **`CMakePresets.json`** (`dev-debug`, `dev-asan`, `dev-tsan`, `release`, `ci-*`).
4. Target-based modern CMake: every library is a target with explicit
   `PUBLIC`/`PRIVATE` dependencies; no global `include_directories` or flags.

## Rationale

C++20 gives the tools that matter for this domain (concepts for clean adapter
interfaces, `span`/`bit_cast` for zero-copy binary decoding, atomics improvements)
without portability risk. Allowing proven C++23 features — especially
`std::expected` for error handling without exceptions on the data path — gets the
benefit while CI guards portability. CMake is what employers and libraries use;
presets make the many build flavours reproducible with one command.

## Consequences

- Positive: modern, interview-relevant code; one-command builds (`cmake --preset dev-debug`).
- Negative: C++23 usage needs discipline; CMake has a learning curve and many legacy idioms to avoid.
- Revisit: when GCC/libc++ support for a wanted C++23 feature lands, add it to the pre-approved list here (by superseding ADR).

## Learning notes

- Why `std::bit_cast` instead of `reinterpret_cast` for decoding packets (strict aliasing, UB).
- `std::expected` vs exceptions vs error codes on a latency-sensitive path.
- Modern CMake: targets, usage requirements, `PUBLIC` vs `PRIVATE` vs `INTERFACE`.
- Interview: "What C++20 features do you use day-to-day and why?"
