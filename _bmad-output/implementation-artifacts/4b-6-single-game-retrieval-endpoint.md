# Story 4b.6: Single Game Retrieval Endpoint

Status: done

<!-- Ultimate context engine analysis completed - comprehensive developer guide created -->

## Story

As a user,
I want to retrieve a complete game by ID via HTTP,
So that I can view full game metadata and move data from any frontend.

## Acceptance Criteria

1. **Given** the server is running and a populated database is loaded
   **when** `GET /api/games/{id}` is called with a valid existing game ID
   **then** HTTP 200 is returned with the full game payload: `id`, players, event/site/date metadata, result, ECO, extra PGN tags, and encoded move sequence (FR05)

2. **Given** a game ID that does not exist
   **when** `GET /api/games/{id}` is called
   **then** HTTP 404 is returned with `{"error":"not_found"}`

3. **Given** an invalid game ID format
   **when** `GET /api/games/{id}` is called with a non-numeric string, negative number, empty segment, zero, or a value greater than `std::uint32_t::max()`
   **then** HTTP 400 is returned with `{"error":"invalid game id"}` (NFR10)

4. **Given** all changes are implemented
   **when** `cmake --preset=dev && cmake --build build/dev` is run
   **then** it succeeds with zero clang-tidy and cppcheck warnings

5. **Given** all changes are implemented
   **when** `ctest --test-dir build/dev` is run
   **then** all tests pass including new HTTP integration tests covering AC 1-3

## Tasks / Subtasks

- [x] Task 1: Add HTTP response DTOs in `source/motif/http/server.cpp` (AC: 1)
  - [x] Add `game_player_response`, `game_event_response`, `game_tag_response`, and `game_response` in `motif::http::detail`
  - [x] Keep DTOs as simple named-namespace aggregates so glaze can serialize them without manual meta unless the compiler proves otherwise
  - [x] Convert from `motif::db::game` to the DTO in a local helper; do not change `motif::db::game` just for HTTP JSON shape

- [x] Task 2: Add `GET /api/games/:id` route in `motif_http` (AC: 1-3)
  - [x] Register the route inside `server::impl::setup_routes()` in `source/motif/http/server.cpp`, near the existing `GET /api/games` route
  - [x] Register a bare-path guard for `/api/games/` that returns HTTP 400 with `{"error":"invalid game id"}` so an empty ID segment is handled deliberately
  - [x] Parse the `id` path parameter with `std::from_chars`; reject partial parses, negative strings, empty strings, zero, and `> UINT32_MAX`
  - [x] Call `database.store().get(game_id)` while holding `server::impl::database_mutex`
  - [x] Map `motif::db::error_code::not_found` to HTTP 404 with `{"error":"not_found"}`
  - [x] Map other DB errors to HTTP 500 with a clear existing-style JSON error such as `{"error":"game retrieval failed"}`
  - [x] Serialize the DTO with `glz::write_json` and return `application/json`

- [x] Task 3: Add HTTP integration tests in `test/source/motif_http/http_server_test.cpp` (AC: 1-3, 5)
  - [x] Test existing game returns HTTP 200 and includes required field names and inserted values
  - [x] Test response includes encoded moves and extra PGN tags
  - [x] Test unknown positive ID returns HTTP 404 with `{"error":"not_found"}`
  - [x] Test invalid IDs return HTTP 400: `abc`, `-1`, `0`, and `4294967296`
  - [x] Test the bare path `/api/games/` returns HTTP 400 with `{"error":"invalid game id"}`
  - [x] Use fixed ports `18114` onward unless the file has already taken them by implementation time

- [x] Task 4: Build and test validation (AC: 4-5)
  - [x] Run `cmake --preset=dev`
  - [x] Run `cmake --build build/dev`
  - [x] Run `ctest --test-dir build/dev`
  - [x] Record results in the Dev Agent Record

## Dev Notes

### Scope Boundary

This story is a thin HTTP adapter over the existing SQLite-backed `game_store::get` API. Do not add a new DB retrieval API unless the existing `get(std::uint32_t)` cannot satisfy the contract. Do not implement PGN/SAN rendering, move decoding, or board replay here; the acceptance criteria require the stored encoded move sequence.

No new dependencies are needed. Do not touch `flake.nix` or `vcpkg.json`.

### API Contract

Endpoint:

```http
GET /api/games/{id}
```

Recommended response shape:

