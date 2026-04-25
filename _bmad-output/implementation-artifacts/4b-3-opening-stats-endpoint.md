# Story 4b.3: Opening Statistics Endpoint

Status: done

## Story

As a user,
I want to retrieve opening statistics for a position via HTTP,
So that I can see move frequency and win/draw/loss breakdowns for any position.

## Acceptance Criteria

1. **Given** the server is running and a populated database is loaded
   **when** `GET /api/openings/{zobrist_hash}/stats` is called with a valid hash
   **then** the response is HTTP 200 with a JSON object; it contains a `continuations` array where each element has `san`, `result_hash`, `frequency`, `white_wins`, `draws`, `black_wins`, and optionally `average_white_elo`, `average_black_elo`, `eco`, `opening_name` (FR17)
   **and** the endpoint responds in under 500ms (NFR02)

2. **Given** a Zobrist hash with no matching continuations (position not in DB, or position is terminal)
   **when** `GET /api/openings/{zobrist_hash}/stats` is called
   **then** HTTP 200 is returned with `{"continuations":[]}`

3. **Given** an invalid Zobrist hash (non-numeric string, negative number, overflow)
   **when** `GET /api/openings/{zobrist_hash}/stats` is called
   **then** HTTP 400 is returned with `{"error":"invalid zobrist hash"}` (NFR10)

4. **Given** all changes are implemented
   **when** `cmake --preset=dev && cmake --build build/dev` is run
   **then** it succeeds with zero clang-tidy and cppcheck warnings

5. **Given** all changes are implemented
   **when** `ctest --test-dir build/dev` is run
   **then** all tests pass including new tests covering AC 1–3

## Tasks / Subtasks

- [x] Task 1: Add `opening_stats` route to `server.cpp` (`motif_http`)
  - [x] Add `#include "motif/search/opening_stats.hpp"` to `server.cpp`
  - [x] Register bare-path error handlers for `/api/openings` and `/api/openings/`
  - [x] Implement `GET /api/openings/:zobrist_hash/stats`: parse hash with `from_chars`, call `opening_stats::query`, serialize `stats` result with glaze

- [x] Task 2: Tests (add `TEST_CASE` blocks to `test/source/motif_http/http_server_test.cpp`)
  - [x] Test: invalid hash → 400 + `{"error":"invalid zobrist hash"}` (port 18095)
  - [x] Test: unknown position (empty DB) → 200 + `{"continuations":[]}` (port 18096)
  - [x] Test: populated DB with known position → 200 + non-empty continuations array (port 18097)

- [x] Task 3: Build + test validation — zero warnings, all tests pass

## Dev Notes

### No CMakeLists Changes Needed

`motif_search` was added as a PRIVATE dependency of `motif_http` in story 4b-2. `opening_stats` lives in `motif_search`, so no `CMakeLists.txt` change is required.

### Task 1: Route Handler in `server.cpp`

#### New include (add after existing `motif/search/position_search.hpp` include)
```cpp
#include "motif/search/opening_stats.hpp"
```

#### Bare-path error handlers (add in `register_routes` before the main route)
```cpp
svr.Get("/api/openings", invalid_hash_handler);
svr.Get("/api/openings/", invalid_hash_handler);
```

#### Route handler
```cpp
svr.Get("/api/openings/:zobrist_hash/stats",
    [&dbmgr](httplib::Request const& req, httplib::Response& res) -> void
    {
        auto const& hash_str = req.path_params.at("zobrist_hash");
        std::uint64_t hash_val {};
        {
            // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            auto const [ptr, ec] = std::from_chars(
                hash_str.data(), hash_str.data() + hash_str.size(), hash_val);
            if (ec != std::errc {} || ptr != hash_str.data() + hash_str.size()) {
                set_json_error(res, http_bad_request, "invalid zobrist hash");
                return;
            }
            // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        }

        auto query_result = motif::search::opening_stats::query(dbmgr, hash_val);
        if (!query_result) {
            set_json_error(res, http_internal_error, "stats query failed");
            return;
        }

        std::string body {};
        [[maybe_unused]] auto const err = glz::write_json(*query_result, body);
        res.set_content(body, "application/json");
        res.status = http_ok;
    });
```

No pagination — continuations are bounded by the chess branching factor (~30 legal moves). The full result is always small; no streaming required.

The path `/api/openings/:zobrist_hash/stats` places the path param before a literal `/stats` suffix. cpp-httplib supports this: the `:zobrist_hash` segment captures the hash, `/stats` must match literally.

### Task 2: Testing Approach

Ports 18080–18088 used by existing tests. New tests use 18089–18091.

**Test: invalid hash** (simplest — no DB setup needed beyond creation)
```cpp
TEST_CASE("server: opening stats rejects invalid hash", "[motif-http]")
{
    auto const tdir = tmp_dir{"ostats_badhash"};
    auto db_res = motif::db::database_manager::create(tdir.path, "ostats-bad-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18089};
    motif::http::server srv{*db_res};
    std::thread server_thread{[&]() -> void {
        [[maybe_unused]] auto s = srv.start(test_port);
    }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli{"localhost", test_port};
    auto const res = cli.Get("/api/openings/not-a-hash/stats");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 400);
    CHECK(res->body == R"({"error":"invalid zobrist hash"})");
}
```

