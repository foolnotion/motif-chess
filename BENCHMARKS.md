# Benchmarks

Performance results recorded on each meaningful change. All timings taken on the release build (`cmake --preset=release`) unless noted otherwise.

---

## 2026-04-24 — Import Strategy Decision

**Machine:** Linux, Clang 21, perf-release build (`-O3 -DNDEBUG -march=x86-64-v3`)

### Import Pipeline — 3.41M games (`bench/data/twic-all.pgn`)

| Benchmark | Attempted | Committed | Skipped | Errors | Import elapsed | Wall | User (s) | Sys (s) | CPU% | Peak RSS |
|---|---|---|---|---|---|---|---|---|---|---|
| Default fast path (`write_positions=false`, sorted rebuild) | 3,411,924 | 3,381,047 | 30,877 | 9,134 | 303,384 ms | 5:04.62 | 448.63 | 145.80 | 195% | 17,983,084 KB |

### Decision

- Direct position writes were removed: they were dramatically slower and did not provide enough practical value over deferred rebuild.
- Partitioned rebuild was evaluated as a replacement candidate, but after making it preserve the normal final `position` table semantics it was still slower than the default path and no longer showed a convincing memory advantage.
- **Default fast path remains the only supported import strategy.**

---

## 2026-04-22 — Story 2.11 post-review (num_workers=4 fixed default, idempotent sort_by_zobrist)

**Machine:** Linux, Clang 21, release build

### Import Pipeline — 10k games

| Benchmark | Wall (s) | User (s) | Sys (s) | CPU% |
|---|---|---|---|---|
| Default fast path (4w, 64L, write_positions=false, sorted rebuild) | 0.82 | 1.07 | 0.33 | 171% |
| Serial fast path candidate (1w, 1L, write_positions=false, sorted rebuild) | 0.91 | 0.91 | 0.09 | 109% |
| Pipeline mode (4w, write_positions=true) | 9.59 | 7.41 | 0.69 | 84% |
| Serial mode (1w, write_positions=true) | 8.32 | 5.62 | 0.39 | 72% |
| SQLite-only serial | 0.40 | 0.36 | 0.03 | 98% |
| SQLite + rebuild (sorted, default) | 0.91 | 0.96 | 0.10 | 115% |
| SQLite + rebuild (unsorted, no index) | 0.85 | 0.77 | 0.07 | 99% |
| SQLite + rebuild (sorted, no index) | 0.90 | 0.91 | 0.10 | 111% |
| Rebuild-only | 0.92 | 0.98 | 0.08 | 116% |
| DuckDB no-index serial (1w, write_positions=true) | 8.37 | 5.61 | 0.42 | 72% |
| SQLite + partitioned rebuild | 0.87 | 0.82 | 0.07 | 102% |

### Import Pipeline — 1M games

| Benchmark | Wall | User (s) | Sys (s) | CPU% |
|---|---|---|---|---|
| Default fast path (4w, 64L, sorted rebuild) | 1:25 | 124.51 | 39.97 | 193% |
| Serial fast path candidate (1w, 1L, sorted rebuild) | 1:39 | 111.03 | 15.29 | 127% |
| SQLite-only serial | 45.5s | 35.80 | 8.86 | 98% |
| SQLite + rebuild (unsorted, no index) | 1:29 | 79.43 | 10.21 | 101% |
| SQLite + rebuild (sorted, no index) | 1:36 | 104.16 | 14.23 | 123% |
| Rebuild-only | 1:37 | 104.94 | 14.33 | 123% |

### Query Latency — 1M games, 82.3M positions, 192 sampled hashes

| Mode | p50 (µs) | p99 (µs) | min (µs) | max (µs) | total (ms) |
|---|---|---|---|---|---|
| Unsorted | 14,438 | 50,061 | 13,771 | 57,711 | 3,032 |
| Sorted by zobrist | 646 | 28,715 | 508 | 38,523 | 274 |

### Key Observations

- **Default fast path wins over serial**: 1:25 vs 1:39 wall time on 1M games (~14% faster)
- **Sorted-by-zobrist gives 22× query latency improvement**: 646µs p50 vs 14,438µs p50
- **SQLite-only is the floor**: 45.5s for 1M games; total pipeline cost is rebuild + SQLite overhead
- **write_positions=false fast path is the production default**: avoids per-move DuckDB overhead during import

---

