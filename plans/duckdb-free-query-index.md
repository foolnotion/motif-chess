# DuckDB-Free Position And Header Query Plan

## Status

Proposal for review. This plan defines a DuckDB replacement while retaining
SQLite as the canonical store for games and headers. It does not authorize
DuckDB removal until every replacement query passes exact-equivalence and
performance gates.

This plan supersedes the prefix-only candidate-index direction in
`plans/opening-explorer-storage-redesign.md`. Prefix postings may remain a
benchmark baseline, but the primary design uses exact full-hash postings so
queries do not depend on collision filtering for correctness or latency.

## Decision

Keep `games.db` as the authoritative relational store. It owns game headers,
players, events, tags, provenance, results, Elo, and encoded moves.

Replace `positions.duckdb` with immutable derived indexes:

1. `position_postings.idx` maps every full 64-bit Zobrist hash to the games
   and plies where it occurs.
2. `opening_tree.idx` caches unfiltered direct-continuation aggregates through
   a configured shallow depth, initially ply 20.

The general query path combines SQLite header filtering, exact posting
intersection, and deterministic replay of only surviving games. It must return
the same results as the current DuckDB path.

## Scope

In scope:

- Position search and position-constrained game lists.
- Unfiltered and filtered opening explorer queries.
- Elo distributions by continuation.
- Full-depth tree traversal.
- Import-time index construction and index lifecycle.
- Exact result equivalence with the current DuckDB public APIs.

Out of scope:

- Replacing SQLite header storage.
- Changing PGN import, identity, or header-filter semantics.
- Adding a bitmap or columnar dependency before measurements justify it.
- Removing DuckDB before all delivery gates pass.

## Required Semantics

### Canonical Store And Generations

SQLite remains the sole source of game/header truth. Derived indexes may refer
to `game_id`, but do not duplicate player, event, result, Elo, or tag truth.

Every index carries the SQLite game-set generation and game count from which it
was built. A query must reject a generation mismatch, or use an explicitly
correct fallback. It must never mix old postings with new headers silently.

### Position Occurrences

An occurrence is `(zobrist_hash, game_id, ply)`. A game may have multiple
occurrences for a hash because repetition can revisit a position. Preserve all
of them.

The contracts remain distinct:

- Occurrence count: every matching `(game_id, ply)`.
- Distinct-game count: each matching game once.
- Continuation frequency: each game contributes at most once per displayed
  `(root_hash, encoded_move, child_hash)` edge, at its minimum root ply.
- Transposition frequency: distinct games reaching the child hash anywhere in
  their replay.

Index terminal roots even when they have no continuation. Preserve
transpositions and repeated positions at every depth.

### Dispatch

`opening_tree.idx` is an accelerator only. A missing aggregate does not prove
that a position is absent; the fallback uses exact postings and replay.

Header-only game queries stay in SQLite. Position-constrained queries intersect
SQLite-filter IDs with exact posting IDs. They do not use hash-prefix collision
candidates.

## Formats

### Manifest

Use Glaze BEVE or JSON for a small derived-index manifest:

```text
format_version
sqlite_schema_version
source_generation
game_count
position_postings_filename
opening_tree_filename
file_sizes
checksums
created_at
```

Publish the manifest only after every referenced file is fully written,
checksummed, closed, and atomically replaced. The manifest is the multi-file
generation commit point.

### `position_postings.idx`

Use a purpose-built binary layout. Full queries need direct lookup and
block-level decoding, not deserialization of an entire object graph.

```text
header
  magic, format version, byte order
  source generation, game count
  directory offset and count
  payload checksum

directory, sorted by full Zobrist hash
  hash: uint64
  posting block offset: uint64
  posting block byte length: uint64
  distinct game count: uint32
  occurrence count: uint64

posting blocks
  ascending game IDs, delta-varint encoded
  occurrence count for each game
  ascending plies, delta-varint encoded
```

The directory enables binary search without reading unrelated blocks. The
reader validates bounds, integer narrowing, order, checksums, and EOF.

### `opening_tree.idx`

