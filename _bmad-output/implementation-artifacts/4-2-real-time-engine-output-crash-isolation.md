# Story 4.2: Real-Time Engine Output & Crash Isolation

Status: done

## Story

As a user,
I want to see engine depth, score, and principal variation updating in real time through a streaming
backend API, and have engine crashes never crash motif-chess,
So that I can analyze positions safely from desktop or web frontends even with unstable third-party
engines.

## Acceptance Criteria

1. **Given** engine analysis is running
   **When** the engine emits `info` lines
   **Then** depth, score (centipawns/mate), multipv index, and principal variation are emitted as
   normalized analysis updates via the SSE stream
   **And** principal variations are exposed in UCI form and SAN form where SAN conversion succeeds
   (FR31, NFR19)
   **And** the HTTP adapter streams those updates via the contract defined in Story 4d.2

2. **Given** the engine process crashes or exits unexpectedly
   **When** `engine_manager` detects the crash
   **Then** the session emits an `error` SSE event with a descriptive message; motif-chess continues
   running normally — no exception propagates out of the callback path (FR32)
   **And** the HTTP server keeps serving other requests and new analysis sessions can be started
   without relaunching (FR32)

3. **Given** `POST /api/engine/engines` is called with a valid name and executable path
   **When** the request is processed
   **Then** the engine is registered with `engine_manager::configure_engine` and subsequent analysis
   requests can use it (FR29)

4. **Given** `GET /api/engine/engines` is called
   **When** the request is processed
   **Then** the response contains the list of registered engines from `engine_manager::list_engines`

5. **Given** a valid `POST /api/engine/analyses` request is made and an engine is configured
   **When** the request is processed
   **Then** `engine_manager::start_analysis` is called with the mapped params and returns a real
   `analysis_id` (not a stub random ID)
   **And** the SSE session is created so `GET .../stream` can be called immediately

6. **Given** no engine is configured and `POST /api/engine/analyses` is called
   **When** the request is processed
   **Then** HTTP 503 is returned with `{"error":"no engine configured"}`

7. **Given** `GET /api/engine/analyses/:analysis_id/stream` is called with a valid analysis_id
   **When** the request is processed
   **Then** the response uses `text/event-stream` content type
   **And** `engine_manager::subscribe` is called with callbacks that push serialized SSE events to
   the stream as the engine produces them
   **And** the stream terminates after the `complete` or `error` terminal event

8. **Given** `GET /api/engine/analyses/:analysis_id/stream` is called with an unknown id
   **When** the request is processed
   **Then** HTTP 404 is returned (replacing the current 501)

9. **Given** `DELETE /api/engine/analyses/:analysis_id` is called for an active session
   **When** the request is processed
   **Then** `engine_manager::stop_analysis` is called and HTTP 204 is returned

10. **Given** `DELETE /api/engine/analyses/:analysis_id` is called for an already-terminal session
    **When** the request is processed
    **Then** HTTP 409 is returned with `{"error":"analysis already terminal"}`

11. **Given** `DELETE /api/engine/analyses/:analysis_id` is called with an unknown id
    **When** the request is processed
    **Then** HTTP 404 is returned (replacing the current 501)

12. **Given** all changes are implemented
    **When** `cmake --preset=dev && cmake --build build/dev && ctest --test-dir build/dev` is run
    **Then** all tests pass with zero new clang-tidy or cppcheck warnings

13. **Given** all changes are implemented
    **When** `cmake --preset=dev-sanitize && cmake --build build/dev-sanitize && ctest --test-dir
    build/dev-sanitize` is run
    **Then** zero ASan/UBSan violations

## Tasks / Subtasks