## 2026-04-22 — Story 3.1 position_search API

**Machine:** Linux, Clang 21, release build

### Position Search API — 1M games, sorted-by-zobrist store, 199 sampled hashes

| Benchmark | Wall | User (s) | Sys (s) | CPU% | p50 (µs) | p99 (µs) | min (µs) | max (µs) | total (ms) |
|---|---|---|---|---|---|---|---|---|---|
| `position_search::find` on sorted position store | 1:24 | 118.66 | 41.51 | 189% | 656 | 13,077 | 522 | 16,211 | 201 |

### Notes

- This benchmark exercises the public `motif_search` API, not the lower-level `motif_db::position_store` query directly.
- Corpus used for the recorded run (via `MOTIF_IMPORT_PERF_PGN`): `/home/bogdb/scid/twic/1m_games.pgn`
- To build a repo-local benchmark corpus, run `scripts/download_twic_pgns.sh --1m` or `scripts/download_twic_pgns.sh --all` and let the perf tests discover the repo-local output automatically.
- Result is comfortably within the Story 3.1 directional guardrail (`p99 < 100ms`) on the available 1M corpus.
- **Remaining AC gap:** the formal acceptance target is `<100ms P99` on a **10M-game** corpus. That has not been fully validated yet because no local 10M corpus benchmark run was performed in this story.

---

## 2026-04-23 — Story 3.2 opening_stats API

**Machine:** Linux, Clang 21, release build (`-O3 -DNDEBUG`)

### Quick Guardrail — 100k games (`bench/data/twic-100k.pgn`)

| Benchmark | Elapsed / total | p50 (µs) | p99 (µs) | min (µs) | max (µs) |
|---|---|---|---|---|---|
| `import_pipeline: default fast path perf` | 8,319 ms | - | - | - | - |
| `opening_stats::query` on sorted position store | 203.996 ms | 545 | 81,185 | 457 | 81,185 |
| `position_search::find` on sorted position store | 103.585 ms | 485 | 1,548 | 366 | 3,218 |

### Full Guardrail — 1,001,092 games (`bench/data/twic-1m.pgn`)

| Benchmark | Elapsed / total | p50 (µs) | p99 (µs) | min (µs) | max (µs) |
|---|---|---|---|---|---|
| `import_pipeline: default fast path perf` | 89,992 ms | - | - | - | - |
| `opening_stats::query` on sorted position store | 312.013 ms | 797 | 116,669 | 633 | 116,669 |
| `position_search::find` on sorted position store | 222.802 ms | 681 | 20,023 | 564 | 41,963 |

### Post-review optimization — 1,001,092 games (`bench/data/twic-1m.pgn`)

Elo now sourced from DuckDB directly (no SQLite player JOINs for Elo). Batch context query drops two player JOINs. Correlated subquery for Opening tag retained (safer than LEFT JOIN). Sampling now uses `REPEATABLE (seed)` for deterministic results.

| Benchmark | p50 (µs) | p99 (µs) | min (µs) | max (µs) |
|---|---|---|---|---|
| `opening_stats::query` on sorted position store (run 1) | 730 | 321,000 | 582 | 321,000 |
| `opening_stats::query` on sorted position store (run 2) | 760 | 706,000 | 590 | 706,000 |

### Disk Size — 818,000 games (deduplicated from 1M PGN)

| Component | Size | Bytes/game |
|---|---|---|
| PGN source (`1m_games.pgn`) | 824 MB | ~1,031 |
| SQLite (`games.db`) | 371 MB | ~465 |
| DuckDB positions (estimated) | ~590 MB | ~739 |
| **Total bundle (measured)** | **371 MB** | **~465** |

The DuckDB position table was empty in the measured test db (data not checkpointed after `sort_by_zobrist`). The estimated ~590 MB is derived from ~40 position rows/game × 19 bytes/row, before DuckDB columnar compression. For comparison, scidb's TWIC database with 3.4M games occupies 1.3 GB (~400 bytes/game, positions in-memory only).

### Notes

