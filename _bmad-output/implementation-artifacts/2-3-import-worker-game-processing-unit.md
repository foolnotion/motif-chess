# Story 2.3: Import Worker — Game Processing Unit

Status: done

## Story

As a developer,
I want a single-game import worker that converts a `pgn::game` into a stored `game` row plus position index entries,
so that the pipeline has a correct, tested unit of work before parallelism is introduced.

## Acceptance Criteria

1. **Given** a valid `pgn::game` struct
   **When** `import_worker::process` is called
   **Then** the game is inserted into the SQLite game store (via `game_store::insert`) with all metadata fields populated
   **And** for each half-move, one row is inserted into the DuckDB `position` table with the correct `zobrist_hash` (from chesslib), `game_id`, `ply`, `result`, `white_elo`, `black_elo` (FR20, AR08)
   **And** SAN→move conversion uses `chesslib` exclusively; motif-chess does not reimplement move parsing (NFR19)

2. **Given** a `pgn::game` where the player name already exists in the database
   **When** `import_worker::process` is called
   **Then** the existing player record is reused — no duplicate player rows (FR03)

3. **Given** a `pgn::game` that is a duplicate of an already-committed game
   **When** `import_worker::process` is called
   **Then** the game is skipped; the function returns `error_code::duplicate` without inserting any rows (FR04)

4. **Given** a `pgn::game` with a `%clk` annotation in move comments
   **When** `import_worker::process` is called
   **Then** the clock data is silently ignored; absence of `%clk` does not cause an error (NFR16)
   **And** all tests pass under `dev-sanitize`

## Tasks / Subtasks

- [x] Add `motif_db` as a dependency of `motif_import` (AC: all)
  - [x] Add `motif_db` to `target_link_libraries` in `source/motif/import/CMakeLists.txt`
  - [x] Add `import_worker.cpp` to `motif_import` STATIC target sources in `source/motif/import/CMakeLists.txt`
  - [x] Verify `cmake --preset=dev && cmake --build build/dev` succeeds with zero new warnings

