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