- The public perf guard is in `test/source/motif_search/opening_stats_test.cpp` and follows the same discovery path as Story 3.1.
- Functional coverage for continuation aggregation, null-Elo averaging, ECO/opening-name lookup, and no-match behavior is in place and passes under both `dev` and `dev-sanitize`.
- `sample_zobrist_hashes` now accepts a `seed` parameter (default 0) and uses DuckDB's `REPEATABLE (seed)` clause, eliminating run-to-run P99 variation caused by different sampled positions. Tests use seed 42.
- P99 latency previously varied between runs (321–706ms) because unseeded reservoir sampling could select extremely popular positions (e.g., after 1.e4) with thousands of games. With a fixed seed, results are reproducible.
- p50 is consistently ~730–760µs, which is fast. The p99 tail is dominated by a handful of high-fanout positions.
- **Remaining NFR02 gap:** The formal acceptance target is `<500ms P99` on a 10M-game corpus. On the current 1M corpus, p99 can exceed 500ms for the most popular positions. The remaining bottleneck is per-game SQLite context fetch (moves blob + eco + opening_name). A future optimization could extract just the continuation byte at offset `2*ply` from each blob rather than deserializing the entire moves vector.

---

## 2026-08-16 — Materialized Opening Continuation Rollup

**Machine:** Linux, Clang 21, isolated Release build without clang-tidy/cppcheck.

### Opening Explorer — 989,967 games (`bench/data/twic-1m.pgn`)

| Benchmark | p50 (µs) | p99 (µs) | min (µs) | max (µs) | total (ms) |
|---|---:|---:|---:|---:|---:|
| `opening_stats::query` with rollup ordered by root hash | 2,949 | 6,744 | 2,289 | 6,744 | 323 |

| Benchmark | Total games | Continuations | Elapsed (ms) |
|---|---:|---:|---:|
| Starting-position explorer | 989,967 | 20 | 19.5 |

### Notes

- The rollup materializes direct edge statistics by `(root_hash, encoded_move, child_hash)` during position-store rebuild.
- The existing HTTP `frequency` field remains the count of games reaching the child hash through any move order. The new `direct_frequency` field reports games that selected the displayed continuation.
- The previous 1M-game runs recorded p99 values of 321-706ms for the dynamic aggregate. This run reduces the sampled p99 to 6.7ms, but its p50 is higher than the former ~0.7ms because every query now reads the compact materialized relation and reconstructs response context.
- Results are on the available 1M corpus. They do not establish the 10M-game performance target.
- The P99 `CHECK` is only enforced on release builds; dev builds emit `WARN` instead.

---

## 2026-08-17 — Import Pipeline Throughput (parser migration, worker scaling, SQL fold)

**Machine:** Linux, 32-core, Clang 21, `build/perf-release` (Release, matches `skip_perf_unless_release_build()` gating). Not yet re-measured on lower-core-count hardware. Full narrative and diagnosis in `docs/handoffs/2026-08-17-opening-explorer-storage.md`, "Session 8".

### Single-threaded ingest, isolating the parser migration (`pgn::parse_string` → `pgn::import_stream`)

| corpus | before | after | change |
|---|---:|---:|---:|
| 100k games | 7,831 ms | 5,790 ms | −26.1% |
| 1M games | 102,734 ms | 83,245 ms | −19.0% |

Per-game component breakdown, 100k corpus, before this session's changes:

| component | µs/game |
|---|---:|
| PGN read + SAN parse | 35.0 |
| + SQLite write | +12.5 |
| + Zobrist hash + position rows + DuckDB append | +31.8 |
| Scidb, whole import (reference) | 35.5 |

### Full pipeline, 3.41M games (`bench/data/twic-all.pgn`), 16 workers (`hw/2` default)

| | attempted | committed | ingest | sort | rollup | total |
|---|---:|---:|---:|---:|---:|---:|
| Before this session | 3,411,924 | 3,363,425 | 347,560 ms | 7,665 ms | 28,621 ms | 386,571 ms |
| After this session | 3,411,919 | 3,366,774 | 298,124 ms | 5,036 ms | 29,855 ms | 336,995 ms |

**−12.8% wall-clock, +3,349 games committed.** The extra commits are verified as a correctness improvement, not leniency: `pgn::parse_string`'s strict-UTF-8 lexy grammar was rejecting entire well-formed games over a single non-UTF-8 byte in an unrelated tag (e.g. `WhiteTeam`); `import_stream` doesn't validate encoding and accepts them. See the handoff doc for the verification method (lockstep `game_stream` vs `import_stream` diagnostic).

### DuckDB appender throughput, isolated

`position_store::insert_batch` alone (no PGN, no SQLite, no file I/O), synthetic 100k games × 87 rows/game (matching real median game length):

