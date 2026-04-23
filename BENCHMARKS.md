# Benchmarks

Performance results recorded on each meaningful change. All timings taken on the release build (`cmake --preset=release`) unless noted otherwise.

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
- The P99 `CHECK` is only enforced on release builds; dev builds emit `WARN` instead.
