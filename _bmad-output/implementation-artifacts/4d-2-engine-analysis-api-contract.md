# Story 4d.2: Engine Analysis API Contract

Status: done

<!-- Ultimate context engine analysis completed - comprehensive developer guide created -->

## Story

As a developer building a browser-based engine analysis panel,
I want a documented HTTP/SSE contract for starting, streaming, and stopping UCI engine analysis,
So that frontend work can proceed against a stable backend boundary before engine implementation details are finalized.

## Acceptance Criteria

1. **Given** the API contract is documented
   **When** a frontend wants to start analysis
   **Then** `POST /api/engine/analyses` accepts a JSON body with: required `fen` (string), optional `engine` (string, defaults to first configured engine), `multipv` (integer 1–5, default 1), and exactly one bounded limit — either `depth` (integer 1–100) or `movetime_ms` (integer 1–300000)
   **And** the response returns HTTP 202 with an opaque string `analysis_id`
   **And** missing FEN, invalid FEN format, invalid multipv range, both/neither limit provided, or invalid limit values return HTTP 400 with a JSON error response

2. **Given** analysis has started
   **When** `GET /api/engine/analyses/{analysis_id}/stream` is called
   **Then** the endpoint produces `text/event-stream` via chunked transfer encoding
   **And** normal `info` events carry: required `depth` (int), optional `seldepth` (int), required `multipv` (int, 1-based), required `score` object (`type`: `"cp"` or `"mate"`, `value`: int), `pv_uci` (array of UCI strings), `pv_san` (array of SAN strings where conversion succeeds, otherwise omitted or empty), and optional `nodes` (int64), `nps` (int), `time_ms` (int)
   **And** a terminal `complete` event is emitted when the engine sends `bestmove`, carrying `best_move_uci` (string) and `ponder_uci` (optional string)
   **And** an `error` event is emitted on engine crash or internal error with a `message` string
   **And** an unknown `analysis_id` returns HTTP 404

3. **Given** analysis is active
   **When** `DELETE /api/engine/analyses/{analysis_id}` is called
   **Then** the contract documents that the backend sends `stop` to the engine and the session transitions to a cancelled terminal state
   **And** HTTP 204 is returned on success
   **And** an unknown `analysis_id` returns HTTP 404
   **And** a session already in a terminal state returns HTTP 409 with a JSON error

4. **Given** module ownership is preserved
   **When** implementation stories are created
   **Then** `motif_engine` uses `ucilib` (`uci::engine`) for subprocess lifecycle
   **And** `motif_http` owns HTTP/SSE session management, JSON/SSE formatting, and `analysis_id` generation
   **And** `chesslib` owns PV conversion to SAN (using `chesslib::san::to_string` from the starting FEN)
   **And** no chess logic, engine protocol, or subprocess management leaks into `motif_http`

5. **Given** local deployment constraints are respected
   **When** the contract is reviewed
   **Then** the API target is `localhost:8080` with no authentication, no multi-tenancy, CORS for the frontend dev origin, and no data leaving the machine
   **And** engine streams are annotated that frontend consumption must use a Web Worker or off-main-thread path

6. **Given** the OpenAPI document is updated
   **When** `docs/api/openapi.yaml` is reviewed
   **Then** all three engine routes, request/response schemas, SSE event shapes, error schemas, and examples are documented
   **And** the schemas include `StartAnalysisRequest`, `StartAnalysisResponse`, `AnalysisInfoEvent`, `AnalysisCompleteEvent`, `AnalysisErrorEvent`, and `ScoreObject`

7. **Given** all changes are implemented
   **When** `cmake --preset=dev && cmake --build build/dev && ctest --test-dir build/dev` is run
   **Then** all existing tests pass with zero new clang-tidy or cppcheck warnings
   **And** zero ASan/UBSan violations under `cmake --preset=dev-sanitize`

## Tasks / Subtasks

