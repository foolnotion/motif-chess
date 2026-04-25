# Story 4b.5: Game List Endpoint

Status: done

<!-- Ultimate context engine analysis completed - comprehensive developer guide created -->

## Story

As a user,
I want to browse the list of games in the database via HTTP with optional filtering,
So that I can find specific games from any frontend.

## Acceptance Criteria

1. **Given** the server is running and a populated database is loaded
   **when** `GET /api/games` is called
   **then** HTTP 200 is returned with a paginated JSON array of games
   **and** each item contains `id`, `white`, `black`, `result`, `event`, `date`, and `eco` (FR26)

2. **Given** query parameters `?player=<name>&result=<result>`
   **when** `GET /api/games?player=Carlsen&result=1-0` is called
   **then** the list is filtered by player-name substring matching either White or Black and exact result value (FR26)

3. **Given** a large database
   **when** `GET /api/games?offset=100&limit=50` is called
   **then** pagination is applied in the SQLite query
   **and** `limit` defaults to 50
   **and** `limit` is clamped to a maximum of 200
   **and** `offset` defaults to 0

4. **Given** invalid pagination values
   **when** `limit` or `offset` is non-numeric, negative, or empty when present
   **then** HTTP 400 is returned with `{"error":"invalid pagination parameters"}` (NFR10)

5. **Given** the database contains no games or filters match no games
   **when** `GET /api/games` is called
   **then** HTTP 200 is returned with an empty JSON array `[]`

6. **Given** all changes are implemented
   **when** `cmake --preset=dev && cmake --build build/dev` is run
   **then** it succeeds with zero clang-tidy and cppcheck warnings

7. **Given** all changes are implemented
   **when** `ctest --test-dir build/dev` is run
   **then** all tests pass including new tests covering AC 1-5

## Tasks / Subtasks

- [x] Task 1: Add DB-level game list support to `motif_db` (AC: 1-3, 5)
  - [x] Add `game_list_entry` and `game_list_query` structs to `source/motif/db/types.hpp`
  - [x] Add `auto list_games(game_list_query const& query) const -> result<std::vector<game_list_entry>>;` to `source/motif/db/game_store.hpp`
  - [x] Implement `game_store::list_games` in `source/motif/db/game_store.cpp` using a prepared SQLite query with bound filter values
  - [x] Join `game` to `player` twice and `event` once; return only summary columns, not move blobs or tags
  - [x] Apply `ORDER BY g.id ASC LIMIT ? OFFSET ?` in SQL for stable, DB-level pagination

- [x] Task 2: Add DB tests for listing behavior (AC: 1-3, 5)
  - [x] Test unfiltered list returns `id`, `white`, `black`, `result`, `event`, `date`, and `eco` from inserted games
  - [x] Test `player` filter matches a substring in either White or Black, e.g. `Carlsen` matches `Magnus Carlsen`
  - [x] Test `result` filter matches exact game result
  - [x] Test combined `player` + `result` filters
  - [x] Test `limit` and `offset` return a stable page ordered by ascending game id
  - [x] Test empty database and unmatched filters return an empty vector

- [x] Task 3: Add `GET /api/games` route in `motif_http` (AC: 1-5)
  - [x] Reuse existing `parse_size` helper in `server.cpp` for `limit` and `offset`
  - [x] Parse optional `player` and `result` query parameters as strings
  - [x] Reject present-but-empty `limit` or `offset` with HTTP 400
  - [x] Default `limit = 50`, clamp `limit` to `200`, default `offset = 0`
  - [x] Call `database.store().list_games(query)` while holding `server::impl::database_mutex`
  - [x] Serialize the returned vector with glaze and return `application/json`

- [x] Task 4: Add HTTP integration tests (AC: 1-5)
  - [x] Test empty DB -> `200 []` (next free port after existing tests)
  - [x] Test populated DB -> `200` array containing required field names
  - [x] Test `player` and `result` filters narrow the list to expected games
  - [x] Test `limit=1&offset=1` returns one item from the second page
  - [x] Test oversized `limit` is clamped to 200 rather than rejected
  - [x] Test invalid pagination (`limit=abc`, `offset=-1`, `limit=`) returns 400

- [x] Task 5: Build + test validation (AC: 6-7)
  - [x] Run `cmake --preset=dev`
  - [x] Run `cmake --build build/dev`
  - [x] Run `ctest --test-dir build/dev`
  - [x] Record results in the Dev Agent Record

## Dev Notes

### Scope Boundary

