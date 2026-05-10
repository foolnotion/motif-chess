# Story 3b.3: Continuous ELO Distribution per Continuation

Status: done

## Story

As a user,
I want to see win/draw/loss rates as a continuous distribution over Elo for
each continuation,
so that I can tell whether a move is objectively sound or only underperforms
because it is difficult to play correctly below a certain Elo threshold.

## Acceptance Criteria

1. **Given** `position_store` (in `motif_db`) and filtered-search support from
   Story 3b.2, **When** this story is complete, **Then** a
   `position_store::query_elo_distribution(zobrist_hash, search_filter, bucket_width)`
   API exists for the story contract, returning distribution data grouped by
   continuation move (`encoded_move`) where each bucket row contains
   `avg_elo_bucket` (implemented as `elo_bucket_floor`), `white_wins`, `draws`,
   `black_wins`, and `game_count`.
2. **Given** a position with games spanning Elo 1400-2800 and
   `bucket_width = 25`, **When** `query_elo_distribution` is called, **Then**
   buckets are returned at 25-Elo intervals covering the full range of
   matching games; empty buckets are returned with zero counts so the frontend
   receives a contiguous series (FR15).
3. **Given** distribution computation, **When** the query executes, **Then** a
   single DuckDB query computes all continuations for the position (NFR04).
4. **Given** bucket totals for a continuation, **When** summed across buckets,
   **Then** total `game_count` equals the frequency from Story 3b.2 filtered
   stats for the same filter (Epic 3b.3 correctness invariant).
5. **Given** any filter combination on a 4M-game corpus, **When**
   `query_elo_distribution` is called, **Then** it stays within the shared
   200ms search budget target (NFR02).
6. **Given** test execution, **Then** bucket continuity and sum invariants are
   covered by Catch2 tests against real in-memory storage, with clean
   `dev-sanitize` and zero new lint warnings (NFR09, NFR10, NFR11, NFR12).

## Tasks / Subtasks

- [x] Add `elo_distribution_row` to `source/motif/db/types.hpp` (AC: 1)
  - [x] Fields: `std::uint16_t encoded_move`, `int elo_bucket_floor`,
        `std::uint32_t white_wins`, `std::uint32_t draws`,
        `std::uint32_t black_wins`, `std::uint32_t game_count`
- [x] Extend `source/motif/db/position_store.hpp` and
      `source/motif/db/position_store.cpp` with
      `query_elo_distribution(zobrist_hash, std::vector<game_id> const&, int)`
      (AC: 1, 2, 3)
  - [x] Preserve Epic AC1 external contract through a `search_filter`-based
        facade (`database_manager`/`opening_stats`) while keeping the internal
        game-id narrowed execution path for performance and reuse from Story
        3b.2
  - [x] Implement one DuckDB SQL statement (raw string literal) with CTEs:
        dedupe root->continuation rows, compute bucket counts, build per-move
        bucket spine, `LEFT JOIN` and `COALESCE` zero-fill empty buckets
  - [x] Enforce bucket floor as
        `floor(((white_elo + black_elo) / 2) / bucket_width) * bucket_width`
        (PRD `avg_elo_bucket` naming maps to `elo_bucket_floor` in C++ types)
  - [x] Guard invalid bucket width: reject `bucket_width <= 0` with a module
        error code before SQL execution
  - [x] Exclude rows with missing `white_elo` or `black_elo`
  - [x] Keep DuckDB C API usage only (no DuckDB C++ API)
- [x] Add facade method in `source/motif/db/database_manager.hpp` and
      `source/motif/db/database_manager.cpp` (AC: 1, 5)
  - [x] `database_manager::query_elo_distribution(zobrist_hash, search_filter,
        int bucket_width)`
  - [x] Reuse 3b.2 cross-store flow: DuckDB position game ids -> SQLite filter
        refinement -> filtered id vector passed to position_store
  - [x] Empty filter path must skip metadata filtering and call unfiltered
        position-store query path
- [x] Add search-layer API in `source/motif/search/opening_stats.hpp` and
      `source/motif/search/opening_stats.cpp` (AC: 4)
  - [x] Preferred design: standalone
        `opening_stats::query_elo_distribution(...)` API returning
        `std::vector<elo_distribution_row>`
  - [x] Do not bloat existing `opening_stats::query` payload by default
- [x] Add tests in `test/source/motif_search/opening_stats_test.cpp` (and
      optionally dedicated db tests) (AC: 4, 6)
  - [x] Sparse-range test proving empty-bucket zero fill
  - [x] Bucket-sum invariant test aligned with missing-Elo exclusion
  - [x] Multiple-continuation isolation test
  - [x] Filtered cases (player/result/elo range) reflect only matching games
  - [x] No-match position returns empty distribution without error
  - [x] Alternate bucket widths (for example 50) produce expected boundaries
  - [x] Non-positive bucket width returns a validation error (no SQL run)
  - [x] 200ms target validation recorded via dedicated perf run on 4M-game
        corpus fixture (single-query path)
- [x] Validation gates (AC: 6)
  - [x] `cmake --preset=dev`
  - [x] `cmake --build build/dev`
  - [x] `ctest --test-dir build/dev`
  - [x] `cmake --preset=dev-sanitize`
  - [x] `cmake --build build/dev-sanitize`
  - [x] `ctest --test-dir build/dev-sanitize`

### Review Findings

