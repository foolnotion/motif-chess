# Story 4b.4: Import Trigger & SSE Progress Stream

Status: done

## Story

As a user,
I want to trigger a PGN import via HTTP and receive real-time progress updates via Server-Sent Events,
So that I can import games from any frontend and monitor progress without polling.

## Acceptance Criteria

1. **Given** the server is running and a database is loaded
   **when** `POST /api/imports` is called with `{ "path": "/path/to/file.pgn" }`
   **then** the import starts asynchronously and the response returns HTTP 202 with `{ "import_id": "<uuid>" }` (FR09)

2. **Given** an import is running
   **when** `GET /api/imports/{import_id}/progress` is called (SSE endpoint)
   **then** events are streamed containing `games_processed`, `games_committed`, `games_skipped`, `elapsed_seconds` (FR13)
   **and** a final event is sent with the complete `import_summary` on completion (FR14)

3. **Given** an import fails to start (file not found, unreadable)
   **when** `POST /api/imports` is called
   **then** HTTP 400 is returned with an error message (NFR10)

4. **Given** an import is running
   **when** `DELETE /api/imports/{import_id}` is called
   **then** the import is gracefully stopped and checkpoint is saved for resume

5. **Given** all changes are implemented
   **when** `cmake --preset=dev && cmake --build build/dev` is run
   **then** it succeeds with zero clang-tidy and cppcheck warnings

6. **Given** all changes are implemented
   **when** `ctest --test-dir build/dev` is run
   **then** all tests pass including new tests covering AC 1–4

## Tasks / Subtasks

- [x] Task 1: Add `request_stop()` to `import_pipeline` (`motif_import`)
  - [x] Add `std::atomic<bool> stop_requested_ {false};` to `import_pipeline` private section in `import_pipeline.hpp`
  - [x] Add `void request_stop() noexcept;` public method declaration to `import_pipeline.hpp`
  - [x] Implement `request_stop()` in `import_pipeline.cpp`: `stop_requested_.store(true, std::memory_order_relaxed);`
  - [x] In `run_from()` batch loop body (after each batch commit + checkpoint write), check `stop_requested_.load()` and break early if set

- [x] Task 2: Add import session management to `server::impl` (`motif_http`)
  - [x] Define `import_session` struct in `motif::http::detail` namespace of `server.cpp`: holds `std::filesystem::path pgn_path`, `std::unique_ptr<import_pipeline>`, `std::atomic<bool> cancel_requested`, `std::atomic<bool> done`, `std::atomic<bool> failed`, `import_summary summary` (written before `done.store(true)`), `std::string error_message`
  - [x] Add `sessions_mutex` (`std::mutex`) and `sessions` (`std::unordered_map<std::string, std::shared_ptr<import_session>>`) to `server::impl`
  - [x] Add `generate_import_id()` free function in anonymous namespace: uses `<random>` and `fmt::format` to produce a 32-char lowercase hex string — no UUID library needed

- [x] Task 3: Add `POST /api/imports` route
  - [x] Add `#include <glaze/json/read.hpp>` and `#include "motif/import/import_pipeline.hpp"` to `server.cpp` (also `<filesystem>`, `<random>`, `<thread>`, `<unordered_map>`, `<atomic>`)
  - [x] Define `import_request_body { std::string path; }` and `import_response { std::string import_id; }` in `motif::http::detail` namespace
  - [x] Register `POST /api/imports` route via `server::impl::setup_routes()`
  - [x] Route handler: parse body with `glz::read_json`; on parse failure → 400 `"invalid request body"`
  - [x] Validate `std::filesystem::exists(pgn_path) && std::filesystem::is_regular_file(pgn_path)`; on failure → 400 `"file not found or not readable"`
  - [x] Create `import_session` (as `shared_ptr`), generate `import_id`, insert into `sessions` map under `sessions_mutex`
  - [x] Launch worker `std::thread` (detached): calls `session->pipeline->run(pgn_path, {})`, stores result into session fields, then sets `session->done.store(true, std::memory_order_release)`
  - [x] Return HTTP 202 with `{ "import_id": "<id>" }` using glaze