**Test: empty continuations (unknown position)**
```cpp
TEST_CASE("server: opening stats returns empty for unknown position", "[motif-http]")
{
    auto const tdir = tmp_dir{"ostats_empty"};
    auto db_res = motif::db::database_manager::create(tdir.path, "ostats-empty-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18090};
    motif::http::server srv{*db_res};
    std::thread server_thread{[&]() -> void {
        [[maybe_unused]] auto s = srv.start(test_port);
    }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli{"localhost", test_port};
    auto const res = cli.Get("/api/openings/99999999999/stats");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 200);
    CHECK(res->body == R"({"continuations":[]})");
}
```

**Test: populated DB (real game data)**

`opening_stats::query` requires a fully populated database: games in SQLite plus DuckDB position/opening_moves tables populated via `rebuild_position_store`. The `seed_positions` helper from 4b-2 tests inserts only into DuckDB directly and does NOT populate game contexts, so it cannot be used here.

Use the `insert_games_and_rebuild` pattern from `opening_stats_test.cpp` in `motif_search_test`. The test needs to:
1. Call `manager.store().insert(game)` to insert a game into SQLite
2. Call `manager.rebuild_position_store()` to populate DuckDB opening_moves
3. Compute the Zobrist hash of the starting position via `chesslib::board{}.hash()`

Helper functions needed in the test file (or inline in the test):
```cpp
// Encode a list of SAN moves to the wire format expected by db::game
auto encode_moves_for_game(std::initializer_list<char const*> sans) -> std::vector<std::uint16_t>
{
    auto board = chesslib::board {};
    auto moves = std::vector<std::uint16_t> {};
    for (char const* const san : sans) {
        auto move = chesslib::san::from_string(board, san);
        REQUIRE(move.has_value());
        moves.push_back(chesslib::codec::encode(*move));
        chesslib::move_maker {board, *move}.make();
    }
    return moves;
}
```

Test seeding:
```cpp
// Insert a game starting from the initial position and rebuild
motif::db::game g {};
g.white = {.name = "Alice", .elo = 1600, .title = std::nullopt, .country = std::nullopt};
g.black = {.name = "Bob",   .elo = 1500, .title = std::nullopt, .country = std::nullopt};
g.result = "1-0";
g.moves  = encode_moves_for_game({"e4", "e5", "Nf3"});

auto inserted = db_res->store().insert(g);
REQUIRE(inserted.has_value());
auto rebuilt = db_res->rebuild_position_store();
REQUIRE(rebuilt.has_value());

// Hash of the starting position (before any moves)
auto const start_hash = chesslib::board{}.hash();
```

The test then hits `/api/openings/{start_hash}/stats` and verifies:
- status == 200
- body contains `"continuations"` key
- body contains `"san"` key (at least one continuation present)

```cpp
REQUIRE(res->status == 200);
CHECK(res->body.find(R"("continuations")") != std::string::npos);
CHECK(res->body.find(R"("san")") != std::string::npos);
```

Note: the test needs `#include <chesslib/board/board.hpp>`, `#include <chesslib/board/move_codec.hpp>`, and `#include <chesslib/util/san.hpp>` added to `http_server_test.cpp` if not already present. Check the existing includes first.

### Glaze Serialization of `opening_stats::stats`

`motif::search::opening_stats::stats` and `motif::search::opening_stats::continuation` are aggregates in the named namespace `motif::search::opening_stats` — glaze-reflectable without any registration.

Fields of `continuation` serialized as JSON:
- `san` → string
- `result_hash` → number (uint64)
- `frequency` → number (uint32)
- `white_wins` → number (uint32)
- `draws` → number (uint32)
- `black_wins` → number (uint32)
- `average_white_elo` → number **or absent** (glaze skips null optionals by default)
- `average_black_elo` → number **or absent**
- `eco` → string **or absent**
- `opening_name` → string **or absent**

Example response — one continuation with full fields:
```json
{"continuations":[{"san":"e4","result_hash":12345678901234567890,"frequency":42,"white_wins":20,"draws":15,"black_wins":7,"average_white_elo":1650.5,"average_black_elo":1620.0,"eco":"B20","opening_name":"Sicilian Defense"}]}
```

Empty result:
```json
{"continuations":[]}
```

### Carry-forwards from 4b.2

- `[[maybe_unused]] auto const err = glz::write_json(...)` — suppress unused result
- `set_json_error(res, status, message)` — reuse existing helper; do not inline error formatting
- pImpl keeps httplib out of `server.hpp` — do NOT include `<httplib.h>` in `server.hpp`
- Trailing return types on all lambdas `-> void`
- `NOLINTBEGIN/NOLINTEND` around multi-line `from_chars` blocks
- `invalid_hash_handler` already defined in `register_routes` scope — reuse for bare-path guards

