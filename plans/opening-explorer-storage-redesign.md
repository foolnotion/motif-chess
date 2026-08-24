# Opening Explorer Storage And Query Redesign

## Status

Proposal for review. This document does not authorize a format migration or a
change to the shipped database contract. It turns the measured Scidb/Motif
comparison into an incremental implementation plan.

The compact `move_hash` / `identity_collision` game identity index is already
implemented as schema v5. The plan preserves and benchmarks it; it does not
propose it as future work.

## Decision Summary

Retain Motif's shallow materialized opening statistics because they provide
better cold-query behavior at multi-million-game scale than Scidb's scanned
tree construction. Replace the full-depth materialized position graph over
time with a compact imported-game codec, a shallow position index, a compact
prefix-postings candidate index, and an on-demand replay fallback for deep
positions.

Borrow three ideas from Scidb:

1. A compact append-only game-data stream plus a fixed-width per-game index.
2. A bounded cache for repeated opening-tree navigation.
3. A cheap per-game position signature that rejects impossible replay
    candidates before decoding their moves.

The primary deep-query candidate mechanism is a prefix-postings index, not a
Scidb-style full-game scan or a signature scan. A signature may be a secondary
rejection step once postings have selected candidates.

Do not borrow Scidb's primary tree-query algorithm as Motif's cold-query path.
Scidb scans games after a signature prefilter; it is fast on small databases
and cached repeats, but cold queries on its 3.4M-game corpus grow to
380-570 ms at deep positions. Motif's shallow materialized rollup is the
appropriate primary path for an interactive opening explorer.

## Measured Baseline

The import inputs and accepted-game counts differ between engines because their
PGN normalizers reject different invalid games. Import timings must therefore
always report attempted, accepted, rejected, and elapsed counts together.

### Native Import And Storage

| Corpus | SCIDb native import | Motif SQLite-only import (schema v4) | Notes |
|---|---:|---:|---|
| 100k PGN | 100,000 written in 3.553 s | 98,807 committed in 4.809 s | Motif excludes 1,193 games |
| 1M PGN | 1,001,092 written in 35.303 s | 989,967 committed in 55.011 s | Motif excludes 11,125 games |

| 100k final artifact | Size | Schema |
|---|---:|---|
| SCIDb `.sci` + `.scg` + `.scn` | 19.86 MB | native Scidb |
| Motif `games.db` only | 29.87 MB | v5 |
| Motif `positions.duckdb` | 238.30 MB | v5 |
| Motif full bundle | 268.17 MB | v5 |

The full Motif schema-v5 100k build took 187.598 s. That number includes replaying every
accepted game into `position`, sorting it by Zobrist hash, and building the
opening rollup. It is not comparable to SCIDb's native game-file import alone.
It is the principal import-performance gap, and it must be broken down before
investing in the compact game codec.

Every future timing and size row must record the schema version, Git revision,
import configuration, input checksum, attempted games, committed games,
skipped games, and parse errors. A size or timing number without these fields
is not a comparable result.

### Full Build Profiling

The current default configuration rebuilds a full-depth position relation,
rewrites it in Zobrist order, then constructs `opening_continuation` through
global aggregation and a position self-join. The game codec changes the source
of moves, but does not automatically remove any of these three costs.

Phase 0 must time these intervals separately:

| Interval | Current implementation boundary |
|---|---|
| Game ingest | PGN read through SQLite batch commit |
| Position replay | `database_manager::rebuild_position_store()` through final position-row append |
| Zobrist sort | `position_store::sort_by_zobrist()` |
| Rollup build | `position_store::rebuild_opening_stats_rollups()` |

Only this breakdown can establish whether the next optimization should target
ingest, position sort, rollup construction, or the full-depth position design.
The current 187.598-second full-build number is not evidence that a game codec
will improve sort or rollup time.

The first schema-v5 100k breakdown measured 184.328 s in position replay,
735 ms in sort, and 724 ms in rollup construction (192.005 s total for the
deferred SQLite-import-plus-rebuild path). Replacing the per-game
`game_store::get()` reads with one minimal SQLite cursor improved replay by
only about 3.4 s, so it was not the dominant cost. The deeper timing found
that `position_store::insert_batch()` spent nearly all of replay time executing
one prepared `game_result` upsert per game. Staging those facts through a
DuckDB appender and using one set-based `INSERT OR IGNORE` per position batch
reduced the deferred full build to 8.032 s: 1.781 s replay, 757 ms sort, and
749 ms rollup. The default inline full build measured 7.872 s on the same
100k input, versus 3.553 s for SCIDb's game-only native import. This is a
remaining 2.2x wall-clock difference, while Motif also materializes its
position index and rollup.