This story is a thin HTTP adapter plus the smallest missing DB API needed to support it. Do not implement the single-game retrieval endpoint here; Story 4b.6 owns `GET /api/games/{id}` and full metadata + move data. This story returns list rows only.

No new dependencies are needed. `motif_http` already links `motif_db`, `motif_search`, and `motif_import`; this story should not touch `flake.nix` or `vcpkg.json`.

### API Contract

Endpoint:

```http
GET /api/games?player=<name>&result=<result>&offset=<n>&limit=<n>
```

Response item:

```json
{
  "id": 1,
  "white": "Magnus Carlsen",
  "black": "Ian Nepomniachtchi",
  "result": "1-0",
  "event": "WCC 2021",
  "date": "2021.12.03",
  "eco": "C88"
}
```

Use string fields for `event`, `date`, and `eco`; serialize missing optional DB values as empty strings so the required field names are always present. Keep the response as a top-level JSON array, matching the story acceptance criteria and the existing `/api/positions/{hash}` response style.

### DB API Design

Add summary/list DTOs to `source/motif/db/types.hpp`:

```cpp
struct game_list_entry
{
    std::uint32_t id {};
    std::string white;
    std::string black;
    std::string result;
    std::string event;
    std::string date;
    std::string eco;
};

struct game_list_query
{
    std::optional<std::string> player;
    std::optional<std::string> result;
    std::size_t limit {50};
    std::size_t offset {0};
};
```

Add the public method to `source/motif/db/game_store.hpp`:

```cpp
auto list_games(game_list_query const& query) const -> result<std::vector<game_list_entry>>;
```

The method should query SQLite directly through `game_store`; do not go through `get()` for each row. Calling `get()` would pull move blobs and tags for every list row, which is unnecessary and would make large lists slower.

### SQL Guidance

Use a stable order and DB-level pagination:

```sql
SELECT
    g.id,
    w.name,
    b.name,
    g.result,
    COALESCE(e.name, ''),
    COALESCE(g.date, ''),
    COALESCE(g.eco, '')
FROM game g
JOIN player w ON w.id = g.white_id
JOIN player b ON b.id = g.black_id
LEFT JOIN event e ON e.id = g.event_id
WHERE (? IS NULL OR instr(w.name, ?) > 0 OR instr(b.name, ?) > 0)
  AND (? IS NULL OR g.result = ?)
ORDER BY g.id ASC
LIMIT ? OFFSET ?
```

Bind the `player` value to all three player placeholders when present, or bind NULLs when absent. Bind `result` to both result placeholders when present, or bind NULLs when absent. Bind `limit` and `offset` as integers after clamping in the HTTP layer.

Player filtering is substring matching because the AC example uses `player=Carlsen`; this should match a stored player name such as `Magnus Carlsen`. Prefer SQLite `instr(name, ?) > 0` over `LIKE` so user input containing `%` or `_` is treated literally without custom escaping. Keep result filtering exact.

### HTTP Route Guidance

Add the route inside `server::impl::setup_routes()` in `source/motif/http/server.cpp`, near the other `/api/...` GET endpoints. Existing helpers and patterns to reuse:

- `parse_size(std::string_view)` for `limit` and `offset`
- `set_json_error(res, http_bad_request, "invalid pagination parameters")`
- `database_mutex` around shared `database_manager` access
- `glz::write_json` with `[[maybe_unused]] auto const err = ...`

Pagination behavior should be:

```cpp
constexpr std::size_t default_game_list_limit {50};
constexpr std::size_t max_game_list_limit {200};

auto const limit_str = req.get_param_value("limit");
auto const offset_str = req.get_param_value("offset");

std::size_t limit = default_game_list_limit;
std::size_t offset = 0;

if (req.has_param("limit")) {
    auto parsed = parse_size(limit_str);
    if (!parsed) {
        set_json_error(res, http_bad_request, "invalid pagination parameters");
        return;
    }
    limit = std::min(*parsed, max_game_list_limit);
}

if (req.has_param("offset")) {
    auto parsed = parse_size(offset_str);
    if (!parsed) {
        set_json_error(res, http_bad_request, "invalid pagination parameters");
        return;
    }
    offset = *parsed;
}
```

For `limit=0`, return `[]` immediately, mirroring the established `/api/positions` behavior after its review fix. `offset` still defaults to zero and is passed through when limit is nonzero.

### Existing Patterns to Preserve