- [x] Implement `import_worker` class (AC: #1, #2, #3, #4)
  - [x] Create `source/motif/import/import_worker.hpp` — declare `class import_worker` and `struct process_result` (see Dev Notes)
  - [x] Create `source/motif/import/import_worker.cpp` — implement `process()` (see Dev Notes for full algorithm)

- [x] Write tests (AC: #1, #2, #3, #4)
  - [x] Create `test/source/motif_import/import_worker_test.cpp`
  - [x] Test: valid 5-move game — game row inserted, 5 position rows inserted, `process_result.positions_inserted == 5`
  - [x] Test: game with existing player name — player row count unchanged after second insert with same player
  - [x] Test: duplicate game — `process` returns `error_code::duplicate`, no new rows in game table
  - [x] Test: `%clk` in move comments — game inserts successfully, no crash
  - [x] Test: SAN parse failure (illegal/unknown SAN in main line) — `process` returns `error_code::parse_error`, no game row inserted
  - [x] Test: game without Elo tags — `position_row.white_elo` and `black_elo` are null, no crash
  - [x] Test: game with zero moves (header only) — game row inserted, zero position rows, `process_result.positions_inserted == 0`
  - [x] Add `import_worker_test.cpp` to `motif_import_test` in `test/CMakeLists.txt`

- [x] Validate build, tests, sanitizers
  - [x] `cmake --preset=dev && cmake --build build/dev` — clean build, zero new warnings
  - [x] `ctest --test-dir build/dev` — all tests pass
  - [x] `cmake --preset=dev-sanitize && cmake --build build/dev-sanitize`
  - [x] `ctest --test-dir build/dev-sanitize` — zero ASan/UBSan violations

### Review Findings

- [x] [Review][Patch] `parse_elo()` accepts malformed numeric tags like `2400abc` and stores them as valid ratings instead of treating them as missing input. [source/motif/import/import_worker.cpp:58]
- [x] [Review][Patch] `ply` is cast to `std::uint16_t` without a length guard, so games longer than 65535 half-moves will wrap and corrupt stored position ordering. [source/motif/import/import_worker.cpp:200]
- [x] [Review][Patch] AC1 coverage is incomplete: the main success test uses 3 moves instead of the required 5 and does not verify that the SQLite game row or required position fields were populated correctly. [test/source/motif_import/import_worker_test.cpp:67]
- [x] [Review][Patch] AC2 coverage is incomplete: the deduplication test never proves player reuse by checking that the player row count stays unchanged after the second insert. [test/source/motif_import/import_worker_test.cpp:99]
- [x] [Review][Patch] The no-Elo test does not verify the required null storage behavior for `position_row.white_elo` and `position_row.black_elo`. [test/source/motif_import/import_worker_test.cpp:222]

---

## Dev Notes

### CMakeLists Change for `motif_import`

`motif_import` must now link to `motif_db`:

```cmake
# source/motif/import/CMakeLists.txt
find_package(spdlog REQUIRED)
find_package(tl-expected REQUIRED)
find_package(pgnlib REQUIRED)

add_library(motif_import STATIC
    motif_import.cpp logger.cpp pgn_reader.cpp import_worker.cpp)

target_include_directories(
    motif_import PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/source>"
)

target_link_libraries(motif_import PUBLIC
    spdlog::spdlog pgnlib::pgnlib tl::expected motif_db)
```

`chesslib::chesslib` is NOT added separately — it propagates transitively from `motif_db` (which PUBLIC-links it). Include `<chesslib/chesslib.hpp>` in `import_worker.cpp` without a separate `find_package`.

### `import_worker` API (Exact)

```cpp
// source/motif/import/import_worker.hpp
// NOLINTNEXTLINE(portability-avoid-pragma-once)
#pragma once

#include <cstddef>
#include <cstdint>

#include <pgnlib/types.hpp>  // NOLINT(misc-include-cleaner)

#include "motif/db/game_store.hpp"
#include "motif/db/position_store.hpp"
#include "motif/import/error.hpp"

namespace motif::import {

struct process_result {
    std::uint32_t game_id{};
    std::size_t   positions_inserted{};
};

class import_worker {
public:
    explicit import_worker(motif::db::game_store&    store,
                           motif::db::position_store& positions) noexcept;

    // Convert pgn::game to stored game row + DuckDB position rows.
    // Errors:
    //   duplicate   — identity key already in DB; no rows written
    //   parse_error — chesslib rejected a SAN move in the main line; no rows written
    //   io_failure  — a DB write failed
    [[nodiscard]] auto process(pgn::game const& pgn_game) -> result<process_result>;

private:
    motif::db::game_store&     store_;
    motif::db::position_store& positions_;
};

} // namespace motif::import
```

### `process()` Algorithm — Full Implementation Guide

```
1. Convert pgn::game → motif::db::game (see "Tag Extraction" section below)
2. Process main-line moves with chesslib (see "Move Processing Loop" section below)
   — on SAN error: return tl::unexpected(error_code::parse_error) immediately (no DB writes)
3. Call store_.insert(db_game)
   — on error_code::duplicate: propagate as-is
   — on other error: propagate as io_failure
4. Fill game_id into all position_rows collected in step 2
5. Call positions_.insert_batch(position_rows)
   — on failure: return io_failure (SQLite row stands; DuckDB is derived and rebuildable per NFR09)
6. Return process_result{.game_id = game_id, .positions_inserted = position_rows.size()}
```

**Critical ordering:** all SAN processing must complete before any DB write (step 2 before step 3). If any SAN in the main line fails, the entire game is rejected — no partial writes.

### Tag Extraction (pgn::game → motif::db::game)

`pgn::game.tags` is `std::vector<pgn::tag>` where each `tag` has `key` and `value` fields.

**Helper (put in anonymous namespace in .cpp):**
```cpp
auto find_tag(std::vector<pgn::tag> const& tags, std::string_view key) -> std::string {
    for (auto const& tag : tags) {
        if (tag.key == key) return tag.value;
    }
    return {};
}
```

**Elo parsing (anonymous namespace in .cpp):**
```cpp
auto parse_elo(std::string const& raw) -> std::optional<std::int16_t> {
    if (raw.empty() || raw == "?") return std::nullopt;
    try {
        auto const val = std::stoi(raw);
        if (val < 0 || val > 32767) return std::nullopt;
        return static_cast<std::int16_t>(val);
    } catch (std::exception const&) {
        return std::nullopt;
    }
}
```

**Mapping:**

| pgn tag | motif::db::game field |
|---|---|
| `White` | `game.white.name` |
| `Black` | `game.black.name` |
| `WhiteElo` | `game.white.elo` (via `parse_elo`) |
| `BlackElo` | `game.black.elo` (via `parse_elo`) |
| `WhiteTitle` | `game.white.title` (nullopt if empty) |
| `BlackTitle` | `game.black.title` (nullopt if empty) |
| `Event` | `game.event_details = db::event{.name=..., .site=..., .date=...}` |
| `Site` | `game.event_details->site` (nullopt if empty) |
| `Date` | `game.date` (nullopt if empty or `"????.??.??"`) |
| `ECO` | `game.eco` (nullopt if empty) |
| `pgn_game.result` | `game.result` (via `pgn_result_to_string`) |
| all other tags | `game.extra_tags` as `{key, value}` pairs |

**Skip from extra_tags:** White, Black, WhiteElo, BlackElo, WhiteTitle, BlackTitle, Event, Site, Date, Result, ECO — all already extracted above.

**pgn::result → string (anonymous namespace in .cpp):**
```cpp
auto pgn_result_to_string(pgn::result res) noexcept -> std::string {
    switch (res) {
        case pgn::result::white:   return "1-0";
        case pgn::result::black:   return "0-1";
        case pgn::result::draw:    return "1/2-1/2";
        case pgn::result::unknown: return "*";
    }
    return "*";
}
```

**pgn::result → int8_t for position_row (anonymous namespace in .cpp):**
```cpp
auto pgn_result_to_int8(pgn::result res) noexcept -> std::int8_t {
    switch (res) {
        case pgn::result::white: return 1;
        case pgn::result::black: return -1;
        default:                 return 0;
    }
}
```

**Event handling:** If `Event` tag is absent or empty, `game.event_details` stays `std::nullopt`. Only set it if the Event name is non-empty.

### Move Processing Loop

```cpp
auto board = chesslib::board{};  // starts at starting position (standard chess)
std::vector<std::uint16_t> encoded_moves;
std::vector<motif::db::position_row> position_rows;

auto const result_int = pgn_result_to_int8(pgn_game.result);
auto const white_elo  = parse_elo(find_tag(pgn_game.tags, "WhiteElo"));
auto const black_elo  = parse_elo(find_tag(pgn_game.tags, "BlackElo"));

for (auto const& node : pgn_game.moves) {
    auto move_result = chesslib::san::from_string(board, node.san);
    if (!move_result) {
        return tl::unexpected(error_code::parse_error);
    }
    encoded_moves.push_back(chesslib::codec::encode(*move_result));

    chesslib::move_maker mm{board, *move_result};
    mm.make();

    position_rows.push_back(motif::db::position_row{
        .zobrist_hash = board.hash(),
        .game_id      = 0,   // placeholder; filled after game_store::insert
        .ply          = static_cast<std::uint16_t>(encoded_moves.size()),  // 1-indexed
        .result       = result_int,
        .white_elo    = white_elo,
        .black_elo    = black_elo,
    });
}
// Assign back to db_game after encoding all moves
db_game.moves = std::move(encoded_moves);
```

**Variations:** `pgn::move_node.variations` is NOT processed. Only the main line (`pgn_game.moves`) is walked. Ignore variation nodes entirely — no error, no crash.

**Comments / `%clk`:** `pgn::move_node.comment` is NOT inspected. Clock data is not stored in this story. Presence or absence never causes an error.

**Ply semantics:** ply is 1-indexed after each move. A game with N moves produces N position_rows with ply values 1..N. A zero-move game produces zero rows.

**`san::from_string` errors:** any error variant (`invalid_syntax`, `no_matching_move`, `ambiguous`) returns `parse_error` from `process`. No distinction between error kinds is needed.

### Known chesslib header NOLINTNEXTLINE requirement

`<chesslib/chesslib.hpp>` pulls in `magic_enum` and `ankerl` headers which may trigger `misc-include-cleaner` if you use only top-level chesslib types. Suppress the entire umbrella include:
```cpp
#include <chesslib/chesslib.hpp>  // NOLINT(misc-include-cleaner)
```
Only include what you directly use in the .cpp file. Since `import_worker.cpp` uses `chesslib::board`, `chesslib::move_maker`, `chesslib::san::from_string`, and `chesslib::codec::encode`, the umbrella header covers all of them.

### Test Setup Pattern

Tests need a `database_manager` with initialized schema. Use temp directories:

```cpp
#include <filesystem>
#include <catch2/catch_test_macros.hpp>
#include "motif/db/database_manager.hpp"
#include "motif/import/import_worker.hpp"

// In each TEST_CASE:
auto tmp = std::filesystem::temp_directory_path() / "iw_test_XXXXX";
std::filesystem::create_directories(tmp);
auto db = motif::db::database_manager::create(tmp, "test").value();

motif::import::import_worker worker{db.store(), db.positions()};

// cleanup:
db.close();
std::filesystem::remove_all(tmp);
```

**Constructing minimal pgn::game inline:**
```cpp
pgn::game make_game(std::string white, std::string black, pgn::result result,
                    std::vector<pgn::move_node> moves) {
    return pgn::game{
        .tags   = {{"White", white}, {"Black", black}},
        .moves  = std::move(moves),
        .result = result,
    };
}

// pgn::move_node requires: number, san (comment/nags/variations optional):
pgn::move_node make_move(int number, std::string san) {
    return pgn::move_node{
        .number = number,
        .san    = std::move(san),
    };
}
```

**A legal 4-move game (no DB dependency — works regardless of pgnlib):**
```cpp
// "Scholar's mate" (but simplified for testing)
// Use a known short legal game: 1.e4 e5 2.Nf3
auto moves = std::vector<pgn::move_node>{
    make_move(1, "e4"),
    make_move(1, "e5"),
    make_move(2, "Nf3"),
};
auto game = make_game("White Player", "Black Player", pgn::result::unknown, moves);
```

**Forcing a SAN failure:**
```cpp
auto bad_moves = std::vector<pgn::move_node>{ make_move(1, "XXXX_invalid") };
auto bad_game  = make_game("A", "B", pgn::result::unknown, bad_moves);
auto res = worker.process(bad_game);
REQUIRE_FALSE(res.has_value());
CHECK(res.error() == motif::import::error_code::parse_error);
```

**Testing duplicate detection:** Call `worker.process(game)` twice with the same game (same White, Black, Event, Date, Result, moves). Second call must return `error_code::duplicate`.

**Testing %clk:** Add a comment with `%clk` to a move node:
```cpp
pgn::move_node clk_move{.number = 1, .san = "e4",
                         .comment = std::optional<std::string>{"[%clk 0:05:00]"}};
```
Verify `process` succeeds and positions are inserted normally.

**Verifying position_rows:** Use `db.positions().row_count()` to verify the count after process.

### CONVENTIONS.md Compliance Checklist

- `#pragma once` with `NOLINTNEXTLINE(portability-avoid-pragma-once)` on line before: ✓
- All identifiers `lower_snake_case`: ✓ (`import_worker`, `process_result`, `process`, `positions_inserted`)
- `result<T>` alias from `error.hpp` for all public API returns: ✓
- No Qt headers: ✓ (`motif_import` is Qt-free)
- No direct DuckDB/SQLite headers in `motif_import`: ✓ (storage via `motif_db` APIs)
- Logger null-check if used: ✓ (`auto log = spdlog::get("motif.import"); if (log) { ... }`)
- Anonymous namespace for TU-local helpers in `.cpp`: ✓ (`find_tag`, `parse_elo`, `pgn_result_to_string`, `pgn_result_to_int8`)
- Test tag: `[motif-import]` (consistent with logger_test, pgn_reader_test)

### File List for This Story

**Files to create:**
- `source/motif/import/import_worker.hpp`
- `source/motif/import/import_worker.cpp`
- `test/source/motif_import/import_worker_test.cpp`

**Files to modify:**
- `source/motif/import/CMakeLists.txt` — add `motif_db` to link libraries, add `import_worker.cpp` to sources
- `test/CMakeLists.txt` — add `import_worker_test.cpp` to `motif_import_test`

**Files NOT to modify:**
- `source/motif/db/` — Epic 1 code is done; do not touch
- `source/motif/import/error.hpp` — no new error codes needed; `duplicate`, `parse_error`, `io_failure` are already defined
- `source/motif/import/pgn_reader.*` — story 2-2 code is complete; do not touch
- `source/motif/import/logger.*` — story 2-1 code is complete; do not touch
- `flake.nix` — no new deps needed
- `vcpkg.json` — no new deps needed (chesslib is not in vcpkg; it comes from Nix only)

### Previous Story Intelligence (2-2 patterns)

- `result<T>` alias from `motif::import::error.hpp` is `tl::expected<T, motif::import::error_code>`. Already includes `eof`, `parse_error`, `io_failure`, `ok`, `invalid_state`.
- `#pragma once` always first line, `NOLINTNEXTLINE(portability-avoid-pragma-once)` on line directly before it.
- Logger null-check pattern: `auto log = spdlog::get("motif.import"); if (log) { ... }` — required since tests may not call `initialize_logging`.
- Build verification: always run `cmake --preset=dev && ctest --test-dir build/dev` as final step, not just compile check.
- All variables named ≥3 characters (clang-tidy `readability-identifier-length`).
- `misc-include-cleaner`: every symbol used in a `.cpp` must have its providing header directly included; use `// NOLINT(misc-include-cleaner)` on includes where the header is an umbrella (pgnlib, chesslib).
- clang-tidy `cppcoreguidelines-pro-bounds-pointer-arithmetic`: avoid pointer arithmetic; use `std::string_view::substr` or spans.
- `std::move(eg).value()` pattern for moving out of `tl::expected` (not `std::move(eg.value())`).

### Architecture References

- [AR08] — DuckDB `position` table schema: `(zobrist_hash UBIGINT, game_id UINTEGER, ply USMALLINT, result TINYINT, white_elo SMALLINT, black_elo SMALLINT)`
- [AR09] — import checkpoint uses `game_id` as last committed ID; `import_worker` returns it in `process_result`
- [FR03] — player deduplication via `game_store::insert` → `find_or_insert_player` (already implemented in Story 1.3)
- [FR04] — duplicate detection via `game_store::insert` → returns `error_code::duplicate` (CONVENTIONS.md: treat as non-fatal, propagate to caller)
- [FR20] — position index populated during import; `import_worker` is the unit that does this
- [NFR09] — SQLite is authoritative; if DuckDB write fails after SQLite insert succeeds, return `io_failure` — the position store can always be rebuilt via `database_manager::rebuild_position_store`
- [NFR10] — SAN parse failure must not crash; return `parse_error` and let the pipeline log and skip
- [NFR19] — chesslib exclusively owns move legality; do not call any chess logic in motif-chess code
- [P2] — error handling: monadic chains preferred, `result.value()` without prior check is forbidden
- [CONVENTIONS.md — Packaging] — `chesslib` is not in vcpkg; it is Nix-only. Do not add to `vcpkg.json`.

---

## Dev Agent Record

### Agent Model Used

claude-sonnet-4-6

### Debug Log References

### Completion Notes List

- Added `duplicate` to `motif::import::error_code` (story spec incorrectly stated it was already defined; AC3 requires it)
- `import_worker` class uses reference members for `game_store` and `position_store`; suppressed `cppcoreguidelines-avoid-const-or-ref-data-members` with NOLINT per story spec API
- Move processing loop completes before any DB write (critical ordering satisfied)
- `extract_game()` helper extracted to keep `process()` cognitive complexity under clang-tidy threshold
- Chesslib included via specific sub-headers (`board/board.hpp`, `board/move_codec.hpp`, `util/san.hpp`) rather than umbrella to satisfy `misc-include-cleaner`
- 75/75 tests pass; 75/75 pass under ASan/UBSan — zero violations

### File List

- `source/motif/import/import_worker.hpp` (created)
- `source/motif/import/import_worker.cpp` (created)
- `test/source/motif_import/import_worker_test.cpp` (created)
- `source/motif/import/CMakeLists.txt` (modified — added motif_db, import_worker.cpp)
- `source/motif/import/error.hpp` (modified — added `duplicate` error code)
- `test/CMakeLists.txt` (modified — added import_worker_test.cpp)

### Change Log

| Date       | Version | Description                                          | Author        |
|------------|---------|------------------------------------------------------|---------------|
| 2026-04-19 | 1.0     | Story created — ready-for-dev                        | Story Agent   |
| 2026-04-19 | 1.1     | Story implemented — import_worker + tests, all pass  | claude-sonnet-4-6 |