```json
{
  "id": 1,
  "white": {
    "name": "Magnus Carlsen",
    "elo": 2865,
    "title": "GM",
    "country": "NO"
  },
  "black": {
    "name": "Ian Nepomniachtchi",
    "elo": 2792,
    "title": "GM",
    "country": "RU"
  },
  "event": {
    "name": "WCC 2021",
    "site": "Dubai",
    "date": "2021"
  },
  "date": "2021.12.03",
  "result": "1-0",
  "eco": "C88",
  "tags": [
    {"key": "Opening", "value": "Ruy Lopez"},
    {"key": "Round", "value": "6"}
  ],
  "moves": [4660, 22136]
}
```

`white` and `black` should always be present. `event`, player `elo`/`title`/`country`, `date`, and `eco` may be absent or null depending on how glaze serializes optionals; tests should assert the required field names and representative populated values rather than rely on null-field formatting. Use `tags` rather than exposing `std::vector<std::pair<std::string, std::string>>` directly as `extra_tags`, because pair serialization is a poor public API shape for frontends.

### Existing DB API to Reuse

`source/motif/db/game_store.hpp` already exposes:

```cpp
auto get(std::uint32_t game_id) -> result<game>;
auto get(std::uint32_t game_id) const -> result<game>;
```

The implementation reconstructs players, event metadata, game date/result/ECO, move blob, and `game_tag` key-value pairs. Existing `motif_db` tests already cover the important DB behavior:

- `game_store: get reconstructs original metadata and moves`
- `game_store: get with event reconstructs event details`
- `game_store: get with extra tags round-trips tag key-value pairs`
- `game_store: get on unknown id returns not_found`

That means this story should focus testing effort on HTTP parsing, status-code mapping, JSON contract, and route integration. Add DB tests only if implementation changes DB behavior.

### HTTP Route Guidance

Add this guard and route in `server::impl::setup_routes()` after the existing exact `GET /api/games` route:

```cpp
svr.Get("/api/games/",
        [](httplib::Request const& /*req*/, httplib::Response& res) -> void
        {
            set_json_error(res, http_bad_request, "invalid game id");
        });

svr.Get("/api/games/:id",
        [this](httplib::Request const& req, httplib::Response& res) -> void
        {
            // parse id, call database.store().get(), map errors, serialize response
        });
```

The exact `/api/games` route already handles the list endpoint, so `/api/games/:id` and `/api/games/` must not change list behavior. Keep route code in `server.cpp`; do not expose httplib or route DTOs through `server.hpp`.

ID parsing should mirror the existing `from_chars` style used for Zobrist hashes and query pagination, but parse into a wide unsigned integer first so overflow beyond `std::uint32_t` can be rejected explicitly:

```cpp
auto parse_game_id(std::string_view id_str) -> std::optional<std::uint32_t>
{
    if (id_str.empty()) {
        return std::nullopt;
    }

    std::uint64_t value {};
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    auto const [ptr, ec] = std::from_chars(id_str.data(), id_str.data() + id_str.size(), value);
    if (ec != std::errc {} || ptr != id_str.data() + id_str.size()) {
        return std::nullopt;
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

    if (value == 0 || value > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(value);
}
```

Use the existing `set_json_error(res, status, message)` helper for all error bodies:

- Invalid ID: `set_json_error(res, http_bad_request, "invalid game id")`
- Missing game: `set_json_error(res, http_not_found, "not_found")`
- Other DB failure: `set_json_error(res, http_internal_error, "game retrieval failed")`

### DTO Guidance

Keep response DTOs in `motif::http::detail`, next to `health_response`, `error_response`, and import DTOs:

```cpp
struct game_player_response
{
    std::string name;
    std::optional<std::int32_t> elo;
    std::optional<std::string> title;
    std::optional<std::string> country;
};

struct game_event_response
{
    std::string name;
    std::optional<std::string> site;
    std::optional<std::string> date;
};

struct game_tag_response
{
    std::string key;
    std::string value;
};

struct game_response
{
    std::uint32_t id {};
    game_player_response white;
    game_player_response black;
    std::optional<game_event_response> event;
    std::optional<std::string> date;
    std::string result;
    std::optional<std::string> eco;
    std::vector<game_tag_response> tags;
    std::vector<std::uint16_t> moves;
};
```

Add a local converter helper in the anonymous namespace or `motif::http::detail`:

```cpp
auto to_game_response(std::uint32_t id, motif::db::game const& source) -> detail::game_response;
```

Copy values directly. Preserve move order exactly. Preserve tag order as returned by `game_store::get`; do not sort tags in the HTTP layer unless a failing test shows the DB order is nondeterministic.

### Existing Patterns to Preserve