- [x] Task 4: Add `GET /api/imports/:import_id/progress` SSE route
  - [x] Register route; look up session by `import_id` under `sessions_mutex`; if not found → 404 `"import not found"`
  - [x] Use `res.set_chunked_content_provider("text/event-stream", ...)` (cpp-httplib's streaming API)
  - [x] In the content provider lambda (captures `shared_ptr<import_session>`): call `session->pipeline->progress()`, format as SSE progress event, write to `sink`
  - [x] If `session->done.load(std::memory_order_acquire)` is true: write final completion event and return `false` to end the stream
  - [x] Sleep 250 ms between events; return `true` to continue; return `false` on `sink.write()` failure (client disconnect)
  - [x] SSE progress event format: `"data: {\"games_processed\":N,\"games_committed\":N,\"games_skipped\":N,\"elapsed_seconds\":F}\n\n"`
  - [x] SSE final event format: `"event: complete\ndata: {\"total_attempted\":N,\"committed\":N,\"skipped\":N,\"errors\":N,\"elapsed_ms\":N}\n\n"` (uses `session->summary`)

- [x] Task 5: Add `DELETE /api/imports/:import_id` route
  - [x] Register route; look up session by `import_id` under `sessions_mutex`; if not found → 404
  - [x] Call `session->cancel_requested.store(true)` and `session->pipeline->request_stop()`
  - [x] Return HTTP 200 with `{ "status": "cancellation_requested" }` immediately (non-blocking; the worker thread will finish async)

- [x] Task 6: Tests in `test/source/motif_http/http_server_test.cpp`
  - [x] Test: nonexistent file → POST returns 400 with error (port 18101)
  - [x] Test: valid PGN file → POST returns 202 with `import_id` field (port 18102)
  - [x] Test: SSE stream → GET returns progress events + final completion event with required fields (port 18103)
  - [x] Test: DELETE → cancellation accepted, import stops (port 18104)

- [x] Task 7: Build + test validation — zero warnings, all tests pass

### Review Findings

- [x] [Review][Patch] Enforce one active import at a time in the HTTP layer [source/motif/http/server.cpp:287]
- [x] [Review][Patch] Detached import worker can outlive `server` and `database_manager` [source/motif/http/server.cpp:311]
- [x] [Review][Patch] DELETE cancellation deletes the resume checkpoint on normal pipeline exit [source/motif/import/import_pipeline.cpp:510]
- [x] [Review][Patch] DELETE may not stop until a full batch commits or a small import reaches EOF [source/motif/import/import_pipeline.cpp:431]
- [x] [Review][Patch] Runtime import failures are emitted to SSE clients as `event: complete` with a default summary [source/motif/http/server.cpp:363]
- [x] [Review][Patch] POST validates existence but not readability and uses throwing filesystem overloads [source/motif/http/server.cpp:296]
- [x] [Review][Patch] `request_stop()` state is sticky across reused `import_pipeline` runs [source/motif/import/import_pipeline.hpp:79]
- [x] [Review][Patch] SSE test does not assert all required progress and final summary fields [test/source/motif_http/http_server_test.cpp:454]
- [x] [Review][Patch] DELETE test does not prove the import stopped or that a resume checkpoint was saved [test/source/motif_http/http_server_test.cpp:486]

## Dev Notes

### CMake Linkage

`motif_http` links `motif_import` privately because `server.cpp` owns the import endpoint implementation and constructs `import_pipeline`. This ensures both `motif_http_test` and `motif_http_server` link the import symbols through the HTTP module.

### Task 1: `import_pipeline` Stop Mechanism

The pipeline's public header (`source/motif/import/import_pipeline.hpp`) needs one addition:

```cpp
// In private section (after existing atomics):
std::atomic<bool> stop_requested_ {false};

// In public section:
void request_stop() noexcept;
```

In `import_pipeline.cpp`, add in the batch processing loop inside `run_from()` — immediately after the batch commit + checkpoint write. The exact location: find the innermost `for`/`while` loop that commits batches and check the flag after each commit:

```cpp
if (stop_requested_.load(std::memory_order_relaxed)) {
    break; // checkpoint is already written; partial import is valid
}
```

The `run()` return value (`import_summary`) naturally reflects only committed games — no change needed there.

### Task 2: Session Management Design

Define in the anonymous namespace of `server.cpp`:

```cpp
struct import_session
{
    std::filesystem::path pgn_path;
    std::unique_ptr<motif::import::import_pipeline> pipeline;
    std::atomic<bool> cancel_requested {false};
    std::atomic<bool> done {false};
    std::atomic<bool> failed {false};
    motif::import::import_summary summary {};
    std::string error_message;
};
```

Add to `server::impl`:
```cpp
std::mutex sessions_mutex;
std::unordered_map<std::string, std::shared_ptr<import_session>> sessions;
```

`import_session` holds a `unique_ptr<import_pipeline>` because each concurrent import needs its own pipeline instance (the pipeline owns per-import atomic counters).

Worker thread pattern — detached for fire-and-forget:
```cpp
auto session = std::make_shared<import_session>();
session->pipeline = std::make_unique<motif::import::import_pipeline>(dbmgr);

std::thread worker {[session, pgn_path = std::filesystem::path{req_body.path}]() -> void {
    auto result = session->pipeline->run(pgn_path, {});
    if (result) {
        session->summary = *result;
    } else {
        session->failed.store(true, std::memory_order_relaxed);
        session->error_message = "import failed";
    }
    session->done.store(true, std::memory_order_release);
}};
worker.detach();
```

Detaching is safe here because the lambda holds a `shared_ptr` — the session outlives the thread even if the server stops.

### UUID Generation

No UUID library is available. Use `fmt::format` with two 64-bit random values:

```cpp
auto generate_import_id() -> std::string
{
    static std::mutex rng_mutex;
    static std::mt19937_64 rng {std::random_device {}()};
    std::uint64_t high {};
    std::uint64_t low {};
    {
        std::scoped_lock lock {rng_mutex};
        high = rng();
        low  = rng();
    }
    return fmt::format("{:016x}{:016x}", high, low);
}
```

The mutex guards the non-thread-safe `mt19937_64` state. Result is a 32-character lowercase hex string.

**`fmt::format` not `std::format`** — per architecture constraint. Include `<fmt/format.h>`.

### Task 3: POST /api/imports Route Handler

```cpp
// In motif::http::detail namespace (alongside health_response, error_response)
struct import_response
{
    std::string import_id;
};

struct import_request_body
{
    std::string path;
};
```

Route registration in `register_routes`:
```cpp
svr.Post("/api/imports",
    [&dbmgr, &impl_ref](httplib::Request const& req, httplib::Response& res) -> void
    {
        detail::import_request_body req_body;
        if (auto err = glz::read_json(req_body, req.body); err) {
            set_json_error(res, http_bad_request, "invalid request body");
            return;
        }

        std::filesystem::path const pgn_path {req_body.path};
        if (!std::filesystem::exists(pgn_path) || !std::filesystem::is_regular_file(pgn_path)) {
            set_json_error(res, http_bad_request, "file not found or not readable");
            return;
        }

        auto const import_id = generate_import_id();
        auto session = std::make_shared<import_session>();
        session->pipeline = std::make_unique<motif::import::import_pipeline>(dbmgr);

        {
            std::scoped_lock lock {impl_ref.sessions_mutex};
            impl_ref.sessions.emplace(import_id, session);
        }

        std::thread worker {[session, pgn_path]() -> void {
            auto result = session->pipeline->run(pgn_path, {});
            if (result) {
                session->summary = *result;
            } else {
                session->failed.store(true, std::memory_order_relaxed);
                session->error_message = "import failed";
            }
            session->done.store(true, std::memory_order_release);
        }};
        worker.detach();

        std::string body {};
        [[maybe_unused]] auto const err = glz::write_json(detail::import_response {import_id}, body);
        res.set_content(body, "application/json");
        res.status = http_accepted;  // 202
    });
```

Add `constexpr int http_accepted {202};` alongside the existing `http_ok`, `http_bad_request`, `http_internal_error` constants.

### Task 4: GET SSE Route Handler

cpp-httplib SSE streaming uses `res.set_chunked_content_provider`. The content provider lambda is called repeatedly; return `true` to continue, `false` to end.

```cpp
svr.Get("/api/imports/:import_id/progress",
    [&impl_ref](httplib::Request const& req, httplib::Response& res) -> void
    {
        auto const& id = req.path_params.at("import_id");
        std::shared_ptr<import_session> session;
        {
            std::scoped_lock lock {impl_ref.sessions_mutex};
            auto const it = impl_ref.sessions.find(id);
            if (it == impl_ref.sessions.end()) {
                set_json_error(res, http_not_found, "import not found");
                return;
            }
            session = it->second;
        }

        res.set_chunked_content_provider("text/event-stream",
            [session](size_t /*offset*/, httplib::DataSink& sink) -> bool
            {
                auto const prog = session->pipeline->progress();
                auto const elapsed = static_cast<double>(prog.elapsed.count()) / 1000.0;
                auto event = fmt::format(
                    "data: {{\"games_processed\":{},\"games_committed\":{},\"games_skipped\":{},\"elapsed_seconds\":{:.3f}}}\n\n",
                    prog.games_processed, prog.games_committed, prog.games_skipped, elapsed);
                if (!sink.write(event.data(), event.size())) {
                    return false;
                }

                if (session->done.load(std::memory_order_acquire)) {
                    auto const& s = session->summary;
                    auto final_event = fmt::format(
                        "event: complete\ndata: {{\"total_attempted\":{},\"committed\":{},\"skipped\":{},\"errors\":{},\"elapsed_ms\":{}}}\n\n",
                        s.total_attempted, s.committed, s.skipped, s.errors, s.elapsed.count());
                    sink.write(final_event.data(), final_event.size());
                    sink.done();
                    return false;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds {250});
                return true;
            });
    });
```

Add `constexpr int http_not_found {404};` alongside other status constants.

**Note on SSE headers**: cpp-httplib automatically adds `Transfer-Encoding: chunked` when using `set_chunked_content_provider`. You may additionally need to set `Cache-Control: no-cache` and `Connection: keep-alive` for SSE compliance; add these in the content provider or in the CORS post-routing handler.

**Note on the `impl_ref` capture**: `register_routes` currently takes `motif::db::database_manager const& dbmgr` and `std::mutex& database_mutex`. To access `sessions` and `sessions_mutex`, either:
- Pass `impl&` to `register_routes` (simplest refactor), or
- Capture `impl_ref` by ref (safe since `impl` outlives all handlers)

The cleanest approach: change `register_routes` signature to accept `impl& impl_ref` directly instead of separate `dbmgr` and `database_mutex` args — then access `impl_ref.database`, `impl_ref.database_mutex`, `impl_ref.sessions`, `impl_ref.sessions_mutex` all in one place. Check if this requires changing `impl` declaration order in `server.cpp` (structs must be defined before use).

### Task 5: DELETE Route Handler

```cpp
svr.Delete("/api/imports/:import_id",
    [&impl_ref](httplib::Request const& req, httplib::Response& res) -> void
    {
        auto const& id = req.path_params.at("import_id");
        std::shared_ptr<import_session> session;
        {
            std::scoped_lock lock {impl_ref.sessions_mutex};
            auto const it = impl_ref.sessions.find(id);
            if (it == impl_ref.sessions.end()) {
                set_json_error(res, http_not_found, "import not found");
                return;
            }
            session = it->second;
        }

        session->cancel_requested.store(true, std::memory_order_relaxed);
        session->pipeline->request_stop();

        res.set_content(R"({"status":"cancellation_requested"})", "application/json");
        res.status = http_ok;
    });
```

### Task 6: Testing Approach

**Logging requirement**: Tests that use the import endpoint must initialize import logging. Use the existing `import_logging_scope` RAII helper (already in `http_server_test.cpp`). The PGN import will crash/assert if logging is not initialized.

**PGN fixture**: Write the three-game PGN literal inline (same pattern as `import_pipeline_test.cpp`). The `http_server_test.cpp` tests do not currently share the `k_three_game_pgn` constant from `motif_import` test files (different test executable). Define a local `constexpr auto three_game_pgn_content = R"pgn(...)pgn";` at file scope in `http_server_test.cpp`.

Three-game PGN content (copy from `import_pipeline_test.cpp:34-68`):
```
[Event "Test"]
[Site "?"]
...
1. e4 e5 2. Nf3 Nc6 3. Bb5 1-0

[Event "Test"]
...
1. d4 d5 2. c4 c6 0-1

[Event "Test"]
...
1. Nf3 Nf6 2. g3 g6 1/2-1/2
```

**Helper: write PGN to temp file**
```cpp
auto write_pgn_fixture(std::filesystem::path const& dir, std::string_view content) -> std::filesystem::path
{
    auto path = dir / "fixture.pgn";
    std::ofstream out {path};
    out << content;
    return path;
}
```

**Test: POST returns 400 for nonexistent file** (port 18101)
```cpp
TEST_CASE("server: import rejects nonexistent file", "[motif-http]")
{
    auto const tdir = tmp_dir {"import_badfile"};
    auto db = motif::db::database_manager::create(tdir.path / "db", "import-bad");
    REQUIRE(db.has_value());

    constexpr std::uint16_t test_port {18101};
    motif::http::server srv {*db};
    std::thread st {[&]() -> void { [[maybe_unused]] auto r = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Post("/api/imports",
        R"({"path":"/nonexistent/path/file.pgn"})", "application/json");

    srv.stop();
    st.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 400);
}
```

**Test: POST returns 202 with import_id** (port 18102)
```cpp
TEST_CASE("server: import accepts valid file", "[motif-http]")
{
    auto const tdir = tmp_dir {"import_valid"};
    auto db = motif::db::database_manager::create(tdir.path / "db", "import-valid");
    REQUIRE(db.has_value());

    auto const pgn_path = write_pgn_fixture(tdir.path, three_game_pgn_content);
    auto logging = import_logging_scope {tdir.path / "logs"};

    constexpr std::uint16_t test_port {18102};
    motif::http::server srv {*db};
    std::thread st {[&]() -> void { [[maybe_unused]] auto r = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Post("/api/imports",
        R"({"path":")" + pgn_path.string() + R"("})", "application/json");

    // Give the import a moment, then stop
    std::this_thread::sleep_for(std::chrono::milliseconds {100});
    srv.stop();
    st.join();
    logging.shutdown();

    REQUIRE(res != nullptr);
    CHECK(res->status == 202);
    CHECK(res->body.find(R"("import_id")") != std::string::npos);
}
```

**Test: SSE streams progress events** (port 18103)
```cpp
TEST_CASE("server: import SSE streams progress and completion", "[motif-http]")
{
    auto const tdir = tmp_dir {"import_sse"};
    auto db = motif::db::database_manager::create(tdir.path / "db", "import-sse");
    REQUIRE(db.has_value());

    auto const pgn_path = write_pgn_fixture(tdir.path, three_game_pgn_content);
    auto logging = import_logging_scope {tdir.path / "logs"};

    constexpr std::uint16_t test_port {18103};
    motif::http::server srv {*db};
    std::thread st {[&]() -> void { [[maybe_unused]] auto r = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    cli.set_read_timeout(10); // 10s max for tiny fixture

    // Start the import
    auto const post_res = cli.Post("/api/imports",
        R"({"path":")" + pgn_path.string() + R"("})", "application/json");
    REQUIRE(post_res != nullptr);
    REQUIRE(post_res->status == 202);

    // Extract import_id
    auto const& body = post_res->body;
    auto const id_start = body.find(R"("import_id":")" ) + 13;
    auto const id_end   = body.find('"', id_start);
    auto const import_id = body.substr(id_start, id_end - id_start);
    REQUIRE(!import_id.empty());

    // Consume SSE stream
    std::string collected_events;
    cli.Get("/api/imports/" + import_id + "/progress",
        httplib::Headers {},
        [&collected_events](const char* data, size_t size) -> bool {
            collected_events.append(data, size);
            return true;
        });

    srv.stop();
    st.join();
    logging.shutdown();

    // At minimum a final completion event must be present
    CHECK(collected_events.find("games_processed") != std::string::npos);
    CHECK(collected_events.find("event: complete") != std::string::npos);
    CHECK(collected_events.find("committed") != std::string::npos);
}
```

**Test: DELETE triggers cancellation** (port 18104)
```cpp
TEST_CASE("server: import DELETE requests cancellation", "[motif-http]")
{
    auto const tdir = tmp_dir {"import_delete"};
    auto db = motif::db::database_manager::create(tdir.path / "db", "import-del");
    REQUIRE(db.has_value());

    auto const pgn_path = write_pgn_fixture(tdir.path, three_game_pgn_content);
    auto logging = import_logging_scope {tdir.path / "logs"};

    constexpr std::uint16_t test_port {18104};
    motif::http::server srv {*db};
    std::thread st {[&]() -> void { [[maybe_unused]] auto r = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};

    auto const post_res = cli.Post("/api/imports",
        R"({"path":")" + pgn_path.string() + R"("})", "application/json");
    REQUIRE(post_res != nullptr);
    REQUIRE(post_res->status == 202);

    // Extract import_id (same extraction as above)
    auto const& body = post_res->body;
    auto const id_start = body.find(R"("import_id":")" ) + 13;
    auto const id_end   = body.find('"', id_start);
    auto const import_id = body.substr(id_start, id_end - id_start);

    auto const del_res = cli.Delete("/api/imports/" + import_id);
    REQUIRE(del_res != nullptr);
    CHECK(del_res->status == 200);
    CHECK(del_res->body.find(R"("cancellation_requested")") != std::string::npos);

    std::this_thread::sleep_for(std::chrono::milliseconds {200});
    srv.stop();
    st.join();
    logging.shutdown();
}
```

**Note on import_id extraction in tests**: The `body.find(R"("import_id":")")` approach is fragile. If glaze inserts spaces, it will break. If this is a problem, use a minimal JSON parse helper or a `contains` check only for AC verification.

### CORS and SSE Headers

The existing CORS post-routing handler sets `Access-Control-Allow-Origin: *` on all responses, including SSE. That's fine. However, SSE clients also expect:
- `Cache-Control: no-cache`
- `Connection: keep-alive`

Consider adding these in the SSE route handler before `set_chunked_content_provider`:
```cpp
res.set_header("Cache-Control", "no-cache");
res.set_header("X-Accel-Buffering", "no"); // disable proxy buffering
```

### Concurrency and Database Safety

The import pipeline manages its own internal synchronization (SQLite WAL + DuckDB single-writer via taskflow). The HTTP `database_mutex` guards position-search and opening-stats reads only. The import worker thread does **not** need to acquire `database_mutex` — doing so would deadlock if a read query is in flight.

Concurrent reads (position search, opening stats) and concurrent writes (import) are safe because:
- SQLite WAL allows concurrent readers + one writer
- DuckDB `rebuild_position_store` is called at end of import, after taskflow stops — no overlap with read queries

However: do **not** run two imports concurrently against the same database. For MVP, the server does not enforce this; a second `POST /api/imports` will start a second pipeline. This is acceptable for a single-user local server but should be documented as a known limitation.

### Includes to Add to `server.cpp`

```cpp
#include <atomic>
#include <filesystem>
#include <fmt/format.h>
#include <glaze/json/read.hpp>
#include <random>
#include <thread>
#include <unordered_map>
#include "motif/import/import_pipeline.hpp"
```

Remove duplicates if already present. Keep include order: stdlib → third-party → project.

### Files to Modify

| File | Change |
|------|--------|
| `source/motif/import/import_pipeline.hpp` | Add `stop_requested_` atomic + `request_stop()` declaration |
| `source/motif/import/import_pipeline.cpp` | Implement `request_stop()`; add stop-flag check in batch loop |
| `source/motif/http/server.cpp` | Add import session struct, session map in impl, UUID generator, POST/GET SSE/DELETE routes; add `http_accepted`, `http_not_found` constants; refactor `register_routes` signature |
| `test/source/motif_http/http_server_test.cpp` | Add `three_game_pgn_content` constant, `write_pgn_fixture` helper, 4 new `TEST_CASE` blocks |
| `source/motif/http/CMakeLists.txt` | Link `motif_import` into `motif_http` for server executable import endpoint symbols |

No new files.

### Carry-forwards from 4b.3

- `[[maybe_unused]] auto const err = glz::write_json(...)` — suppress unused result
- `set_json_error(res, status, message)` — reuse existing helper
- pImpl keeps httplib out of `server.hpp` — do NOT include `<httplib.h>` in `server.hpp`
- Trailing return types on all lambdas `-> void`
- `NOLINTBEGIN/NOLINTEND` around any new `from_chars` blocks
- `invalid_hash_handler` pattern is defined within `register_routes` — new routes can define similar local lambdas

### Known Limitation (deferred)

- The HTTP layer enforces one active import at a time. Completed sessions remain addressable for progress/final-summary lookup.
- Sessions are never pruned from the map — for a long-running server with many imports this leaks memory. Acceptable for MVP.
- The worker thread write to `session->summary` before `done.store()` has no mutex. This is safe by the `memory_order_release/acquire` pair on `done`: any load that sees `done == true` will also see the completed `summary` write (release-acquire barrier).

## Dev Agent Record

### Agent Model Used

claude-sonnet-4-5

### Debug Log References

None — implementation was clean on first approach after fixing:
- `server::impl` private-member access: solved by moving route registration into `server::impl::setup_routes()` member function instead of a free `register_routes` function
- clang-tidy warnings: const scoped_lock, short variable names, magic numbers, internal-linkage — all resolved

### Completion Notes List

- `request_stop()` added to `import_pipeline` with `stop_requested_` atomic; stop check now also prevents new reads, preserves the resume checkpoint on cancellation, and resets state on each run
- `server.cpp` restructured: `import_session` and DTOs in `motif::http::detail`; `server::impl` gains `sessions_mutex` + `sessions` map; all route registration moved to `server::impl::setup_routes()` to preserve private-nested-type access
- `generate_import_id()` produces 32-char lowercase hex using `mt19937_64` + `fmt::format` with an internal mutex for thread safety
- SSE content provider polls `pipeline->progress()`, writes progress event, checks `done` flag (release-acquire pair with worker), sends final `event: complete` event or `event: error`, and terminates stream
- Import workers are owned by `server::impl` as joinable `std::jthread`s; server teardown requests stop before joining so workers cannot outlive the server object
- HTTP POST rejects a second active import with 409 and validates PGN readability with non-throwing filesystem checks plus an open test
- Review fixes added coverage for unreadable files, single-active-import rejection, full SSE field assertions, and cancellation checkpoint preservation
- Full dev test suite: 154/154 passing; perf tests skipped in dev build when corpus/release gate is unavailable

### File List

- `source/motif/import/import_pipeline.hpp`
- `source/motif/import/import_pipeline.cpp`
- `source/motif/http/CMakeLists.txt`
- `source/motif/http/server.cpp`
- `source/motif/http/main.cpp`
- `test/source/motif_http/http_server_test.cpp`

### Change Log

- 2026-04-25: Implemented story 4b-4 — import trigger, SSE progress stream, cancellation endpoint
- 2026-04-25: Code review fixes — single active import, joined worker ownership, checkpoint-preserving cancellation, stronger validation and tests
