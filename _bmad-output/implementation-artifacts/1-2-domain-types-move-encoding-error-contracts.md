# Story 1.2: Domain Types, Move Encoding & Error Contracts

Status: done

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a developer,
I want the core domain types, move encoding, and error handling contracts defined in `motif_db`,
so that all other modules have a stable, shared foundation for representing chess games without duplicating or reinventing these definitions.

## Acceptance Criteria

1. **Given** the `motif_db` module scaffold exists
   **When** the domain types story is complete
   **Then** `source/motif/db/types.hpp` defines `game`, `player`, `event`, and `position` structs in the `motif::db` namespace with `lower_snake_case` members and `#pragma once`

2. **And** `source/motif/db/error.hpp` defines `motif::db::error_code` as an `enum class` covering at minimum: `ok`, `not_found`, `schema_mismatch`, `io_failure`, `duplicate`

3. **And** `source/motif/db/move_codec.hpp` wraps `chesslib::codec` encode/decode for `uint16_t` moves; tests verify round-trip encode→decode for all move types (quiet, capture, promotion, castling, en passant)

4. **And** all public functions return `tl::expected<T, motif::db::error_code>`; no exceptions thrown

5. **And** `motif_db_test` contains at least one test per public function; all pass under `dev-sanitize`

## Tasks / Subtasks

