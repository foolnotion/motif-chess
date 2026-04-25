# Story 4b.1: HTTP Server Scaffold

Status: done

## Story

As a developer,
I want an HTTP server scaffold using cpp-httplib with CORS support, database configuration at startup, and a health endpoint,
So that all subsequent endpoint stories (4b.2–4b.6) have a running server with common infrastructure ready.

## Acceptance Criteria

1. **Given** the `motif_http` module is configured and a valid DB path is provided
   **when** the server starts
   **then** it listens on a configured port (default: 8080) and exposes a `GET /health` endpoint returning HTTP 200 with body `{"status":"ok"}`

2. **Given** a request to any endpoint
   **when** the response is sent
   **then** CORS headers `Access-Control-Allow-Origin: *`, `Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS`, and `Access-Control-Allow-Headers: Content-Type` are present on the response

3. **Given** the server executable is started with `--db <path>` CLI argument **or** `MOTIF_DB_PATH` environment variable
   **when** the server starts
   **then** it opens or creates a database bundle at that path before accepting connections

4. **Given** the server executable is started with no `--db` argument and no `MOTIF_DB_PATH` set
   **when** the server starts
   **then** it exits with a non-zero status code and prints a clear error message to stderr

5. **Given** all changes are implemented
   **when** `cmake --preset=dev && cmake --build build/dev` is run
   **then** it succeeds with zero clang-tidy and cppcheck warnings

6. **Given** all changes are implemented
   **when** `ctest --test-dir build/dev` is run
   **then** all tests pass including `motif_http_test` which verifies the health endpoint returns HTTP 200 with `{"status":"ok"}` and CORS headers are present

## Tasks / Subtasks

- [x] Task 0: Add cpp-httplib dependency — **PREREQUISITE: requires Bogdan's explicit approval to modify flake.nix** (CLAUDE.md rule)
  - [x] Get explicit approval from Bogdan to add `cpp-httplib` to `flake.nix`
  - [x] Add `httplib` to `flake.nix` under `buildInputs` alongside other deps (nixpkgs name: `httplib`)
  - [ ] Add `{ "name": "cpp-httplib" }` to `vcpkg.json` `"dependencies"` array (deferred to end-of-epic per `CONVENTIONS.md`)

- [x] Task 1: Create `motif_http` module skeleton
  - [x] Create `source/motif/http/` directory with `CMakeLists.txt`
  - [x] Add `error.hpp` — `motif::http::error_code` enum
  - [x] Add `add_subdirectory(source/motif/http)` to root `CMakeLists.txt` (after existing modules)

- [x] Task 2: Implement HTTP server class (AC: 1, 2)
  - [x] Create `source/motif/http/server.hpp` with `motif::http::server` public API
  - [x] Create `source/motif/http/server.cpp` implementing `server::start`, `server::stop`, `server::is_running`
  - [x] Implement universal CORS via `set_post_routing_handler` + `Options(".*", ...)` handler
  - [x] Implement `GET /health` route serializing `health_response` struct via glaze

- [x] Task 3: Server executable entry point (AC: 3, 4)
  - [x] Create `source/motif/http/main.cpp` with CLI arg parsing (`--db`, `--port`)
  - [x] Implement `MOTIF_DB_PATH` environment variable fallback
  - [x] Open or create database bundle via `database_manager::open` / `database_manager::create`
  - [x] Start server on configured port; block until SIGINT/SIGTERM

- [x] Task 4: Tests (AC: 1, 2, 6)
  - [x] Create `test/source/motif_http/http_server_test.cpp`
  - [x] Add `motif_http_test` executable target to `test/CMakeLists.txt`
  - [x] Cover: health endpoint returns 200, body is `{"status":"ok"}`, CORS `Access-Control-Allow-Origin: *` header present

### Review Findings

- [x] [Review][Decision] Mid-story `vcpkg.json` dependency update conflicts with project packaging workflow — resolved by choosing end-of-epic Windows alignment; `vcpkg.json` change reverted for this story.
- [x] [Review][Patch] HTTP module files are tracked while CMake references them [CMakeLists.txt:36]
- [x] [Review][Patch] Story review-state gating fixed after making implementation reproducible from tracked files [_bmad-output/implementation-artifacts/sprint-status.yaml:78]
- [x] [Review][Defer] Baseline dev build reports pre-existing clang-tidy/cppcheck warnings outside this story scope [source/motif/db/database_manager.cpp:444] — deferred, pre-existing

## Dev Notes

