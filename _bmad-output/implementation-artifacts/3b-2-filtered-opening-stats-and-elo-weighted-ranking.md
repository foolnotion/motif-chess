# Story 3b.2: Filtered Opening Stats & ELO-Weighted Ranking

Status: done

## Story

As a user,
I want opening statistics at any position to reflect only games matching my current filter, with continuations ranked by an Elo-weighted score,
so that I can see how strong players handle each position and identify which moves are objectively best rather than just most popular.

## Acceptance Criteria

1. **Given** `opening_stats::query` with an optional `search_filter` parameter, **When** the filter is set, **Then** all statistics (frequency, white_wins, draws, black_wins, average_white_elo, average_black_elo) are computed over the filtered game set only. **And** the `continuation` struct gains a `double elo_weighted_score` field.

2. **Given** an empty or absent `search_filter`, **When** `opening_stats::query` is called, **Then** behavior is identical to the pre-3b implementation — no regression (FR12, NFR08).

3. **Given** the ELO-weighted score, **When** it is computed for a continuation, **Then** the formula is: `score = Σ(result_value(g) × avg_elo(g)) / Σ avg_elo(g)`, where `result_value` is +1 (side to move wins), 0 (draw), −1 (side to move loses), and `avg_elo(g) = (white_elo + black_elo) / 2` for games where both are available; games with missing Elo data are excluded from the weighted score computation but included in frequency counts. **And** the score is returned as a field for the caller to use as a sort key — the API does not sort; sort order is the caller's responsibility (FR13, FR14).

4. **Given** two continuations with equal ELO-weighted score, **When** the caller sorts by score, **Then** ties are broken by frequency (descending), then SAN alphabetically (NFR07).

5. **Given** a position with a known game set, **When** `opening_stats::query` is called with a filter, **Then** the returned stats exactly match manually computed values from the filtered game set (NFR06).

6. **Given** any filter combination on a 4M-game corpus, **When** `opening_stats::query` is called, **Then** the query completes in under 200ms (NFR02).

7. **Given** any test scenario, **Then** all new public functions have at least one Catch2 v3 test (NFR09). **And** tests use real in-memory DuckDB — no mocks (NFR10). **And** all tests pass under `cmake --preset=dev-sanitize` (NFR12). **And** zero new clang-tidy or cppcheck warnings (NFR11).

## Tasks / Subtasks

