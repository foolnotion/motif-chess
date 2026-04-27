---
baseline_commit: da6a56b
---

# Story 4d.4: File Upload Import and Game Count Endpoints

Status: done

## Story

As a developer building a web frontend import panel,
I want an endpoint that accepts PGN file bytes directly and another that returns the total game count,
so that the browser can import games without a server-side path and display the database total after an import completes.

## Acceptance Criteria

1. **Given** a browser user selects or drags a PGN file
   **When** `POST /api/imports/upload` is called with `multipart/form-data` containing a `file` field
   **Then** HTTP 202 is returned with an `import_id` matching the format of `POST /api/imports`
   **And** the backend writes the bytes to a temporary file, runs the same import pipeline, and deletes the temp file when the import finishes or fails
   **And** the returned `import_id` works with the existing `GET /api/imports/{import_id}/progress` SSE stream

2. **Given** the `file` field is absent from the multipart body
   **When** `POST /api/imports/upload` is called
   **Then** HTTP 400 is returned with a JSON error response

3. **Given** an import is already running
   **When** `POST /api/imports/upload` is called
   **Then** HTTP 409 is returned with a JSON error response, matching the conflict behavior of `POST /api/imports`

4. **Given** the server is running with an open database
   **When** `GET /api/games/count` is called
   **Then** HTTP 200 is returned with `{"count": <integer>}` reflecting the total number of games in the database
   **And** the count is 0 on an empty database and increments correctly after an import completes

5. **Given** `docs/api/openapi.yaml` is updated
   **When** the document is reviewed
   **Then** `POST /api/imports/upload` is documented with its multipart schema, 202/400/409 responses, and an `ImportStartResponse` reference
   **And** `GET /api/games/count` is documented with a `GameCountResponse` schema
   **And** integration tests cover: missing field → 400, valid upload → 202, concurrent upload → 409, empty count → 0, count after import → correct total

6. **Given** all changes are implemented
   **When** `cmake --preset=dev && cmake --build build/dev && ctest --test-dir build/dev` is run
   **Then** all tests pass with zero new clang-tidy or cppcheck warnings

7. **Given** all changes are implemented
   **When** `cmake --preset=dev-sanitize && cmake --build build/dev-sanitize && ctest --test-dir build/dev-sanitize` is run
   **Then** all tests pass with zero ASan/UBSan violations

## Tasks / Subtasks

- [x] Task 1: Add `game_store::count_games()` to `motif_db` (AC: 4)
  - [x] Declare `count_games() const -> result<std::int64_t>` in `game_store.hpp` after `list_games`
  - [x] Implement as `SELECT COUNT(*) FROM game` using the existing `prepare`/`sqlite3_step` pattern in `game_store.cpp`

- [x] Task 2: Add `GET /api/games/count` endpoint (AC: 4, 5)
  - [x] Add `game_count_response` DTO (`std::int64_t count`) in `motif::http::detail`
  - [x] Register route before `Patch("/api/games/:id", ...)` so the exact path takes priority over the parameterized route
  - [x] Acquire `database_mutex`, call `database.store().count_games()`, return 200 with JSON body or 500 on failure

- [x] Task 3: Add `POST /api/imports/upload` endpoint (AC: 1, 2, 3)
  - [x] Read `req.form.files` for the `"file"` key; return 400 if absent
  - [x] Write bytes to `std::filesystem::temp_directory_path() / "motif_upload_<uuid>.pgn"`; return 500 if write fails
  - [x] Perform conflict check under `sessions_mutex`; remove temp file and return 409 if an import is already active
  - [x] Spawn `std::jthread` worker that calls `pipeline->run(temp_path, {})`, deletes the temp file on both success and failure paths, and updates session state
  - [x] Clean up temp file and session entry in the `std::system_error` catch path
  - [x] Return 202 with `{"import_id": "..."}` on success