Retain the current immutable shallow aggregate format, but harden it before
live use:

- Add source generation and a payload checksum.
- Write to a temporary file, then atomically replace it.
- Validate node-hash ordering, count narrowing, record bounds, and EOF.
- Use only when its configured depth covers the request.

Glaze does not replace either index body. A generic BEVE object graph would
still require an offset directory, external merge construction, block bounds,
and custom compression. Use Glaze at the manifest boundary only.

## Construction

Build indexes after successful SQLite import and duplicate reconciliation.
Never publish an index built from pre-dedup rows.

1. Obtain a stable SQLite generation marker and game count.
2. Stream `replay_game_record` values in ascending game ID order.
3. Replay each game and emit `(hash, game_id, ply)` tuples.
4. Sort bounded batches by `(hash, game_id, ply)` and spill runs to staging.
5. K-way merge runs and write compressed posting blocks plus the directory.
6. Build the shallow aggregate in the same replay or a separately benchmarked
   pass.
7. Checksum and atomically publish index files, then publish the manifest.
8. Remove staging files only after successful manifest publication.

The builder must not retain every tuple or a full-corpus aggregate map in RAM.
The existing spill-and-merge `hash_visit_counter` is a pattern, not a complete
posting builder.

### Manual Mutations

Bulk import publishes one rebuilt immutable generation. Do not synchronously
rebuild full postings for a single manual mutation.

Initially, mark indexes stale after a manual insert, edit, or delete and route
position queries through a correct replay fallback until rebuild. A mutable
delta segment and compaction are later optimizations, not first-parity work.

## Query Paths

### Position Search

1. Binary-search the exact hash directory.
2. Decode one posting block.
3. Fetch game contexts from SQLite by game ID.
4. Apply deterministic game-ID ordering and pagination.

### Position-Constrained Game Lists

1. SQLite evaluates header filters to ascending candidate IDs.
2. The posting block supplies ascending matching game IDs.
3. Intersect IDs.
4. SQLite materializes matching game contexts.

### Explorer, Tree, And Elo

For an unfiltered shallow request covered by `opening_tree.idx`, return the
precomputed aggregate. Otherwise:

1. Decode root occurrences.
2. Apply any SQLite header filter.
3. Replay each surviving game from its root ply through the requested depth.
4. Deduplicate per-game edges and calculate W/D/L, Elo, ECO, and
   transposition fields with the existing definitions.

Elo distributions use the same path and SQLite game contexts, bucketing Elo in
process. A bounded cache is permitted only after cold-query profiling; its key
includes index generation, root hash, depth, and canonical filter state.

## Delivery Phases

### Phase 0: Contracts And Oracle

- Document DuckDB behavior for every public position and explorer API.
- Create deterministic fixtures for transpositions, repetitions, terminal
  positions, duplicate games, missing Elo, and metadata filters.
- Compare existing DuckDB and replacement results field-for-field.
- Define source-generation and manifest contracts.

Exit gate: each public contract has independent fixtures and expected results.

### Phase 1: Exact Postings Prototype

- Implement external spill/merge and the postings reader.
- Test corruption, ordering, generation mismatch, and random access.
- Measure build time, peak RSS, bytes, and bytes per occurrence at 1M games.
- Replace only position lookup/count/game-ID APIs behind test dispatch.

Exit gate: exact position-search parity and a bounded-memory 1M build.

### Phase 2: Position-Constrained Game Search

- Implement SQLite-ID/posting-ID intersection.
- Replace `database_manager::find_games` position filtering.
- Test ordering, pagination, filters, no matches, and stale behavior.

Exit gate: exact parity for all position-constrained game-list fixtures.

### Phase 3: Replay Fallbacks

- Implement explorer/tree/Elo aggregation from postings and replay.
- Route filtered queries and deep unfiltered queries through it.
- Compare every continuation field and Elo bucket with DuckDB.

Exit gate: exact deep parity including transposition and repetition fixtures.

### Phase 4: Live Shallow Index

- Harden, publish, and dispatch `opening_tree.idx`.
- Retain the replay fallback for every uncovered query.

