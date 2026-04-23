# Story 3.1: Position Search by Zobrist Hash

Status: done

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a user,
I want to look up any chess position by its Zobrist hash and retrieve all games in the database that reached that position,
so that I can instantly find every game where a specific position occurred.

## Acceptance Criteria

1. **Given** the DuckDB `position` table is populated,
   **when** `position_search::find` is called with a `zobrist_hash`,
   **then** the function returns a list of `(game_id, ply, result, white_elo, black_elo)` rows matching that hash (FR16),
   **and** the query completes in under 100ms at P99 for a database of 10M games (NFR01).

2. **Given** a Zobrist hash with no matching rows,
   **when** `position_search::find` is called,
   **then** an empty result set is returned (not an error).

3. **Given** any search,
   **then** Zobrist hash computation uses `chesslib` exclusively,
   **and** motif-chess never derives hashes independently.

4. **Given** all changes are implemented,
   **when** tests run,
   **then** all tests pass under `dev` and `dev-sanitize`,
   **and** all modified public API behavior has test coverage.

## Tasks / Subtasks

- [x] Task 1: Create the `motif_search` position search API surface (AC: 1, 2, 4)
  - [x] Add `source/motif/search/position_search.hpp` with the public `motif::search` API for lookup by Zobrist hash
  - [x] Add `source/motif/search/position_search.cpp` implementing the lookup via `motif_db` public APIs only
  - [x] Add `source/motif/search/error.hpp` defining `motif::search::error_code` and the module `result<T>` alias used by the public API
  - [x] Update `source/motif/search/CMakeLists.txt` to compile the new files and link `motif_db`
  - [x] Keep the API minimal and return `tl::expected<..., motif::search::error_code>`

- [x] Task 2: Preserve module boundaries and data contracts (AC: 1, 2, 3)
  - [x] Reuse `motif::db::position_match` as the returned row shape unless a clearly justified `motif::search` wrapper type is needed
  - [x] Ensure `motif_search` does not include Qt, SQLite, or DuckDB headers directly
  - [x] Ensure all storage access goes through `motif::db::database_manager` / `position_store` public APIs
  - [x] Do not introduce any new Zobrist hashing logic; callers must supply the hash computed by `chesslib`

- [x] Task 3: Add focused search tests (AC: 1, 2, 3, 4)
  - [x] Replace the `motif_search` placeholder test with real tests for successful lookup and empty-result lookup
  - [x] Use real temp-dir or in-memory database fixtures backed by `database_manager`
  - [x] Seed at least one known position through existing `motif_db` / import-safe mechanisms, then verify returned rows exactly match `game_id`, `ply`, `result`, `white_elo`, and `black_elo`
  - [x] Add a regression test that proves no-match search returns an empty vector, not an error

- [x] Task 4: Add performance guardrails for Story 3.1 (AC: 1, 4)
  - [x] Add or adapt `motif_search` performance coverage for Zobrist lookup latency using the existing large-corpus benchmark flow
  - [x] Reuse the sorted-by-zobrist benchmark baseline from Epic 2 as the expected storage layout
  - [x] Define and document the closest practical verification path for the AC's 10M-game / <100ms P99 target if a 10M corpus is not locally available
  - [x] Record new benchmark results in `BENCHMARKS.md` after implementation

### Review Findings

- [x] [Review][Patch] `position_search::find` test assumes stable row order without `ORDER BY` [`test/source/motif_search/position_search_test.cpp:193`]
- [x] [Review][Patch] Story 3.1 performance coverage depends on a machine-local corpus path and skips on other machines [`test/source/motif_search/position_search_test.cpp:53`]
- [x] [Review][Patch] `sprint-status.yaml` update introduced inconsistent YAML indentation in touched keys [`_bmad-output/implementation-artifacts/sprint-status.yaml:38`]
- [x] [Review][Defer] Opening a bundle with a missing or replaced `positions.duckdb` can silently succeed and make searches return false negatives [`source/motif/db/database_manager.cpp:391`] — deferred, pre-existing
- [x] [Review][Defer] Search query reads `ply` back with a signed 16-bit accessor even though rebuild accepts values up to `uint16_t` max [`source/motif/db/position_store.cpp:164`] — deferred, pre-existing