- [x] Task 1: Design and document the engine analysis contract in `docs/api/openapi.yaml` (AC: 1, 2, 3, 5, 6)
  - [x] Add `POST /api/engine/analyses` route with `StartAnalysisRequest` body and `StartAnalysisResponse` (HTTP 202) plus 400 error cases
  - [x] Add `GET /api/engine/analyses/{analysis_id}/stream` as `text/event-stream` with `AnalysisInfoEvent`, `AnalysisCompleteEvent`, `AnalysisErrorEvent` documented as SSE event shapes (not JSON response schemas)
  - [x] Add `DELETE /api/engine/analyses/{analysis_id}` with 204 success and 404/409 error cases
  - [x] Define `ScoreObject` component schema with `type` (`"cp"` | `"mate"`) and `value` (int)
  - [x] Include examples: starting analysis from the initial position with `depth: 20`, a sample `info` event with `pv_san`, a `complete` event, an `error` event, a 400 error for missing FEN, and a 409 error for already-cancelled session
  - [x] Add prose notes in the description fields: SSE event ordering guarantee (info* → complete|error), Web Worker consumption requirement, `pv_san` omission rule, and `analysis_id` opacity guarantee

- [x] Task 2: Add `motif_engine` headers — `engine_manager.hpp` and `error.hpp` — as the contract stub (AC: 4)
  - [x] Create `source/motif/engine/engine_manager.hpp` in `namespace motif::engine` declaring `engine_manager` class with: `start_analysis(fen, params) -> tl::expected<std::string, error_code>` (returns opaque `analysis_id`), `stop_analysis(analysis_id) -> tl::expected<void, error_code>`, and `subscribe(analysis_id, info_cb, complete_cb, error_cb) -> tl::expected<subscription, error_code>` for SSE wiring
  - [x] Create or update `source/motif/engine/error.hpp` with `motif::engine::error_code` enum covering at minimum: `analysis_not_found`, `analysis_already_terminal`, `engine_not_configured`, `engine_start_failed`
  - [x] Keep `motif_engine.cpp` as a minimal stub (`// Phase 2 stub`) — do NOT implement the engine logic in this story
  - [x] Do NOT link `ucilib` in `motif_engine`'s `CMakeLists.txt` yet — this story is a contract only; `ucilib` wiring is Phase 2 implementation work
  - [x] Add `motif_engine` to `motif_http`'s CMake link dependencies only if it is not already linked

- [x] Task 3: Add HTTP route stubs in `source/motif/http/server.cpp` returning documented responses (AC: 1, 2, 3)
  - [x] Add `StartAnalysisRequest` and `StartAnalysisResponse` DTOs to `motif::http::detail`
  - [x] Register `POST /api/engine/analyses`: validate body fields (FEN present, multipv 1–5, exactly one of depth/movetime_ms), return HTTP 400 on validation failure, return HTTP 202 with `{"analysis_id": "<opaque-string>"}` stub (the opaque string can be a UUID or random hex — use `<random>` already included, not a new dep)
  - [x] Register `GET /api/engine/analyses/:analysis_id/stream`: return HTTP 501 Not Implemented with JSON error `{"error":"engine analysis not yet implemented"}` — this route must exist and be documented; the SSE body is Phase 2 work
  - [x] Register `DELETE /api/engine/analyses/:analysis_id`: return HTTP 501 Not Implemented with JSON error — same rationale
  - [x] All three routes must appear in the correct order (exact paths before parameterised paths); register engine routes after the legal-moves routes from Story 4d.1 and before any catch-all routes

- [x] Task 4: Add integration tests for the contract surface (AC: 1, 3, 7)
  - [x] Add tests to `test/source/motif_http/http_server_test.cpp` following existing patterns (unused ports, `wait_for_ready`, `httplib::Client`)
  - [x] Test `POST /api/engine/analyses` with a valid body returns HTTP 202 and a non-empty `analysis_id` string
  - [x] Test `POST /api/engine/analyses` with missing `fen` returns HTTP 400
  - [x] Test `POST /api/engine/analyses` with invalid `fen` returns HTTP 400
  - [x] Test `POST /api/engine/analyses` with both `depth` and `movetime_ms` provided returns HTTP 400
  - [x] Test `POST /api/engine/analyses` with neither `depth` nor `movetime_ms` provided returns HTTP 400
  - [x] Test `POST /api/engine/analyses` with `multipv` out of range (0 or 6) returns HTTP 400
  - [x] Test `GET /api/engine/analyses/unknown-id/stream` returns HTTP 404 or HTTP 501 (match what stub returns)
  - [x] Test `DELETE /api/engine/analyses/unknown-id` returns HTTP 404 or HTTP 501 (match what stub returns)
  - [x] Add direct `motif_engine` stub API tests for `start_analysis`, `stop_analysis`, and `subscribe`
  - [x] Do NOT write SSE streaming tests — that is Phase 2 implementation work