Exit gate: shallow aggregate equivalence and cold/warm latency measurements.

### Phase 5: DuckDB Removal

Status: complete. DuckDB has been removed from runtime code, tests, CMake,
Nix, and vcpkg. Exact postings are the sole durable position index.

Done:

- `database_manager::create()` no longer creates `positions.duckdb`. It
  publishes a trivial empty (0-game) postings generation immediately instead,
  so a bundle with no imports yet never falls back to DuckDB just because
  nothing has been imported.
- `database_manager::open()` rebuilds missing, stale, or corrupt postings
  directly from canonical SQLite. Repair failure fails closed without
  publishing partial bytes. A valid bundle with a leftover
  `positions.duckdb` ignores that file and leaves it untouched.
- Every query and mutation path (`query_position_matches`,
  `position_game_ids`, `query_elo_distribution`,
  `query_unfiltered_opening_stats`, `query_filtered_opening_stats`,
  `query_tree_slice`, `find_games`, `patch_game_metadata`, `remove_game`)
  uses postings and canonical replay, and fails closed (`error_code::io_failure`)
  rather than serving stale or absent derived data.
- One-release migration window: `open()` makes a single best-effort attempt
  to build and publish postings for a legacy bundle, using the same
  staged-build/validate/atomic-publish path as every other postings rebuild.
  Migration replays canonical SQLite and never reads or deletes a leftover
  `positions.duckdb`. On failure, `open()` fails closed without changing the
  published manifest; the attempt is safe to retry.
- `create_scratch()` uses the same on-disk `create()` path as a persistent
  bundle (games.db, manifest.json, and derived-index files) staged in a
  private temporary directory that is removed on close, rather than an
  in-memory SQLite connection.
- `database_workspace::recent_with_status()` no longer requires
  `positions.duckdb` to mark a bundle available.
- Clean close clears the legacy dirty marker when checksum-verified postings
  cover canonical SQLite.
- Import and manual-game mutation paths update canonical SQLite and mark
  postings stale immediately
  (`prepare_canonical_mutation()`), consistent with this plan's "Manual
  Mutations" policy, and every position-query path fails closed until an
  explicit rebuild -- there is no live replay-on-demand fallback yet for a
  postings-only bundle after a manual mutation.

Deferred follow-up work:
- A live replay-on-demand fallback for manual mutations on a postings-only
  bundle (today: fail closed until an explicit rebuild), if that gap proves
  too disruptive in practice.
- Moving `open()`'s migration attempt off the synchronous open path for very
  large legacy bundles, if it proves too slow in practice (no large-corpus
  measurement was taken for this change).

## Benchmark Gates

Every run records Git revision, corpus checksum, attempted/committed/skipped/
error counts, worker configuration, wall time, peak RSS, temporary disk, and
final artifact sizes.

| Metric | Required comparison |
|---|---|
| Base build | Wall time and peak RSS versus DuckDB rebuild |
| Storage | `games.db + indexes` versus `games.db + positions.duckdb` |
| Position search | Cold/warm p50/p99 for rare, typical, and popular hashes |
| Filtered explorer | Low, medium, and high match cardinalities |
| Deep tree | Root, ply 10, 20, 40, median, and deep-p99 positions |
| Import | Default import with and without index construction |
| Manual edits | Stale behavior and rebuild/compaction time |

Current 1M reference measurements:

| Path | Total | Retained bundle |
|---|---:|---:|
| SQLite plus DuckDB positions | 48.555 s | about 2.28 GiB, separately measured |
| SQLite plus standalone shallow `opening_tree.idx` | 46.706 s | 520 MiB |

The shallow candidate is not a full DuckDB-replacement measurement. It does
not cover full-depth, filtered, or position-search queries.

## Risks

- Exact full postings may approach the current position-table storage cost.
- Popular positions can have large posting blocks and expensive filtered replay.
- Collapsing repetitions to one game ID would make tree semantics incorrect.
- Multi-file index publication must be atomic at the manifest generation level.
- Stale indexes must fail closed or use a correct fallback.