- `motif_http` remains a thin adapter; it should not include SQLite or DuckDB headers.
- Keep `httplib.h` in `server.cpp` only; do not expose it through `server.hpp`.
- Route lambdas use trailing return types (`-> void`) to satisfy clang-tidy.
- Use `fmt::format` only if string formatting is needed; do not use `std::format`.
- Use `tl::expected` error flow in public APIs; no exceptions across module boundaries.
- SQL in project guidance prefers raw string literals. If using a dynamic SQL builder becomes necessary, keep the fragments simple and do not interpolate user values; bind all user-controlled values.
- All identifiers use `lower_snake_case`; template parameters only use `CamelCase`.

### Testing Guidance

For DB tests, build games using the existing helpers in `test/source/motif_db/game_store_test.cpp` where possible. Insert several games with distinct players, results, dates, ECO codes, and optional event details; then assert list output values directly from `game_store::list_games`.

For HTTP tests, reuse existing `tmp_dir`, `wait_for_ready`, and inserted-game helpers in `test/source/motif_http/http_server_test.cpp`. Prefer counting `"id"` occurrences or parsing minimal JSON only if an existing local helper already does so. Do not introduce a new JSON library.

Use the next available fixed ports after the current HTTP test block. Existing tests already use multiple ports around `18080`-`18104`; inspect the file before choosing new constants to avoid collisions.

### Previous Story Intelligence

Story 4b.4 added import session ownership and cancellation; do not disturb those routes while adding `/api/games`. The current server uses one shared `database_mutex` for DB-backed read endpoints. Use that mutex for `list_games` as well.

Story 4b.3 found that glaze cannot auto-reflect non-aggregate types with user-declared constructors. Keep `game_list_entry` as a simple aggregate with no constructors so `std::vector<game_list_entry>` serializes without manual `glz::meta`.

Story 4b.2 fixed pagination issues after review: stable ordering matters, `limit=0` must not return the full result set, and invalid numeric params must fail closed with HTTP 400. Apply those lessons here from the start.

Story 4b.1 established the pImpl boundary and CORS middleware. Do not include `httplib.h` in public headers or add CORS logic per route.

### Architecture and Project References

- [Source: `_bmad-output/planning-artifacts/epics.md` - Story 4b.5]
- [Source: `_bmad-output/planning-artifacts/epics.md` - FR26 coverage]
- [Source: `_bmad-output/planning-artifacts/architecture.md` - module dependency direction and Qt-free backend rule]
- [Source: `CONVENTIONS.md` - DuckDB C API ban, error handling, module boundaries, SQL, and testing rules]
- [Source: `_bmad-output/project-context.md` - C++/testing/build constraints]
- [Source: `_bmad-output/implementation-artifacts/4b-2-position-search-endpoint.md` - pagination and HTTP validation lessons]
- [Source: `_bmad-output/implementation-artifacts/4b-3-opening-stats-endpoint.md` - glaze serialization and DB mutex lessons]
- [Source: `_bmad-output/implementation-artifacts/4b-4-import-trigger-sse-progress-stream.md` - current server route/session structure]

## Dev Agent Record

### Agent Model Used

GPT-5.5

### Debug Log References

- Red phase: `cmake --build build/dev` failed on missing `motif::db::game_list_query` and `game_store::list_games`, confirming DB tests covered the missing API.
- Red phase: `./build/dev/test/motif_http_test "server: game list*"` failed with 404 responses for `/api/games`, confirming HTTP tests covered the missing route.
- Validation: `./build/dev/test/motif_db_test "game_store: list_games*"` passed, 37 assertions in 4 test cases.
- Validation: `./build/dev/test/motif_http_test "server: game list*"` passed, 255 assertions in 6 test cases.
- Validation: `cmake --preset=dev && cmake --build build/dev` passed.
- Validation: `ctest --test-dir build/dev` passed, 164/164 tests.
- Validation: `cmake --preset=dev-sanitize && cmake --build build/dev-sanitize && ctest --test-dir build/dev-sanitize` passed, 164/164 tests.

### Completion Notes List

- Added DB list DTOs and `game_store::list_games`, using a bound SQLite query with `instr` player substring filtering, exact result filtering, summary-only joins, and stable `ORDER BY g.id ASC LIMIT ? OFFSET ?` pagination.
- Added `GET /api/games` as a thin HTTP adapter with default `limit=50`, max `limit=200`, default `offset=0`, invalid pagination handling, `limit=0 -> []`, DB mutex protection, and glaze JSON array serialization.
- Added DB and HTTP integration coverage for required fields, empty results, combined filters, pagination, limit clamping, and invalid pagination.
- Cleaned story-introduced clang-tidy/include-cleaner warnings; remaining warning output observed during sanitizer build is from pre-existing unrelated files.

### File List

