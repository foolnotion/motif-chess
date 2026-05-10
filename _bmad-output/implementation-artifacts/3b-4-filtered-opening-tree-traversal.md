# Story 3b.4: Filtered Opening Tree Traversal

Status: in-progress

## Story

As a user,
I want the opening tree to reflect only positions and statistics from games matching my filter,
so that when I filter by opponent, the tree shows their actual repertoire rather than the full database.

## Acceptance Criteria

1. **Given** `opening_tree::open` with an optional `search_filter` parameter, **When** the filter is set, **Then** all tree node statistics (frequency, white_wins, draws, black_wins, average_white_elo, average_black_elo, elo_weighted_score) are computed over the filtered game set. **And** the filter is passed through to all underlying `position_store` and `opening_stats` calls. **And** Elo-weighted score (from Story 3b-2) is included in each `node_continuation`.

2. **Given** `opening_tree::expand` with an optional `search_filter` parameter, **When** a lazy node expansion is triggered, **Then** the same filter is applied to the on-demand DuckDB query for that node's children (FR16).

3. **Given** an empty or absent `search_filter`, **When** `opening_tree::open` or `expand` is called, **Then** behavior is identical to the pre-3b implementation — no regression.

4. **Given** a filtered tree opened with `prefetch_depth = 3` on a 4M-game corpus, **When** `opening_tree::open` is called, **Then** the call completes in under 500ms (NFR03).

5. **Given** a filter that matches zero games at a node, **When** that node is included in the tree, **Then** the node has zero continuations and is marked `is_expanded = true` — not an error.

6. **Given** any test scenario, **Then** filtered tree node statistics match manually computed values from the filtered game set. **And** all tests pass under `cmake --preset=dev-sanitize` (NFR12). **And** zero new clang-tidy or cppcheck warnings (NFR11).

## Tasks / Subtasks