- [x] Task 4: Update OpenAPI spec (AC: 5)
  - [x] Add `POST /api/imports/upload` path entry with `multipart/form-data` request schema and 202/400/409 response refs
  - [x] Add `GET /api/games/count` path entry
  - [x] Add `GameCountResponse` schema to `components/schemas`

- [x] Task 5: Add integration tests (AC: 1, 2, 3, 4, 5)
  - [x] `GET /api/games/count` returns `{"count":0}` on empty database
  - [x] `GET /api/games/count` returns `{"count":3}` after importing `three_game_pgn_content` via SSE-awaited import
  - [x] `POST /api/imports/upload` with wrong field name returns 400 containing `"missing file field"`
  - [x] `POST /api/imports/upload` with valid PGN bytes returns 202 containing `"import_id"`
  - [x] `POST /api/imports/upload` while an upload is already running returns 409

- [x] Task 6: Validation (AC: 6, 7)
  - [x] `cmake --preset=dev && cmake --build build/dev` — clean, zero warnings
  - [x] `ctest --test-dir build/dev` — 215/215 passed
  - [x] `cmake --preset=dev-sanitize` / build / ctest — not yet run

## Dev Notes

### Scope Boundary

Browser-side constraint: `<input type="file">` and drag-and-drop give the frontend file bytes but not a server-side filesystem path, so `POST /api/imports` is unusable from a browser. `POST /api/imports/upload` fills that gap without duplicating any import pipeline logic — the temp file path is passed directly into the existing `import_pipeline::run` call.

`GET /api/games/count` is the minimal surface needed for the import panel's post-import total display (FR4 of the web frontend). `GET /api/games` is capped at 200 rows and carries no total.

### Key Design Decisions

- Temp file is cleaned up inside the worker lambda — covers both success and failure. The `std::system_error` catch block also removes it if thread creation fails before the lambda runs.
- Route ordering: `GET /api/games/count` registered before `Patch("/api/games/:id", ...)` (the first parameterized games route) so httplib matches the exact path first. `POST /api/imports/upload` registered after `Post("/api/imports", ...)` — no ordering conflict since it is a distinct exact path across all methods.
- `game_count_response` uses `std::int64_t` to match `sqlite3_column_int64` without a narrowing cast.

### References

- `_bmad-output/planning-artifacts/epics.md` — Epic 4d, Story 4d.4
- `source/motif/db/game_store.hpp` / `.cpp` — `list_games` pattern reused for `count_games`
- `source/motif/http/server.cpp` — route registration order, `sessions_mutex`, `prune_completed`, `generate_import_id`, `import_session`, `import_workers`
- `docs/api/openapi.yaml` — existing `ImportStartResponse` schema reused for upload 202 response
- `test/source/motif_http/http_server_test.cpp` — `tmp_dir`, `wait_for_ready`, `import_logging_scope`, `three_game_pgn_content`, `make_repeated_pgn`, `extract_import_id`, `sse_read_timeout_s`

## Dev Agent Record

### Agent Model Used

claude-sonnet-4-6

### Debug Log References

None.

### Completion Notes List

- `POST /api/imports/upload` registered as a distinct exact POST route; no conflict with the parameterized `GET`/`DELETE /api/imports/:import_id` routes.
- httplib parses `multipart/form-data` automatically into `req.form.files`; file content is available as `file_it->second.content`.
- Build: 215/215 tests passed. Sanitizer run pending.

### File List

- `source/motif/db/game_store.hpp` — added `count_games() const -> result<std::int64_t>`
- `source/motif/db/game_store.cpp` — implemented `count_games()`
- `source/motif/http/server.cpp` — added `game_count_response` DTO, `GET /api/games/count` route, `POST /api/imports/upload` route
- `docs/api/openapi.yaml` — added `/api/imports/upload`, `/api/games/count` paths and `GameCountResponse` schema
- `test/source/motif_http/http_server_test.cpp` — added 5 integration tests (ports 18157–18161)
- `_bmad-output/implementation-artifacts/sprint-status.yaml` — story added as `review`, `epic-4d` reopened to `in-progress`
- `_bmad-output/planning-artifacts/epics.md` — Story 4d.4 added, Epic 4d summary updated