- [x] Task 5: Validation (AC: 7)
  - [x] Run `cmake --preset=dev`
  - [x] Run `cmake --build build/dev`
  - [x] Run `ctest --test-dir build/dev`
  - [x] Run `cmake --preset=dev-sanitize`
  - [x] Run `cmake --build build/dev-sanitize`
  - [x] Run `ctest --test-dir build/dev-sanitize`
  - [x] Apply `clang-format` to all touched C++ files and record results in the Dev Agent Record

### Review Findings

- [x] [Review][Patch] Invalid FEN is accepted by the start-analysis route [`source/motif/http/server.cpp`:825]
- [x] [Review][Patch] `StartAnalysisRequest` does not encode the exact-one limit invariant [`docs/api/openapi.yaml`:1005]
- [x] [Review][Patch] `engine_manager.hpp` uses `std::vector` without including `<vector>` [`source/motif/engine/engine_manager.hpp`:3]
- [x] [Review][Patch] Engine event stubs cannot represent omitted SSE fields [`source/motif/engine/engine_manager.hpp`:34]
- [x] [Review][Patch] Public `motif_engine` API stubs have no direct tests [`test/source`:1]

## Dev Notes

### Scope: Contract-Only Story

This is an API **contract** story, not an implementation story. The goal is:
1. A complete OpenAPI specification the frontend team can code against.
2. HTTP route stubs that return the documented status codes (202, 404, 501) so integration tests verify the routes are registered correctly.
3. `motif_engine` header stubs that Phase 2 implementation stories can fill in without breaking the HTTP layer.

**Do not** implement actual engine subprocess management, SSE streaming of engine output, or `ucilib` integration in this story. Stub routes returning HTTP 501 are correct and expected for streaming and deletion endpoints.

### ucilib API Reference

ucilib is already available at `/nix/store/4mg6mh4aclbzykpv7mb9a62yx59f5vzq-ucilib/include/ucilib/`. The engine API for Phase 2 reference:

```cpp
// #include <ucilib/ucilib.hpp>

namespace uci {
  // go_params — exactly one of these should be set for bounded analysis:
  struct go_params {
      std::optional<int> depth;
      std::optional<milliseconds> movetime;
      std::optional<int> multipv_via_set_option; // set via set_option("MultiPV", value)
      // ... others
  };

  struct info {
      std::optional<int> depth;
      std::optional<int> seldepth;
      std::optional<int> multipv;    // 1-based; absent if MultiPV=1
      std::optional<score> score;    // score.type is score_type::cp or score_type::mate
      std::optional<int64_t> nodes;
      std::optional<int> nps;
      std::optional<int> time_ms;
      std::vector<std::string> pv;   // UCI move strings, e.g. ["e2e4", "e7e5"]
  };

  struct best_move {
      std::string move;
      std::optional<std::string> ponder;
  };

  class engine {
  public:
      auto start(std::string const& path) -> tl::expected<engine_id, std::error_code>;
      auto quit() -> tl::expected<void, std::error_code>;
      auto is_ready(milliseconds timeout = milliseconds{5000}) -> tl::expected<void, std::error_code>;
      auto set_option(std::string_view name, std::string_view value = "") -> tl::expected<void, std::error_code>;
      auto set_position(std::string_view fen, std::vector<std::string> const& moves = {}) -> tl::expected<void, std::error_code>;
      auto go(go_params const& params = {}) -> tl::expected<void, std::error_code>;
      auto stop() -> tl::expected<void, std::error_code>;
      void on_info(info_callback cb);         // called on each UCI info line
      void on_bestmove(bestmove_callback cb); // called once when engine sends bestmove
  };
}
```

