# Story 3.3: Lazy Opening Tree with Prefetch

Status: ready-for-dev

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a user,
I want to navigate an opening tree where the first 5 levels are prefetched on open and deeper nodes are loaded on demand,
so that I can click through the main line of an opening rapidly without per-move latency.

## Acceptance Criteria

1. **Given** a user opens the opening tree from any position,
   **when** `opening_tree::open` is called with a `root_hash` and `prefetch_depth` (default: 5),
   **then** the first `prefetch_depth` levels of the tree are loaded in memory using exactly one DuckDB query and one batched SQLite query — not one query per node (AR10, FR18, FR19),
   **and** each node contains per-continuation statistics: SAN, result hash, frequency, white wins, draws, black wins, average white Elo, average black Elo, ECO code, and opening name.

2. **Given** the user expands a node at depth > `prefetch_depth`,
   **when** `opening_tree::expand` is called on that node,
   **then** the children of that node are loaded via a single `opening_stats::query` call (reuse Story 3.2 path) and attached to the node,
   **and** nodes already in the prefetch cache are not re-queried (calling expand on an already-expanded node is a no-op).

3. **Given** `prefetch_depth` is passed to `opening_tree::open` by the caller (the app layer reads it from config),
   **when** `opening_tree::open` is called with a non-default depth,
   **then** prefetching uses the configured depth instead of the default 5 (AR10).

4. **Given** any tree traversal,
   **then** the tree is never fully materialized in memory — only expanded nodes hold child data; unexpanded `node_continuation.subtree` is null (FR19).

5. **Given** all changes are implemented,
   **when** tests run,
   **then** all public API functions in `opening_tree` have coverage,
   **and** all tests pass under `dev` and `dev-sanitize`.

## Tasks / Subtasks

- [ ] Task 1: Add `position_store::query_tree_slice` to `motif_db` (AC: 1)
  - [ ] Add `tree_position_row` struct to `source/motif/db/types.hpp`
  - [ ] Add `query_tree_slice(uint64_t root_hash, uint16_t max_depth) -> result<vector<tree_position_row>>` to `position_store.hpp` and implement in `position_store.cpp`
  - [ ] The single DuckDB query is a self-JOIN on `position`: games that contain `root_hash` at some ply P, joined to rows in `[P+1, P+max_depth]` from the same game

- [ ] Task 2: Add `opening_tree` API in `motif_search` (AC: 1, 2, 3, 4)
  - [ ] Add `source/motif/search/opening_tree.hpp` with all public types and functions
  - [ ] Add `source/motif/search/opening_tree.cpp` implementing `open` and `expand`
  - [ ] Update `source/motif/search/CMakeLists.txt` to compile `opening_tree.cpp`
  - [ ] `open`: call `query_tree_slice`, batch-fetch game contexts from `game_store`, build tree in-memory
  - [ ] `expand`: call `opening_stats::query` for the node's hash, populate children, mark `is_expanded = true`

- [ ] Task 3: Tests (AC: 1, 2, 3, 4, 5)
  - [ ] Add `test/source/motif_search/opening_tree_test.cpp`
  - [ ] Update `test/CMakeLists.txt` to include `opening_tree_test.cpp` in `motif_search_test`
  - [ ] Cover: open prefetches correct depth, no re-query on expand of prefetched node, custom prefetch_depth, on-demand expand beyond prefetch, empty root, performance

## Dev Notes

### API Design — Non-Negotiable

