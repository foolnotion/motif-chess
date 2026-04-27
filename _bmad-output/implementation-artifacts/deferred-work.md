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

## Deferred from: code review of 4b-1-http-server-scaffold.md (2026-04-25)

- Baseline dev build reports clang-tidy/cppcheck warnings outside Story 4b.1 touched files (for example `source/motif/db/database_manager.cpp:444`), so AC5 zero-warning verification remains blocked by pre-existing issues.

## Deferred from: code review of 4b-5-game-list-endpoint (2026-04-25)

- `game_continuation_context` struct and `get_continuation_contexts` method added out of scope — possible prep work for tree view / story 4b.6 extension; belongs to opening stats feature [types.hpp, game_store.hpp, game_store.cpp]
- `import_workers` vector grows without bound; completed `jthread`s never pruned [server.cpp] — from story 4b-4, not actionable in this story
- SSE progress handler dereferences `session->pipeline` without null guard [server.cpp] — from story 4b-4
- `memory_order_relaxed` for `failed` flag fragile; relies on implicit sequencing after acquire load of `done` [server.cpp] — from story 4b-4; document ordering proof or upgrade to `acquire`
- `get_continuation_contexts` silently drops entries with zero-length move blob — output vector smaller than input with no correlation [game_store.cpp] — out of scope for this story
- Hardcoded port numbers in all HTTP tests flake on parallel runs [http_server_test.cpp] — pre-existing pattern across all stories
- Non-const `get_continuation_contexts` delegates to const via `const_cast` with no rationale [game_store.cpp] — out of scope
- Import DELETE test does not verify early termination — passes even if `request_stop` is a no-op [http_server_test.cpp] — from story 4b-4
- Dynamic SQL in `get_continuation_contexts` has no guard against `SQLITE_LIMIT_VARIABLE_NUMBER` (300 × 3 = 900 vars; limit 999) [game_store.cpp] — out of scope
- `sessions` map never evicts completed import sessions — unbounded growth over many imports [server.cpp] — from story 4b-4
- SSE chunk provider loops forever if worker thread deadlocks before `done.store` [server.cpp] — from story 4b-4
- TOCTOU: file readability checked before async pipeline opens it [server.cpp] — from story 4b-4; design choice

## Deferred from: code review of 4b-6-single-game-retrieval-endpoint (2026-04-25)

- `std::jthread` constructor can throw `std::system_error` in POST /api/imports — orphaned session with `done=false` permanently blocks all future imports [source/motif/http/server.cpp] — 4b-4 code
- `import_session::pgn_path` field never assigned after construction — dead field, reads will return empty path [source/motif/http/server.cpp] — 4b-4 code
- SSE error event embeds `session->error_message` directly into format string without JSON escaping — malformed JSON if message contains `"`, `\`, or newline [source/motif/http/server.cpp] — 4b-4 code
- SSE chunked content provider calls `sleep_for(250ms)` on httplib's thread — blocks thread pool slot for the full poll interval, exhausted under concurrent imports [source/motif/http/server.cpp] — 4b-4 code
- `error_message` and `summary` written by worker, read by SSE callback — correctness relies on undocumented release/acquire ordering through `done`; one refactor away from a data race [source/motif/http/server.cpp] — 4b-4 code
- `cancel_requested` atomic is set by DELETE handler but never read anywhere — dead state, misleading in a concurrency-sensitive struct [source/motif/http/server.cpp] — 4b-4 code
- `import_pipeline` and `import_session` constructed before the active-import conflict check — resources wasted on every 409 response [source/motif/http/server.cpp] — 4b-4 code
- `game_response::result` is an unvalidated `std::string` — corrupt or non-standard DB values propagate directly into API responses [source/motif/http/server.cpp] — design gap, no spec requirement to validate at HTTP layer
- Destructor iterates `sessions` under `sessions_mutex` while `import_workers` jthreads join implicitly after the lock-guarded body exits — worker threads may still be running when destructor returns [source/motif/http/server.cpp] — 4b-4 code
- `opening_stats_endpoint_limit` set to 500ms (5× the position-search budget) with no documented rationale [test/source/motif_http/http_server_test.cpp] — 4b-3 code

## Deferred from: code review of 4b-3-opening-stats-endpoint (2026-04-25)

- `glz::write_json` error discarded — serialization failure returns 200 with empty body [server.cpp:199] — pre-existing pattern, all route handlers in server.cpp use the same `[[maybe_unused]]` suppression
- Server thread may outlive `srv` on `wait_for_ready` failure [http_server_test.cpp] — pre-existing test pattern, `std::terminate` hazard on REQUIRE failure before join

## Deferred from: code review of 4b-7-openapi-spec (2026-04-26)

- Missing 500 responses on multiple endpoints — server.cpp can return 500 on internal errors; internal errors are generally not spec'd in OpenAPI
- result type inconsistency across endpoints — PositionHit.result is integer (int8_t encoded), GameListEntry/GameFull.result is string; pre-existing C++ design
- result enum/int8_t no constraint — PositionHit.result and game result query param lack enum constraints; pre-existing C++ types
- moves array lacks 16-bit range constraint — nice-to-have but not caused by this change
- anyOf vs type array style inconsistency — GameFull.event uses anyOf, other nullable fields use type: ['X', 'null']; cosmetic
- searchPositions operationId naming subjective — not a bug
- GameFull.date duplicates GameEvent.date — pre-existing C++ struct design accurately reflected
- Bare array responses — pre-existing API design
- Cancel on completed import returns 200 — server behavior issue, spec accurately documents what the server returns
- SSE reconnect replays final event — server behavior, not a spec accuracy issue
- result query param not validated server-side — server issue, spec documents intended enum
- import_id no format/pattern — opaque by design, 32-char random hex

## Deferred from: code review of 4c-1-http-api-contract-hardening (2026-04-26)

- `glz::write_json` error suppressed on opening-stats route — pre-existing pattern across all server.cpp routes (lines 337, 408, 442, 503, 600); serialization failure returns 200 with empty body
- `CHECK_FALSE` negative assertion on numeric result_hash form is marginally weak — vacuously passes if field is absent; covered by the positive quoted-form CHECK
- Exact continuation count not asserted in opening-stats populated-DB test — test would pass even with duplicate or phantom continuations
- `fmt::format("{}", game_index)` + `operator+` string concatenation in clamp-limit test — inconsistent with the `fmt::format` inline pattern used in `make_repeated_pgn`; cosmetic
- `to_opening_stats_response` copies `san`, `eco`, `opening_name` strings from `const&` source — move-from-value alternative not exploited; micro-optimization

## Deferred from: code review of 4d-4-file-upload-import-and-game-count-endpoints (2026-04-27)

- `std::filesystem::temp_directory_path()` can throw `filesystem_error` without JSON error response — pre-existing pattern of unguarded filesystem exceptions in server.cpp
- Unbounded `import_workers` growth — no reaping of completed workers, pre-existing from original `/api/imports` endpoint
- Missing test-failure injection flags for upload endpoint (`fail_next_import_worker_start_for_test` / `fail_next_import_worker_run_for_test` not replicated for upload path) — test infrastructure gap, not a functional bug