| | value |
|---|---:|
| ns/row | 225.9 |
| µs/game (87 rows) | 19.65 |

At `hw/2` workers, 1M games: SQLite-write-only ingest is 24.2 µs/game; full ingest (+ position build + DuckDB append) is 76.0 µs/game. The isolated appender number accounts for ~38% of that ~51 µs/game gap — the rest (vector copy, any non-hidden zobrist-hash cost) is not yet isolated further.

### DuckDB appender: scalar → vectorized chunk-append

`position_store::insert_batch` rewritten from row-by-row `duckdb_appender_begin_row`/`append_*`/`end_row` (6 API calls/row) to `duckdb_create_data_chunk` + direct pointer writes into each vector + `duckdb_append_data_chunk` (~2048 rows/chunk). `position` has no nullable columns, so no validity-mask handling needed.

| benchmark | before | after | change |
|---|---:|---:|---:|
| isolated appender (synthetic, 100k games × 87 rows) | 225.9 ns/row | 178.2 ns/row | **−21.1%** |
| 1M, serial ingest | 83,245 ms | 79,124 ms | −5.0% |
| 1M, 16 workers, ingest | 75,382 ms | 71,578 ms | −5.0% |
| 3.4M, 16 workers, total elapsed | 336,995 ms | 332,794 ms | −1.25% |

Unlike the parser migration, this still helps at full 16-way parallelism (~5% at 1M) since it targets stage2's actual serial bottleneck — but smaller than the isolated 21% suggests, and smaller again at 3.4M scale where sort/rollup are a larger share of total time.

**Cumulative, this whole investigation, 3.4M-game corpus:** 386,571 ms → 332,794 ms total elapsed — **13.9% faster wall-clock**, plus 3,349 more games committed (verified correctness fix, see handoff doc).

### Notes

- Parser migration win is largest single-threaded (−19% to −26%) and nearly disappears at 16-way parallelism (75,382 ms vs 75,651 ms at 1M) — once parsing is cheap, the serial SQLite-write + DuckDB-append stage becomes the pipeline's ceiling regardless of parser speed.
- `num_workers` default changed from a hardcoded `4` to `hardware_concurrency() / 2` (floor 1, no cap) — only validated on this 32-core box.
- SQL-fold (single-statement duplicate-check-and-insert in `game_writer::insert`) and SQLite pragma tuning (`synchronous=NORMAL`, `cache_size`, `mmap_size`, `wal_autocheckpoint`) were tried first; both real but small (~1.3% and ~1.7% respectively) since neither was the dominant cost.
- `import_stream` holds the whole PGN file in memory (vs. the old line-by-line reader); isolated ingest-only peak RSS delta is ~1.1 GB on the 896 MB 1M-game corpus (so ~3 GB on the full 3 GB `twic-all.pgn`) — proportional to file size, not the dominant contributor to the ~12.7 GB peak RSS measured for the full default path (sort/rollup phase, not isolated further, is the more likely cause).
- **Open question, not a settled one:** the parser migration (`import_stream`) reportedly wasn't adopted earlier specifically because of RAM cost and negligible speed impact at production parallelism. This session's numbers corroborate the speed point at `hw/2`+ workers (nearly flat) and confirm real RAM cost proportional to file size. It does still deliver a real single-threaded/low-core win and fixes a strict-UTF-8 game-rejection bug (see handoff doc) — whether that's worth the RAM cost is a product call, not decided here.

---

## 2026-08-20 — SQLite Game Store and Compact Opening Index

**Machine:** Linux, 32-core, Clang 21, `build/perf-release`. Corpus:
`bench/data/twic-1m.pgn` (896 MiB). Runs were serialized.

| Path | Attempted | Committed | Ingest | Replay | Sort | Rollup / index build | Total | Bundle |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Normal import: SQLite + DuckDB positions | 1,001,090 | 991,278 | 10,753 ms | 14,182 ms | 7,157 ms | 7,289 ms rollup | 48,555 ms | not retained |
| Candidate: SQLite + `opening_tree.idx` | 1,001,090 | 991,278 | 18,181 ms | - | - | 28,525 ms index build | 46,706 ms | 520 MiB |

The candidate stores `games.db` in 278 MiB and `opening_tree.idx` in 242 MiB.
It was 1,849 ms (3.8%) faster than the same-run normal import while producing
a 520 MiB retained bundle. The normal path's separately measured retained
bundle is about 2.28 GiB (`games.db` plus `positions.duckdb`), so the candidate
is about 4.4x smaller.