```cpp
// source/motif/search/opening_tree.hpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "motif/search/error.hpp"

namespace motif::db { class database_manager; }

namespace motif::search::opening_tree
{

struct node;  // forward declaration required for recursive structure

struct node_continuation
{
    std::string san;
    std::uint64_t result_hash {};           // zobrist hash of the position after this move
    std::uint32_t frequency {};
    std::uint32_t white_wins {};
    std::uint32_t draws {};
    std::uint32_t black_wins {};
    std::optional<double> average_white_elo;
    std::optional<double> average_black_elo;
    std::optional<std::string> eco;
    std::optional<std::string> opening_name;
    std::unique_ptr<node> subtree;          // null until expanded or prefetched
};

struct node
{
    std::uint64_t zobrist_hash {};
    std::vector<node_continuation> continuations;  // sorted: freq desc, then SAN asc
    bool is_expanded {false};              // true once continuations were loaded
};

struct tree
{
    node root;
    std::size_t prefetch_depth {5};
};

// Opens the opening tree from root_hash. Prefetches prefetch_depth levels eagerly
// via one DuckDB query + one SQLite batch query. Returns a tree with root.is_expanded == true
// and all nodes within prefetch_depth having is_expanded == true; nodes at the
// boundary have non-null subtrees with is_expanded == false.
[[nodiscard]] auto open(motif::db::database_manager const& db,
                        std::uint64_t root_hash,
                        std::size_t prefetch_depth = 5) -> result<tree>;

// Expands a node on demand: loads its continuations via opening_stats::query.
// No-op if node.is_expanded is already true (do NOT re-query).
[[nodiscard]] auto expand(motif::db::database_manager const& db,
                          node& n) -> result<void>;

}  // namespace motif::search::opening_tree
```

### New `motif_db` Type — `tree_position_row`

Add to `source/motif/db/types.hpp` (in the same block as `opening_move_stat`):

```cpp
struct tree_position_row
{
    std::uint32_t game_id {};
    std::uint16_t root_ply {};     // ply of the root_hash in this game
    std::uint16_t depth {};        // 1 = direct child, 2 = grandchild, …
    std::uint64_t child_hash {};   // zobrist_hash of the position at root_ply + depth
    std::int8_t result {};
    std::optional<std::int16_t> white_elo;
    std::optional<std::int16_t> black_elo;
};
```

Add to `position_store.hpp`:

```cpp
auto query_tree_slice(std::uint64_t root_hash, std::uint16_t max_depth) const
    -> result<std::vector<tree_position_row>>;
```

### Single DuckDB Query for Prefetch

The `query_tree_slice` query is a self-JOIN. Implement in `position_store.cpp` using the same string-interpolation pattern as `query_opening_moves` (prepared statements are a deferred P1 backlog item — do NOT add them here):

```sql
SELECT
    p_root.game_id,
    p_root.ply AS root_ply,
    CAST(p_cont.ply - p_root.ply AS USMALLINT) AS depth,
    p_cont.zobrist_hash AS child_hash,
    p_cont.result,
    p_cont.white_elo,
    p_cont.black_elo
FROM position p_root
JOIN position p_cont
    ON  p_root.game_id = p_cont.game_id
    AND p_cont.ply > p_root.ply
    AND p_cont.ply <= p_root.ply + {max_depth}
WHERE p_root.zobrist_hash = {root_hash}
ORDER BY p_root.game_id, p_cont.ply
```

Inject `root_hash` and `max_depth` via `std::to_string` (matching the pattern used in `query_opening_moves`). Retrieve columns: col 0 = game_id (UINTEGER), col 1 = root_ply (USMALLINT), col 2 = depth (USMALLINT), col 3 = child_hash (UBIGINT), col 4 = result (TINYINT), col 5 = white_elo (SMALLINT nullable), col 6 = black_elo (SMALLINT nullable).

Use `duckdb_value_uint16` (not `duckdb_value_int16`) for `root_ply` and `depth` — this is the fix that was applied in Story 3.2 for `ply` fields.

### Tree Construction Algorithm

After `query_tree_slice` and a batch `get_game_contexts` call, `open` builds the tree. The algorithm must be documented in comments within `opening_tree.cpp`:

```
1. Group tree_position_row entries by game_id.
2. For each game_id, determine root_ply (all rows for same game have same root_ply).
3. For depth=1 rows: derive the continuation SAN from game_context.moves[root_ply]
   (chesslib replay — same approach as opening_stats::query).
   Aggregate by (san, child_hash_at_depth_1) → node_continuation for root.
4. For depth=2 rows: derive SAN from game_context.moves[root_ply+1].
   Aggregate by (child_hash_at_depth_1, san_at_depth_2) → node_continuation for
   the matching depth-1 child node.
5. Continue recursively up to max_depth.
6. At max_depth: create node_continuation entries with subtree = make_unique<node>
   {zobrist_hash = child_hash, is_expanded = false}.
   All shallower nodes: subtree = make_unique<node> {..., is_expanded = true}.
7. Apply the same dedup logic as opening_stats: seen = unordered_set<continuation_key>
   per node; skip duplicate (game_id, encoded_move) pairs.
8. Apply the same Elo averaging: ignore null; return std::nullopt if no values.
9. Apply the same opening_name selection: keep the longest name per ECO code.
10. Sort continuations: frequency desc, then SAN asc.
```

