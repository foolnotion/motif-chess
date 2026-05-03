# 005 — Position Encoded Move

## Overview

Store the encoded move that reached each position directly in the DuckDB
`position` table. This eliminates the SQLite round-trip in `opening_tree::open`
for the critical tree-building loop, cutting latency for high-fan-out positions
(notably the starting position) from ~3 s to an expected < 500 ms on a 1M-game
corpus.

## Background

`opening_tree::open` currently works in two phases:

1. **DuckDB query** — `query_tree_slice` self-joins the `position` table and
   returns one row per reachable (game, ply) pair.
2. **SQLite fetch** — `get_game_contexts` fetches the full moves BLOB for every
   matching game so the code can read `moves[root_ply + depth - 1]` (the
   encoded move) and replay the board to verify hashes.

For a position reached by N games, phase 2 issues ⌈N / 999⌉ SQLite prepared
statements, each reading a full moves BLOB. For the starting position on a 1M-
game corpus this is ~1001 round-trips fetching ~1M BLOBs just to extract a
single 2-byte value from each. Measured cost: ~2.9 s.

If `encoded_move` is stored in the position table, `query_tree_slice` returns it
directly. The only remaining SQLite access needed is eco/opening attribution,
which is one lookup per distinct continuation (~20 for starting position) via the
existing `get_continuation_contexts`.

## Schema Change

### DuckDB `position` table

Add one column:

```sql
encoded_move USMALLINT NOT NULL
```

Full DDL after change:

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

### `types.hpp`

Add `encoded_move` to `position_row`:

```cpp
struct position_row {
    std::uint64_t zobrist_hash {};
    std::uint32_t game_id {};
    std::uint16_t ply {};
    std::uint16_t encoded_move {};   // ← new; 0 for ply == 0
    std::int8_t   result {};
    std::optional<std::int16_t> white_elo;
    std::optional<std::int16_t> black_elo;
};
```

Add `encoded_move` to `tree_position_row`:

```cpp
struct tree_position_row {
    std::uint32_t game_id {};
    std::uint16_t root_ply {};
    std::uint16_t depth {};
    std::uint16_t encoded_move {};   // ← new; move that reached this position
    std::uint64_t child_hash {};
    std::int8_t   result {};
    std::optional<std::int16_t> white_elo;
    std::optional<std::int16_t> black_elo;
};
```

### `import_pipeline.cpp`

`prepare_game` already computes the encoded move for each ply during move replay;
populate `position_row::encoded_move` there. ply=0 stores `0`.

### `position_store.cpp` — `create_position_table`

Add `encoded_move USMALLINT NOT NULL` to the DDL.

### `position_store.cpp` — `insert_batch`

Bind the new column.

### `position_store.cpp` — `query_tree_slice`

Add `p_cont.encoded_move` to the SELECT. Populate `tree_position_row::encoded_move`.

### `opening_tree.cpp` — `open`

Replace the two-phase fetch with:

1. Call `query_tree_slice` — now returns `encoded_move` per row.
2. Group rows by `(game_id, root_ply)` as before.
3. **Drop the `get_game_contexts` call** from the hot loop. Use
   `row.encoded_move` directly in place of `context.moves[move_ply]`.
4. **Drop the `replay_position` hash-verification calls** (lines 300–302 and
   340–343). These were sanity checks on the position table; with `encoded_move`
   coming from the same table they are redundant.
5. For building the BFS node tree (second loop), the parent board is still needed
   to generate SAN via `chesslib::san::to_string`. Derive it by replaying the
   encoded-move chain from the root (which is already known or cheap to
   reconstruct from the position table), rather than replaying from game start.
6. For eco/opening attribution, call `get_continuation_contexts` with one
   `(sample_game_id, sample_ply)` pair per distinct continuation — O(distinct
   moves) lookups instead of O(N games).

## Performance Targets

| Scenario | Before | Target |
|---|---|---|
| Starting position, 1M games, depth=1 | ~2.9 s | < 500 ms |
| Starting position, 3.4M games, depth=1 | ~10 s (projected) | < 2 s |
| Typical mid-game position, 1M games, depth=5 | baseline | no regression |

## Prerequisites

- Spec 002 (import pipeline) complete.
- Spec 003 (search / opening tree) complete.

## Acceptance Criteria

- [ ] `position_row` carries `encoded_move`; ply=0 rows store `0`.
- [ ] Import pipeline populates `encoded_move` correctly for all plies.
- [ ] `query_tree_slice` returns `encoded_move` in each result row.
- [ ] `opening_tree::open` no longer calls `get_game_contexts` in the main loop.
- [ ] All existing opening-tree unit tests pass unchanged.
- [ ] `opening_tree::open from starting position aggregates first moves correctly`
      (small synthetic corpus) still passes.
- [ ] `opening_tree::open from starting position on real corpus` passes and
      reports elapsed ≤ 500 ms (excluding import time).
- [ ] No regression in the `opening_tree::open performance on sorted position
      store` p99 latency test.
- [ ] `position_row` size verified: `sizeof(position_row) == 28` (with alignment
      padding after `encoded_move`; verify and document actual layout).
- [ ] Zero new clang-tidy warnings.