- [x] Define shared domain types in `motif_db` (AC: #1)
  - [x] Create `source/motif/db/types.hpp`
  - [x] Define `motif::db::player`, `motif::db::event`, `motif::db::position`, and `motif::db::game`
  - [x] Use `#pragma once`, `motif::db` namespace, and `lower_snake_case` field names throughout
  - [x] Make the type set sufficient for Story 1.3 (`game_store`) and later schema work without introducing SQLite/DuckDB-specific coupling into the type definitions

- [x] Define the public error contract (AC: #2, #4)
  - [x] Create `source/motif/db/error.hpp`
  - [x] Define `enum class error_code` with at least `ok`, `not_found`, `schema_mismatch`, `io_failure`, and `duplicate`
  - [x] Add any small helper API needed to make `error_code` usable in tests or logging without exceptions
  - [x] Ensure all new public functions introduced in this story use `tl::expected<T, error_code>`

- [x] Implement move codec wrapper (AC: #3, #4)
  - [x] Create `source/motif/db/move_codec.hpp`
  - [x] Add the public encode/decode wrapper functions around `chesslib::codec` for `uint16_t` move values
  - [x] Keep the wrapper API minimal and module-local to `motif_db` concerns; do not reimplement chess move encoding rules by hand
  - [x] If non-header implementation is needed, add a corresponding `.cpp` file and update `source/motif/db/CMakeLists.txt`

- [x] Wire `motif_db` build dependencies for Story 1.2 (AC: #3, #4, #5)
  - [x] Update `source/motif/db/CMakeLists.txt` so the module can include and build against `chesslib` and `tl::expected`
  - [x] Preserve `${PROJECT_SOURCE_DIR}/source` as the public include root
  - [x] Keep the module as a `STATIC` library and do not introduce Qt or any other higher-layer dependency
  - [x] Do not modify `flake.nix` without explicit approval; note that Nix already provides `chesslib` and `tl-expected`, while root `vcpkg.json` is not yet aligned

- [x] Replace placeholder `motif_db` tests with focused API tests (AC: #3, #5)
  - [x] Replace `test/source/motif_db/placeholder_test.cpp` with real tests or split into multiple files mirroring source concerns
  - [x] Add at least one test per public function introduced in `error.hpp` / `move_codec.hpp`
  - [x] Add round-trip encode/decode coverage for quiet, capture, promotion, castling, and en passant cases
  - [x] Keep Catch2 v3 style and module tag naming aligned with repo conventions

- [x] Verify clean build and sanitizers (AC: #5)
  - [x] `cmake --preset=dev && cmake --build build/dev`
  - [x] `ctest --test-dir build/dev`
  - [x] `cmake --preset=dev-sanitize && cmake --build build/dev-sanitize`
  - [x] `ctest --test-dir build/dev-sanitize`

## Dev Notes

### Architecture Notes

- `motif_db` owns shared types, schema, CRUD, and DB bundle lifecycle. It may depend on SQLite, DuckDB, chesslib, glaze, `tl::expected`, and fmt, but it must never include Qt headers.
- Include root remains `source/`; includes must use fully qualified paths such as `#include "motif/db/types.hpp"`.
- Test layout mirrors source layout. `motif_db_test` is the per-module executable already defined in `test/CMakeLists.txt`.

### Project Context Rules

- Error handling must use `tl::expected<T, E>` only. Do not use exceptions or `std::expected`.
- Naming conventions are strict: identifiers are `lower_case`; namespaces stay `motif::db`; use `#pragma once` in headers.
- Every public API function must have at least one test.
- C++20, const correctness, no raw owning pointers, and zero warnings under clang-tidy / cppcheck remain mandatory.

### Move Encoding Rules

- The move encoding is fixed by spec: 16-bit CPW-style encoding implemented in `chesslib::codec`.
- Do not invent a new move encoding or duplicate the bit-level encoding logic manually unless the wrapper strictly needs small adapter code.
- The acceptance criteria explicitly require round-trip coverage for quiet moves, captures, promotions, castling, and en passant.

### Dependency Notes

- `flake.nix` already includes `chesslib` and `tl-expected` in the Nix environment.
- Root `vcpkg.json` currently lists only `fmt` and test dependencies, so cross-platform dependency metadata is not yet aligned with the Nix environment.
- Repo rules say dependency additions must update both `flake.nix` and `vcpkg.json`, but `flake.nix` must not be modified without explicit approval. If Story 1.2 needs dependency metadata alignment beyond CMake wiring, call that out before changing dependency manifests.

### Type-Design Guidance

- Keep the types neutral and reusable for later stories (`game_store`, `database_manager`, DuckDB rebuild).
- Avoid baking persistence concerns such as table IDs, SQL row wrappers, or transport-specific formatting into the core types unless they are clearly part of the public `motif_db` API.
- Story 1.3 depends on these contracts, so favor stable, minimal interfaces over speculative abstractions.

### Testing Guidance

- Existing placeholder test file is only a scaffold from Story 1.1 and should be replaced by real tests in this story.
- Catch2 v3 only. Prefer clear module-tagged names like `"move_codec: encode/decode castling"`, `"error_code: expected contract"`.
- Sanitize builds are part of this story’s acceptance, not optional polish.

### Project Structure Notes

**Files to create:**
- `source/motif/db/types.hpp`
- `source/motif/db/error.hpp`
- `source/motif/db/move_codec.hpp`

**Files likely to modify:**
- `source/motif/db/CMakeLists.txt`
- `source/motif/db/motif_db.cpp` or replacement implementation files if the placeholder is removed
- `test/source/motif_db/placeholder_test.cpp` or replacement `motif_db` test files
- Root CMake/package wiring only if required to make `chesslib` / `tl::expected` visible to `motif_db`

### References

- [Source: `_bmad-output/planning-artifacts/epics.md` § "Story 1.2"] — user story and acceptance criteria
- [Source: `_bmad-output/planning-artifacts/architecture.md` § "Architectural Boundaries"] — module ownership and allowed dependencies
- [Source: `_bmad-output/planning-artifacts/architecture.md` § "Include Path Convention"] — include-root and include-style rules
- [Source: `_bmad-output/planning-artifacts/architecture.md` § "Test Organization"] — per-module test executable and naming conventions
- [Source: `_bmad-output/project-context.md` § "Technology Stack"] — `tl::expected`, Catch2 v3, in-house libraries, no exceptions
- [Source: `_bmad-output/project-context.md` § "Critical Implementation Rules / Naming conventions"] — lower_case rule and related style constraints
- [Source: `specs/001-database-schema/spec.md` § "Move Encoding"] — authoritative 16-bit move encoding definition
- [Source: `specs/001-database-schema/spec.md` § "Public API"] — prior design examples for game/domain types and storage APIs
- [Source: `source/motif/db/CMakeLists.txt`] — current module scaffold
- [Source: `test/CMakeLists.txt`] — existing `motif_db_test` harness

## Dev Agent Record

### Agent Model Used

claude-sonnet-4-6

### Debug Log References

- Story created after Story 1.1 was closed as done.
- Verified that Nix already provides `chesslib` and `tl-expected` for the motif-chess flake.
- Code review identified that the downstream workaround in `source/motif/db/CMakeLists.txt` was the wrong layer for fixing package metadata.
- Verified upstream `foolnotion/chesslib` packaging: `cmake/install-config.cmake` currently calls `find_dependency()` only for public dependencies (`magic_enum`, `small_vector`, `tl-expected`, `unordered_dense`) while exported target `chesslib::chesslib` also carries private link-only dependencies `fmt::fmt`, `libassert::assert`, and `std::mdspan`.
- Confirmed with `nix build .#default --no-link` that motif-chess fails during CMake configure when downstream relies on the broken exported package metadata.
- Removed the downstream `find_package(libassert)` / `find_package(mdspan)` workaround from `motif_db`. Correct fix belongs upstream in `foolnotion/chesslib` package config.
- Upstream chesslib package was fixed and motif-chess `flake.lock` was refreshed to the new chesslib revision.
- After consuming the fixed upstream package, motif-chess package builds required explicit Nix consumer visibility for `libassert`, `cpptrace`, and `libdwarf` in `packages.default.buildInputs` because the exported CMake dependency chain now resolves correctly through `chesslib -> libassert -> cpptrace`.
- Nix package builds then exposed a separate motif-chess packaging issue: the repo still had template install rules targeting a nonexistent `motif_exe`. Replaced them with real install rules for motif libraries and public headers, wired from the root `CMakeLists.txt`.
- Final validation passed: `nix build .#default --no-link`, `cmake --preset=dev && cmake --build build/dev && ctest --test-dir build/dev`, and `cmake --preset=dev-sanitize && cmake --build build/dev-sanitize && ctest --test-dir build/dev-sanitize` all succeed in the Nix environment.

### Completion Notes List

- Story 1.1 moved from `review` to `done`
- Story 1.2 marked `ready-for-dev` in sprint tracking
- Story 1.2 implementation artifact created with architecture, context, testing, and dependency notes
- All headers implemented: `types.hpp`, `error.hpp`, `move_codec.hpp` in `source/motif/db/`
- `error.hpp` provides `error_code` enum class and `result<T>` alias; `to_string()` helper for logging
- `move_codec.hpp` wraps `chesslib::codec::encode/decode`; all functions return `result<T>`
- `test/source/motif_db/move_codec_test.cpp` replaces placeholder; covers `error_code` string conversion and all 5 move types (quiet, capture, promotion, castling, en passant)
- `test/CMakeLists.txt` updated to use `move_codec_test.cpp`
- Original dev and sanitize test runs passed before code review.
- Code review follow-up removed a downstream CMake workaround and narrowed the blocker to upstream chesslib package metadata used by the Nix consumer path.
- Motif now consumes the fixed upstream chesslib revision through `flake.lock`.
- Nix package inputs were aligned with the exported dependency chain by adding `libassert`, `cpptrace`, and `libdwarf` to `packages.default` in `flake.nix`.
- Real install rules were added for motif libraries and headers so `nix build` can complete the install phase.
- Final validation passed for package, dev, and sanitize builds; story returned to `review`.
- Review was approved on 2026-04-17 and the story was marked `done`.

### File List

**Created:**
- `_bmad-output/implementation-artifacts/1-2-domain-types-move-encoding-error-contracts.md`
- `source/motif/db/types.hpp`
- `source/motif/db/error.hpp`
- `source/motif/db/move_codec.hpp`
- `test/source/motif_db/move_codec_test.cpp`

**Modified:**
- `CMakeLists.txt`
- `_bmad-output/implementation-artifacts/sprint-status.yaml`
- `_bmad-output/implementation-artifacts/1-1-project-scaffold-restructure.md`
- `cmake/install-rules.cmake`
- `flake.lock`
- `flake.nix`
- `source/motif/db/CMakeLists.txt`
- `test/CMakeLists.txt`

## Change Log

- 2026-04-15: Story created for Epic 1 / Story 1.2 and sprint tracking advanced from Story 1.1 to Story 1.2.
- 2026-04-16: Implementation complete. All headers, tests, and CMake wiring in place. Dev and sanitize builds passed.
- 2026-04-16: Code review follow-up removed downstream package-config workaround. Remaining blocker is upstream chesslib exported CMake package metadata for Nix consumers.
- 2026-04-16: Updated to the fixed upstream chesslib package, aligned Nix package inputs with the exported dependency chain, added real install rules, and revalidated package/dev/sanitize builds.
- 2026-04-17: Review approved and story status moved to `done`.