### Compact Index Query Latency

`opening_tree_index::query_opening_stats` was sampled at every opening ply
through ply 20 for 2,000 imported games.

| Queries | Rows | Mean | p50 | p99 | Max |
|---:|---:|---:|---:|---:|
| 41,822 | 368,729 | 0.53 us | 0.55 us | 1.24 us | 8.85 us |

### Scope

The compact index is still a prototype replacement only for unfiltered,
shallow opening-explorer queries. DuckDB remains required for position search,
filtered explorer queries, Elo distributions, and full-depth traversal. The
index build is intentionally not wired into the normal import path.

---

## 2026-08-22 — Compact Position Postings and Opening Explorer

**Machine:** Linux, 32-core, Clang 21, `build/perf-release`. Corpus:
`bench/data/twic-1m.pgn`. The DuckDB, postings, and postings-plus-tree imports
ran serially in one process. Results are one detailed run after three earlier
postings-format validation runs established a 67.409-67.962 s range.

### Total Import

| Path | Attempted | Committed | Total | Peak RSS | Derived artifacts |
|---|---:|---:|---:|---:|---:|
| SQLite + DuckDB | 1,001,090 | 991,278 | 60,116 ms | 9,475 MiB | 2,039 MiB DuckDB |
| SQLite + exact postings | 1,001,090 | 991,278 | 68,323 ms | 1,238 MiB | 1,206 MiB postings |
| SQLite + postings + shallow tree | 1,001,090 | 991,278 | 126,282 ms | 1,452 MiB | 1,206 MiB postings + 242 MiB tree |

The exact-postings path is the current DuckDB-free position-query import. The
tree remains opt-in, so 68.323 s is the current default derived-index total when
DuckDB rebuilding is disabled. Building both indexes adds 57.959 s.

### Import Components

| Path | Ingest | Dedup | Derived wall | Accounted builder phases | Validation/publication and other remainder |
|---|---:|---:|---:|---:|---:|
| DuckDB | 14,650 ms | 8,953 ms | 35,261 ms | replay 18,000 + sort 8,886 + rollup 8,375 ms | 1,250 ms |
| Exact postings | 14,590 ms | 8,976 ms | 44,755 ms | 38,369 ms | 6,386 ms |
| Postings + tree | 14,478 ms | 8,882 ms | 102,921 ms | postings 38,369 + tree 60,356 ms | 4,196 ms |

Builder-local postings phases were 12,070 ms replay, 6,796 ms spill, 16,980 ms
merge, 76 ms metadata write, 345 ms directory write, and 2,102 ms final write.
The build indexed 88,221,427 occurrences across 68,907,007 hashes in 85 spill
runs. The output comprised 420,327,977 posting bytes, 9,912,780 metadata bytes,
828,439,768 compressed-directory bytes, and 5,921,696 sparse-directory bytes.

Builder-local tree phases were 22,947 ms replay, 916 ms root merge, 6,150 ms
edge merge, 11,978 ms child-frequency merge, about 6,264 ms child-frequency
loading, and 12,101 ms final index write.

### Opening Explorer

The raw immutable tree reader sampled 41,822 positions through ply 20:

| API | Queries | Mean | p50 | p99 | Max |
|---|---:|---:|---:|---:|---:|
| `opening_tree_index::query_opening_stats` | 41,822 | 5.36 us | 4.77 us | 10.50 us | 37.78 us |

The public `opening_stats::query` API sampled 512 unique non-starting shallow
positions twice:

| Pass | Mean | p50 | p99 | Max |
|---|---:|---:|---:|---:|
| First | 1,209.25 us | 963.95 us | 3,021.06 us | 85,460.71 us |
| Warm | 1,193.23 us | 937.89 us | 3,039.12 us | 84,501.05 us |

The starting-position public query took 3,233.676 ms on the first call and
3,191.127 ms warm p50. This is not tree-reader latency. The public API first
calls `position_game_ids()` to determine `total_games`, which decodes the
starting position's approximately one-million-game posting block before using
the shallow aggregate. Repeated calls remain equally slow because this path has
no result cache. The next explorer optimization should use the postings
directory's `distinct_game_count` summary for unfiltered `total_games`, avoiding
posting-block decode entirely.

### Complete Starting-Position Root Follow-up

