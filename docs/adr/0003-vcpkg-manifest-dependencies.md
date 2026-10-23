# ADR-0003: Manage C++ dependencies with vcpkg in manifest mode

- **Status:** Proposed
- **Date:** 2026-10-19
- **Deciders:** Sivakannan, Claude

## Context

Expected third-party libraries: Boost (Beast/Asio), OpenSSL, spdlog/fmt, yaml-cpp,
simdjson, GoogleTest, Google Benchmark, later a storage engine (SQLite/DuckDB).
Builds must be **reproducible** on macOS ARM64, Linux x86-64 and CI, with the same
versions everywhere.

## Options considered

### Option A — System packages (Homebrew / apt)
- Pros: zero setup, fast.
- Cons: different versions on each machine; Homebrew updates silently; not reproducible.

### Option B — CMake `FetchContent` / CPM.cmake
- Pros: no extra tool; source pinned by git tag; great for small header-only libs.
- Cons: rebuilds dependencies inside our build tree; Boost and OpenSSL are painful this way; no binary cache.

### Option C — Conan 2
- Pros: powerful, binary packages, profiles for cross-compilation; used in industry.
- Cons: requires Python and its own recipe/profile concepts; more moving parts.

### Option D — vcpkg (manifest mode)
- Pros: `vcpkg.json` in the repo declares dependencies; `builtin-baseline` pins every
  version to one commit of the registry; integrates as a CMake toolchain file;
  binary caching for CI; large catalogue (Boost, OpenSSL, everything above).
- Cons: builds from source the first time (Boost takes a while); custom triplets needed
  for fine-grained compiler flags.

## Decision

Use **vcpkg in manifest mode** (Option D):

- `vcpkg.json` at the repo root, with a pinned `builtin-baseline`.
- vcpkg wired via `CMAKE_TOOLCHAIN_FILE` in `CMakePresets.json` (path from `$VCPKG_ROOT`).
- Only Boost **components** we use (`boost-beast`, `boost-asio`, …), never all of Boost.
- CI uses vcpkg binary caching so dependency builds happen once.
- Every new dependency must be justified (README of the module or an ADR).

## Rationale

Reproducibility across two OSes and CI is the top requirement, and vcpkg delivers it
with the fewest moving parts for a CMake project. It is widely used, so it is a
transferable skill. Conan is a fine alternative; we can revisit if we need advanced
cross-compilation or prebuilt binaries for deployment.

## Consequences

- Positive: same dependency versions everywhere; one command to set up a new machine.
- Negative: first build is slow (minutes for Boost/OpenSSL); vcpkg must be installed and `VCPKG_ROOT` set.
- Revisit if: build times become a daily pain, or deployment needs prebuilt packages.

## Learning notes

- How toolchain files and `find_package(... CONFIG)` work.
- ABI compatibility: why all dependencies should be built with the same compiler and standard library.
- Interview: "How do you keep builds reproducible across machines?"