### Opening Lookup

| 100k tree lookup | SCIDb | Motif schema v5 |
|---|---:|---:|
| Root, first lookup | 4.153 ms | 11.354 ms |
| Root, repeat lookup | effectively 0 ms | 6.666 ms |
| Ply 20, first lookup | 3-5 ms in the walked line | 2.518 ms |
| Ply 40, first lookup | not measured on the same position | 9.274 ms |

The 100k result does not replace the multi-million-game cold-query baseline.
On Scidb's 3.406M-game native TWIC database, its tree scan measured about
78 ms at ply 20, 48 ms at ply 54, and 380-570 ms after ply 85. Motif's prior
3.36M rollup baseline was p50 6.572 ms and p99 57.154 ms before the current
compact game-index work; it must be remeasured before setting a final target.
No fresh 3.36M schema-v5 bundle has yet been retained, so the older full-bundle
size tables below remain historical measurements, not v5 projections.

## Target Architecture

```text
                     imported PGN
                         |
                         v
              compact append-only game stream
                 |                         |
                 v                         v
       fixed-width game index       names and uncommon metadata
                 |
                 +--------------------------+
                 |                          |
                 v                          v
  shallow opening-continuation rollup   capped position index
                 |                          |
                 v                          v
          opening-explorer cache      replay fallback + signatures
```

### Compact Game Stream

Imported games are immutable. Store their moves and optional uncommon metadata
in an append-only data file addressed by offset and length. Store a fixed-size
index record per game with the move-data offset, move count, result, ratings,
date representation, player/event IDs, and replay signature.

Manual and editable games may remain in SQLite initially. A later migration can
either rewrite an edited imported game into an overlay or explicitly convert it
to a manual record. Do not let this compatibility question block the import
codec prototype.

The format must specify byte order and a versioned header. Motif's current raw
`uint16_t` move blob is host-endian, so a new portable format should encode
moves explicitly as little-endian 16-bit values before introducing further
compression.

### Shallow Rollup

Continue materializing direct continuation statistics for shallow roots. The
initial cap remains `root_ply <= 20` until real usage and latency measurements
choose another value.

Reducing the rollup cap to 10 is an isolated experiment:

| Rollup cap | Measured rows | Measured size |
|---|---:|---:|
| 20 | 12.8M | 0.48 GB |
| 10 | 1.0M | 0.04 GB |

The existing fallback preserves correctness when a rollup row is absent. It
does not preserve the shallow latency guarantee, so do not change the default
cap until the benchmark gate below passes.

### Capped Position Index And Replay Fallback

The dense `position` table is the dominant remaining space cost:

| Slim position coverage | Measured size |
|---|---:|
| Full depth | 3.28 GB |
| `ply <= 40` | 1.38 GB |
| `ply <= 20` | 0.40 GB |

The capped table is an accelerator, not the source of truth. For a root hash
past the cap, the query path must:

1. Obtain candidate game IDs from a shallow predecessor index or replay
   signature.
2. Reject candidates whose signature cannot match the requested position.
3. Decode only surviving move streams until the requested position or game end.
4. Aggregate the next move, result, and ratings exactly as the current dynamic
   path does.

The fallback must return the same result as the uncapped position table for
transpositions, repeated positions, terminal positions, and filtered queries.
No cap may ship before this equivalence is established.

### Prefix-Postings Candidate Index

The replay fallback needs an exact, bounded candidate source. A compact
`position_prefix_postings` sidecar supplies it without retaining a full row for
every occurrence.

For every position reached while importing a game, derive a fixed-width prefix
from its 64-bit Zobrist hash. Append the game ID to that prefix's postings list
at most once per game. The on-disk index consists of a versioned header,
prefix-to-postings offsets, and delta-coded sorted game IDs. The full Zobrist
hash, ply, move, result, and Elo do not appear in a postings record.

For a deep query, derive the target hash's prefix, load its candidate game IDs,
apply metadata filtering where requested, then replay each surviving game from
the compact move stream. A game contributes only if replay reaches the complete
64-bit target hash. This preserves exact semantics: a matching game cannot be
absent because its target position generated the same prefix during import;
prefix collisions only add candidates that replay rejects.