## Dev Notes

### Story Foundation

- Epic 3 starts immediately after Epic 2 completed the sorted-by-zobrist migration and concurrency validation.
- Story 3.1 should build on the current production reality, not the older ART-index design language still present in parts of the planning docs.
- The key implementation goal is a small, correct `motif_search` wrapper over existing `motif_db` query capability, not a new storage subsystem.

### Current Code State

- `source/motif/search/motif_search.cpp` is still a one-line placeholder and `test/source/motif_search/placeholder_test.cpp` is still a placeholder test.
- `motif::db::position_store` already exposes `query_by_zobrist(std::uint64_t) -> result<std::vector<position_match>>`.
- `motif::db::database_manager` already exposes `positions()` and is the public storage boundary that `motif_search` is expected to consume.
- Epic 2 established sorted-by-zobrist as the default rebuild layout and removed the ART index path.
- `motif_search` currently has no module-local error contract, so this story must define one before introducing a public API.

### Architectural Compliance

- `motif_search` owns search/query orchestration; `motif_db` owns storage and DuckDB details.
- `motif_search` must remain Qt-free and must not include SQLite or DuckDB headers directly.
- All storage access must go through `motif_db` public APIs; do not bypass `database_manager` or call DuckDB C API from `motif_search`.
- Trust `CONVENTIONS.md` over stale architecture wording where they disagree, especially on DuckDB API guidance.

### Technical Requirements

- Use `chesslib` exclusively for Zobrist hash computation; this story should not add any independent board-hash implementation.
- Favor the smallest correct API in `motif_search`; this story only needs position lookup, not opening stats or tree traversal.
- Return an empty result for a missing hash, not an error.
- Preserve the current sorted-by-zobrist storage assumption when reasoning about performance.
- Keep naming in `lower_snake_case`; no `k_` constants.
- Query the real DuckDB-backed `position` table only through `motif_db` public APIs; do not copy the stale `position_index` name from older planning text.

### Library / Framework Requirements

- C++23, Clang 21, `tl::expected`, Catch2 v3.
- DuckDB C API is banned outside `motif_db` internals by project convention and architecture boundary.
- No new dependencies.

### File Structure Requirements

Files most likely to change:
- `source/motif/search/CMakeLists.txt`
- `source/motif/search/motif_search.cpp` or its replacement/removal if the placeholder is no longer needed after adding real search sources
- `source/motif/search/error.hpp`
- `source/motif/search/position_search.hpp`
- `source/motif/search/position_search.cpp`
- `test/source/motif_search/placeholder_test.cpp`
- `test/source/motif_search/position_search_test.cpp`

Likely supporting files to read before implementation:
- `source/motif/db/database_manager.hpp`
- `source/motif/db/position_store.hpp`
- `source/motif/db/types.hpp`
- `BENCHMARKS.md`

### Testing Requirements

- Every public API function added in `motif_search` must have tests.
- Use real SQLite/DuckDB-backed fixtures via `database_manager`; no mocks for storage.
- Minimum verification flow:
  - `cmake --preset=dev`
  - `cmake --build build/dev`
  - `ctest --test-dir build/dev --output-on-failure`
- Sanitizer gate:
  - `cmake --preset=dev-sanitize`
  - `cmake --build build/dev-sanitize`
  - `ctest --test-dir build/dev-sanitize --output-on-failure`
- Add or adapt performance coverage so Story 3.1 can be checked against the <100ms P99 target directionally, and update `BENCHMARKS.md` with fresh measured numbers.
- If a 10M corpus is unavailable, the implementation notes and benchmark entry must state the exact corpus used and describe the remaining gap to full AC validation rather than implying the 10M target was fully proven.

### Git Intelligence Summary

- Recent commits are still Epic 2 closeout work: retrospective cleanup, review findings, malformed-game tests, performance benchmarks, and query latency work.
- The strongest carry-forward signal is that sorted-by-zobrist is now the intended query layout and already has benchmark evidence recorded in `BENCHMARKS.md`.