`replay_position` and `chesslib` SAN conversion are available — copy the helper pattern from `opening_stats.cpp`. Do NOT inline a new SAN implementation.

### `expand` Implementation

```cpp
auto expand(motif::db::database_manager const& db, node& n) -> result<void>
{
    if (n.is_expanded) { return {}; }   // no-op: already loaded

    auto stats_res = opening_stats::query(db, n.zobrist_hash);
    if (!stats_res) { return tl::unexpected {stats_res.error()}; }

    n.continuations.reserve(stats_res->continuations.size());
    for (auto const& c : stats_res->continuations) {
        // Get the child hash by calling position_search::find or querying
        // opening_stats to get the resulting position.
        // PROBLEM: opening_stats::continuation does not carry result_hash.
        // SOLUTION: add result_hash to opening_stats::continuation (see below).
        n.continuations.push_back(node_continuation {
            .san            = c.san,
            .result_hash    = c.result_hash,   // see extension below
            .frequency      = c.frequency,
            .white_wins     = c.white_wins,
            .draws          = c.draws,
            .black_wins     = c.black_wins,
            .average_white_elo = c.average_white_elo,
            .average_black_elo = c.average_black_elo,
            .eco            = c.eco,
            .opening_name   = c.opening_name,
            .subtree        = std::make_unique<node>(node {
                .zobrist_hash = c.result_hash,
                .is_expanded  = false,
            }),
        });
    }
    n.is_expanded = true;
    return {};
}
```

### Extend `opening_stats::continuation` with `result_hash`

**This story must add `result_hash` to `opening_stats::continuation`** — it is needed by `expand` to populate child nodes, and it was deliberately left out of Story 3.2 because Story 3.3 is the consumer.

```cpp
// In opening_stats.hpp, add one field to continuation:
struct continuation
{
    std::string san;
    std::uint64_t result_hash {};          // ADD THIS — zobrist hash after the move
    std::uint32_t frequency {};
    // …rest unchanged
};
```

`opening_stats.cpp` must populate `result_hash` during aggregation. The hash of the resulting position after a continuation move is computed via `chesslib` during the replay loop that is already in the `query` function. Capture it at the point where the continuation move is made and store it in `continuation_aggregate` alongside the other fields.

### Architecture Compliance

- `opening_tree` lives in `motif_search` — no DuckDB or SQLite headers in `opening_tree.hpp` or `opening_tree.cpp`.
- All storage access routes through `database_manager` → `position_store` / `game_store` public APIs.
- `opening_stats::query` is called from `expand` — this is an intra-module call within `motif_search`, not a boundary violation.
- Qt-free: no `#include <Q*>` anywhere in the search module.
- No new dependencies — all functionality uses chesslib, motif_db, and tl::expected which are already linked by motif_search.

### Library / Framework Requirements

- C++23, Clang 21, `tl::expected`, Catch2 v3.
- `std::unique_ptr<node>` for the recursive tree (required to break the incomplete-type cycle).
- No `k_` prefix on constants.
- `lower_snake_case` for all identifiers.
- DuckDB C API is banned outside `motif_db` internals. Do not call `duckdb_*` from `opening_tree.cpp`.

### File Structure Requirements

Files to create:
- `source/motif/search/opening_tree.hpp`
- `source/motif/search/opening_tree.cpp`
- `test/source/motif_search/opening_tree_test.cpp`

Files to modify:
- `source/motif/db/types.hpp` — add `tree_position_row`
- `source/motif/db/position_store.hpp` — add `query_tree_slice` declaration
- `source/motif/db/position_store.cpp` — implement `query_tree_slice`
- `source/motif/search/opening_stats.hpp` — add `result_hash` to `continuation`
- `source/motif/search/opening_stats.cpp` — populate `result_hash` during aggregation
- `source/motif/search/CMakeLists.txt` — add `opening_tree.cpp`
- `test/CMakeLists.txt` — add `opening_tree_test.cpp` to `motif_search_test`

### Testing Requirements