This is deliberately distinct from a move trie. The query key is a position
hash, so the index includes transpositions and repeated positions. It is also
distinct from a per-game Bloom filter: a Bloom filter needs a full sequential
scan to discover candidates and is therefore unsuitable as the primary cold
query path at multi-million-game scale.

The initial prototype should compare 20-, 22-, and 24-bit prefixes. Each run
must report sidecar bytes, bytes per indexed position occurrence, postings
count, candidates per query, replayed games per query, and cold/warm latency
at root, ply 10, 20, 40, median, and deep-p99 positions. The shallow rollup
continues to serve shallow roots, so prefix collision behavior at highly common
opening positions does not define the deep-path budget.

Do not replace the production position table until this sidecar has passed the
reference-oracle equivalence suite. It is a prototype data structure, not a
schema migration.

### Import Reader Memory Gate

`pgn::import_stream` materially reduces parser allocations, but its current
public API has no bounded-memory file mode. Its path constructor reads the
whole file into an internal buffer; its `std::string_view` constructor instead
borrows a caller-owned contiguous buffer. Motif currently chooses the latter
and retains a full-file `std::string` in `pgn_reader`, so peak RSS grows with
PGN size (measured at about 1.1 GB for an 896 MB 1M-game corpus).

Do not add another downstream PGN boundary parser as the permanent remedy.
The required remedy is an upstream `pgnlib` incremental import API that accepts
bounded chunks, recovers at a game boundary, and guarantees that returned tag
and SAN views stay valid until the consumer advances the stream. Motif can then
retain only a bounded carry-over plus the current game. Byte offsets and
malformed-game recovery must retain their existing semantics.

Until that API exists, retain `import_stream` only if the product accepts its
documented RAM budget. A short-lived benchmark may compare the old bounded
reader plus per-game `import_stream(std::string_view)` against the full-file
reader, but it is not a replacement parser design and must measure acceptance,
resume offsets, throughput, and peak RSS before any adoption.

### Cache

Add a bounded LRU cache of unfiltered `opening_stats::stats`, keyed by root
Zobrist hash and a data-generation number. A data-generation increment on
import, deletion, Elo update, or rollup rebuild invalidates all cached entries
without per-entry bookkeeping.

Start with unfiltered results only. Filtered queries have an unbounded key
space and must not silently reuse an unfiltered or differently filtered entry.
The initial capacity should be benchmarked at 256 entries to match the Scidb
navigation cache, then adjusted based on retained-memory measurements.

## Delivery Phases

### Phase 0: Baseline And Guardrails

Purpose: make subsequent storage wins comparable and prevent a recurrence of
the identity-index import regression.

- Preserve benchmark corpora and record source SHA-256, attempted count,
  accepted count, skipped count, and parse-error count.
- Run SCIDb `cdb2sci` and Motif SQLite-only import on 100k and 1M corpora.
- Record Motif's schema version, Git revision, and complete import configuration
  beside every benchmark and storage result.
- Record PGN reader mode, peak RSS attributable to reader buffering, and whether
  the source buffer is bounded by a configured maximum.
- Run Motif full build on 100k and a fresh comparable multi-million-game
  corpus. Retain the artifact for table-level size analysis.
- Split every full build into game ingest, position replay, Zobrist sort, and
  rollup construction. Record wall time, CPU time, peak RSS, rows written, and
  temporary disk high-water mark for each interval.
- Benchmark root, ply 10, 20, 40, median, and deep-p99 positions in fresh
  processes and repeated navigation sequences.
- Keep the compact `move_hash` / `identity_collision` duplicate index covered
  by exact duplicate and distinct-move tests.

Exit gate: the report includes schema/config revision, comparable counts,
component timings, disk sizes, cold latencies, warm latencies, and peak RSS. A
result that lacks its accepted-game count or a full-build component breakdown
is incomplete.

### Phase 1: Unfiltered Explorer Cache

Purpose: improve repeated board navigation without a file-format change.

- Implement the generation-keyed LRU cache around the public unfiltered
  `opening_stats::query` boundary.
- Invalidate after every mutation that currently drops or rebuilds rollups.
- Test cache hit, invalidation after import/delete/Elo update, and filtered
  query bypass.

Exit gate: repeat root lookup is within 1 ms on the benchmark machine, and the
cache does not change result values after a mutation.

### Phase 2: Compact Game-Codec Prototype

Purpose: determine whether a Scidb-like imported-game representation justifies
replacing SQLite for immutable imported games.