- [x] Task 1: Add `elo_weighted_score` to `node_continuation` struct (AC: #1)
  - [x] Add `double elo_weighted_score {0.0};` field to `node_continuation` in `opening_tree.hpp`
  - [x] Update `build_continuation` helper in `opening_tree.cpp` to populate the field from the aggregate
  - [x] Update `expand` to populate `elo_weighted_score` from `opening_stats::continuation`
- [x] Task 2: Add `elo_weighted_score` to in-memory aggregation (AC: #1)
  - [x] Add `double weighted_contrib_sum {0.0}` and `double elo_weight_sum {0.0}` to `tree_continuation_aggregate` in `opening_tree.cpp`
  - [x] Update `note_elo` (or add a new helper) to accumulate: `weighted_contrib_sum += result * (white_elo + black_elo) / 2.0` and `elo_weight_sum += (white_elo + black_elo) / 2.0` when both Elo values are present
  - [x] Compute score in `build_continuation`: `elo_weighted_score = elo_weight_sum > 0 ? weighted_contrib_sum / elo_weight_sum : 0.0`
- [x] Task 3: Add filtered overload to `opening_tree::open` (AC: #1, #3, #4)
  - [x] Add overload: `open(database_manager const&, zobrist_hash, size_t prefetch_depth, search_filter const&)` to `opening_tree.hpp`
  - [x] Implementation: if filter is structurally empty (all `search_filter` optionals unset), delegate to existing unfiltered `open()` — zero regression risk
  - [x] If filter has metadata fields: uses cross-store pattern (DuckDB → SQLite → DuckDB filtered slice)
  - [x] Zero games matching: if filtered game IDs are empty, return tree with root only, `is_expanded = true`, zero continuations (AC #5)
- [x] Task 4: Add filtered overload to `opening_tree::expand` (AC: #2, #3)
  - [x] Add overload: `expand(database_manager const&, node&, search_filter const&)` to `opening_tree.hpp`
  - [x] Implementation: call `opening_stats::query(database, n.zobrist_hash, filter)` (the 3b-2 overload)
  - [x] Populate `elo_weighted_score` in the resulting `node_continuation`
  - [x] Existing 2-param `expand` delegates to the 3-param version with empty filter
- [x] Task 5: Tests — filtered tree open (AC: #4, #5, #6)
  - [x] Test: unfiltered `open` returns identical tree to pre-3b behavior (regression guard)
  - [x] Test: filter by player_name — tree reflects only that player's games at each node
  - [x] Test: filter by min_elo/max_elo — nodes only include games within Elo range
  - [x] Test: filter by result — only games with matching result contribute
  - [x] Test: filter matching zero games at root — returns tree with root only, `is_expanded = true`
  - [x] Test: filter matching zero games at an interior node (some games at root but not at deeper position) — that node has zero continuations
  - [x] Test: filtered `open` with `prefetch_depth = 0` — returns unexpanded root regardless of filter
  - [x] Test: `elo_weighted_score` present in node continuations — matches manually computed value
  - [x] Test: filtered tree stats match manually computed values from filtered game set
- [x] Task 6: Tests — filtered tree expand (AC: #2, #6)
  - [x] Test: `expand` with filter produces correct children matching filtered stats
  - [x] Test: `expand` without filter (backward compat) produces same children as before
  - [x] Test: `expand` with filter on already-expanded node — returns immediately, no duplicate children
  - [x] Test: `expand` with filter that matches zero games — node gets zero continuations, `is_expanded = true`
- [x] Task 7: Build and lint validation (AC: #6)
  - [x] `cmake --preset=dev && cmake --build build/dev` — zero warnings (source files)
  - [x] `ctest --test-dir build/dev` — all 312 tests pass
  - [x] `cmake --preset=dev-sanitize && cmake --build build/dev-sanitize && ctest --test-dir build/dev-sanitize` — all 313 tests pass, zero violations

## Dev Notes

### Scope Boundary

This story threads `search_filter` through `opening_tree::open` and `opening_tree::expand`. It also adds `elo_weighted_score` to `node_continuation`. It does **not** implement:
- Elo distribution per continuation in tree nodes (caller fetches separately via 3b-3 API)
- HTTP endpoint changes (story 3b-5)
- Any Qt or web frontend changes
- Changes to the prefetch algorithm or depth semantics

### Filter Strategy for `opening_tree::open`

The `open()` function works in two phases:
1. **DuckDB query** (`query_tree_slice`) fetches ALL position rows within depth
2. **In-memory aggregation** groups rows by (game_id, root_ply) and builds aggregates per node

Adding filter support has two approaches:

**Approach A — filter at SQL level (recommended):**
- Use the filtered `query_tree_slice(hash, depth, filtered_game_ids)` from story 3b-2
- Only matching game rows come back from DuckDB — less data transfer, faster for small filtered sets
- In-memory aggregation is unchanged — it just works on a smaller input

**Approach B — filter in memory after query:**
- Call unfiltered `query_tree_slice`, get all rows, then discard rows whose game_id is not in the filtered set
- Simpler code but pulls full dataset from DuckDB — defeats the purpose of filtering at 4M-game scale

Use Approach A. It requires the filtered `query_tree_slice` overload from story 3b-2 to be implemented first.

### Cross-Store Filter Flow for `open`

```
1. distinct_game_ids_by_zobrist(root_hash)        → vector<game_id>  (DuckDB)
2. find_games_with_ids(game_ids, metadata_filter)  → game_list_result (SQLite)
3. Extract game IDs from result.games              → vector<game_id>
4. query_tree_slice(root_hash, depth, filtered_ids) → vector<tree_position_row> (DuckDB)
5. In-memory aggregation → build tree (unchanged)
```

This mirrors the `database_manager::find_games` pattern from 3b-1 and the `opening_stats::query` filter pattern from 3b-2.

### Elo-Weighted Score in In-Memory Aggregation

The existing `tree_continuation_aggregate` accumulates `white_elo_sum`, `white_elo_count`, `black_elo_sum`, `black_elo_count` for averages. Add two more accumulators for the weighted score:

```cpp
double weighted_contrib_sum {0.0};  // Σ (result × avg_elo)
double elo_weight_sum {0.0};        // Σ avg_elo
```

In `note_elo` or a new helper, when both Elo values are present:
```cpp
auto const avg_elo = (static_cast<double>(*white_elo) + static_cast<double>(*black_elo)) / 2.0;
weighted_contrib_sum += static_cast<double>(result) * avg_elo;
elo_weight_sum += avg_elo;
```

Then in `build_continuation`:
```cpp
.elo_weighted_score = aggregate.elo_weight_sum > 0.0
    ? aggregate.weighted_contrib_sum / aggregate.elo_weight_sum
    : 0.0
```

**Important:** The `result` parameter must be passed to the accumulation function alongside Elo. Currently `note_result` and `note_elo` are called separately — merge into a single `note_result_and_elo(aggregate, result, white_elo, black_elo)` or pass result to `note_elo`.

### `expand` Filter Wiring

Story 3b-2 adds `opening_stats::query(database, hash, filter)`. The filtered `expand` just calls that overload:

```cpp
auto expand(database_manager const& database, node& n, search_filter const& filter) -> result<void>
{
    if (n.is_expanded) { return {}; }
    auto stats_res = opening_stats::query(database, n.zobrist_hash, filter);
    // ... same conversion loop as existing expand, plus elo_weighted_score ...
}
```

The existing 2-param `expand`:
```cpp
auto expand(database_manager const& database, node& n) -> result<void>
{
    return expand(database, n, search_filter {});
}
```

### `node_continuation` Field Addition

Add `double elo_weighted_score {0.0};` to `node_continuation` in `opening_tree.hpp`. This field is populated both in the `open` path (via in-memory aggregation) and the `expand` path (via `opening_stats::query` result).

**Also update `expand`** to copy `elo_weighted_score` from `opening_stats::continuation` when building `node_continuation` objects. The existing `expand` copies fields one-by-one — add the new field to that initialization list.

### Empty Filter = Zero Regression

Both new overloads check if the `search_filter` has no metadata fields (all optional fields are nullopt). If empty, delegate to the existing unfiltered methods. This guarantees NFR08: empty filter produces identical results to absent filter, and existing callers are unaffected.

### Zero-Match Node Behavior (AC #5)

When a filter matches zero games at a position, `open` returns a tree with:
- Root node at the given hash
- `continuations` is empty
- `is_expanded = true` (there's nothing to expand — we confirmed there are zero matching continuations)

This is not an error condition. The frontend shows an empty tree.

For `expand` with zero matches: the node gets `is_expanded = true` with an empty `continuations` vector.

### Performance Consideration

The filtered `open` adds an extra SQLite round-trip (metadata filtering) before the DuckDB query. For a 4M-game corpus:
- DuckDB `distinct_game_ids_by_zobrist` at the starting position returns ~4M IDs (common case)
- SQLite `find_games_with_ids` with a player_name filter scans those IDs with an IN clause — batched at 999
- DuckDB `query_tree_slice` with filtered IDs is fast (column scan + filter)

The 500ms NFR03 budget should be achievable. The bottleneck is likely the SQLite IN-clause filtering for large game ID sets. If needed, the temp-table approach from 3b-1 can be used here too.

### NFR03 Verification Evidence

- Acceptance target tracked in this story: filtered `opening_tree::open` with `prefetch_depth = 3` on a 4M-game corpus under 500ms (AC #4).
- Coverage in implementation: Story 3b.4 adds functional filtered-open tests and keeps existing `opening_tree` performance test coverage (`test/source/motif_search/opening_tree_test.cpp`, `[performance]` cases).
- Current verification status: this story artifact does not include a dedicated recorded 4M filtered-open benchmark measurement; performance evidence must be captured in a follow-up benchmark log entry before story status moves from `review` to `done`.

### Dependency on Story 3b-2

This story requires the filtered `query_tree_slice` overload and the filtered `opening_stats::query` overload from story 3b-2. Both must be complete before this story starts.

### Previous Story Intelligence

- **Story 3b-1:** `search_filter`, `game_store::find_games_with_ids`, cross-store filter pattern, batch-IN-clause
- **Story 3b-2:** Filtered `position_store::query_opening_stats` and `query_tree_slice`, `elo_weighted_score` in SQL, `opening_stats::query` overload with filter
- **Story 3 (opening_tree):** `open()` uses `query_tree_slice` + in-memory aggregation + forward BFS for board states + bottom-up node construction. `expand()` delegates to `opening_stats::query`. The board-lifetime fix (avoid caching `chesslib::board` values across rehashes) is in place.
- **Board lifetime lesson from 3b-1 review:** Copy the parent board before inserting children into `board_at_hash` (already fixed in current code at line 337: `auto const parent_board = parent_it->second;`)

### References

- [Source: source/motif/search/opening_tree.hpp] — `node_continuation`, `node`, `tree` structs; `open` and `expand` signatures
- [Source: source/motif/search/opening_tree.cpp:179-396] — `open` implementation: query_tree_slice → in-memory aggregation → forward BFS → bottom-up build
- [Source: source/motif/search/opening_tree.cpp:398-431] — `expand` implementation: delegates to `opening_stats::query`
- [Source: source/motif/search/opening_tree.cpp:70-85] — `tree_continuation_aggregate` struct to extend with elo_weighted_score accumulators
- [Source: source/motif/search/opening_tree.cpp:110-122] — `note_elo` helper to extend
- [Source: source/motif/search/opening_tree.cpp:132-158] — `build_continuation` helper to extend
- [Source: source/motif/db/position_store.hpp:27] — `query_tree_slice` signature (filtered overload from 3b-2)
- [Source: source/motif/search/opening_stats.hpp:19-31] — `continuation` struct with `elo_weighted_score` (from 3b-2)
- [Source: source/motif/db/types.hpp:142-153] — `search_filter` struct
- [Source: source/motif/db/database_manager.cpp:534-552] — cross-store filter pattern
- [Source: _bmad-output/planning-artifacts/epics.md#Story-3b.4] — epic AC definitions
- [Source: _bmad-output/planning-artifacts/prd-003-search.md#FR16] — filter passthrough to tree
- [Source: CONVENTIONS.md] — DuckDB C API only, no mocks, no Qt in motif_search

## Dev Agent Record

### Agent Model Used

claude-sonnet-4-6

### Debug Log References

- `cmake --preset=dev && cmake --build build/dev`
- `ctest --test-dir build/dev`
- `cmake --preset=dev-sanitize && cmake --build build/dev-sanitize`
- `ctest --test-dir build/dev-sanitize`

### Completion Notes List

- Added `elo_weighted_score` field to `node_continuation` struct in opening_tree.hpp
- Extended `tree_continuation_aggregate` with `weighted_contrib_sum`/`elo_weight_sum` accumulators; updated `note_elo` signature to accept `result` for joint accumulation
- Implemented filtered `open` overload: cross-store pattern (DuckDB `distinct_game_ids_by_zobrist` → SQLite `find_game_ids_with_filter` → DuckDB `query_tree_slice` with filtered IDs)
- Empty filter delegates to existing unfiltered `open`; zero-match returns `is_expanded=true` root with no continuations
- Wired `elo_weighted_score` into `expand` (was already implemented with filter in 3b-2; added field copy from `opening_stats::continuation`)
- 13 new test cases: 9 for filtered `open`, 4 for filtered `expand`; all pass under dev and dev-sanitize builds
- 312/312 dev tests pass, 313/313 sanitize tests pass; zero new clang-tidy/cppcheck warnings in source code
- NFR11 evidence source: clang-tidy/cppcheck checks run through the `dev` preset build gate (`cmake --build build/dev`) and remained clean for touched files in this story

### File List

- source/motif/search/opening_tree.hpp
- source/motif/search/opening_tree.cpp
- test/source/motif_search/opening_tree_test.cpp

### Change Log

- 2026-05-09: Story 3b.4 implemented — filtered opening tree traversal with elo_weighted_score in node_continuation

### Review Findings

- [x] [Review][Patch] Clarify "empty filter" semantics beyond metadata-only checks [_bmad-output/implementation-artifacts/3b-4-filtered-opening-tree-traversal.md:37]
- [x] [Review][Patch] Add explicit NFR03 benchmark evidence (<500ms on 4M corpus) [_bmad-output/implementation-artifacts/3b-4-filtered-opening-tree-traversal.md:19]
- [x] [Review][Patch] Add lint evidence references for NFR11 claim (clang-tidy/cppcheck) [_bmad-output/implementation-artifacts/3b-4-filtered-opening-tree-traversal.md:225]
- [x] [Review][Defer] Guard very large root match sets to avoid memory spikes [_bmad-output/implementation-artifacts/3b-4-filtered-opening-tree-traversal.md:176] — deferred, pre-existing
- [x] [Review][Defer] Add fallback path for large SQLite IN filtering to protect NFR03 [_bmad-output/implementation-artifacts/3b-4-filtered-opening-tree-traversal.md:177] — deferred, pre-existing
- [x] [Review][Defer] Validate contradictory Elo bounds (min_elo > max_elo) [_bmad-output/implementation-artifacts/3b-4-filtered-opening-tree-traversal.md:160] — deferred, pre-existing
- [x] [Review][Defer] Deduplicate filtered IDs before aggregation for defensive correctness [_bmad-output/implementation-artifacts/3b-4-filtered-opening-tree-traversal.md:108] — deferred, pre-existing
- [x] [Review][Defer] Cap prefetch depth to protect against explosive traversal [_bmad-output/implementation-artifacts/3b-4-filtered-opening-tree-traversal.md:35] — deferred, pre-existing
