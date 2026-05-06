# 005 — Position Encoded Move

## Overview

Store the encoded move that reached each position directly in the DuckDB
`position` table. This eliminates SQLite round-trips in both `opening_tree::open`
and `opening_stats::query` for the critical aggregation loops, cutting latency for
high-fan-out positions (notably the starting position) from O(N_games) SQLite
calls to a single DuckDB self-join.

## Background

### opening_tree::open (DONE)

`opening_tree::open` previously worked in two phases:

1. **DuckDB query** — `query_tree_slice` self-joins the `position` table and
   returns one row per reachable (game, ply) pair.
2. **SQLite fetch** — `get_game_contexts` fetched the full moves BLOB for every
   matching game so the code could read `moves[root_ply + depth - 1]` (the
   encoded move) and replay the board to verify hashes.

For a position reached by N games, phase 2 issued ⌈N / 999⌉ SQLite prepared
statements. Measured cost at starting position, 1M games: ~2.9 s.

Fix shipped in this branch: `encoded_move` is now stored in the position table
and returned by `query_tree_slice`. The O(N_games) SQLite call is gone. Measured
result: ~650 ms.

### opening_stats::query (DONE)

`opening_stats::query` previously had the same O(N_games) SQLite pattern in two
separate places:

1. **Board + validity loop** — iterates all `opening_move_stat` rows, calls
   `get_opening_context(game_id)` once per unique game to (a) validate that
   `ply <= moves.size()` and (b) replay the board at the root. For the starting
   position this is ~1M individual SQLite queries; the board is trivially
   `chesslib::board{}` and requires no query at all.

2. **Continuation move extraction** — `get_continuation_contexts` is called on
   all N rows and batches them 300 at a time, querying `substr(g.moves, ply*2+1,
   2)` to extract the encoded continuation move. For 1M rows this is ~3333 SQLite
   round-trips. The same value is now available as `encoded_move` in the DuckDB
   position table for the *next* ply.

The shipped fix takes the same idea but uses a dedicated DuckDB aggregation
query: `query_opening_stats(hash)`. DuckDB now returns one row per distinct
continuation with `cont_encoded_move`, `cont_hash`, aggregate counts, average
elos, and sample game ids for ECO/opening-name lookup. SQLite remains only for
batched `get_game_contexts(sample_ids)` lookups used to resolve root-board
replay for non-starting positions and to attribute ECO/opening names.

## Schema Change

### DuckDB `position` table

Add one column (already applied):

```sql
encoded_move USMALLINT NOT NULL
```

Full DDL:

```sql
CREATE TABLE position (
    zobrist_hash  UBIGINT   NOT NULL,
    game_id       UINTEGER  NOT NULL,
    ply           USMALLINT NOT NULL,
    encoded_move  USMALLINT NOT NULL,
    result        TINYINT   NOT NULL,
    white_elo     SMALLINT,
    black_elo     SMALLINT
)
```

The column at `ply = 0` stores `0` (sentinel — the starting position is not
reached by any move). All other plies store the chesslib 16-bit encoding of the
move that reached that position.

### Memory impact

`position_row` grows from 24 to 26 bytes. For a 3.4M-game corpus with ~40
average positions per game: ~136M rows × 2 bytes = ~272 MB additional storage.

## Code Changes

### Already shipped

- `types.hpp`: `position_row` and `tree_position_row` carry `encoded_move`
- `position_store.cpp`: DDL, `insert_batch`, and `query_tree_slice` updated
- `import_pipeline.cpp` and `database_manager.cpp`: populate `encoded_move`
- `opening_tree.cpp`: uses `row.encoded_move`; forward BFS replaces `replay_position`
- `rebuild_position_store`: drops and recreates table so schema changes auto-apply

### opening_stats.cpp — shipped query() rewrite

The final implementation uses a dedicated aggregate query:

```
auto rows = database.positions().query_opening_stats(zobrist_hash)
```

**Board reconstruction**

- If any row has `root_ply == 0`: board is the default `motif::chess::board {}`;
  no SQLite call is needed.
- Otherwise: fetch sampled game contexts once with `get_game_contexts`, then
  replay one matching sample game to `root_ply` through `motif::chess::replay`.
  This is bounded by the number of distinct continuations, not matching games.

**Aggregation loop**

Iterate the aggregated DuckDB rows directly. Key fields are
`row.cont_encoded_move` for SAN generation and `row.cont_hash` for the result
position hash. Frequency and result totals are already pre-aggregated in DuckDB.

**ECO attribution**

Collect sampled game ids from `eco_sample_min` / `eco_sample_max` per distinct
continuation. Batch-fetch ECO and opening names via `get_game_contexts(sample_ids)`.
SQLite work is now O(distinct continuations), not O(N matching games).

## Performance Targets

| Scenario | Before | Target |
|---|---|---|
| `opening_tree` starting position, 1M games, depth=1 | ~2.9 s | < 500 ms ✓ |
| `opening_tree` starting position, 3.4M games, depth=1 | ~10 s (projected) | < 2 s |
| `opening_stats` starting position, 1M games | ~2 s (measured) | < 200 ms |
| `opening_stats` p99, sampled positions, 1M games | ~2 s (measured) | < 500 ms |

## Prerequisites

- Spec 002 (import pipeline) complete.
- Spec 003 (search / opening tree) complete.

## Acceptance Criteria

### opening_tree (already satisfied)

- [x] `position_row` carries `encoded_move`; ply=0 rows store `0`.
- [x] Import pipeline populates `encoded_move` correctly for all plies.
- [x] `query_tree_slice` returns `encoded_move` in each result row.
- [x] `opening_tree::open` no longer calls `get_game_contexts` in the main loop.
- [x] All existing opening-tree unit tests pass unchanged.
- [x] `opening_tree::open from starting position aggregates first moves correctly` passes.
- [x] `opening_tree::open from starting position on real corpus` passes and reports elapsed < 1 s.
- [x] No regression in `opening_tree::open performance on sorted position store` p99.

### opening_stats (completed)

- [x] `opening_stats::query` no longer calls `get_opening_context` in a per-game loop.
- [x] `opening_stats::query` no longer calls `get_continuation_contexts`.
- [x] Board at root is derived without SQLite when `root_ply == 0`.
- [x] Existing opening-stats unit tests were updated and pass with the new typed interfaces.
- [x] Performance test: `opening_stats::query from starting position on real corpus` exists.
- [x] Performance test: `opening_stats::query performance on sorted position store` exists with p99 target < 500 ms.
- [x] The dev build is green after the strong-type and facade refactor.