Key Phase 2 implementation notes (for context when designing the contract):
- `multipv` is set via `set_option("MultiPV", "3")` before `go()`, not as a `go_params` field.
- `info.multipv` is 1-based and only present when MultiPV > 1.
- `info.pv` is a `std::vector<std::string>` of UCI strings from the starting position. PV-to-SAN conversion requires replaying from the starting FEN using `chesslib::move_maker`.
- `engine::on_info` / `engine::on_bestmove` callbacks are called from the engine's reader thread — the HTTP SSE provider must use thread-safe signaling (like the `condition_variable` pattern from the import SSE in `server.cpp`).

### SAN Conversion for PV

`pv_san` is derived by replaying the `pv` UCI move list from the starting FEN. Pattern:

```cpp
// For each move in uci::info::pv:
//   board = chesslib::fen::read(starting_fen).value()
//   for each uci_str in pv:
//     move = chesslib::uci::from_string(board, uci_str).value_or(...)
//     san = chesslib::san::to_string(board, move)
//     chesslib::move_maker{board, move}.make()
//     append san
```

If any UCI string fails to parse (engine bug, truncated PV), truncate `pv_san` at that point rather than emitting a broken array. Omit `pv_san` entirely if conversion fails on the first move.

### Existing HTTP Structure

The route implementation lives entirely in `source/motif/http/server.cpp`. Patterns to preserve:

- `set_json_error(res, status, message)` writes `{"error":"..."}` — use this for all error responses.
- `glz::read_json` / `glz::write_json` for JSON I/O.
- DTOs must be in `motif::http::detail` namespace (glaze reflection requires named namespace).
- `database_mutex` must NOT be acquired by engine routes — engine analysis is independent of the database.
- Route registration order matters: exact/static paths before parameterised paths.
- `POST /api/engine/analyses` is an exact path, safe to register anywhere before the generic catch-all (none exists currently, but maintain the ordering discipline).
- `GET /api/engine/analyses/:analysis_id/stream` is parameterised — register after the exact POST route.

### Existing SSE Pattern

The import SSE in `server.cpp` shows the established pattern for `text/event-stream`:

```cpp
res.set_chunked_content_provider(
    "text/event-stream",
    [session, start_time = std::chrono::steady_clock::now()](size_t /*offset*/, httplib::DataSink& sink) -> bool {
        // ... poll session state with cv.wait_for(lock, sse_poll_interval, ...)
        // write: fmt::format("event: info\ndata: {}\n\n", json_str)
        // write: fmt::format("event: complete\ndata: {}\n\n", json_str)
        // write: fmt::format("event: error\ndata: {}\n\n", error_json)
        // return false to close the stream
    });
```

SSE event format: `event: <name>\ndata: <json>\n\n`. The `event:` line is required when named events are used (not just `data:`). Use named events (`info`, `complete`, `error`) so the frontend can distinguish event types.

### analysis_id Generation

Use a simple random hex string (16 bytes = 32 hex chars). The `<random>` header is already included in `server.cpp`. Example:

```cpp
auto generate_analysis_id() -> std::string {
    std::random_device rd;
    std::mt19937_64 gen{rd()};
    std::uniform_int_distribution<std::uint64_t> dist;
    return fmt::format("{:016x}{:016x}", dist(gen), dist(gen));
}
```

Do not use a UUID library — keep it simple and avoid new dependencies.

### OpenAPI Documentation Structure

The SSE stream response cannot be described as a standard JSON response schema because it is a stream of events. Use an OpenAPI 3.1 pattern:

```yaml
responses:
  '200':
    description: |-
      Server-Sent Events stream. Event types:
        - `info`: analysis update (see AnalysisInfoEvent schema for data field shape)
        - `complete`: engine finished (see AnalysisCompleteEvent)
        - `error`: engine failed (see AnalysisErrorEvent)
      Consume from a Web Worker or equivalent off-main-thread path.
    content:
      text/event-stream:
        schema:
          type: string
          description: Raw SSE stream; see event schemas in components.
```