- [x] Task 1: Wire `engine.on_error()` in `register_callbacks` in `motif_engine.cpp` (AC: 2)
  - [ ] In `register_callbacks(session* sess_ptr)`, add a third callback registration after
    `on_info` and `on_bestmove`:
    ```cpp
    sess_ptr->engine.on_error(
        [sess_ptr](std::error_code ec) -> void {
            error_callback callback;
            {
                const std::scoped_lock cb_lock{sess_ptr->callback_mutex};
                if (!sess_ptr->terminal.exchange(true, std::memory_order_relaxed)) {
                    callback = sess_ptr->on_error;
                }
            }
            if (callback) {
                try {
                    callback(error_event{.message = fmt::format("engine crashed: {}", ec.message())});
                } catch (...) {
                    fmt::print(stderr, "engine_manager: on_error callback threw\n");
                }
            }
        });
    ```
  - [ ] No changes to the `session` struct — `uci::engine engine` remains the last member;
    no watchdog jthread is needed

- [x] Task 2: Add `engine_manager` to `server::impl` (AC: 3–11)
  - [ ] Add `motif::engine::engine_manager engine_mgr` member to `server::impl` struct in
    `server.cpp` (owned by server; Qt-free module, no CMake changes needed — motif_engine already
    linked as PRIVATE)
  - [ ] Add `analyses_mutex` and `analyses` map to `server::impl`:
    `std::mutex analyses_mutex;` and
    `std::unordered_map<std::string, std::shared_ptr<detail::analysis_sse_session>> analyses;`

- [x] Task 3: Add SSE session and glaze-serializable event structs to `server.cpp` (AC: 7)
  - [ ] Add to `motif::http::detail` namespace (glaze requirement — named namespace, not anon):
    ```cpp
    struct analysis_sse_session {
        std::mutex queue_mutex;
        std::deque<std::string> event_queue;
        std::condition_variable cv;
        std::atomic<bool> terminal{false};
    };
    struct configure_engine_request  { std::string name; std::string path; };
    struct engine_list_entry          { std::string name; std::string path; };
    struct engine_list_response       { std::vector<engine_list_entry> engines; };
    struct sse_score_value            { std::string type; int value{}; };
    struct sse_analysis_info_event {
        int depth{};
        std::optional<int> seldepth;
        int multipv{1};
        sse_score_value score;
        std::vector<std::string> pv_uci;
        std::optional<std::vector<std::string>> pv_san;
        std::optional<std::int64_t> nodes;
        std::optional<int> nps;
        std::optional<int> time_ms;
    };
    struct sse_analysis_complete_event {
        std::string best_move_uci;
        std::optional<std::string> ponder_uci;
    };
    struct sse_analysis_error_event   { std::string message; };
    ```
  - [ ] Add free helper functions in the anonymous namespace of `server.cpp`:
    - `make_info_sse_event(motif::engine::info_event const&) -> std::string` — serializes to
      `"event: info\ndata: <json>\n\n"` using glaze
    - `make_complete_sse_event(motif::engine::complete_event const&) -> std::string`
    - `make_error_sse_event(motif::engine::error_event const&) -> std::string`

- [x] Task 4: Implement `POST /api/engine/engines` (AC: 3)
  - [ ] Parse `configure_engine_request` JSON body; return 400 on parse error
  - [ ] Return 400 if `name` or `path` is empty
  - [ ] Call `impl_->engine_mgr.configure_engine({.name=..., .path=...})`; return 400 on error
    (`engine_not_configured` means empty path — already validated)
  - [ ] Return 200 with `{"status":"ok"}`

- [x] Task 5: Implement `GET /api/engine/engines` (AC: 4)
  - [ ] Call `impl_->engine_mgr.list_engines()`
  - [ ] Map to `engine_list_response`, serialize with glaze, return 200

- [x] Task 6: Wire `POST /api/engine/analyses` to real `engine_manager` (AC: 5, 6)
  - [ ] Map `start_analysis_request` → `motif::engine::analysis_params`:
    - `fen` — already validated by existing FEN check
    - `engine` — pass through (empty = first registered)
    - `multipv` — `req_body.multipv.value_or(1)`
    - `depth`, `movetime_ms` — pass through optionals
  - [ ] Remove the stub `generate_analysis_id()` function from server.cpp (engine_manager now owns IDs)
  - [ ] Call `impl_->engine_mgr.start_analysis(params)`:
    - On `engine_not_configured` → 503 `{"error":"no engine configured"}`
    - On `invalid_analysis_params` → 400 (should not occur after HTTP validation, but handle defensively)
    - On `engine_start_failed` → 503 `{"error":"engine failed to start"}`
  - [ ] On success: create `analysis_sse_session`, store under `analysis_id` in `analyses` map
    (hold `analyses_mutex`)
  - [ ] Return 202 with `{"analysis_id": "<id>"}`