### Project Structure Notes

- `motif_search` already exists as a target but is effectively unimplemented.
- There is already a module-specific test location at `test/source/motif_search/`.
- `motif_db::position_store` already has the low-level query primitive needed for this story; the story should wrap and expose it cleanly rather than duplicating it.

### References

- [Source: `_bmad-output/planning-artifacts/epics.md#Story-31-Position-Search-by-Zobrist-Hash`]
- [Source: `_bmad-output/planning-artifacts/architecture.md#Module-Structure`]
- [Source: `_bmad-output/planning-artifacts/architecture.md#DuckDB-PositionIndex-Schema`]
- [Source: `_bmad-output/planning-artifacts/architecture.md#Strict-prohibitions`]
- [Source: `_bmad-output/planning-artifacts/architecture.md#FR-Category--Module-Mapping`]
- [Source: `source/motif/db/position_store.hpp`]
- [Source: `source/motif/db/database_manager.hpp`]
- [Source: `source/motif/db/types.hpp`]
- [Source: `source/motif/search/CMakeLists.txt`]
- [Source: `CONVENTIONS.md#DuckDB--C-API-only`]
- [Source: `CONVENTIONS.md#Module-boundaries`]
- [Source: `CONVENTIONS.md#Testing`]
- [Source: `BENCHMARKS.md`]

## Dev Agent Record

### Agent Model Used

openai/gpt-5.4

### Debug Log References

- Reviewed `epics.md` Story 3.1 acceptance criteria and Epic 3 context
- Reviewed `architecture.md` module structure, schema, and strict prohibitions
- Reviewed `CONVENTIONS.md` DuckDB/API boundary and testing rules
- Reviewed current `motif_search` placeholder source and tests
- Reviewed `motif_db` public APIs: `database_manager` and `position_store`
- `cmake --build build/dev -j 16 --target motif_search_test`
- `build/dev/test/motif_search_test`
- `cmake --build build/release -j 16 --target motif_search_test`
- `time build/release/test/motif_search_test "position_search::find performance on sorted position store"`
- `cmake --build build/dev -j 16 && ctest --test-dir build/dev --output-on-failure`
- `cmake --preset=dev-sanitize && cmake --build build/dev-sanitize -j 16 && ctest --test-dir build/dev-sanitize --output-on-failure`

### Completion Notes List

- Story file created from the first backlog entry in sprint order: `3-1-position-search-by-zobrist-hash`
- Story context is aligned with current post-Epic-2 code reality: sorted-by-zobrist is the storage baseline
- Developer guidance explicitly steers implementation toward a small `motif_search` wrapper over existing `motif_db` APIs
- `BENCHMARKS.md` is now treated as a maintained artifact and called out in this story
- Added `motif::search::error_code` and a minimal `position_search::find` API that wraps `motif_db::position_store::query_by_zobrist`
- Replaced the placeholder `motif_search` test with real success, empty-result, and performance coverage
- Marked `motif_db::position_store` read-only query methods as `const` so search can operate through a const database reference
- Recorded release benchmark results for the public search API on the 1M corpus and documented the remaining 10M AC validation gap

### Change Log

- 2026-04-22: Implemented `motif_search` position lookup API, replaced placeholder tests with real coverage, added performance guardrail benchmark, and updated `BENCHMARKS.md`

### File List

- `_bmad-output/implementation-artifacts/3-1-position-search-by-zobrist-hash.md`
- `_bmad-output/implementation-artifacts/sprint-status.yaml`
- `BENCHMARKS.md`
- `source/motif/db/position_store.cpp`
- `source/motif/db/position_store.hpp`
- `source/motif/search/CMakeLists.txt`
- `source/motif/search/error.hpp`
- `source/motif/search/position_search.cpp`
- `source/motif/search/position_search.hpp`
- `test/CMakeLists.txt`
- `test/source/motif_search/placeholder_test.cpp` (deleted)
- `test/source/motif_search/position_search_test.cpp`