Define `AnalysisInfoEvent`, `AnalysisCompleteEvent`, `AnalysisErrorEvent` as component schemas that document the `data` field JSON shape, even though they are not directly referenced in the response content type.

### Wire Shapes

Start analysis request:
```json
{"fen": "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", "multipv": 3, "depth": 20}
```

Start analysis response (HTTP 202):
```json
{"analysis_id": "a3f1c2d4e5b6a7c8d9e0f1a2b3c4d5e6"}
```

SSE `info` event data:
```json
{"depth": 12, "seldepth": 18, "multipv": 1, "score": {"type": "cp", "value": 32}, "pv_uci": ["e2e4", "e7e5", "g1f3"], "pv_san": ["e4", "e5", "Nf3"], "nodes": 1234567, "nps": 987654, "time_ms": 1250}
```

SSE `complete` event data:
```json
{"best_move_uci": "e2e4", "ponder_uci": "e7e5"}
```

SSE `error` event data:
```json
{"message": "engine process terminated unexpectedly"}
```

### Architecture Compliance

- `motif_http` is a thin adapter; it must not contain chess logic or engine subprocess management.
- `motif_engine` → `ucilib` only (Phase 2; stub in this story).
- `motif_http` → `motif_engine` (the header dependency for type sharing is acceptable; actual engine calls are Phase 2).
- No Qt headers anywhere in `motif_engine` or `motif_http`.
- All identifiers: `lower_snake_case`.
- All fallible public APIs: `tl::expected<T, E>`.
- All string construction: `fmt::format`. No `std::format`, `std::ostringstream`, `std::to_string`.
- All console output: `fmt::print`. No `std::cout` or `std::cerr`.
- DTOs in `motif::http::detail` namespace (glaze reflection requirement).
- `httplib.h` confined to `server.cpp` — do not leak through any header.
- Do not add new dependencies; do not modify `flake.nix` or `vcpkg.json`.

### Previous Story Intelligence

Story 4d.1 (legal moves and move validation) established:
- `set_json_error(res, status, msg)` for all error responses — use this, do not inline error JSON.
- Route ordering discipline: exact static paths before parameterised catch-alls.
- DTOs in `motif::http::detail` with glaze reflection.
- `database_mutex` is for DB/search operations only — engine endpoints must NOT acquire it.
- JSON DTOs use `std::optional<std::string>` for optional fields; glaze serialises these consistently.
- OpenAPI updates are always in the same story as route implementation.
- Test file: `test/source/motif_http/http_server_test.cpp` using `httplib::Client`, `wait_for_ready`, hardcoded ports, `tmp_dir`.

Epic 4b established SSE streaming via `res.set_chunked_content_provider("text/event-stream", ...)` with a polling loop using `condition_variable` and `sse_poll_interval`. The engine SSE will follow the same pattern.

Epic 4c established: HTTP 409 for "wrong state" transitions (e.g., cancelling an already-cancelled session). Use this for `DELETE` on a terminal session.

### Module Boundary Reminder

The architecture is explicit (see `motif_http → motif_engine` link):

```
motif_http → motif_db, motif_import, motif_search, motif_engine
motif_engine → (ucilib only — Phase 2)
```

`motif_engine` must be in `motif_http`'s CMake link list for the header stub to compile. Check `source/motif/http/CMakeLists.txt` — if `motif_engine` is not linked yet, add it.

### References

- [Source: `_bmad-output/planning-artifacts/epics.md` — Epic 4d and Story 4d.2]
- [Source: `_bmad-output/planning-artifacts/architecture.md` — motif_engine module, GUI↔Backend comms, local deployment constraints]
- [Source: `_bmad-output/implementation-artifacts/4d-1-legal-moves-and-move-validation-endpoints.md` — patterns established in previous story]
- [Source: `CONVENTIONS.md` — fmt, naming, module boundaries, error handling]
- [Source: `source/motif/http/server.cpp` — SSE pattern, route ordering, DTOs, set_json_error, database_mutex]
- [Source: `source/motif/engine/CMakeLists.txt` and `motif_engine.cpp` — current stub state]
- [Source: `docs/api/openapi.yaml` — current OpenAPI organisation to extend]
- [Source: `/nix/store/4mg6mh4aclbzykpv7mb9a62yx59f5vzq-ucilib/include/ucilib/engine.hpp` and `types.hpp` — ucilib API]
- [Source: `test/source/motif_http/http_server_test.cpp` — HTTP integration test patterns]