- [x] Task 1: Add `elo_weighted_score` to `continuation` struct (AC: #1, #3)
  - [x] Add `double elo_weighted_score {0.0};` to `opening_stats::continuation` in `opening_stats.hpp`
  - [x] Add `std::optional<double> elo_weighted_score;` to `opening_stat_agg_row` in `types.hpp` (carries the computed value from DuckDB)
- [x] Task 2: Add filtered overload to `position_store::query_opening_stats` (AC: #1, #5)
  - [x] Add `query_opening_stats(zobrist_hash hash, std::vector<game_id> const& game_ids) const` to `position_store` — filters DuckDB query to only the given game IDs
  - [x] Add `count_distinct_games_by_zobrist(zobrist_hash hash, std::vector<game_id> const& game_ids) const` — filtered count variant
  - [x] Modify the SQL: add `AND p_root.game_id IN (...)` clause to the `deduped` CTE and corresponding `AND p.game_id IN (...)` to the `child_agg` CTE's subquery
  - [x] Add Elo-weighted score computation to the SQL: `weighted_contrib` and `elo_weight` in `deduped` CTE; `SUM(weighted_contrib) / NULLIF(SUM(elo_weight), 0) AS elo_weighted_score` in final SELECT
- [x] Task 3: Add filtered overload to `position_store::query_tree_slice` (AC: #1)
  - [x] Add `query_tree_slice(zobrist_hash root_hash, uint16_t max_depth, std::vector<game_id> const& game_ids) const` — same SQL with game_id filter
- [x] Task 4: Wire `search_filter` into `opening_stats::query` (AC: #1, #2, #5)
  - [x] Add overload `query(database_manager const& database, zobrist_hash hash, search_filter const& filter)` to `opening_stats.hpp`
  - [x] Implementation: if filter is empty (no non-pagination fields set), delegate to existing unfiltered `query(database, hash)` — zero regression risk
  - [x] Cross-store path: `distinct_game_ids_by_zobrist` → `find_game_ids_with_filter` (new game_store method, no pagination cap) → `query_opening_stats(hash, filtered_ids)`
  - [x] Populate `elo_weighted_score` from the new DuckDB column via `build_stats` helper
  - [x] Keep default sort by frequency desc, then SAN asc (unchanged)
- [x] Task 5: Wire `search_filter` into `opening_tree::expand` (AC: #1)
  - [x] Add `expand(database_manager const& db, node& n, search_filter const& filter)` overload to `opening_tree.hpp`
  - [x] Delegate to `opening_stats::query(database, n.zobrist_hash, filter)` instead of the unfiltered version
  - [x] The existing `expand(db, node)` overload delegates to `expand(db, node, empty_filter)` for backward compat
- [x] Task 6: Tests — filtered opening stats (AC: #5, #6, #7)
  - [x] Test: unfiltered query returns identical results to pre-3b behavior (regression guard)
  - [x] Test: filter by player_name — stats reflect only that player's games at the position
  - [x] Test: filter by min_elo/max_elo — games outside Elo range excluded; games without Elo excluded when filter active
  - [x] Test: filter by result — only games with matching result contribute to stats
  - [x] Test: filter by eco_prefix — only games with matching ECO contribute
  - [x] Test: combined filters (AND semantics) — all conditions must hold
  - [x] Test: filter matching zero games at position — returns empty stats, not an error
  - [x] Test: Elo-weighted score correctness — manually computed 1000/6000 ≈ 0.1667, verified within 0.001
  - [x] Test: Elo-weighted score with missing Elo — games with null Elo excluded from score but counted in frequency
  - [x] Test: Elo-weighted score tie-breaking — frequency desc, then SAN asc
  - [x] Test: transposition-aware filtering — game not passing through parent_hash excluded from filtered_ids
- [x] Task 7: Tests — filtered opening tree expand (AC: #7)
  - [x] Test: `expand` with filter produces correct children matching filtered stats
  - [x] Test: `expand` without filter (backward compat) produces same children as before
- [x] Task 8: Build and lint validation (AC: #7)
  - [x] `cmake --preset=dev && cmake --build build/dev` — zero warnings
  - [x] `ctest --test-dir build/dev` — all 290 tests pass
  - [x] `cmake --preset=dev-sanitize && cmake --build build/dev-sanitize && ctest --test-dir build/dev-sanitize` — zero violations

### Review Findings

- [x] [Review][Patch] Empty-filter path drops `elo_weighted_score` and returns `0.0` for every continuation [source/motif/search/opening_stats.cpp:148]
- [x] [Review][Patch] Filtered stats path materializes the full game-id set twice and emits a giant DuckDB `IN (...)` list, which conflicts with the story's 200ms performance target [source/motif/search/opening_stats.cpp:180]
- [x] [Review][Patch] Connection-scoped `_position_game_ids` temp table can be overwritten by overlapping filtered queries on the same SQLite connection [source/motif/db/game_store.cpp:213]
- [x] [Review][Patch] New public APIs `game_store::find_game_ids_with_filter`, `position_store::query_opening_stats(..., game_ids)`, and `position_store::count_distinct_games_by_zobrist(..., game_ids)` have no direct Catch2 coverage [source/motif/db/game_store.hpp:62]

## Dev Notes

### Scope Boundary

This story adds `search_filter` support to `opening_stats::query` and `opening_tree::expand`. It does **not** implement:
- Elo distribution per continuation (story 3b-3)
- Filtered `opening_tree::open` with prefetch (story 3b-4)
- HTTP endpoint changes (story 3b-5)
- Any Qt or web frontend changes

The `opening_stats::continuation` struct gains `elo_weighted_score` for downstream consumption. The sort order in `opening_stats::query` remains frequency-first. The caller (Qt tree view, HTTP endpoint) decides when to sort by Elo-weighted score instead.

### Cross-Store Filter Strategy

The key architectural challenge: `search_filter` contains metadata filters (player_name, eco_prefix, result) that live in **SQLite**, but opening stats are computed in **DuckDB**. The filter must be resolved to a game ID set before the DuckDB query.

**Strategy — reuse the 3b-1 pattern:**

1. Get all game IDs at the position from DuckDB: `position_store::distinct_game_ids_by_zobrist(hash)` → `vector<game_id>`
2. Apply metadata filters via SQLite: `game_store::find_games_with_ids(game_ids, metadata_filter)` where `metadata_filter` is the input `search_filter` with the `position` field cleared. This returns a `game_list_result` from which we extract the game IDs.
3. Use the filtered game ID set in a new DuckDB query: `position_store::query_opening_stats(hash, filtered_game_ids)` — the SQL adds `AND p_root.game_id IN (...)` to limit aggregation to the filtered set.

This mirrors `database_manager::find_games(search_filter)` from story 3b-1 exactly.

**Important:** `game_store::find_games_with_ids` returns `game_list_result` with `games` and `total_count`. We need only the game IDs from the `games` vector. Since `find_games_with_ids` paginates, we must either: (a) request all results at once (limit = filtered count, offset = 0), or (b) extract game IDs in batches. Option (a) is simpler — call `find_games_with_ids` with the full count first, then use those IDs. Alternatively, add a `game_store::find_game_ids_with_ids(vector<game_id>, search_filter)` that returns only IDs without pagination overhead. Evaluate during implementation.

**Optimization for empty metadata filter:** If the `search_filter` has only DuckDB-resolvable fields (none currently — all metadata filters are SQLite-side), skip the SQLite round-trip. In practice, the only case where we can skip is an empty filter → use existing unfiltered path.

### Elo-Weighted Score SQL

Add to the existing `query_opening_stats` SQL. The score is computed within the `deduped` CTE:

```sql
-- In deduped CTE, add computed columns:
CASE
  WHEN p_root.white_elo IS NOT NULL AND p_root.black_elo IS NOT NULL
  THEN p_root.result * (p_root.white_elo + p_root.black_elo) / 2.0
  ELSE NULL
END AS weighted_contrib,
CASE
  WHEN p_root.white_elo IS NOT NULL AND p_root.black_elo IS NOT NULL
  THEN (p_root.white_elo + p_root.black_elo) / 2.0
  ELSE NULL
END AS elo_weight

-- In the final SELECT (grouped by encoded_move, child_hash):
SUM(d.weighted_contrib) / NULLIF(SUM(d.elo_weight), 0) AS elo_weighted_score
```

`NULLIF` ensures division-by-zero returns NULL (no games with Elo data → no score), which maps to `std::nullopt` in the agg row, then `0.0` in the `continuation` struct.

**Alternative:** Compute in C++ from the existing `white_wins`, `draws`, `black_wins`, `avg_white_elo`, `avg_black_elo` columns. This is **incorrect** because those averages collapse per-game variation. The weighted average of per-game results requires per-game data. Must be done in SQL.

### Existing `opening_stat_agg_row` Extension

Add to `opening_stat_agg_row` in `types.hpp`:
```cpp
std::optional<double> elo_weighted_score;  // between result rows from DuckDB
```

And a new column index in `opening_stats_col` namespace in `position_store.cpp`:
```cpp
constexpr idx_t elo_weighted_score = 11;
```

### `opening_stats::query` Overload Design

Keep the existing 2-parameter `query(database, hash)` signature untouched. Add a 3-parameter overload:

```cpp
[[nodiscard]] auto query(database_manager const& database, zobrist_hash hash) -> result<stats>;
[[nodiscard]] auto query(database_manager const& database, zobrist_hash hash, search_filter const& filter) -> result<stats>;
```

The 2-parameter version stays as-is. The 3-parameter version:
1. If filter has no metadata fields (player_name, min_elo, max_elo, result, eco_prefix all nullopt), delegate to the unfiltered 2-parameter version (NFR08: identical results)
2. Otherwise, execute the cross-store filter path

### `opening_tree::expand` Overload Design

Same pattern:

```cpp
auto expand(database_manager const& db, node& n) -> result<void>;
auto expand(database_manager const& db, node& n, search_filter const& filter) -> result<void>;
```

The existing overload delegates to the new one with an empty filter. `opening_tree::open` is NOT modified in this story — that's story 3b-4.

### Previous Story Intelligence

- **Story 3b-1:** Established `search_filter`, `game_list_result`, `game_store::find_games(search_filter)`, `game_store::find_games_with_ids(vector<game_id>, search_filter)`, `database_manager::find_games(search_filter)`, and the cross-store DuckDB→SQLite game ID intersection pattern. The `distinct_game_ids_by_zobrist` method already exists. The batch-IN-clause pattern for >999 IDs is implemented and tested. The `opening_tree` board-lifetime fix is in place.
- **Story 3 (opening_stats, opening_tree):** The 3-CTE `query_opening_stats` SQL with transposition-aware `child_agg`, the board replay via `find_root_board`, the ECO resolution via `resolve_eco`, and the `expand`-delegates-to-`opening_stats::query` pattern.
- **Code-review patches from 3b-1:** Bind return values must be checked; `txn_guard` for multi-statement operations; test >999 batch boundary.

### Module Boundary Rules

- `motif_search` depends on `motif_db` — it may call `position_store` and `game_store` methods through `database_manager` only
- `motif_search` must NOT access DuckDB or SQLite directly — all storage through `motif_db` APIs
- `motif_search` must NOT include any Qt headers
- New `position_store` overloads with `vector<game_id>` parameter are `motif_db` additions — acceptable

### DuckDB IN Clause for Game IDs

DuckDB has no 999-parameter limit like SQLite. A single `IN (1, 2, 3, ...)` clause with thousands of values is fine. Build the SQL string dynamically with `std::ostringstream` (existing pattern in `position_store.cpp`).

### Testing Guidance

Extend the existing test infrastructure in `test/source/motif_search/opening_stats_test.cpp`:

- Reuse `hash_after_sans`, `encode_moves`, `make_game` helpers
- Create a fixture with 10–15 games with varied Elo, results, ECO codes, player names at a shared position
- For each test: insert games, rebuild position store, query with filter, verify stats match manual computation
- Elo-weighted score: create a small known dataset (e.g., 3 games at a position: one where side-to-move wins at avg Elo 2500, one draw at 2000, one loss at 1500). Compute expected score manually: (1×2500 + 0×2000 + (-1)×1500) / (2500+2000+1500) = 1000/6000 ≈ 0.1667. Verify returned score matches within tolerance.
- Use `REQUIRE_THAT(score, WithinAbs(expected, 0.001))` for floating-point comparison (Catch2 v3 matcher)

Run `cmake --preset=dev-sanitize` to verify zero ASan/UBSan violations.

### References

- [Source: source/motif/search/opening_stats.hpp] — current `query` signature and `continuation` struct
- [Source: source/motif/search/opening_stats.cpp:62-147] — current `query` implementation, 3-CTE SQL, ECO resolution
- [Source: source/motif/db/position_store.hpp:28] — `query_opening_stats` signature to extend
- [Source: source/motif/db/position_store.cpp:272-358] — `query_opening_stats` SQL with `deduped`, `child_hashes`, `child_agg` CTEs
- [Source: source/motif/db/position_store.cpp:225-270] — `query_tree_slice` SQL to extend
- [Source: source/motif/db/types.hpp:90-103] — `opening_stat_agg_row` struct to extend
- [Source: source/motif/db/types.hpp:142-153] — `search_filter` struct (already exists from 3b-1)
- [Source: source/motif/db/database_manager.cpp:534-552] — cross-store filter pattern (`find_games`)
- [Source: source/motif/db/game_store.hpp] — `find_games_with_ids(vector<game_id>, search_filter)` for SQLite metadata filtering
- [Source: source/motif/search/opening_tree.hpp] — `expand` signature to extend
- [Source: source/motif/search/opening_tree.cpp] — `expand` delegates to `opening_stats::query`
- [Source: _bmad-output/implementation-artifacts/3b-1-search-filter-model-and-filtered-game-list.md] — previous story with filter patterns
- [Source: _bmad-output/planning-artifacts/prd-003-search.md#FR11-FR14] — filtered opening explorer FRs
- [Source: _bmad-output/planning-artifacts/prd-003-search.md#ELO-Weighted] — Elo-weighted score formula
- [Source: _bmad-output/planning-artifacts/epics.md#Story-3b.2] — epic AC definitions
- [Source: CONVENTIONS.md] — SQL raw string literals, DuckDB C API only, no mocks

## Dev Agent Record

### Agent Model Used

claude-sonnet-4-6

### Debug Log References

None — no external debug artifacts needed.

### Completion Notes List

- `game_store::find_game_ids_with_filter` added as a public method bypassing the 500-row pagination cap of `find_games_with_ids`; needed because filtered opening stats must see all matching game IDs, not just the first page.
- The transposition test was corrected: `distinct_game_ids_by_zobrist(parent_hash)` returns only games that pass through the parent position, so games that reach child_hash via a different transposition path are excluded from `filtered_ids` and do not inflate the frequency count. This is correct behavior.
- `fmt::format` with `{0}`/`{1}` placeholders used throughout `position_store.cpp` for DuckDB SQL construction per CONVENTIONS (no `std::ostringstream` in new code).
- `build_stats` anonymous-namespace helper extracted to share eco/board/san construction logic between both `query` overloads.

### File List

- `source/motif/db/types.hpp` — added `std::optional<double> elo_weighted_score` to `opening_stat_agg_row`
- `source/motif/db/position_store.hpp` — added three filtered overload declarations
- `source/motif/db/position_store.cpp` — implemented filtered `query_opening_stats`, `query_tree_slice`, `count_distinct_games_by_zobrist`; added `elo_weighted_score` column and `build_game_id_list` helper
- `source/motif/db/game_store.hpp` — added `find_game_ids_with_filter` public declaration
- `source/motif/db/game_store.cpp` — implemented `find_game_ids_with_filter`
- `source/motif/search/opening_stats.hpp` — added `elo_weighted_score` field to `continuation`; added 3-param `query` declaration
- `source/motif/search/opening_stats.cpp` — added `build_stats` helper; implemented 3-param `query` overload
- `source/motif/search/opening_tree.hpp` — added filtered `expand` overload declaration
- `source/motif/search/opening_tree.cpp` — implemented filtered `expand`; existing `expand` delegates to it
- `test/source/motif_search/opening_stats_test.cpp` — 11 new `[filter]` TEST_CASEs + `make_game_named` helper
- `test/source/motif_search/opening_tree_test.cpp` — 2 new `[filter]` TEST_CASEs + `make_game_named` helper

### Change Log

- Added `elo_weighted_score` (SQL-computed via weighted average of result × avg_elo) to `opening_stat_agg_row` and `opening_stats::continuation`
- Added filtered DuckDB overloads: `position_store::query_opening_stats(hash, game_ids)`, `query_tree_slice(root_hash, max_depth, game_ids)`, `count_distinct_games_by_zobrist(hash, game_ids)`
- Added `game_store::find_game_ids_with_filter` — unbounded SELECT returning all matching IDs
- Added `opening_stats::query(database, hash, filter)` 3-param overload implementing cross-store filter strategy
- Added `opening_tree::expand(database, node, filter)` overload; existing overload delegates to it with empty filter