- Every public function (`open`, `expand`) must have tests.
- Use real SQLite/DuckDB-backed fixtures via `database_manager::create`; no mocks.
- Use `test_helpers::is_sanitized_build` (from `test/source/test_helpers.hpp`) for build-type guards in perf tests.
- Test file organization: mirror `opening_stats_test.cpp` patterns — `tmp_dir` RAII fixture, `make_game` helper, `insert_games_and_rebuild` helper, `hash_after_sans` helper.
- Required test cases:
  - `open` returns root with correct continuations at depth 1
  - `open` prefetches to the configured depth (verify depth-3 node is_expanded == true)
  - `open` leaves boundary nodes with is_expanded == false (depth = prefetch_depth node's subtrees)
  - `expand` populates children for an unexpanded node
  - `expand` is a no-op on an already-expanded node (verify no DB call by checking idempotency)
  - `open` with empty root (no matching games) returns tree with empty root continuations
  - `open` with custom `prefetch_depth = 2` respects the configured depth
  - `result_hash` in continuations matches expected zobrist hash (compute via `hash_after_sans`)
  - Performance test: `open` on a 1M-game corpus with the standard perf guard (skip in sanitize builds and dev builds; enforce only in release builds, same pattern as opening_stats perf test)
- Sanitizer gate: `cmake --preset=dev-sanitize && ctest --test-dir build/dev-sanitize --output-on-failure`

### Previous Story Intelligence (from Stories 3.1 and 3.2)

**Carry forward — mandatory:**
- Dedup pattern: `continuation_key {game_id, encoded_move}` with golden-ratio hash combiner (`0x9e3779b97f4a7c15ULL` pattern). Apply the same dedup to tree construction — each (game_id, encoded_move) pair contributes at most once per node at each depth level.
- Orphaned row handling: if `game_context` is not found for a `game_id`, skip all rows for that game and continue; do not return an error.
- `opening_name` selection: keep the longest name per ECO code (not lexicographically smallest).
- Deterministic continuation ordering: sort by `frequency` descending, then `san` ascending.
- `duckdb_value_uint16` for `ply`-derived fields (NOT `duckdb_value_int16` — the signed accessor was a pre-existing bug fixed in Story 3.2).
- `is_sanitized_build` lambda is now in `test/source/test_helpers.hpp` — include it directly (`#include "test_helpers.hpp"`). Do NOT redefine it locally.
- RAII for DuckDB handles in test helpers: use `duckdb_handle_guard` pattern introduced in Story 3.2 `database_manager_test.cpp` if you need raw DuckDB access in tests. Prefer `database_manager` API for all storage in tests.

**Carry forward — context:**
- Performance tests: wrap with `skip_perf_unless_release_build()` helper from `import_pipeline_test.cpp` (or replicate the same logic: SKIP in sanitize builds and debug builds, enforce CHECK only in release).
- Named perf sample seed: use `perf_sample_seed = uint64_t{42}` for reproducibility.
- `sample_zobrist_hashes` with a fixed seed produces deterministic samples across runs.
- NFR02 (500ms P99) gap for high-fanout positions remains in the deferred backlog from Story 3.2 — do not attempt to close it in Story 3.3 unless it is specifically required by the 3.3 ACs.

**Watch out for:**
- `opening_stats::continuation` is being extended with `result_hash`. Update all existing usages of `continuation` in `opening_stats_test.cpp` to accommodate the new field (it zero-initializes, so existing tests should still compile — but verify that no test asserts `sizeof(continuation)` or similar).
- The tree construction allocates `std::unique_ptr<node>` for each child. For a 5-level prefetch on a popular opening (e.g., after 1.e4), the tree can be wide. Prefer iterative DFS/BFS construction over deep recursion to avoid stack overflow in deep prefetch scenarios.
- `motif_search_test` already includes `position_search_test.cpp` and `opening_stats_test.cpp`. Add `opening_tree_test.cpp` to the same target — do NOT create a new test executable.

### Git Intelligence Summary

- Recent commits: `feat(search): add position_search and opening_stats modules` and `refactor(import): adopt simplified rebuild API, extract pgn_helpers`. The search module pattern is established: dedicated `<feature>.hpp` + `<feature>.cpp` pair, small public API, all aggregation in the `.cpp`, typed through `motif_db` public APIs.
- `test/source/` layout mirrors `source/motif/` layout — `opening_tree_test.cpp` goes under `test/source/motif_search/`.
- `BENCHMARKS.md` is maintained for search-related perf work. Add Story 3.3 perf results if a full-corpus test is run.

### Project Structure Notes

- `motif_search` target currently compiles `motif_search.cpp`, `opening_stats.cpp`, `position_search.cpp`. Add `opening_tree.cpp` to `add_library(motif_search STATIC ...)` in `source/motif/search/CMakeLists.txt`.
- `motif_search_test` links `motif_search` and `motif_import` (needed for `initialize_logging`). No changes to link libraries are needed for Story 3.3.
- `test/CMakeLists.txt` already adds `target_include_directories(motif_search_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)` — this is what allows `#include "test_helpers.hpp"` to work.

### References

- [Source: `_bmad-output/planning-artifacts/epics.md#Story-33-Lazy-Opening-Tree-with-Prefetch`]
- [Source: `_bmad-output/planning-artifacts/architecture.md#Opening-Tree-Traversal`]
- [Source: `_bmad-output/planning-artifacts/architecture.md#Module-Structure`]
- [Source: `_bmad-output/planning-artifacts/architecture.md#DuckDB-PositionIndex-Schema`]
- [Source: `_bmad-output/planning-artifacts/architecture.md#AR10`]
- [Source: `_bmad-output/planning-artifacts/prd.md#FR18-FR19`]
- [Source: `source/motif/search/opening_stats.hpp`]
- [Source: `source/motif/search/opening_stats.cpp`]
- [Source: `source/motif/db/position_store.hpp`]
- [Source: `source/motif/db/position_store.cpp`]
- [Source: `source/motif/db/types.hpp`]
- [Source: `source/motif/db/game_store.hpp`]
- [Source: `test/source/motif_search/opening_stats_test.cpp`]
- [Source: `test/source/test_helpers.hpp`]
- [Source: `CONVENTIONS.md#DuckDB--C-API-only`]
- [Source: `CONVENTIONS.md#Module-boundaries`]
- [Source: `CONVENTIONS.md#Testing`]

## Dev Agent Record

### Agent Model Used

claude-sonnet-4-6

### Debug Log References

- Reviewed sprint-status.yaml to identify Story 3.3 as next backlog story in Epic 3
- Reviewed epics.md Epic 3 Story 3.3 acceptance criteria and AR10
- Reviewed architecture.md module structure, opening-tree traversal, DuckDB schema
- Reviewed opening_stats.hpp and opening_stats.cpp for API patterns and reuse opportunities
- Reviewed position_store.hpp for existing DuckDB query methods
- Reviewed types.hpp for existing DB row types
- Reviewed Story 3.2 completion notes and review findings for carry-forward intelligence
- Reviewed test_helpers.hpp, opening_stats_test.cpp for test patterns
- Designed query_tree_slice self-JOIN DuckDB query for single-query prefetch
- Identified need to extend opening_stats::continuation with result_hash for expand path
- Documented tree construction algorithm explicitly to prevent LLM mistakes
- Carried forward: dedup key, hash combiner, orphan handling, opening_name selection, ply uint16 accessor fix

### Completion Notes List

- Ultimate context engine analysis completed — comprehensive developer guide created
- Story 3.3 builds directly on Story 3.2: reuses opening_stats::query for expand(), extends continuation struct with result_hash
- position_store::query_tree_slice provides the single DuckDB query required by AR10/AC1
- opening_stats::continuation must have result_hash added — populate via chesslib replay at the aggregation step in opening_stats.cpp
- expand() delegates to opening_stats::query — no duplicate aggregation logic
- Tree never fully materializes: subtree = nullptr for unexpanded nodes; unique_ptr breaks the recursive struct cycle
- All Story 3.2 carry-forwards explicitly documented to prevent regression

### File List

- `source/motif/db/types.hpp`
- `source/motif/db/position_store.hpp`
- `source/motif/db/position_store.cpp`
- `source/motif/search/opening_stats.hpp`
- `source/motif/search/opening_stats.cpp`
- `source/motif/search/opening_tree.hpp`
- `source/motif/search/opening_tree.cpp`
- `source/motif/search/CMakeLists.txt`
- `test/CMakeLists.txt`
- `test/source/motif_search/opening_tree_test.cpp`