### CRITICAL BLOCKER — cpp-httplib Dependency

cpp-httplib is **not** in `flake.nix` or `vcpkg.json`. Per CLAUDE.md, modifying `flake.nix` requires **explicit user approval** — do NOT touch `flake.nix` without that approval. Both files must be updated together when the dependency is added.

- **Nix package name:** `cpp-httplib` (available in nixpkgs)
- **vcpkg port name:** `cpp-httplib`
- **CMake `find_package` name:** `httplib`
- **CMake target:** `httplib::httplib`
- cpp-httplib is header-only; the CMake target provides include paths only

Add to `flake.nix` `buildInputs` list (example, after `duckdb`):
```
cpp-httplib
```

Add to `vcpkg.json` `"dependencies"` array:
```json
{ "name": "cpp-httplib" }
```

### Module Structure

New files to create:
```
source/motif/http/
├── CMakeLists.txt           (defines motif_http static lib + motif_http_server executable)
├── error.hpp                (motif::http::error_code enum)
├── server.hpp               (motif::http::server public API)
├── server.cpp               (implementation)
└── main.cpp                 (server executable entry point)
test/source/motif_http/
└── http_server_test.cpp
```

Files to modify:
- `CMakeLists.txt` (root) — add `add_subdirectory(source/motif/http)`
- `test/CMakeLists.txt` — add `motif_http_test` executable
- `flake.nix` — add `cpp-httplib` (requires explicit approval)
- `vcpkg.json` — add `cpp-httplib`

### CMake Layout

`source/motif/http/CMakeLists.txt`:
```cmake
find_package(httplib REQUIRED)
find_package(glaze REQUIRED)
find_package(tl-expected REQUIRED)
find_package(spdlog REQUIRED)

add_library(motif_http STATIC
    server.cpp
)

target_include_directories(
    motif_http PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/source>"
)

target_link_libraries(motif_http PUBLIC
    httplib::httplib
    tl::expected
    spdlog::spdlog
    motif_db
)
target_compile_features(motif_http PUBLIC cxx_std_23)

# Server executable
add_executable(motif_http_server main.cpp)
target_link_libraries(motif_http_server PRIVATE motif_http)
target_compile_features(motif_http_server PRIVATE cxx_std_23)
```

Root `CMakeLists.txt` — add after `add_subdirectory(source/motif/engine)`:
```cmake
add_subdirectory(source/motif/http)
```

`test/CMakeLists.txt` — add new test executable:
```cmake
add_executable(motif_http_test
    source/motif_http/http_server_test.cpp
)
target_link_libraries(motif_http_test PRIVATE motif_http motif_import Catch2::Catch2WithMain)
target_compile_features(motif_http_test PRIVATE cxx_std_23)
target_include_directories(motif_http_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)
catch_discover_tests(motif_http_test)
```

### Error Handling

`source/motif/http/error.hpp`:
```cpp
#pragma once

namespace motif::http {
enum class error_code { ok, db_open_failed, listen_failed };
}
```

All `server` public API functions return `tl::expected<T, motif::http::error_code>`. Follow the Tier-1 / Tier-2 two-tier model from the architecture (error_code for the return type, `error_info` only for logging).

### Server Public API

`source/motif/http/server.hpp`:
```cpp
#pragma once

#include <cstdint>
#include <memory>
#include "tl/expected.hpp"
#include "motif/http/error.hpp"

namespace motif::db { class database_manager; }

namespace motif::http {

class server {
public:
    explicit server(motif::db::database_manager& db);
    ~server();

    // Starts listening on `port`. Blocks until stop() is called.
    [[nodiscard]] auto start(std::uint16_t port = 8080) -> result<void>;

    // Signals the server to stop; thread-safe.
    auto stop() -> void;

    [[nodiscard]] auto is_running() const -> bool;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

template<typename T>
using result = tl::expected<T, error_code>;

} // namespace motif::http
```

Use the pImpl idiom (`struct impl`) to keep `httplib.h` out of `server.hpp`. This is important because `httplib.h` is large and pulls in network headers — keeping it in the `.cpp` reduces compile times and prevents leaking platform headers through `server.hpp`.

### CORS Implementation

In `server.cpp`, after constructing `httplib::Server`:

```cpp
// Universal CORS — development mode
svr_.set_post_routing_handler([](const httplib::Request&, httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type");
});

// Handle OPTIONS preflight requests
svr_.Options(".*", [](const httplib::Request&, httplib::Response& res) {
    res.status = 200;
});
```