- [x] Task 7: Implement `GET /api/engine/analyses/:analysis_id/stream` (AC: 7, 8)
  - [ ] Look up `analyses[analysis_id]` under `analyses_mutex`; return 404 if not found
  - [ ] Call `impl_->engine_mgr.subscribe(analysis_id, on_info, on_complete, on_error)`:
    - Each callback serializes the event and pushes to `sse_session->event_queue` under
      `sse_session->queue_mutex`, then calls `sse_session->cv.notify_one()`
    - `on_complete` and `on_error` also set `sse_session->terminal = true` and call
      `cv.notify_all()`
    - On subscribe failure (analysis_not_found) → 404
  - [ ] Set `res.set_chunked_content_provider("text/event-stream", ...)`:
    ```cpp
    [sse_session](size_t, httplib::DataSink& sink) -> bool {
        std::vector<std::string> pending;
        bool is_terminal = false;
        {
            std::unique_lock lock{sse_session->queue_mutex};
            sse_session->cv.wait_for(lock, std::chrono::milliseconds{250},
                [&] { return !sse_session->event_queue.empty()
                           || sse_session->terminal.load(); });
            while (!sse_session->event_queue.empty()) {
                pending.push_back(std::move(sse_session->event_queue.front()));
                sse_session->event_queue.pop_front();
            }
            is_terminal = sse_session->terminal.load();
        }
        for (auto const& ev : pending) {
            if (!sink.write(ev.data(), ev.size())) { return false; }
        }
        return !is_terminal;
    }
    ```

- [x] Task 8: Implement `DELETE /api/engine/analyses/:analysis_id` (AC: 9, 10, 11)
  - [ ] Call `impl_->engine_mgr.stop_analysis(analysis_id)`:
    - On `analysis_not_found` → 404 `{"error":"analysis not found"}`
    - On `analysis_already_terminal` → 409 `{"error":"analysis already terminal"}`
    - On `engine_stop_failed` → 500
  - [ ] Return 204 on success

- [x] Task 9: Update `docs/api/openapi.yaml` (AC: 3, 4, 12)
  - [ ] Add `POST /api/engine/engines` with `ConfigureEngineRequest` schema and 200/400 responses
  - [ ] Add `GET /api/engine/engines` with `EngineListResponse` schema
  - [ ] Add 503 response to `POST /api/engine/analyses` for `no engine configured`

- [x] Task 10: Add/update tests (AC: 1–11)
  - [ ] In `test/source/motif_engine/engine_manager_test.cpp`:
    - Add crash detection test: create fake engine that exits without bestmove on `go`, verify
      `on_error` callback fires within 1 second
    - Use `make_fake_engine("      exit 1\n")` — engine exits without bestmove; ucilib fires
      `on_error_(make_error_code(errc::engine_crashed))` from its reader thread
  - [ ] In `test/source/motif_http/http_server_test.cpp`:
    - Helper `make_fast_complete_engine()` from `engine_manager_test.cpp` — replicate the shell
      script generator (or extract to a shared header if feasible)
    - `POST /api/engine/engines` — valid registration returns 200
    - `POST /api/engine/engines` — empty path returns 400
    - `POST /api/engine/analyses` — with engine configured uses a real fake engine and returns 202
      with non-empty analysis_id
    - `POST /api/engine/analyses` — engine not configured returns 503
    - `GET /api/engine/analyses/:id/stream` — unknown id returns 404 (update existing test from
      "404 or 501" to strict 404)
    - `GET /api/engine/analyses/:id/stream` — valid id receives `info` then `complete` SSE events
      (use `make_fast_complete_engine`, verify event types in response body)
    - `DELETE /api/engine/analyses/:id` — unknown id returns 404 (update existing test from
      "404 or 501" to strict 404)
    - `DELETE /api/engine/analyses/:id` — active session returns 204
    - `DELETE /api/engine/analyses/:id` — already-terminal returns 409
    - All real-engine tests must clean up the fake engine script file afterward