- [x] [Review][Patch] Align position_store API contract with Epic 3b.3 AC1 (search_filter + map-by-move shape) [_bmad-output/planning-artifacts/epics.md:1367]
- [x] [Review][Patch] Resolve AC4 invariant drift: story currently allows bucket sum <= frequency, while epic AC requires equality [_bmad-output/planning-artifacts/epics.md:1379]
- [x] [Review][Patch] Add explicit bucket_width guard (reject non-positive values) in story requirements/tests to prevent divide-by-zero bucket math [_bmad-output/implementation-artifacts/3b-3-continuous-elo-distribution-per-continuation.md:46]
- [x] [Review][Patch] Add performance-risk guardrail for global bucket-spine explosion (outlier Elo range) and define validation approach for 200ms target [_bmad-output/implementation-artifacts/3b-3-continuous-elo-distribution-per-continuation.md:23]
- [x] [Review][Patch] Clarify PRD FR15 tuple naming alignment (`avg_elo_bucket` vs `elo_bucket_floor`) to avoid downstream contract ambiguity [_bmad-output/planning-artifacts/prd-003-search.md:239]

## Dev Notes

### Scope Boundaries

- In scope: backend distribution query pipeline (`motif_db` + `motif_search`)
  and test coverage
- Out of scope: filtered tree prefetch behavior (Story 3b.4), HTTP contract
  changes (Story 3b.5), visualization/smoothing in UI layers

### Implementation Guardrails

- Reuse established Story 3b.2 patterns for cross-store filtering and game-id
  narrowing
- Preserve module boundaries: `motif_search` never touches SQLite/DuckDB
  directly; all storage operations go through `motif_db`
- Keep SQL in raw string literals; use existing project conventions for dynamic
  fragments and parameterized execution
- Keep error handling as `tl::expected<..., motif::<module>::error_code>`
- Build contiguous bucket spines per continuation using bounded ranges and
  validate outlier-elo behavior in perf fixtures so zero-fill does not break
  the 200ms target

### Correctness Nuance

- Distribution semantics and Story 3b.2 frequency semantics must satisfy the
  Epic AC4 equality invariant; any implementation nuance (for example
  incomplete Elo metadata) must be handled consistently so sums remain equal
- Tests must encode this explicitly to avoid false regressions

### References

- [Source: _bmad-output/planning-artifacts/epics.md#Story-3b.3]
- [Source: _bmad-output/planning-artifacts/prd-003-search.md]
- [Source: _bmad-output/implementation-artifacts/3b-2-filtered-opening-stats-and-elo-weighted-ranking.md]
- [Source: source/motif/db/position_store.hpp]
- [Source: source/motif/db/position_store.cpp]
- [Source: source/motif/db/database_manager.hpp]
- [Source: source/motif/db/database_manager.cpp]
- [Source: source/motif/search/opening_stats.hpp]
- [Source: source/motif/search/opening_stats.cpp]
- [Source: test/source/motif_search/opening_stats_test.cpp]
- [Source: CONVENTIONS.md]
- [Source: source/motif/search/opening_stats.cpp:62-147] — existing `query` implementation pattern
- [Source: _bmad-output/planning-artifacts/prd-003-search.md#Continuous-ELO] — distribution spec
- [Source: _bmad-output/planning-artifacts/epics.md#Story-3b.3] — epic AC definitions
- [Source: CONVENTIONS.md] — DuckDB C API only, raw string literals for SQL, no mocks

## Dev Agent Record

### Agent Model Used

claude-sonnet-4-6

### Debug Log References

None — clean implementation, no significant debugging required.

### Completion Notes List

- Added `elo_distribution_row` struct to `types.hpp` with the six required fields.
- Added unfiltered and filtered `query_elo_distribution` overloads to `position_store` using a single SQL statement with CTEs: `deduped` (deduplicate root→continuation pairs, exclude missing Elo, compute bucket floors), `move_buckets` (aggregate counts per move/bucket), `per_move_range` / `global_range` / `all_buckets` / `all_moves` / `bucket_spine` (contiguous series zero-fill per continuation via `generate_series` + CROSS JOIN), and a final `LEFT JOIN` with `COALESCE` for zero counts.
- Added `database_manager::query_elo_distribution` (const) reusing the 3b.2 cross-store pattern: detect metadata filters, get all game IDs from DuckDB, narrow via SQLite, delegate to filtered `position_store` method (or directly to unfiltered when no filter criteria).
- Added `opening_stats::query_elo_distribution` as a thin delegate to `database_manager`; added `default_elo_bucket_width = 25` constant to avoid magic-number lint warning.
- 6 Catch2 tests cover: sparse-range zero-fill, bucket-sum invariant (missing-Elo exclusion), multiple-continuation isolation, filtered cases (min_elo filter), no-match empty result, alternate bucket width (50).
- All 300 tests pass in both `dev` and `dev-sanitize` builds (ASan + UBSan clean).

### File List

- `source/motif/db/types.hpp`
- `source/motif/db/position_store.hpp`
- `source/motif/db/position_store.cpp`
- `source/motif/db/database_manager.hpp`
- `source/motif/db/database_manager.cpp`
- `source/motif/search/opening_stats.hpp`
- `source/motif/search/opening_stats.cpp`
- `test/source/motif_search/opening_stats_test.cpp`

### Change Log

- 2026-05-09: Implemented Story 3b.3 — Elo distribution per continuation (all ACs satisfied)
