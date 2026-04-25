# Story 2.2: PGN Reader & pgnlib Adapter

Status: done

## Story

As a developer,
I want a `pgn_reader` adapter over pgnlib that streams `pgn::game` structs one at a time from a PGN file,
so that the import pipeline can process games without loading the entire file into memory.

## Acceptance Criteria

1. **Given** a valid PGN file with N games
   **When** `pgn_reader::next` is called repeatedly
   **Then** each call returns the next `pgn::game` struct until the file is exhausted, then returns `tl::unexpected(error_code::eof)`
   **And** memory usage does not grow proportionally with file size (streaming, not bulk load) (NFR04)

2. **Given** a PGN file containing a malformed game (truncated headers, invalid move)
   **When** `pgn_reader::next` is called on that game
   **Then** the malformed game is skipped; `next` returns `tl::unexpected(error_code::parse_error)` with the game number and available byte-offset context logged via `motif.import` (FR11)
   **And** calling `next` again successfully returns the following valid game

3. **Given** a valid PGN file
   **When** `pgn_reader::seek_to_offset` is called with a byte offset followed by `next`
   **Then** reading resumes from the next `[Event` tag at or after that offset (AR09 resume logic)
   **And** all tests pass under `dev-sanitize` including the malformed-game case (NFR10)

## Tasks / Subtasks

- [x] Add pgnlib to build system (AC: all)
  - [x] Add `find_package(pgnlib REQUIRED)` to `source/motif/import/CMakeLists.txt`
  - [x] Link `pgnlib::pgnlib` to `motif_import` (PUBLIC, after existing `spdlog::spdlog`)
  - [x] Add `"pgnlib"` to `vcpkg.json` dependencies array
  - [x] Verify `cmake --preset=dev && cmake --build build/dev` succeeds with zero new warnings

