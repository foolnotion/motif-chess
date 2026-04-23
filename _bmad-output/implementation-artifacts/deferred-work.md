## Deferred from: code review of 2-1-spdlog-logger-infrastructure (2026-04-19)

- Logger startup/config wiring is still unspecified [source/motif/import/logger.hpp:12] — this story is about infrastructure, initialization belongs in the client app (UI, cli game import client, etc)

## Deferred from: Story 2.8 (2026-04-21)

- Memory layout optimization for prepared_game (cache miss reduction) — Task 2 of Story 2.9
- Pipeline batching optimization (futex contention reduction) — Task 3 of Story 2.9  
- Arena allocator implementation for prepared_game slots (allocation overhead reduction) — Task 4 of Story 2.9
- SQLite per-game transaction batching — Not in Story 2.9 (requires motif_db changes), deferred to future storage optimization
- Taskflow redesign for batch game handoff — Part of Task 3 in Story 2.9

## Deferred from: code review of 2-5-import-completion-summary-error-logging (2026-04-22)

- `count_games` heuristic can over/undercount `[Event "` markers without PGN structural awareness — acceptable for progress estimation but not authoritative
- No input validation for `num_lines=0` or `num_workers=0` in `import_config` — causes UB in taskflow, pre-existing
- `rollback_sqlite_batch` silently discards rollback failure — pre-existing, no good recovery path
- `position_rows` always built even when `write_positions=false` — pre-existing perf concern, requires passing config into `prepare_game()`
- Default `num_workers=1` and `k_default_lines=1` changed for serial benchmarking — revert to `hardware_concurrency`/`64` after perf validation

## Deferred from: code review of 2-10-apply-d009-sorted-zobrist-plus-deferred-cleanup (2026-04-22)

- `rebuild_position_store()` can return after starting a DuckDB transaction without rolling it back when SQLite statement preparation fails [source/motif/db/database_manager.cpp:522] — pre-existing transaction-handling gap in a touched function

## Deferred from: code review of 2-11-concurrency-exploration-for-serial-import (2026-04-22)

- `import_worker::process()` ignores write_positions flag — not on active pipeline path, API inconsistency only
- `rebuild_partitioned_position_store()` lacks transaction wrapping — DELETE FROM position is not in BEGIN/COMMIT, irreversible on failure
- Misleading recovery message when `write_positions=true` and position insert fails — suggests rebuild_position_store but that path requires `write_positions=false`
- `tx_res` reused for COMMIT query result instead of `commit_res` — pre-existing variable naming bug, results in minor resource leak
- Off-by-one boundary check: `prepare_game` uses `>= 65535` while `build_position_rows` uses `> 65535` — pre-existing inconsistency
- Test constants use `k_` prefix (k_three_game_pgn, etc.) — violates CONVENTIONS.md naming rule, pre-existing

## Deferred from: code review of 3-1-position-search-by-zobrist-hash (2026-04-23)

- Opening a bundle with a missing or replaced `positions.duckdb` can silently succeed and make searches return false negatives [source/motif/db/database_manager.cpp:391] — pre-existing bundle validation gap
- Search query reads `ply` back with a signed 16-bit accessor even though rebuild accepts values up to `uint16_t` max [source/motif/db/position_store.cpp:164] — pre-existing long-game overflow risk

## Deferred from: code review of 3-2-opening-statistics-per-position (2026-04-23)

- No DuckDB prepared statements for `query_opening_moves`/`query_by_zobrist`/`sample_zobrist_hashes` [position_store.cpp:175-230] — pre-existing, uses string-concatenated SQL instead of `duckdb_prepare`/`duckdb_bind_*`, flagged in devlog W18 as P1 backlog
- Remaining NFR02 p99 tail for high-fanout positions — `opening_stats::query` p50 is ~730µs but p99 varies 320–700ms on 1M corpus because popular positions (e.g. after 1.e4) require per-game SQLite context fetches for eco/opening_name/moves. Extracting just the continuation byte at offset `2*ply` rather than deserializing the full moves blob, or caching game contexts, would close this gap.