The summary-count change removed one redundant posting decode but did not fix
the starting position: repetitions can return to that position after ply 20,
so the manager correctly rejected the shallow aggregate and used full replay.
Opening-tree format v5 now marks the starting node complete and aggregates its
edges at every ply. Two serialized 1M Release runs measured:

| Metric | Run 1 | Run 2 |
|---|---:|---:|
| Starting position first | 1.066 ms | 1.084 ms |
| Starting position warm p50 | 0.898 ms | 0.926 ms |
| Non-starting warm p50 | 0.881 ms | 0.888 ms |
| Non-starting warm p99 | 1.439 ms | 1.430 ms |
| Postings + tree total import | 128.421 s | 128.276 s |
| Tree builder-local time | 61.847 s | 61.716 s |
| Tree artifact | 246 MiB | 246 MiB |

Compared with the preceding 3.1-3.2 second starting-position measurements, the
complete root improves warm latency by roughly 3,400x. The tree costs about 4
MiB more storage and approximately one additional second of builder-local work.

### Follow-up — Unfiltered `total_games` via postings summary

`opening_stats::query`'s unfiltered path now calls `database_manager::position_summary()`
first and uses its `distinct_game_count` for `total_games` whenever a valid,
non-stale postings generation covers the queried hash; it decodes the hash's
posting block only through the pre-existing fallback (`position_game_ids()`),
which still runs when postings are absent, stale, or do not index the hash.
This removes the full posting-block decode from the previously measured
3.2 s starting-position path in `source/motif/search/opening_stats.cpp`. The
1M-corpus benchmark above was not re-run to produce a new number for this
entry; correctness and dispatch are covered by
`test/source/motif_search/opening_stats_test.cpp` (`[postings]` tag) and the
existing `database_manager::position_summary` valid/stale/absent coverage in
`test/source/motif_db/position_postings_test.cpp`.

### Follow-up — Reuse postings counts during tree construction

The manager-backed tree build now consumes the postings index's sorted
`distinct_game_count` summary stream for child transposition frequencies. It
does not construct the tree's independent full-depth visit counter; standalone
tree builds retain that path as an oracle. Two serialized 1M Release runs
measured:

| Metric | Previous | Run 1 | Run 2 |
|---|---:|---:|---:|
| Postings + tree import | 128.276-128.421 s | 109.225 s | 109.060 s |
| Tree accounted phases | about 55.5 s corrected baseline | 37.035 s | 37.242 s |
| Tree replay | about 23.0 s | 11.590 s | 11.747 s |
| Child count materialize/load | about 18.2 s merge + load | 6.396 s | 6.352 s |
| Child spill runs | 84 | 0 | 0 |
| Tree artifact | 246 MiB | 246 MiB | 246 MiB |
| Process peak RSS | 1,452 MiB detailed baseline | 1,427 MiB | 1,514 MiB |

The tree's accounted phases improve by about 33%; full postings-plus-tree
import is about 15% faster. The corrected phase totals subtract child-frequency
loading from the old `index_write_elapsed`, which previously included and
therefore double-counted that work. Raw and public query latency remained
within the preceding range. The counts are semantically identical: postings
group occurrences by game and hash, while the standalone tree counter
deduplicates each hash once per game. Build-time validation rejects missing
counts and counts below a direct edge's distinct-game frequency.

### Follow-up — Buffered final tree encoding

The final tree encoder now accumulates fixed-width and varint bytes into a 64
KiB block before writing. The binary format and reader are unchanged. Three
serialized 1M Release runs measured a combined load-plus-write phase of
10.981-11.081 seconds. With corrected non-overlapping metrics, the detailed run
reported 6.282 seconds loading child frequencies and 4.799 seconds encoding and
writing the tree. The preceding unbuffered runs imply 5.692-5.745 seconds of
serialization after subtracting their 6.352-6.396 second loads, so buffering
improves the targeted phase by about 16%.

The complete corrected tree phase total was 36.390 seconds, versus
37.035-37.242 seconds before buffering. End-to-end postings-plus-tree import
was 108.591-108.860 seconds, compared with 109.060-109.225 seconds before; this
small total improvement is close to run variance. Artifact size remained 246
MiB and query latency remained in the preceding range. `index_write_elapsed`
remains inclusive of child lookup/loading so it has the same meaning in the
preload and low-memory binary-search paths; serialization is derived only for
the preload benchmark by subtracting `child_frequency_load_elapsed`.