The `set_post_routing_handler` runs after every matched route handler, so CORS headers are added unconditionally. This satisfies AC2 without duplicating the header logic in every route handler.

### Health Endpoint — glaze Serialization

Use glaze (already in the project) for the health JSON response — do NOT reach for nlohmann or manual string construction.

In `server.cpp`:
```cpp
#include "glaze/glaze.hpp"

namespace {
struct health_response {
    std::string status {"ok"};
};
} // anonymous namespace

// In route registration:
svr_.Get("/health", [](const httplib::Request&, httplib::Response& res) {
    std::string body{};
    glaze::write_json(health_response{}, body);
    res.set_content(body, "application/json");
    res.status = 200;
});
```

`glaze::write_json` with `lower_snake_case` field names maps directly to `{"status":"ok"}` without aliases.

### Server Entry Point (`main.cpp`)

CLI parsing logic (no third-party CLI library needed for this story — manual argv scan is sufficient):

```cpp
// Priority: --db arg > MOTIF_DB_PATH env var
// Port: --port arg > MOTIF_HTTP_PORT env var > 8080

// On missing DB path: print to stderr, return EXIT_FAILURE

// Open or create DB bundle:
auto db_result = database_manager::open(db_path);
if (!db_result) {
    db_result = database_manager::create(db_path);
}
// On failure: log via spdlog, return EXIT_FAILURE

// Initialize spdlog logger ("motif.http") before starting server
```

