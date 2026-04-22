---
date: '2026-04-21'
type: 'technical-design-note'
topic: 'Sparse position indexing for PGN import throughput'
status: 'proposed'
related:
  - _bmad-output/implementation-artifacts/2-8-reprofile-and-tune-remaining-import-storage-path.md
  - source/motif/db/position_store.cpp
  - source/motif/import/import_pipeline.cpp
---

# Sparse Position Indexing Design Note

## Purpose

This note proposes a concrete next-step design for improving PGN import throughput after Story 2.8 profiling.

The current profiling result is:

- the DuckDB appender path is already correct and no longer the main issue
- final DuckDB index rebuild is real cost, but not the dominant remaining cost
- the hottest remaining compute bucket is SAN-to-move resolution plus per-ply board advancement
- the current architecture writes one `position_row` per ply, which multiplies both import work and storage volume by game length

The most promising structural option is therefore to stop indexing every single position and instead index every `N`th position, then replay at most `N - 1` plies during search.

## Recommendation

Recommended next design direction:

1. Introduce sparse position anchors in DuckDB.
2. Keep full canonical move streams only in SQLite `game.moves`.
3. During search, use the nearest stored anchor for a candidate game and replay forward in memory to the requested ply window.
4. Choose `N` empirically from benchmark data rather than guessing.

Recommended first benchmark set: `N in {4, 8, 12, 16}`.

## Current State

Current imported position schema:

```cpp
struct position_row {
    std::uint64_t zobrist_hash{};
    std::uint32_t game_id{};
    std::uint16_t ply{};
    std::int8_t result{};
    std::optional<std::int16_t> white_elo;
    std::optional<std::int16_t> black_elo;
};
```

Current behavior:

- every legal move is SAN-decoded
- every move is applied to a board
- every resulting position hash is appended to DuckDB
- position row count is approximately total plies across all games

For long imports, this means import work scales with every ply even though many search features may not require every ply to be indexed directly.

## Proposed Model

### Core Idea

Store only anchor positions in DuckDB.

For a game with plies `1..P`, store rows only where:

- `ply == 1`, if we want immediate first-move reachability, and/or
- `ply % N == 0`

Suggested default anchor rule for evaluation:

- store every `N`th ply
- also store the final ply when it is not already aligned to `N`

That gives bounded replay and ensures terminal positions remain directly indexed.

### Why This Helps

If average game length is `L` plies, sparse indexing reduces DuckDB position rows by about `N x` in the steady state.

Approximate effects:

- DuckDB append calls drop by about `N x`
- DuckDB file size drops by about `N x` for the position table before compression effects
- per-ply row materialization drops by about `N x`
- import still needs SAN resolution for replayable move streams, but no longer needs to persist every intermediate position

This does not remove SAN decoding from import, but it removes a large amount of downstream per-ply row production and storage maintenance.

## Search Strategy

### Query Shape

Search for a target board position should become a two-stage process:

1. Use the target position's Zobrist hash to fetch candidate anchor rows from DuckDB.
2. For each candidate game, replay moves around the anchor in memory to verify whether the exact requested position occurs.

### Replay Bound

If anchors are every `N` plies, the worst-case replay distance is:

- backward replay not needed if search is expressed as "find positions reachable after anchor"
- forward replay up to `N - 1` plies from a matching prior anchor

If exact arbitrary-ply lookup is required and the nearest earlier anchor is used, replay bound is at most `N - 1` plies.

This is the main tradeoff:

- import becomes cheaper
- query verification becomes slightly more expensive
- replay cost is bounded and predictable

## Schema Options

### Option A: Reuse Existing `position` Table

Keep the same table and just store fewer rows.

Pros:

- minimal schema change
- no additional migration complexity
- existing `zobrist_hash`, `game_id`, `ply` semantics still hold

Cons:

- table name no longer implies "every position"
- future code may accidentally assume dense coverage

### Option B: Rename to `position_anchor`

Create an explicit sparse-anchor table.

Suggested schema:

```sql
CREATE TABLE IF NOT EXISTS position_anchor (
    zobrist_hash  UBIGINT   NOT NULL,
    game_id       UINTEGER  NOT NULL,
    ply           USMALLINT NOT NULL,
    result        TINYINT   NOT NULL,
    white_elo     SMALLINT,
    black_elo     SMALLINT
)
```

Pros:

- semantically correct
- makes the sparse contract explicit
- safer for future search code

Cons:

- slightly larger migration/change surface

Recommendation: prefer Option B if this becomes the long-term design. Prefer Option A only for a short spike or benchmark branch.

## Import-Side Changes

### Minimal Change Path

In `prepare_game()` and the rebuild path, keep advancing the board through every move, but emit a `position_row` only at anchor plies.

Pseudo-rule:

```cpp
auto const ply = static_cast<std::uint16_t>(encoded_moves.size());
if (should_store_anchor(ply, total_ply_count, anchor_stride)) {
    position_rows.push_back(...);
}
```

