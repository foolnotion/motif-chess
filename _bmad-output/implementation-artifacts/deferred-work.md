## Deferred from: code review of 2-1-spdlog-logger-infrastructure (2026-04-19)

- Logger startup/config wiring is still unspecified [source/motif/import/logger.hpp:12] — this story is about infrastructure, initialization belongs in the client app (UI, cli game import client, etc)

## Deferred from: Story 2.8 (2026-04-21)

- SQLite per-game transaction batching — requires motif_db changes, deferred to future storage optimization

## Deferred from: code review of 2-5-import-completion-summary-error-logging (2026-04-22)

- `count_games` heuristic can over/undercount `[Event "` markers without PGN structural awareness — acceptable for progress estimation but not authoritative
- No input validation for `num_lines=0` or `num_workers=0` in `import_config` — causes UB in taskflow, pre-existing
- `rollback_sqlite_batch` silently discards rollback failure — pre-existing, no good recovery path

## Deferred from: code review of 2-11-concurrency-exploration-for-serial-import (2026-04-22)

- Test constants use `k_` prefix (k_three_game_pgn, etc.) — violates CONVENTIONS.md naming rule, pre-existing

## Deferred from: code review of 3-1-position-search-by-zobrist-hash (2026-04-23)

- Opening a bundle with a missing or replaced `positions.duckdb` can silently succeed and make searches return false negatives [source/motif/db/database_manager.cpp:391] — pre-existing bundle validation gap

## Deferred from: code review of 3-2-opening-statistics-per-position (2026-04-23)

- No DuckDB prepared statements for `query_opening_moves`/`query_by_zobrist`/`sample_zobrist_hashes` [position_store.cpp:175-230] — pre-existing, uses string-concatenated SQL instead of `duckdb_prepare`/`duckdb_bind_*`, flagged in devlog W18 as P1 backlog
- Remaining NFR02 p99 tail for high-fanout positions — `opening_stats::query` p50 is ~730µs but p99 varies 320–700ms on 1M corpus because popular positions (e.g. after 1.e4) require per-game SQLite context fetches for eco/opening_name/moves. Extracting just the continuation byte at offset `2*ply` rather than deserializing the full moves blob, or caching game contexts, would close this gap.

## Deferred from: code review of 3-2-opening-statistics-per-position (2026-04-24, second pass)

- DDL inside DuckDB transaction + failed COMMIT rollback risk [database_manager.cpp rebuild_position_store] — sort_by_zobrist runs DDL inside the explicit transaction; if COMMIT fails, rollback is called. DuckDB transactional DDL should handle this correctly; failure implies underlying storage failure with no viable recovery regardless.
- DuckDB Appender transaction participation version-sensitivity [database_manager.cpp rebuild_position_store] — insert_batch uses duckdb_appender on the same connection as an active BEGIN TRANSACTION. Modern DuckDB (pinned via vcpkg/nix) participates appended data in the surrounding transaction; pre-v0.9 behavior differed. Integration tests confirm correct behavior for current version.

## Deferred from: code review of 3-2-opening-statistics-per-position (2026-04-24, groups C+D)

- `x86-64-v3` via `ci-linux` inherited by `ci-sanitize` [CMakePresets.json] — `motif_ENABLE_X86_64_V3=ON` propagates to all CI builds including sanitizer runs. CMakeLists.txt guards on processor architecture type, not ISA level (AVX2/BMI2). SIGILL risk on pre-Haswell x86_64 CI runners; acceptable while CI runs on known-compatible hardware.
- Position data durability: crash between `commit_sqlite_batch` and `rebuild_position_store` completion leaves DuckDB empty [import_pipeline.cpp:532-541] — SQLite is intact; next successful import corrects DuckDB. No resume path detects or reports the gap.
- DuckDB COMMIT C API error indistinguishable from actual commit success [database_manager.cpp rebuild_position_store] — if commit succeeded but the C API returned an error, function returns `io_failure` while data is actually durable. Caller cannot distinguish the two outcomes; no viable recovery regardless.
- Resume→rebuild-fail→resume: re-imports already-committed games [import_pipeline.cpp] — checkpoint correctly preserved on rebuild failure, but on next resume duplicate games hit SQLite rejection path. Pre-existing checkpoint semantics; no new risk from removing direct-write strategy.
- `batch_size=0` not validated in `import_config` — `batch_pending >= 0` (unsigned) always true; commits + checkpoints after every inserted game. Pathologically slow but not incorrect. Variant of pre-existing `num_workers=0`/`num_lines=0` gap deferred from 2-5.
- `make_game` static counter in `opening_stats_test.cpp` never reset between TEST_CASEs [opening_stats_test.cpp:114-115] — player names are non-deterministic if tests run as a subset or with `--jobs` parallel-ctest. Pre-existing pattern from earlier test stories.

## Deferred from: code review of 3-2-opening-statistics-per-position (2026-04-24, final pass)

- NFR02 AC1 gap: p99 reaches 700ms on 1M corpus for high-fanout positions (e.g., after 1.e4), 200ms over the 500ms target. Not critical; will decide later whether to address. Fix path: extract only `moves[2*ply]` from moves blob instead of full deserialization, or cache game contexts per query [opening_stats.cpp, position_store.cpp].
- `dominant_eco` tie-break rule (alphabetical ECO code on count ties) documented only in `.cpp` comment, not in `opening_stats.hpp` public API — Story 3.3 depends on output shape; the tie-break rule should appear in the header.

## Deferred from: code review of 3-3-lazy-opening-tree-with-prefetch (2026-04-24)

- Quadratic `replay_position` calls — `open()` replays the board from ply 0 for every row in the occurrence loop and again for every node in the bottom-up build pass, with no incremental reuse across depths. `O(occurrences × max_depth × avg_root_ply)` move applications. Performance concern only; no correctness impact. [`source/motif/search/opening_tree.cpp:300,340,389`]
- Zobrist collision merges different positions into the same node aggregate — two board positions sharing the same Zobrist hash would have their continuation statistics silently merged and produce an incorrect `result_hash`. Pre-existing architecture risk inherent to 64-bit Zobrist hashing. [`source/motif/search/opening_tree.cpp:313`]
- `const_cast` delegation in `game_store::get_game_contexts` non-const overload — the non-const overload delegates to the const one via `const_cast`. Safe and the non-const overload is unused in practice (all callers hold `database_manager const&`); should be removed when cleaning up the `game_store` API. [`source/motif/db/game_store.cpp`]