- [x] Task 11: Validate build and sanitizers (AC: 12, 13)
  - [ ] `cmake --preset=dev && cmake --build build/dev`
  - [ ] `ctest --test-dir build/dev`
  - [ ] `cmake --preset=dev-sanitize && cmake --build build/dev-sanitize`
  - [ ] `ctest --test-dir build/dev-sanitize`
  - [ ] `clang-format` on all touched `.cpp` and `.hpp` files (not CMakeLists.txt)

## Dev Notes

### Scope Boundary

This story wires `motif_engine` into the HTTP layer and implements crash detection. It does NOT:
- Add Qt engine config UI (Story 7.1's `app_config` / `engine_paths`)
- Change `engine_manager`'s public API signatures (locked by 4d.2 contract)
- Modify `flake.nix` or `vcpkg.json` (flake.lock was updated by `nix flake update ucilib`)

### Crash Detection via `uci::engine::on_error`

ucilib now exposes `on_error(error_callback cb)` where
`error_callback = std::function<void(std::error_code)>`. This fires from the reader thread
when `reproc::drain` returns and `in_search_` is still true (i.e. `go` was sent but no `bestmove`
arrived — the engine crashed).

The error code is `make_error_code(uci::errc::engine_crashed)`. Use `ec.message()` for the
human-readable description.

**`session` struct is unchanged** — `uci::engine engine` remains the last member (destructs first
via LIFO, joining the reader thread before callbacks are destroyed). No watchdog jthread needed.

**New ucilib installed path:** `/nix/store/gqsxy47x8x7dr99gwsxswsfw8b1ksgzk-ucilib/`

The key new API:
```cpp
using error_callback = std::function<void(std::error_code)>;
void on_error(error_callback cb);  // fires on engine crash; std::error_code == engine_crashed
```

**Ordering constraint:** Register `on_error` in `register_callbacks` before `go()` is called,
same as `on_info` and `on_bestmove`. The `terminal.exchange(true)` inside the callback prevents
double-fire if both `on_bestmove` and `on_error` somehow race (they cannot in ucilib's design
since `in_search_` is cleared before `on_bestmove` fires, but the guard costs nothing).

**`fmt::format("engine crashed: {}", ec.message())` note:** `ec.message()` for
`uci::errc::engine_crashed` is implementation-defined string from the uci error category. Use it
directly — do not hardcode a message string.

### SSE Architecture in HTTP Server

The `analysis_sse_session` is a thread-safe event queue connecting engine callbacks (fired on
ucilib's internal reader thread) to the httplib content provider (runs on httplib's thread pool).

**Key threading invariants:**
- Callbacks push to `event_queue` and notify under `queue_mutex` (same as import pattern)
- Content provider drains queue and returns `false` when `terminal=true` and queue is empty
- `analyses_mutex` guards the `analyses` map (separate from `queue_mutex` — no lock ordering issue)
- The `analysis_sse_session` is shared_ptr — safe to capture in both the subscribe callbacks and
  the content provider lambda

**Subscribe timing:** The GET stream handler calls `subscribe` before setting the content provider.
Since `subscribe` can replay `last_complete` (implemented in Story 4.1), late subscribers that call
GET after the engine completes will immediately receive the complete event. This avoids the race
where POST returns 202 but GET stream arrives after bestmove.

### HTTP Error Mapping

| `engine::error_code`        | HTTP status | Response body                            |
|-----------------------------|-------------|------------------------------------------|
| `engine_not_configured`     | 503         | `{"error":"no engine configured"}`       |
| `invalid_analysis_params`   | 400         | `{"error":"invalid analysis params"}`    |
| `engine_start_failed`       | 503         | `{"error":"engine failed to start"}`     |
| `analysis_not_found`        | 404         | `{"error":"analysis not found"}`         |
| `analysis_already_terminal` | 409         | `{"error":"analysis already terminal"}`  |
| `engine_stop_failed`        | 500         | `{"error":"engine stop failed"}`         |

### glaze Struct Placement Rule

**All glaze-serializable structs must be in a named namespace** (`motif::http::detail`), not an
anonymous namespace. This is a pre-existing constraint (glaze reflection requires external linkage).
`analysis_sse_session` does NOT need glaze serialization (it's internal state) and can be placed
in the anonymous namespace or directly in `server::impl`.

### Fake Engine Test Pattern (established in Story 4.1)

Engine integration tests use shell-script fake engines created at test time in
`std::filesystem::temp_directory_path()`. The pattern from `engine_manager_test.cpp`:

```cpp
auto make_fast_complete_engine() -> std::filesystem::path {
    return make_fake_engine(
        "      printf '%s\\n' 'info depth 1 seldepth 1 multipv 1 score cp 13 nodes 42 "
        "nps 1000 time 1 pv e2e4 e7e5'\n"
        "      printf '%s\\n' 'bestmove e2e4 ponder e7e5'\n");
}
```

For a crash-detection test, use an engine that exits without bestmove on `go`:
```cpp
auto make_crashing_engine() -> std::filesystem::path {
    // Exits immediately when 'go' is received — no bestmove sent.
    // ucilib detects this via in_search_ flag and fires on_error.
    return make_fake_engine("      exit 1\n");
}
```

HTTP tests should replicate or import this helper pattern.

**Cleanup:** Always call `std::filesystem::remove(fake_engine)` in test teardown.

### No New CMake Changes

`motif_engine` is already listed as PRIVATE in `source/motif/http/CMakeLists.txt` (line 23).
No CMake changes needed for this story.

`server.hpp` does NOT need to change — `engine_manager` is an implementation detail of `server::impl`
owned in `server.cpp`. The `server` public interface remains `server(database_manager&)`.

### Thread Safety in server::impl

- `engine_mgr` is called from httplib's thread pool (multiple concurrent HTTP requests)
- `analyses_mutex` guards the `analyses` map (same pattern as `sessions_mutex` for imports)
- The `analysis_sse_session` queue operations use their own `queue_mutex` — no lock ordering
  dependency on `analyses_mutex`

### Existing Tests That Change

The two existing tests that accept "404 or 501" must be tightened to strict 404:
- `"server: GET /api/engine/analyses/unknown-id/stream returns 404 or 501"` → strict 404
- `"server: DELETE /api/engine/analyses/unknown-id returns 404 or 501"` → strict 404

The `"server: POST /api/engine/analyses valid body returns 202 with analysis_id"` test currently
passes because the stub always returns 202. After this story, it will call `engine_manager::start_analysis`
which requires an engine to be configured. Either:
a) Update the test to first call `POST /api/engine/engines` with a fake engine path, OR
b) Expect 503 when no engine is configured