- `_bmad-output/implementation-artifacts/4b-5-game-list-endpoint.md`
- `_bmad-output/implementation-artifacts/sprint-status.yaml`
- `source/motif/db/game_store.cpp`
- `source/motif/db/game_store.hpp`
- `source/motif/db/types.hpp`
- `source/motif/http/server.cpp`
- `test/source/motif_db/game_store_test.cpp`
- `test/source/motif_http/http_server_test.cpp`

### Review Findings

- [x] [Review][Patch] Player filter is case-sensitive: `instr()` matches byte-for-byte — fixed: `instr(lower(w.name), lower(?)) > 0` [game_store.cpp]
- [x] [Review][Patch] DB layer has no upper-bound cap on `entries.reserve(query.limit)` — fixed: `reserve(std::min(query.limit, 256))` + INT64_MAX overflow guard [game_store.cpp]
- [x] [Review][Defer] `game_continuation_context` struct and `get_continuation_contexts` added out of scope — deferred, possible prep work for tree view / story 4b.6 extension; belongs to opening stats feature [types.hpp, game_store.hpp, game_store.cpp]
- [x] [Review][Patch] `default_game_list_limit` exported from `motif_db` public header — fixed: constant moved to `server.cpp`; struct default uses literal with NOLINT [types.hpp, server.cpp]

- [x] [Review][Patch] `sqlite3_bind_int64` return values not checked in `list_games` — fixed: returns `io_failure` on bind error [game_store.cpp]
- [x] [Review][Patch] NULL `g.result` not COALESCEd unlike `event`/`date`/`eco` columns — fixed: `COALESCE(g.result, '')` [game_store.cpp SQL]
- [x] [Review][Patch] `import_logging_scope` destructor silently swallows `shutdown_logging` failure — fixed: `static_cast<void>` with explanatory comment [http_server_test.cpp]
- [x] [Review][Patch] `query.offset`/`query.limit` not guarded before cast to `sqlite3_int64` — fixed: early-return guard for values > INT64_MAX [game_store.cpp]
- [x] [Review][Patch] Empty `player=` query param treated as active filter — fixed: empty string treated as absent [server.cpp]
- [x] [Review][Patch] Empty `result=` query param treated as active filter — fixed: empty string treated as absent [server.cpp]
- [x] [Review][Patch] No HTTP integration test for `limit=0` returning `[]` — fixed: test added at port 18113 [http_server_test.cpp]

- [x] [Review][Defer] `import_workers` vector grows without bound; completed `jthread`s never pruned [server.cpp] — deferred, from story 4b-4
- [x] [Review][Defer] SSE progress handler dereferences `session->pipeline` without null guard [server.cpp] — deferred, from story 4b-4
- [x] [Review][Defer] `memory_order_relaxed` for `failed` flag is fragile — relies on implicit sequencing after acquire load of `done`; should document the ordering proof or upgrade to `acquire` [server.cpp] — deferred, from story 4b-4
- [x] [Review][Defer] `get_continuation_contexts` silently drops entries with zero-length move blob — output vector smaller than input with no per-entry correlation [game_store.cpp] — deferred, out of scope for this story
- [x] [Review][Defer] Hardcoded port numbers in all HTTP tests flake on parallel runs [http_server_test.cpp] — deferred, pre-existing pattern
- [x] [Review][Defer] Non-const `get_continuation_contexts` delegates to const via `const_cast` with no rationale [game_store.cpp] — deferred, out of scope
- [x] [Review][Defer] Import DELETE test does not verify early termination — passes even if `request_stop` is a no-op [http_server_test.cpp] — deferred, from story 4b-4
- [x] [Review][Defer] Dynamic SQL in `get_continuation_contexts` has no guard against `SQLITE_LIMIT_VARIABLE_NUMBER` (300 rows × 3 params = 900; limit is 999) [game_store.cpp] — deferred, out of scope
- [x] [Review][Defer] `sessions` map never evicts completed import sessions — unbounded growth over many import cycles [server.cpp] — deferred, from story 4b-4
- [x] [Review][Defer] SSE chunk provider loops forever if worker thread deadlocks before `done.store` [server.cpp] — deferred, from story 4b-4
- [x] [Review][Defer] TOCTOU: file readability checked before async pipeline opens it — deleted or replaced file causes silent worker failure [server.cpp] — deferred, from story 4b-4

## Change Log

- 2026-04-25: Implemented Story 4b.5 game list endpoint, DB list query API, focused DB/HTTP tests, and validation gates
- 2026-04-25: Story 4b.5 created - game list HTTP endpoint with DB-level filtering and pagination guidance