### Files to Modify

| File | Change |
|------|--------|
| `source/motif/http/server.cpp` | Add `#include "motif/search/opening_stats.hpp"`, add route + bare-path guards |
| `test/source/motif_http/http_server_test.cpp` | Add 3 new `TEST_CASE` blocks; possibly add chesslib includes |

No new files. No `CMakeLists.txt` changes.

## Dev Agent Record

### Agent Model Used

claude-sonnet-4-6

### Debug Log References

- `glz::write_json` failed on `opening_stats::stats` because `stats() = default` makes it a non-aggregate in C++23. Fixed by adding `glz::meta<stats>` specialization in `server.cpp` using `glz::object(&stats::continuations)`.
- Dev notes specified ports 18089–18091 for new tests, but those ports were already used by existing position search pagination tests. Used ports 18095–18097 instead.
- Dev notes specified using `chesslib::board{}.hash()` (starting position) for the populated DB test, but `rebuild_position_store` only stores positions AFTER moves (ply = i+1), so the starting position hash is never in the DB. Fixed by querying from the position AFTER e4, which IS stored.

### Completion Notes List

- Added `GET /api/openings/:zobrist_hash/stats` route to `server.cpp` with full `from_chars` validation, `opening_stats::query` call, and glaze JSON serialization.
- Added bare-path guards for `/api/openings` and `/api/openings/` returning 400.
- Added `glz::meta<motif::search::opening_stats::stats>` specialization to enable glaze reflection on the non-aggregate `stats` struct.
- Added `<glaze/core/common.hpp>` and `<glaze/core/meta.hpp>` includes to satisfy clang-tidy `misc-include-cleaner`.
- Added 3 HTTP integration tests covering AC 1–3; all pass with zero new warnings.
- Full regression suite: 144/144 tests pass.

### File List

- `source/motif/http/server.cpp`
- `test/source/motif_http/http_server_test.cpp`

## Change Log

- 2026-04-25: Story 4b.3 created — opening statistics HTTP endpoint
- 2026-04-25: Implementation complete — route, glaze meta fix, 3 integration tests; all 144 tests pass

## Review Findings

- [x] [Review][Patch] AC1 NFR02 latency requirement (<500ms) has no test — Add a `[performance]` test asserting the opening-stats endpoint responds in under 500ms on a small corpus. `http_server_test.cpp:596-599` exercises position search only.
- [x] [Review][Patch] AC1 test doesn't assert required response fields [`http_server_test.cpp:694-695`] — The populated-DB test only checks `contains("continuations")` and `contains("san")`. Required fields `result_hash`, `frequency`, `white_wins`, `draws`, `black_wins` are unasserted per AC1.
- [x] [Review][Patch] AC3 "negative number" input case untested [`http_server_test.cpp:601-623`] — AC3 explicitly lists "negative number" as an invalid hash category. Only `"not-a-hash"` is tested. Add a test for `GET /api/openings/-1/stats`.
- [x] [Review][Patch] AC3 "overflow" input case untested [`http_server_test.cpp:601-623`] — AC3 explicitly lists "overflow" as an invalid hash category. Add a test for a value exceeding `UINT64_MAX` (e.g. `"18446744073709551616"`).
- [x] [Review][Patch] No explicit `glz::meta` for `continuation` — optional field serialization untested [`server.cpp:24-31`] — Only `stats` has a manual glaze meta. `continuation` relies on auto-reflection. Optional fields (`average_white_elo`, `eco`, etc.) serialization correctness is not validated by any test.
- [x] [Review][Defer] `glz::write_json` error discarded — pre-existing pattern [`server.cpp:199`] — deferred, pre-existing
- [x] [Review][Defer] Server thread may outlive `srv` on `wait_for_ready` failure [`http_server_test.cpp`] — deferred, pre-existing

### Review Findings

- [x] [Review][Patch] AC1/NFR02 latency is not proven and may already be violated — Fixed by adding a batched `game_store::get_continuation_contexts` helper so `opening_stats::query` fetches only the requested continuation move for matching rows instead of full move blobs for every game; also added a dev-suite 500ms assertion to the populated HTTP test.
- [x] [Review][Patch] Serialize DB-backed HTTP handlers with a mutex — Fixed by guarding `/api/positions` and `/api/openings` DB calls with a shared HTTP adapter mutex around the shared `database_manager`.
- [x] [Review][Patch] Opening-stats perf test uses the 100ms position-search threshold instead of the 500ms story target [`test/source/motif_http/http_server_test.cpp:38`, `test/source/motif_http/http_server_test.cpp:328`] — Fixed with a dedicated 500ms opening-stats threshold.
- [x] [Review][Patch] Opening-stats perf test can skip `shutdown_logging()` after a post-init assertion failure [`test/source/motif_http/http_server_test.cpp:259`] — Fixed with an RAII import logging scope that shuts down in its destructor.