Use approach (a) — configure a real fake engine and verify a real analysis_id is returned.

### Architecture Compliance

- `motif_engine` remains Qt-free (confirmed: no Qt in engine_manager.hpp or motif_engine.cpp)
- All identifiers: `lower_snake_case`
- No `using namespace` in headers
- `#pragma once` on all headers
- `fmt::format`/`fmt::print` for all string/console ops — not `std::format`, `std::to_string`,
  `std::cout`
- `tl::expected` for all fallible operations
- No raw owning pointers

### Project Structure Notes

Files modified in this story:
```text
source/motif/engine/motif_engine.cpp      ← add crash_watchdog to session struct + launch logic
source/motif/http/server.cpp              ← add engine_mgr to impl, wire routes, add SSE session
docs/api/openapi.yaml                     ← add /api/engine/engines routes, 503 to analyses POST
test/source/motif_engine/engine_manager_test.cpp  ← add crash watchdog test
test/source/motif_http/http_server_test.cpp       ← add engine HTTP integration tests
```

No new files required. No `server.hpp` changes required. No CMakeLists.txt changes required.

### References

- [Source: `_bmad-output/planning-artifacts/epics.md` — Epic 4, Story 4.2 AC]
- [Source: `_bmad-output/implementation-artifacts/4-1-engine-configuration-lifecycle.md` — previous
  story; session struct layout, fake engine pattern, ucilib API, thread-safety notes]