Initialization order:
1. Parse CLI args / env vars
2. Validate db_path is provided; exit if not
3. Initialize spdlog logger for `motif.http`
4. Open or create DB bundle
5. Construct `motif::http::server` with the DB
6. Call `server.start(port)` (blocks until SIGINT/SIGTERM via httplib's built-in signal handling)

### Testing Approach

cpp-httplib includes an `httplib::Client` that can be used directly in tests without spawning a subprocess. Start the server in a background `std::thread`, poll `is_running()` with a short timeout to confirm readiness, make requests with the client, then stop and join.

**Important:** Use an ephemeral/high port for tests (e.g., 18080) to avoid port conflicts with a running dev server. If 18080 is in use, the test should pick the next available port or use port 0 (system-assigned). For simplicity in Story 4b.1, using 18080 with a `REQUIRE(res != nullptr)` guard is acceptable.

Test template:
```cpp
#include <thread>
#include <chrono>
#include "httplib.h"
#include "motif/http/server.hpp"

// Helper: create a temp database_manager
// (mirror the pattern from motif_db tests: use database_manager::create on a tmp path)

TEST_CASE("server: health endpoint returns 200 with ok status", "[motif-http]") {
    auto tmp = std::filesystem::temp_directory_path() / "motif_http_test_XXXX";
    // ... create tmp dir ...
    auto db_res = motif::db::database_manager::create(tmp);
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port = 18080;
    motif::http::server srv{*db_res};

    std::thread server_thread{[&]{ srv.start(test_port); }};

    // Wait for server to be ready
    for (int i = 0; i < 20 && !srv.is_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(srv.is_running());

    httplib::Client cli{"localhost", test_port};
    auto res = cli.Get("/health");
    REQUIRE(res != nullptr);
    CHECK(res->status == 200);
    CHECK(res->body == R"({"status":"ok"})");

    // Check CORS header
    auto cors_header = res->get_header_value("Access-Control-Allow-Origin");
    CHECK(cors_header == "*");

    srv.stop();
    server_thread.join();
}
```

`test/source/motif_http/` does NOT need a `test_helpers.hpp` include for this story — the db_manager fixture is straightforward without the perf/sample helpers from motif_search tests.

### Namespace & Conventions

- Namespace: `motif::http` (matches CMake target name `motif_http`)
- All identifiers `lower_snake_case` including HTTP handler lambdas
- `#pragma once` in every header
- No `using namespace` in any header
- Logger name: `"motif.http"` (matches `motif.<module>` pattern from P5)

### Architecture Compliance

- `motif_http` is Qt-free — no `#include <Q*>` anywhere
- `motif_http` → `motif_db` dependency only for Story 4b.1 (later stories will add `motif_search` and `motif_import`)
- httplib stays in `server.cpp` via pImpl — no httplib headers leak into `server.hpp`
- All module boundaries respected — no direct SQLite or DuckDB access in `motif_http`
- Epic 4b architectural note confirms: "No new ARs — Epic 4b is a thin HTTP adapter over existing stable APIs"

### Epic 4b Pre-conditions (Context)

The Epic 3 retrospective listed these as pre-conditions before Epic 4b starts:
1. **Triage deferred items** — close or promote before Epic 4b starts (tracked in `deferred-work.md`)
2. **Document `dominant_eco` tie-break rule in `opening_stats.hpp`** — Story 4b.1 does not use `opening_stats`, but Stories 4b.3 onwards depend on it. These are pre-conditions for the epic, not a blocker for this scaffold story.

### Previous Story Context (Epic 3)

Story 4b.1 is the first story of a new epic and module. No direct carry-forwards from Epic 3 code patterns — the motif_http module is a fresh addition. However, carry these project-wide conventions:
- `tl::expected<T, error_code>` for all public API return types
- Two-tier error model: `error_code` enum (Tier 1) + `error_info` for logging only (Tier 2)
- `lower_snake_case` everywhere
- `#pragma once` in all headers
- Anonymous namespaces for TU-local helpers in `.cpp` files
- Every serializable struct needs a round-trip test (but `health_response` is a one-way write in this story — no round-trip test required)

### Git Context

Recent commits show:
- Feature branch + PR workflow is the established pattern (Epic 3 retro action item #1)
- Conventional commit prefix: `feat:` for new features, `chore:` for dependency/CMake changes
- This story introduces a new module → branch name: `feat/story-4b-1-http-server-scaffold`

## Dev Agent Record

### Agent Model Used

claude-sonnet-4-6

### Debug Log References

- Obtained explicit approval from Bogdan to modify flake.nix (nixpkgs package name is `httplib`, not `cpp-httplib`)
- cpp-httplib v0.30.2 resolved from nixpkgs; CMake target `httplib::httplib`; header `<httplib.h>`
- OPENSSL support compiled in via nixpkgs httplib package (CPPHTTPLIB_OPENSSL_SUPPORT=1)
- glaze reflection error: `health_response` in anonymous namespace has no external linkage — moved to `motif::http::detail` named namespace
- pImpl idiom (`server::impl` struct) successfully hides `httplib.h` from the public `server.hpp`
- `spdlog::spdlog` removed from explicit motif_http link libs — propagates transitively via motif_db PUBLIC link
- CORS via `set_post_routing_handler` + `Options(".*")` — headers applied to all responses including preflight
- Three tests all pass: health 200, CORS headers, OPTIONS preflight
- 129/129 tests pass in dev build, 130/130 in dev-sanitize build (0 regressions, 0 ASan/UBSan violations)

### Completion Notes List

- `motif_http` static library created: `server.hpp` (pImpl, httplib-free header), `server.cpp` (httplib::Server with CORS + /health), `error.hpp`
- `motif_http_server` executable: `main.cpp` with `--db`/`--port` CLI args and `MOTIF_DB_PATH`/`MOTIF_HTTP_PORT` env var fallbacks; `database_manager::open` then `::create` pattern
- Server uses glaze (`glz::write_json`) for `{"status":"ok"}` health response — no manual string construction
- All lambdas use trailing return type `-> void` per `.clang-tidy` `modernize-use-trailing-return-type` rule
- `parse_port` helper uses `std::from_chars` (exception-free, avoids empty catch)
- Test fixture: RAII `tmp_dir`, `wait_for_ready` poll loop (40 × 5ms), three distinct ports (18080–18082)
- Zero clang-tidy warnings in new files; zero ASan/UBSan violations

### File List

- `flake.nix` — added `httplib` to buildInputs
- `vcpkg.json` — added `cpp-httplib` dependency
- `source/motif/http/CMakeLists.txt` — new file: motif_http library + motif_http_server executable
- `source/motif/http/error.hpp` — new file: motif::http::error_code enum + result alias
- `source/motif/http/server.hpp` — new file: server class public API (pImpl, default_port constant)
- `source/motif/http/server.cpp` — new file: httplib::Server with CORS middleware + /health route
- `source/motif/http/main.cpp` — new file: server executable entry point
- `test/source/motif_http/http_server_test.cpp` — new file: 3 test cases (health, CORS, OPTIONS)
- `CMakeLists.txt` — added add_subdirectory(source/motif/http)
- `test/CMakeLists.txt` — added motif_http_test executable target

## Change Log

- 2026-04-25: Story 4b.1 created — HTTP server scaffold using cpp-httplib, CORS, health endpoint, new motif_http module
- 2026-04-25: Story 4b.1 implemented — motif_http library + motif_http_server executable; 3 new tests; 129/129 dev + 130/130 sanitizer pass