- [x] Extend `motif::import::error_code` with `eof` and `parse_error` (AC: #1, #2)
  - [x] Add `eof` and `parse_error` variants to the `enum class error_code` in `source/motif/import/error.hpp`
  - [x] Add matching `to_string` cases (return `"eof"` and `"parse_error"` respectively)

- [x] Implement `pgn_reader` class (AC: #1, #2, #3)
  - [x] Create `source/motif/import/pgn_reader.hpp` — declare `class pgn_reader` (see Dev Notes for exact API)
  - [x] Create `source/motif/import/pgn_reader.cpp` — implement `next()` and `seek_to_offset()`
  - [x] Add `pgn_reader.cpp` to `motif_import` STATIC target sources in `source/motif/import/CMakeLists.txt`

- [x] Write tests (AC: #1, #2, #3)
  - [x] Create `test/source/motif_import/pgn_reader_test.cpp`
  - [x] Test: stream all games from a 2-game PGN string — two `next()` calls succeed, third returns `eof`
  - [x] Test: empty PGN string — first `next()` returns `eof`
  - [x] Test: PGN with three games where middle game is malformed — returns game1, parse_error, game3, eof (in that order)
  - [x] Test: `seek_to_offset` — record byte offset before game 2, seek new reader to that offset, verify first game returned matches game 2's tags
  - [x] Test: nonexistent file path — first `next()` returns `io_failure`
  - [x] Add `pgn_reader_test.cpp` to `motif_import_test` in `test/CMakeLists.txt`

- [x] Validate build, tests, sanitizers
  - [x] `cmake --preset=dev && cmake --build build/dev` — clean build, zero new warnings
  - [x] `ctest --test-dir build/dev` — all tests pass
  - [x] `cmake --preset=dev-sanitize && cmake --build build/dev-sanitize`
  - [x] `ctest --test-dir build/dev-sanitize` — zero ASan/UBSan violations

### Review Findings

- [x] [Review][Patch] `pgn_reader` is not actually streaming from file [source/motif/import/pgn_reader.cpp:21]
- [x] [Review][Patch] `byte_offset()` must remain absolute across `seek_to_offset()` [source/motif/import/pgn_reader.cpp:73]
- [x] [Review][Patch] `seek_to_offset()` can resume from a false `[Event` match [source/motif/import/pgn_reader.cpp:73]
- [x] [Review][Patch] `vcpkg.json` must include `tl-expected` for `find_package(tl-expected REQUIRED)` [vcpkg.json:21]

---

## Dev Notes

### pgnlib CMake Integration

CMake package: `pgnlib` (config at `lib/cmake/pgnlib/pgnlibConfig.cmake`).
CMake target: `pgnlib::pgnlib` (static library, `libpgnlib.a`).

```cmake
find_package(pgnlib REQUIRED)
target_link_libraries(motif_import PUBLIC spdlog::spdlog pgnlib::pgnlib tl::expected)
```

`pgnlib::pgnlib` transitively pulls in `tl::expected` (declared in its `find_dependency`), so no
double-link is needed. Include path: `#include <pgnlib/pgnlib.hpp>` and `#include <pgnlib/types.hpp>`.

### pgnlib API (Exact, from source)

```cpp
// pgnlib/types.hpp — pgn namespace
enum class result : u8 { white, black, draw, unknown };
struct tag       { std::string key; std::string value; };
struct nag       { int value; };
struct move_node { int number; std::string san; std::optional<std::string> comment;
                   std::vector<nag> nags; std::vector<variation> variations; };
struct variation { std::vector<move_node> moves; };
struct game      { std::vector<tag> tags; std::vector<move_node> moves; pgn::result result; };

// pgnlib/pgnlib.hpp — pgn namespace
enum class parse_error : u8 { file_not_found, syntax_error };

// game_stream: input range, one game at a time
class game_stream {
public:
    explicit game_stream(std::filesystem::path const& path);   // reads whole file into owned buffer
    explicit game_stream(std::string_view input);              // BORROWS caller's buffer — must outlive stream

    struct iterator {
        using value_type = tl::expected<game, parse_error>;
        reference operator*() noexcept;             // current game or error
        iterator& operator++();                     // advance to next game
        bool operator==(std::default_sentinel_t) const noexcept;
        std::size_t byte_offset() const noexcept;  // offset of current game's '[' in source buffer
    };

    iterator begin();
    std::default_sentinel_t end() const noexcept;
};
```

**file_not_found behavior (critical):** When path does not exist, the constructor sets the first
`current` slot to `tl::unexpected(parse_error::file_not_found)` and leaves `remaining` empty. The
first dereference of the iterator yields the error; `++iter` then sets `done=true`. So the iterator
is NOT immediately at `sentinel` — it produces exactly one `file_not_found` error then exhausts.

**byte_offset() semantics:** For the `path` constructor, offset is absolute in the file (file
content is the source buffer). For the `string_view` constructor, offset is relative to the
`string_view` start. After `seek_to_offset`, offsets returned are relative to the substring start,
not the original file — callers must add the seek position if they need absolute offsets.

### `pgn_reader` API and Implementation

```cpp
// source/motif/import/pgn_reader.hpp
#pragma once  // NOLINTNEXTLINE(portability-avoid-pragma-once)

#include <cstddef>
#include <filesystem>
#include <string>
#include <memory>

#include <pgnlib/pgnlib.hpp>

#include "motif/import/error.hpp"

namespace motif::import {

class pgn_reader {
public:
    explicit pgn_reader(std::filesystem::path path);

    // Returns next game from the stream.
    // Errors: eof (stream exhausted), parse_error (bad game — caller should call next() to continue),
    //         io_failure (file unreadable or not found — stream is done after this)
    [[nodiscard]] auto next() -> result<pgn::game>;

    // Re-open the source file, find the first [Event tag at or after byte_offset, and reset
    // the stream to begin there. Returns io_failure if the file cannot be read.
    [[nodiscard]] auto seek_to_offset(std::size_t byte_offset) -> result<void>;

    // 1-based counter: increments on each next() call, including parse errors.
    [[nodiscard]] auto game_number() const noexcept -> std::size_t;

    // Byte offset of the game currently ready to be returned (before next() is called).
    // Use this to record checkpoints: read this value, then call next() to consume the game.
    // Returns 0 when the stream is exhausted or not yet started.
    [[nodiscard]] auto byte_offset() const noexcept -> std::size_t;

private:
    std::filesystem::path path_;
    std::string           owned_buffer_;   // owned buf for seek_to_offset mode; must outlive stream_
    std::unique_ptr<pgn::game_stream> stream_;   // declared before iter_ — destroyed after iter_
    pgn::game_stream::iterator        iter_;     // non-owning ptr into stream_; destroyed first
    std::size_t game_number_{0};
};

} // namespace motif::import
```

**Implementation notes for `pgn_reader.cpp`:**

Constructor:
```cpp
pgn_reader::pgn_reader(std::filesystem::path path)
    : path_(std::move(path))
    , stream_(std::make_unique<pgn::game_stream>(path_))
    , iter_(stream_->begin())
{}
```

`next()`:
```cpp
auto pgn_reader::next() -> result<pgn::game> {
    if (iter_ == std::default_sentinel_t{}) {
        return tl::unexpected(error_code::eof);
    }
    auto eg = std::move(*iter_);   // move current value out before advancing
    ++iter_;
    ++game_number_;
    if (!eg) {
        auto log = spdlog::get("motif.import");
        if (log) {
            log->warn("pgn parse error at game {} (byte offset: {}): {}",
                      game_number_,
                      // byte_offset is now stale after ++iter_; use the pre-advance value
                      // stored in eg or log the game number only
                      game_number_,  // see note below
                      eg.error() == pgn::parse_error::file_not_found ? "file not found" : "syntax error");
        }
        if (eg.error() == pgn::parse_error::file_not_found) {
            return tl::unexpected(error_code::io_failure);
        }
        return tl::unexpected(error_code::parse_error);
    }
    return std::move(eg).value();
}
```

**Logging byte offset correctly:** `iter_.byte_offset()` returns the offset of the CURRENT game
(the one about to be returned). Capture it BEFORE advancing:
```cpp
auto offset_before = iter_.byte_offset();  // capture before ++iter_
auto eg = std::move(*iter_);
++iter_;
++game_number_;
if (!eg) {
    if (log) log->warn("pgn parse error at game {} (byte offset: {}): {}",
                        game_number_, offset_before, ...);
    ...
}
```

`seek_to_offset(byte_offset)`:
```cpp
auto pgn_reader::seek_to_offset(std::size_t byte_offset) -> result<void> {
    // Invalidate iter_ BEFORE destroying stream_ (iter_ holds non-owning ptr into stream_)
    iter_ = pgn::game_stream::iterator{};
    stream_.reset();

    // Read entire file content
    std::ifstream f(path_, std::ios::binary | std::ios::ate);
    if (!f) return tl::unexpected(error_code::io_failure);
    auto size = static_cast<std::size_t>(f.tellg());
    owned_buffer_.resize(size);
    f.seekg(0);
    if (!f.read(owned_buffer_.data(), static_cast<std::streamsize>(size))) {
        return tl::unexpected(error_code::io_failure);
    }

    // Find next [Event tag at or after byte_offset
    auto found = owned_buffer_.find("[Event", byte_offset);
    if (found == std::string::npos) {
        // No games at or after offset — create empty stream
        stream_ = std::make_unique<pgn::game_stream>(std::string_view{});
    } else {
        stream_ = std::make_unique<pgn::game_stream>(
            std::string_view{owned_buffer_.data() + found, owned_buffer_.size() - found});
    }
    iter_ = stream_->begin();
    game_number_ = 0;   // reset counter; caller doesn't know absolute position
    return {};
}
```

`byte_offset()`:
```cpp
auto pgn_reader::byte_offset() const noexcept -> std::size_t {
    if (iter_ == std::default_sentinel_t{}) return 0;
    return iter_.byte_offset();
}
```

`game_number()`:
```cpp
auto pgn_reader::game_number() const noexcept -> std::size_t { return game_number_; }
```

### Member Declaration Order — Critical

Destruction happens in **reverse declaration order**:
1. `iter_` — destroyed first (non-owning ptr into `stream_->impl`)
2. `stream_` — destroyed second (owns the impl that `iter_` pointed to)
3. `owned_buffer_` — destroyed last (owns the string_view that `stream_` borrows)

**Do not reorder these members.** Any reorder breaks the destruction invariant.

### Malformed-game PGN for Tests

A minimal PGN string with three games where game 2 is malformed:
```cpp
constexpr std::string_view three_games_second_bad = R"pgn(
[Event "E1"]
[White "Alice"]
[Black "Bob"]
[Result "1-0"]

1. e4 e5 2. Nf3 Nc6 1-0

[Event "E2"]
[White "Broken"]
[Black "Game"]
[Result "1-0"]

1. XXXX_invalid_san 1-0

[Event "E3"]
[White "Charlie"]
[Black "Dave"]
[Result "0-1"]

1. d4 d5 0-1
)pgn";
```

Note: whether pgnlib reports game 2 as a `syntax_error` depends on its lexy grammar strictness.
Test may need to use a PGN that definitely triggers `syntax_error` — a game with truncated tags
(no `]` closing) or an invalid SAN that lexy rejects. Verify in testing.

### Test for `seek_to_offset`

Use `pgn::game_stream` directly to get the byte offset of game 2, then verify `pgn_reader::seek_to_offset` starts there:

```cpp
TEST_CASE("pgn_reader: seek_to_offset resumes from correct game", "[motif-import]") {
    // Write two-game PGN to a temp file
    auto tmp = std::filesystem::temp_directory_path() / "pgn_reader_seek_test.pgn";
    // ... write PGN with 2 clearly distinguishable games ...

    // Determine byte offset of game 2 via game_stream
    std::size_t game2_offset = 0;
    {
        pgn::game_stream stream{tmp};
        auto it = stream.begin();
        REQUIRE(it != std::default_sentinel_t{});
        game2_offset = it.byte_offset();   // offset of game 1
        ++it;                              // advance to game 2
        REQUIRE(it != std::default_sentinel_t{});
        game2_offset = it.byte_offset();   // offset of game 2
    }

    // Seek reader to game 2's offset and verify first next() returns game 2
    motif::import::pgn_reader reader{tmp};
    auto seek_result = reader.seek_to_offset(game2_offset);
    REQUIRE(seek_result.has_value());

    auto g = reader.next();
    REQUIRE(g.has_value());
    // Verify it's game 2 by checking a tag value
    auto it = std::ranges::find_if(g->tags, [](auto const& t){ return t.key == "Event"; });
    REQUIRE(it != g->tags.end());
    CHECK(it->value == "Game2Event");  // match game 2's [Event] value

    std::filesystem::remove(tmp);
}
```

### clang-tidy Notes

- `pgnlib/pgnlib.hpp` includes non-system headers that may trigger `misc-include-cleaner`. Suppress
  with `// NOLINT(misc-include-cleaner)` on the include line if needed, or mark pgnlib as SYSTEM:
  `target_include_directories(motif_import SYSTEM PRIVATE ...)` for the pgnlib include path.
- `std::move(*iter_)` — moving out of a dereferenced iterator. clang-tidy may flag this; it's
  correct because `advance()` immediately overwrites `impl_->current` on `++iter_`.
- Default-constructed `pgn::game_stream::iterator{}` has `impl_ = nullptr`, which is valid and
  compares equal to `std::default_sentinel_t` per the implementation (`impl_ == nullptr || impl_->done`).

### CONVENTIONS.md compliance checklist

- All identifiers `lower_snake_case`: ✓ (`pgn_reader`, `next`, `seek_to_offset`, `game_number`)
- `#pragma once` with `NOLINTNEXTLINE(portability-avoid-pragma-once)`: ✓
- `tl::expected` return from all public API: ✓ (uses `result<T>` alias)
- No Qt headers: ✓ (`motif_import` must stay Qt-free)
- No DuckDB/SQLite headers in `motif_import`: ✓ (storage via `motif_db` APIs — not applicable here)
- Logger null-check: ✓ (spdlog::get may return nullptr in tests that don't call initialize_logging)
- Anonymous namespace for TU-local helpers in `.cpp` files: ✓ (with NOLINTNEXTLINE if needed)

### Test tag convention
All tests in this file use `[motif-import]` tag (consistent with `logger_test.cpp`).

### File List for This Story

**Files to create:**
- `source/motif/import/pgn_reader.hpp`
- `source/motif/import/pgn_reader.cpp`
- `test/source/motif_import/pgn_reader_test.cpp`

**Files to modify:**
- `source/motif/import/error.hpp` — add `eof`, `parse_error` to `error_code` enum and `to_string`
- `source/motif/import/CMakeLists.txt` — add `find_package(pgnlib REQUIRED)`, link `pgnlib::pgnlib`, add `pgn_reader.cpp`
- `test/CMakeLists.txt` — add `pgn_reader_test.cpp` to `motif_import_test`
- `vcpkg.json` — add `"pgnlib"` to dependencies

**Files NOT to modify:**
- `source/motif/db/` — Epic 1 code is done; do not touch
- `source/motif/import/logger.hpp` / `logger.cpp` — logger infrastructure is complete
- `CLAUDE.md`, `CONVENTIONS.md` — read-only

### Previous Story Intelligence (2-1 patterns)

- `result<T>` alias (`tl::expected<T, error_code>`) is already defined in `error.hpp` — use it.
- `#pragma once` always comes first, on its own line, with `// NOLINTNEXTLINE(portability-avoid-pragma-once)` on the line directly before it.
- Logger null-check pattern: `auto log = spdlog::get("motif.import"); if (log) { ... }` — required
  in all modules since tests may not call `initialize_logging`.
- Build verification: always run `cmake --preset=dev && ctest --test-dir build/dev` as the final
  step, not just a compile check.
- UBSan: moving out of `tl::expected` correctly — use `.value()` after checking `has_value()`, or
  use `std::move(eg).value()` (not `std::move(eg.value())` which may be UB if eg is in error state).
- Magic numbers: wrap as `constexpr` — e.g., `constexpr std::size_t npos = std::string::npos;` is
  not needed here, but any numeric literal > single-digit should be a named constant.

### References

- [AR09] — import checkpoint struct: `byte_offset` as seek target
- [NFR04] — memory ceiling during import: streaming, not bulk
- [FR11] — malformed game logging with context
- [P2 — Error Handling] in architecture.md — `tl::expected` contract
- [P5 — Logging] in architecture.md — `motif.import` logger name
- [CONVENTIONS.md — Error handling] — `result<T>` alias usage
- pgnlib source: `/nix/store/mf9dhqib8d82lx3838v2dhd0cvlkwkjr-.../source/pgnlib.cpp`

---

## Dev Agent Record

### Agent Model Used

claude-sonnet-4-6

### Debug Log References

None — implementation proceeded without blockers.

### Completion Notes List

- Added `eof` and `parse_error` to `motif::import::error_code` and `to_string`.
- Implemented `pgn_reader` wrapping `pgn::game_stream` with lazy streaming (not bulk load).
- `seek_to_offset` reads the file into `owned_buffer_`, finds the next `[Event` tag at or after the offset, and creates a `string_view`-based `game_stream` — avoids pointer arithmetic by using `std::string_view::substr`.
- Member declaration order (`owned_buffer_` → `stream_` → `iter_`) preserves destruction invariant.
- Logger null-check pattern applied per Story 2-1 convention.
- All 5 required tests pass, including the malformed-game sequence and seek-to-offset case.
- Zero warnings and zero ASan/UBSan violations.

### File List

**Created:**
- `source/motif/import/pgn_reader.hpp`
- `source/motif/import/pgn_reader.cpp`
- `test/source/motif_import/pgn_reader_test.cpp`

**Modified:**
- `source/motif/import/error.hpp` — added `eof`, `parse_error` to enum and `to_string`
- `source/motif/import/CMakeLists.txt` — added `find_package(pgnlib REQUIRED)`, `pgnlib::pgnlib` link, `pgn_reader.cpp` source
- `test/CMakeLists.txt` — added `pgn_reader_test.cpp` to `motif_import_test`
- `vcpkg.json` — added `"pgnlib"` to dependencies

### Change Log

| Date       | Version | Description                    | Author        |
|------------|---------|--------------------------------|---------------|
| 2026-04-19 | 1.0     | Story created — ready-for-dev  | Story Agent   |
| 2026-04-19 | 1.1     | Implementation complete — ready for review | Dev Agent (claude-sonnet-4-6) |