- `motif_http` remains a thin adapter; it must not include SQLite or DuckDB headers.
- `database_mutex` guards shared `database_manager` access in DB-backed HTTP routes. Use it for `GET /api/games/:id`.
- `httplib.h` stays in `server.cpp`; do not expose it through public headers.
- Route lambdas use trailing return types (`-> void`) to satisfy clang-tidy.
- Use `std::from_chars` for numeric parsing; no exceptions or `std::stoul`.
- Use `glz::write_json` with `[[maybe_unused]] auto const err = ...`, matching existing routes.
- Use `fmt::format` only if formatting is needed; never `std::format`.
- All identifiers use `lower_snake_case`; template parameters only use `CamelCase`.
- SQL belongs in raw string literals when SQL changes are needed, but this story should not need new SQL.

### Testing Guidance

Reuse existing helpers in `test/source/motif_http/http_server_test.cpp`:

- `tmp_dir`
- `wait_for_ready`
- `make_http_player`
- `insert_http_game`
- `count_game_list_ids` only for list tests, not this endpoint

`insert_http_game` currently inserts summary fields and a one-move vector. For this story, either extend it conservatively or add a second local helper that inserts a fuller `motif::db::game` with:

- player ELO/title/country values
- `event_details` with name/site/date
- game `date`, `result`, `eco`
- at least two encoded moves
- `extra_tags`, including an `Opening` tag and one arbitrary tag

The happy-path HTTP test should assert:

- status `200`
- body contains `"id":<inserted_id>`
- body contains `"white"`, `"black"`, `"event"`, `"tags"`, and `"moves"`
- body contains inserted player/event/tag strings
- body contains each inserted encoded move value

Use substring assertions unless there is already a local JSON parsing helper. Do not add a new JSON dependency for tests.

Suggested invalid-ID requests:

```http
GET /api/games/abc
GET /api/games/-1
GET /api/games/0
GET /api/games/4294967296
```

All should return `400` and `{"error":"invalid game id"}`. Unknown but syntactically valid ID, such as `99999`, should return `404` and `{"error":"not_found"}`.

### Previous Story Intelligence

Story 4b.5 added `GET /api/games` and DB-level list support. Do not regress the list endpoint: `/api/games` must continue to return a top-level JSON array, preserve filtering and pagination behavior, and keep `limit=0 -> []`.

Story 4b.5 also found that empty query parameters should be handled deliberately. For this story, path parameters are stricter: an empty or missing ID is invalid, not an alias for list retrieval.

Story 4b.3 found that glaze cannot auto-reflect non-aggregate types with user-declared constructors. Keep new response DTOs as plain aggregates. If glaze cannot reflect `std::optional<game_event_response>` or nested DTOs automatically, add the smallest explicit `glz::meta` specialization in `server.cpp`.

Story 4b.2 fixed pagination and numeric parsing issues after review. Apply the same fail-closed parsing discipline from the start: partial parses, negative strings, overflow, and empty values must return 400.

Story 4b.1 established the pImpl boundary and CORS middleware. Do not include `httplib.h` in public headers or add route-specific CORS logic.

### Architecture and Project References

- [Source: `_bmad-output/planning-artifacts/epics.md` - Story 4b.6]
- [Source: `_bmad-output/planning-artifacts/epics.md` - FR05 coverage]
- [Source: `_bmad-output/planning-artifacts/architecture.md` - module dependency direction and Qt-free backend rule]
- [Source: `CONVENTIONS.md` - error handling, module boundaries, SQL, and testing rules]
- [Source: `_bmad-output/project-context.md` - C++/testing/build constraints]
- [Source: `_bmad-output/implementation-artifacts/4b-5-game-list-endpoint.md` - game-list route and HTTP test patterns]
- [Source: `_bmad-output/implementation-artifacts/4b-3-opening-stats-endpoint.md` - glaze serialization lessons]
- [Source: `_bmad-output/implementation-artifacts/4b-2-position-search-endpoint.md` - numeric parsing and 400 error lessons]

## Dev Agent Record

### Agent Model Used

GPT-5.5

### Debug Log References

- Red: `build/dev/test/motif_http_test "server: single game*"` failed before implementation with HTTP 404 responses.
- Green: `cmake --build build/dev && build/dev/test/motif_http_test "server: single game*"` passed after implementing the route.
- Validation: `ctest --test-dir build/dev -R "server:.*(single game|game list)" --output-on-failure` passed 9/9 game endpoint tests.
- Full dev gate: `cmake --preset=dev && cmake --build build/dev && ctest --test-dir build/dev --output-on-failure` passed 167/167 tests.
- Sanitizer gate: `cmake --preset=dev-sanitize && cmake --build build/dev-sanitize && ctest --test-dir build/dev-sanitize --output-on-failure` passed 167/167 tests.