- [Source: `_bmad-output/implementation-artifacts/deferred-work.md` — AC7 crash detection deferred
  from 4.1; watchdog is downstream workaround pending ucilib upstream change]
- [Source: `source/motif/engine/engine_manager.hpp` — public API (do not change)]
- [Source: `source/motif/engine/motif_engine.cpp` — session struct to extend with crash_watchdog]
- [Source: `source/motif/engine/error.hpp` — existing error codes]
- [Source: `source/motif/http/server.cpp` — impl struct, SSE pattern from import (see lines 1630-1688), glaze struct placement rule]
- [Source: `source/motif/http/server.hpp` — no changes required]
- [Source: `source/motif/http/CMakeLists.txt` — motif_engine already linked PRIVATE]
- [Source: `test/source/motif_engine/engine_manager_test.cpp` — make_fake_engine helpers]
- [Source: `test/source/motif_http/http_server_test.cpp` — existing engine test cases to update]
- [Source: `docs/api/openapi.yaml` — existing engine endpoint schemas and SSE event shapes]
- [Installed: `/nix/store/gqsxy47x8x7dr99gwsxswsfw8b1ksgzk-ucilib/include/ucilib/engine.hpp`]
- [Installed: `/nix/store/gqsxy47x8x7dr99gwsxswsfw8b1ksgzk-ucilib/include/ucilib/types.hpp`]
- [Source: `CONVENTIONS.md` — naming, fmt, testing, module boundaries, no exceptions]

## Dev Agent Record

### Agent Model Used

claude-sonnet-4-6

### Debug Log References

- ucilib `quit()` bug: when engine crashes, reader thread sets `running_=false` before calling
  `on_error_`. `quit()` returned early (checked `running_`) without joining the thread, leaving a
  joinable `std::thread` that caused `std::terminate()` in `~impl()`. Fixed in local ucilib source
  at `/home/bogdb/src/motif/ucilib/source/engine.cpp` by separating process-stop from thread-join
  logic: join is now unconditional when `reader_thread.joinable()`.
- SSE info events lost race: fast fake engines could complete before `subscribe()` was called at
  GET time. Fixed by: (1) adding `info_history` buffer to `session` and replaying in `subscribe()`,
  and (2) moving `subscribe()` call to POST handler so events are captured from the start.
- ucilib fix merged to master (`foolnotion/ucilib` PR #2, commit `0926064`). `flake.lock` updated
  via `nix flake update ucilib` to lock `60abbdac`. CMakeCache `ucilib_DIR` points to the new
  Nix store path `/nix/store/mibzsdb4q0v7mksqiy3ig6fq9zdv8wb6-ucilib`. Reconfiguring with
  `cmake --preset=dev` will pick it up automatically.

### Completion Notes List

- All 13 AC verified: 244/244 tests pass (dev build), 90/90 non-perf tests pass (sanitize build),
  zero ASan/UBSan violations, zero new clang-tidy/cppcheck warnings.
- Local ucilib fix required (upstream bug); override documented above.

### File List

- `source/motif/engine/motif_engine.cpp` — on_error wiring, info_history buffer, subscribe replay, destructor fix
- `source/motif/http/server.cpp` — engine_mgr integration, SSE session, all engine routes, early subscribe
- `docs/api/openapi.yaml` — new engine endpoints, 503 response, new schemas
- `test/source/motif_engine/engine_manager_test.cpp` — crash detection test
- `test/source/motif_http/http_server_test.cpp` — engine HTTP integration tests
- `/home/bogdb/src/motif/ucilib/source/engine.cpp` — quit() thread-join bug fix (merged upstream)
- `flake.lock` — updated ucilib to `60abbdac` (includes the fix)