### Follow-up — Per-game tree scratch reuse

The tree builder now retains per-game edge/root hash-container capacity across
games. Two initial serialized 1M runs measured replay at 10.929-11.074 seconds,
down from the buffered-writer baseline of 11.836-11.942 seconds (6.5-8.5%).
Later runs alongside directory instrumentation measured 11.003-11.218 seconds.
The builder also latches an accumulation failure so partial spill/counter state
cannot be reused or finalized.

### Follow-up — Position-postings directory v6

Exact v5 instrumentation attributed the 828,439,768-byte directory to:

| Field | Bytes |
|---|---:|
| Hash deltas | 403,486,860 |
| Posting-offset deltas | 68,697,012 |
| Posting lengths | 68,966,408 |
| Occurrence counts | 68,919,729 |
| Distinct-game counts | 68,919,693 |
| Minimum plies | 72,266,086 |
| Maximum plies | 72,338,956 |

Format v6 removes the derivable offset deltas, stores 40-bit hash-delta lows
with sparse high-part exceptions, and omits occurrence counts when they equal
distinct-game counts. Three serialized 1M runs produced the same exact section
sizes:

| Metric | v5 | v6 | Change |
|---|---:|---:|---:|
| Directory | 828,439,768 B | 649,481,128 B | -21.6% |
| Full postings artifact | 1,206 MiB | 1,035 MiB | -14.2% |
| Peak temporary estimate | 2,368 MiB | 2,198 MiB | -170 MiB |
| Postings-only import | about 68-70 s | 67.614-68.346 s | modest improvement |
| Postings + tree import | about 108.6-109.9 s | 107.028-107.471 s | modest improvement |

The v6 directory contains 4,845,024 bytes of block headers, 17,226,752 bytes
of bitmaps, 344,317,077 bytes of hash encoding, 68,966,408 bytes of posting
lengths, 601,132 bytes of occurrence exceptions, 68,919,693 bytes of
distinct-game counts, and 144,605,042 bytes of ply bounds. Query semantics,
one-block directory lookup, sorted summary streaming, the 420,327,977-byte
posting payload, and the 9,912,780-byte game-metadata section are unchanged.

### Follow-up - Exact postings as the import default

The production inventory confirmed that exact and filtered position search,
opening statistics, Elo distributions, and deep bounded traversal can all use
the published postings generation. Stale or absent postings fail closed unless
the DuckDB table is known to be synchronized, while explicit DuckDB builds and
existing DuckDB-only bundles retain their fallback behavior.

`import_config {}` now builds exact postings without first rebuilding DuckDB.

Three isolated fresh-process Release runs on `bench/data/twic-1m.pgn` measured:

| Run | Attempted | Committed | Ingest | DuckDB replay/sort/rollup | Total | Attempted throughput |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 1,001,090 | 991,278 | 14.566 s | 0 / 0 / 0 ms | 81.359 s | 12,305 games/s |
| 2 | 1,001,090 | 991,278 | 14.632 s | 0 / 0 / 0 ms | 81.520 s | 12,280 games/s |
| 3 | 1,001,090 | 991,278 | 14.635 s | 0 / 0 / 0 ms | 81.728 s | 12,249 games/s |

The isolated median is 81.520 seconds, 20.6% faster than the old literal
default's 102.734 seconds. A same-session serialized comparison measured DuckDB
at 59.847 seconds, postings at 66.959 seconds with 1,110 MiB peak RSS, and
postings plus the opening tree at 105.280 seconds. Its postings builder spent
12.174 seconds replaying, 6.882 seconds spilling, 16.857 seconds merging, and
2.105 seconds in metadata, directory, and final writes; the derived-path
remainder was 5.343 seconds.

The 14.561-second gap between the isolated median and the comparison's postings
case is larger than normal variance and appears sensitive to process context or
run order. It is not yet attributed. Use the isolated median as the production
default result; use the comparison only for contemporaneous path and peak-RSS
comparisons.

### Release-boundary dependency removal

DuckDB was subsequently removed from source, CMake, Nix, and vcpkg. The change
does not alter the postings format or the measurements above. Legacy bundles
without valid postings rebuild them from canonical SQLite and leave any old
`positions.duckdb` file untouched. Scratch and oracle tests now exercise the
same SQLite-plus-postings architecture as persistent bundles.