## Dev Agent Record

### Agent Model Used

Claude Sonnet 4.6

### Debug Log References

- `generate_analysis_id()` added as a standalone free function mirroring `generate_import_id()` — uses the same `static std::mutex` + `mt19937_64` pattern to avoid contention.
- `http_not_implemented` (501) constant added alongside the existing HTTP status constants.
- `engine_manager.hpp` uses standard library-only DTOs (header-only, no chesslib dependency) — Phase 2 will add chesslib for PV→SAN conversion in the implementation layer.
- Code review follow-up: start-analysis now validates FEN with chesslib before issuing an `analysis_id`; OpenAPI encodes the exact-one depth/movetime invariant; optional SSE fields are represented as `std::optional`.
- clang-format applied after review fixes.

### Completion Notes List

- Task 1: Added three engine routes to `docs/api/openapi.yaml` with all required schemas (`StartAnalysisRequest`, `StartAnalysisResponse`, `ScoreObject`, `AnalysisInfoEvent`, `AnalysisCompleteEvent`, `AnalysisErrorEvent`), examples, and prose notes for SSE event ordering, Web Worker requirement, `pv_san` omission rule, and `analysis_id` opacity.
- Task 2: Created `source/motif/engine/error.hpp` with `error_code` enum (4 values) and `source/motif/engine/engine_manager.hpp` with full contract interface. Updated `motif_engine.cpp` with Phase 2 stub implementations. Added `tl-expected` link to `motif_engine` CMakeLists. Added `motif_engine` to `motif_http` PRIVATE link list.
- Task 3: Added `start_analysis_request` / `start_analysis_response` DTOs to `motif::http::detail`. Added `http_not_implemented` constant. Registered all three engine routes with full validation for POST (FEN presence, multipv range 1–5, exactly one limit, limit range bounds) and 501 stubs for GET stream and DELETE. Route ordering: exact POST before parameterised GET/DELETE.
- Task 4: Added 9 integration tests using ports 18130–18138. All test cases use the established `tmp_dir` / `wait_for_ready` / `httplib::Client` pattern. Added `start_analysis_response` DTO to `motif_http_test` namespace for JSON parsing. Added direct `motif_engine` API stub tests.
- Task 5: All 193 tests pass on both `dev` and `dev-sanitize` presets. Zero ASan/UBSan violations. clang-format applied; rebuild clean with no new warnings.

### File List

- `docs/api/openapi.yaml` — added three engine routes and six component schemas
- `source/motif/engine/error.hpp` — new file: `motif::engine::error_code` enum
- `source/motif/engine/engine_manager.hpp` — new file: contract interface for Phase 2
- `source/motif/engine/motif_engine.cpp` — updated: Phase 2 stub implementations
- `source/motif/engine/CMakeLists.txt` — added `tl-expected` find_package and link
- `source/motif/http/CMakeLists.txt` — added `motif_engine` to PRIVATE link list
- `source/motif/http/server.cpp` — added DTOs, `http_not_implemented`, `generate_analysis_id()`, and three engine route registrations
- `test/CMakeLists.txt` — added `motif_engine_test`
- `test/source/motif_engine/engine_manager_test.cpp` — added direct stub API tests
- `test/source/motif_http/http_server_test.cpp` — added `start_analysis_response` DTO and 9 integration test cases

### Change Log

- 2026-04-26: Implemented story 4d-2 — engine analysis API contract. OpenAPI spec extended with POST/GET/DELETE engine routes and all SSE event schemas. `motif_engine` header stubs created. HTTP route stubs registered with full validation. Review findings resolved with FEN validation, exact-one OpenAPI limits, optional event fields, and direct engine stub tests. 193/193 tests pass on dev and dev-sanitize.