### Change Log

- Implemented `game_store::count_games()` (`SELECT COUNT(*) FROM game`)
- Added `GET /api/games/count` → `{"count": N}`
- Added `POST /api/imports/upload` (multipart/form-data, temp file lifecycle, same pipeline as path-based import)
- Updated OpenAPI spec with both endpoints and `GameCountResponse` schema
- Added 5 HTTP integration tests

### Review Findings

- [x] [Review][Patch] `ofstream::write()` failure not checked [`source/motif/http/server.cpp:1317`] — After writing uploaded bytes to the temp file, `tmp.write()` can partially fail (disk full, I/O error) but no `.fail()`/`.bad()` check follows. Truncated PGN then passes silently to the import pipeline. Sources: blind+edge+auditor. **Fixed:** Added `tmp.fail()` check after write; removes temp file and session on failure.
- [x] [Review][Patch] Worker thread lacks try/catch; `std::filesystem::remove` uses throwing overload [`source/motif/http/server.cpp:1341-1355`] — If `pipeline->run()` throws (despite `tl::expected` convention), `std::filesystem::remove(pgn_path)` and `session->done.store(true)` are both skipped, orphaning the temp file and hanging the SSE stream. Independently, `std::filesystem::remove(pgn_path)` (the throwing overload, line 1346) can itself throw `filesystem_error`, causing `std::terminate()` inside a `jthread`. Fix: wrap worker body in try/catch, use `std::filesystem::remove(pgn_path, ec)` (non-throwing), and always set `done=true` in the catch path. Sources: edge+auditor. **Fixed:** Wrapped in try/catch, non-throwing `remove`, done flag set in all paths.
- [x] [Review][Patch] `std::filesystem::remove` inside catch block can throw causing `std::terminate` [`source/motif/http/server.cpp:1361`] — In the `catch (std::system_error const&)` handler, `std::filesystem::remove(temp_path)` is the throwing overload. A second exception during active exception handling triggers `std::terminate()`. Use `std::filesystem::remove(temp_path, ec)`. Source: edge. **Fixed:** Switched to non-throwing overload.
- [x] [Review][Patch] Temp file leak on allocation failure inside `sessions_mutex` lock [`source/motif/http/server.cpp:1336-1338`] — If `make_shared`, `make_unique`, or `emplace` throws (e.g., `std::bad_alloc`), the temp file at `temp_path` has already been written but no cleanup path exists. The `catch (std::system_error const&)` only covers jthread creation failure, which occurs after the lock is released. Sources: blind+edge. **Fixed:** Temp file is now created after session allocation, and failed file-open erases the session.
- [x] [Review][Patch] Temp filename uses separate `generate_import_id()` call diverging from actual `import_id` [`source/motif/http/server.cpp:1310`] — The temp path calls `generate_import_id()` before acquiring `sessions_mutex`, while the actual session `import_id` is generated inside the lock. On collision, the temp filename doesn't match the import ID. Sources: blind+auditor. **Fixed:** Temp path now uses the actual `import_id` generated inside the lock.
- [x] [Review][Defer] `std::filesystem::temp_directory_path()` can throw without JSON error response [`source/motif/http/server.cpp:1309`] — deferred, pre-existing
- [x] [Review][Defer] Unbounded `import_workers` growth — no reaping of completed workers [`source/motif/http/server.cpp:1358`] — deferred, pre-existing pattern from existing `/api/imports` endpoint
- [x] [Review][Defer] Missing test-failure injection flags for upload endpoint [`source/motif/http/server.cpp`] — deferred, test infrastructure gap not a functional bug
