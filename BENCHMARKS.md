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