This preserves correctness while making the first implementation small.

### Future Optimization Path

If sparse indexing proves correct and useful, a second optimization can avoid some row-object churn by:

- still resolving and applying every SAN move
- but only allocating/storing anchor rows

This is simpler and safer than trying to skip board advancement during import.

## Search-Side Changes

Search will need an in-memory verification step that:

1. loads `game.moves` from SQLite for each candidate `game_id`
2. reconstructs the board from the start position
3. replays moves until the target ply window is reached
4. verifies the exact board hash before accepting the match

This implies a useful API shape in `motif_db` or `motif_search`:

```cpp
verify_game_contains_position(game_id, target_hash, anchor_ply, max_forward_replay)
```

The verifier must be bounded and deterministic. For stride `N`, `max_forward_replay` is `N - 1`.

## Expected Tradeoffs

### Benefits

- materially fewer DuckDB rows
- faster imports
- smaller DuckDB database file
- reduced index-build cost when indexing is enabled
- simpler path to meeting the import NFR than trying to shave a few percent from SAN resolution alone

### Costs

- exact position search becomes a candidate-generation plus verification workflow
- higher query CPU per candidate
- more importance on good candidate pruning and bounded replay
- more search implementation complexity than dense direct lookup

### Risks

- if many games share common anchors, candidate sets may become large
- if `N` is too large, replay cost may overwhelm search latency targets
- if the final position is not anchored, endgame-heavy queries may regress disproportionately

## Anchor Policy Variants

### Variant 1: Fixed Stride

Store plies `N, 2N, 3N, ...`.

Best first implementation.

### Variant 2: Fixed Stride Plus Final Ply

Store `N, 2N, 3N, ...` plus the terminal ply.

Recommended default for evaluation because it protects final-position queries cheaply.

### Variant 3: Phase-Aware Density

Examples:

- every 4 plies in opening, every 8 or 12 later
- denser near tactical/endgame phases

This is higher complexity and should be deferred until fixed-stride data says it is necessary.

## Choosing `N`

Do not choose `N` by intuition.

Use these decision criteria:

1. Import throughput gain must be material.
2. Candidate verification latency must stay within search NFR targets.
3. DuckDB database size reduction must be meaningful.
4. Query accuracy must remain exact after replay verification.

Recommended benchmark matrix:

- `N = 4`
- `N = 8`
- `N = 12`
- `N = 16`

Measure for each:

- import wall time on 10k fixture
- extrapolated row-count reduction
- DuckDB file size
- candidate count for representative position queries
- exact query latency with replay verification
- worst-case replay distance actually observed

Initial expectation:

- `N = 4` is low-risk and may already give meaningful storage relief
- `N = 8` is the most likely balanced default
- `N >= 12` may be attractive for import speed but risks query-latency regression

## Benchmark Plan

### Import Benchmarks

For each stride `N`:

- run release perf import on the 10k PGN fixture
- record `summary.elapsed`
- record whole-process wall time
- record DuckDB file size after import
- record row count in the anchor table

### Search Benchmarks

For a representative sample of positions:

- opening position match
- common middlegame structure
- tactical position with many transpositions
- endgame position

Measure:

- number of candidate anchor rows returned by DuckDB
- number of verified matches
- average replay plies per verified candidate
- P50/P95/P99 query latency

### Acceptance Gate For Adoption

Adopt sparse indexing only if all are true:

- import time improves by at least 2x relative to dense full-position storage
- exact search remains correct
- representative query latency remains within target budget or within an agreed temporary budget for Epic 3

## Migration Strategy

Recommended implementation order:

1. Add a benchmark-only configurable anchor stride to import and rebuild paths.
2. Keep dense mode as `N = 1` for comparison.
3. Prototype search-side replay verifier against the sparse table.
4. Benchmark the matrix above.
5. Only then decide whether sparse indexing becomes the default architecture.

This keeps the decision reversible and evidence-driven.

## Recommendation For Next Story

Recommended next story scope:

1. Add configurable sparse anchor indexing to import and rebuild paths.
2. Add a small search-verification prototype over `game.moves` replay.
3. Benchmark `N in {4, 8, 12, 16}` on import time, row count, database size, and exact-query latency.
4. Choose one of:
   - keep dense indexing
   - adopt sparse indexing with a fixed default `N`
   - move to a hybrid anchor policy

## Final Recommendation

Sparse position indexing is the strongest current candidate for solving the import throughput problem without fighting the profile one small hotspot at a time.

It is not yet proven to be the right permanent architecture, but it is the right next experiment because:

- it attacks the multiplicative per-ply storage cost directly
- it aligns with the current profile better than more appender tuning
- it is compatible with exact search through bounded replay verification

Recommended first target: evaluate `N = 8` as the likely balance point, but benchmark `4/8/12/16` before making it permanent.