- Define a versioned binary format with a header, a fixed index record, a
  little-endian move stream, and dictionary IDs for player/event/site values.
- Build a one-way importer that writes the codec alongside the existing
  SQLite database. Do not switch reads yet.
- Implement random game retrieval and a byte-for-byte semantic comparison with
  `game_store::get` for a corpus sample.
- Measure data-file size, index size, name-table size, import time, game-open
  latency, and memory mapping behavior.

The codec prototype must separately report its stream-write time and the time
to replay from the stream into the current position/rollup builders. A smaller
or faster game codec does not pass this phase if the complete import path has
not been measured.

Exit gate: imported-game storage is materially smaller than the current
fingerprint-indexed SQLite layout, random retrieval remains within the current
UI budget, and the format survives close/reopen on Linux and Windows test
fixtures.

### Phase 3: Replay Query Prototype

Purpose: prove that capped storage can retain deep-query correctness.

- Add a replay implementation over the compact move stream.
- Build `position_prefix_postings` beside the existing full-depth position
  table. Evaluate 20-, 22-, and 24-bit prefixes rather than selecting one from
  estimates alone.
- Apply metadata filters to the candidate IDs, then replay remaining games and
  verify the complete Zobrist hash before aggregation.
- Add a per-game signature only if profiling shows replay rejection remains a
  material cost after prefix selection.
- Build a reference oracle using the current uncapped `position` table.
- Compare replay results against the oracle across shallow, median, deep,
  transposed, repeated, and terminal positions.
- Include metadata-filtered queries in the oracle suite; candidate selection
  may change the execution plan but not query semantics.
- Prototype caps at ply 40 and ply 20; do not alter production defaults.

Exit gate: exact result equivalence for the oracle suite and an agreed deep
query latency budget, including candidate and replay counts. If replay cannot
meet the budget, retain more shallow index coverage rather than returning
incomplete data.

### Phase 4: Production Migration

Purpose: switch new imports only after the codec and replay paths are proven.

- Write new imported games to the compact codec while retaining SQLite for
  manual records and migration metadata.
- Build shallow rollups and the capped index independently from the complete
  decoded stream in bulk. Never build a rollup from a capped position index:
  an edge rooted at the cap needs its child one ply beyond that cap.
- Provide an explicit conversion command for existing bundles, with a
  disk-space estimate and resumable checkpoint.
- Preserve the old bundle until the converted bundle validates successfully.
- Define rollback as reopening the untouched old bundle; never perform an
  in-place destructive rewrite.

Exit gate: upgrade, validation, rollback, interrupted conversion, and manual
game edits are all tested on an on-disk fixture.

## Decisions Required Before Phase 2

1. Is the compact codec only for imported games, with SQLite retained for
   manual/editable games? Recommended: yes for the first release.
2. What is the persistent compatibility policy? Recommended: versioned bundles
   with explicit, resumable conversion and rollback to the old bundle.
3. What cold and warm latency budgets apply at 3M and 10M games?
4. Which Phase 0 component owns the full-import budget, and what target applies
   independently to ingest, position replay/sort, and rollup construction?
5. Is `root_ply <= 10` acceptable if real corpus measurements show its dynamic
   fallback is within the shallow-query budget?
6. Does the product need arbitrary metadata-filtered explorer results to be
   cacheable, or is cache support for the unfiltered navigation path sufficient?
7. What peak-RSS budget is acceptable for a multi-gigabyte PGN import while an
   upstream bounded-memory `pgnlib` import API is unavailable?

## Explicit Non-Goals

- Reimplementing or embedding the Scidb `.sci` format.
- Replacing Motif's shallow rollup with a full-game scan.
- Capping the current `position` table without replay fallback.
- Using a per-game Bloom-filter scan as the primary deep-query candidate path.
- Maintaining a second downstream PGN parser to compensate for an upstream
  streaming API gap.
- Automatically running SQLite `VACUUM` during a schema or bundle migration.
- Claiming byte-for-byte database equivalence across engines with different
  PGN acceptance and metadata-normalization policies.

## Evidence And Related Work

- `docs/handoffs/2026-08-17-opening-explorer-storage.md`
- `source/motif/db/position_store.cpp`
- `source/motif/search/opening_stats.cpp`
- `source/motif/db/game_writer.cpp`
- `/home/bogdb/src/scidb-fork/src/db/db_tree.cpp`
- `/home/bogdb/src/scidb-fork-svn/src/db/sci/sci_codec.cpp`