### Completion Notes List

- Added single-game HTTP DTOs and local conversion from `motif::db::game`, preserving metadata, extra tags, and encoded move order.
- Added `GET /api/games/:id` plus `/api/games/` guard with strict `std::from_chars` parsing and DB error mapping to 400/404/500 JSON responses.
- Added HTTP integration coverage for complete game payloads, missing IDs, invalid ID formats, overflow, zero, negative IDs, and the bare path guard.

### File List

- `_bmad-output/implementation-artifacts/4b-6-single-game-retrieval-endpoint.md`
- `_bmad-output/implementation-artifacts/sprint-status.yaml`
- `source/motif/http/server.cpp`
- `test/source/motif_http/http_server_test.cpp`

### Review Findings

- [x] [Review][Patch] glz::write_json return value ignored in GET /api/games/:id — serialization failure returns 200 OK with empty body [source/motif/http/server.cpp]
- [x] [Review][Patch] Missing "result" field in AC1 happy path assertion — test never asserts body contains `"result"` key or `"1-0"` value [test/source/motif_http/http_server_test.cpp]
- [x] [Review][Patch] Missing player ELO/title/country assertions in AC1 happy path — insert_detailed_http_game sets elo/title/country but no assertion checks them [test/source/motif_http/http_server_test.cpp]
- [x] [Review][Patch] Second extra tag ("Round"/"6") not asserted — only Opening/Ruy Lopez checked; Round/6 silently untested [test/source/motif_http/http_server_test.cpp]
- [x] [Review][Patch] "eco" JSON key not asserted in happy path — test checks value "C88" but not the field name key [test/source/motif_http/http_server_test.cpp]
- [x] [Review][Patch] "date" JSON key not asserted in happy path — test checks value "2021.12.03" but not the field name key [test/source/motif_http/http_server_test.cpp]
- [x] [Review][Defer] glz::write_json errors ignored in POST /api/imports — pre-existing, deferred in 4b-3 [source/motif/http/server.cpp]
- [x] [Review][Defer] jthread constructor can throw system_error — orphaned session with done=false blocks all future imports [source/motif/http/server.cpp] — deferred, pre-existing (4b-4)
- [x] [Review][Defer] import_session::pgn_path never assigned after construction — dead field, never read [source/motif/http/server.cpp] — deferred, pre-existing (4b-4)
- [x] [Review][Defer] SSE error event embeds error_message in format string without JSON escaping [source/motif/http/server.cpp] — deferred, pre-existing (4b-4)
- [x] [Review][Defer] SSE sleep_for(250ms) blocks httplib thread pool under concurrent requests [source/motif/http/server.cpp] — deferred, pre-existing (4b-4)
- [x] [Review][Defer] error_message/summary thread visibility relies on undocumented release/acquire ordering [source/motif/http/server.cpp] — deferred, pre-existing (4b-4)
- [x] [Review][Defer] cancel_requested atomic never read — dead state, misleading in concurrency struct [source/motif/http/server.cpp] — deferred, pre-existing (4b-4)
- [x] [Review][Defer] import_pipeline constructed before conflict check — wasted allocation on 409 response [source/motif/http/server.cpp] — deferred, pre-existing (4b-4)
- [x] [Review][Defer] import_workers never pruned — already deferred in 4b-5 [source/motif/http/server.cpp]
- [x] [Review][Defer] sessions map grows without bound — already deferred in 4b-5 [source/motif/http/server.cpp]
- [x] [Review][Defer] TOCTOU on file readability check — already deferred in 4b-5 [source/motif/http/server.cpp]
- [x] [Review][Defer] count_game_list_ids substring scan produces false positives — already deferred in 4b-5 [test/source/motif_http/http_server_test.cpp]
- [x] [Review][Defer] game_response::result unvalidated before serialization — design gap, no spec requirement to validate [source/motif/http/server.cpp] — deferred, pre-existing
- [x] [Review][Defer] Destructor calls request_stop inside sessions_mutex while jthreads join implicitly after body — ordering hazard [source/motif/http/server.cpp] — deferred, pre-existing (4b-4)
- [x] [Review][Defer] opening_stats_endpoint_limit set to 500ms (5× the positions budget) with no documented rationale [test/source/motif_http/http_server_test.cpp] — deferred, pre-existing (4b-3)

## Change Log

- 2026-04-25: Story 4b.6 created - single-game retrieval HTTP endpoint with full-game JSON contract and focused test guidance
- 2026-04-25: Implemented single-game retrieval endpoint and HTTP integration tests; story moved to review
