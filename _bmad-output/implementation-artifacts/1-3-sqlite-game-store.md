# Story 1.3: SQLite Game Store

Status: done

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a developer,
I want a fully-tested SQLite game store that can insert, retrieve, and delete chess games with player/event deduplication,
so that the database layer correctly persists game data with referential integrity.

## Acceptance Criteria

1. **Given** domain types from Story 1.2 exist
   **When** a game is inserted via `game_store::insert`
   **Then** the game is persisted to the `game`, `player`, `event`, and `game_tag` tables
   **And** inserting a game where the white player name already exists reuses the existing `player` row (FR03)
   **And** inserting a game where the event name already exists reuses the existing `event` row (FR03)
   **And** inserting a duplicate game returns `error_code::duplicate` when `skip_duplicates` is set (FR04)

2. **Given** a game has been inserted with a known game ID
   **When** `game_store::get` is called with that ID
   **Then** the returned `game` struct contains the original metadata and the correct decoded move sequence (FR05)

3. **Given** a game has been inserted
   **When** `game_store::remove` is called with that game's ID
   **Then** the game row and all associated `game_tag` rows are deleted (FR06)
   **And** the associated `player` and `event` rows are NOT deleted (they may be referenced by other games)

4. **Given** any test scenario
   **Then** all tests use an in-memory SQLite instance (`:memory:`) with no on-disk files
   **And** all tests pass under `cmake --preset=dev-sanitize` with zero ASan/UBSan violations (NFR11)

## Tasks / Subtasks

- [x] Define the `game_store` public API and SQLite-backed implementation (AC: #1, #2, #3)
  - [x] Create `source/motif/db/game_store.hpp` with a minimal public API in `motif::db`
  - [x] Create `source/motif/db/game_store.cpp` and implement SQLite-backed CRUD operations
  - [x] Keep all public fallible functions on `tl::expected<T, error_code>` and do not use exceptions
  - [x] Reuse the Story 1.2 shared domain types instead of introducing SQLite-specific transport structs

- [x] Implement schema creation required for the store tests and runtime use (AC: #1, #3)
  - [x] Create only the tables needed by this story: `game`, `player`, `event`, `tag`, and `game_tag`
  - [x] Create the minimum indexes and foreign keys needed to preserve referential integrity and support deduplication
  - [x] Keep schema and SQL naming aligned with the architecture's singular `lower_snake_case` convention
  - [x] Do not pull manifest lifecycle or `PRAGMA user_version` work forward from Story 1.4 unless needed as a small internal helper

- [x] Implement player, event, and duplicate-game handling (AC: #1)
  - [x] Reuse an existing `player` row when the same player name is inserted again
  - [x] Reuse an existing `event` row when the same event name is inserted again
  - [x] Define and document how duplicate detection works for Story 1.3 so tests can assert it deterministically
  - [x] Return `error_code::duplicate` when `skip_duplicates` is set and the same game is inserted again

- [x] Implement retrieval and deletion semantics (AC: #2, #3)
  - [x] Ensure `game_store::get` reconstructs the original metadata and move sequence from SQLite rows and the encoded move blob
  - [x] Ensure `game_store::remove` deletes the game and `game_tag` rows only
  - [x] Preserve `player` and `event` rows after deletion even if they are no longer referenced

- [x] Wire build dependencies for SQLite game store support (AC: #1, #2, #3, #4)
  - [x] Update `source/motif/db/CMakeLists.txt` to compile the new implementation files
  - [x] Add the correct SQLite package discovery and link target in CMake without introducing forbidden dependency acquisition methods
  - [x] Keep `motif_db` as a static library and preserve `${PROJECT_SOURCE_DIR}/source` as the public include root
  - [x] Do not introduce Qt, DuckDB-specific logic, or manifest management code into this story

- [x] Replace the current `motif_db` test focus with game-store coverage (AC: #1, #2, #3, #4)
  - [x] Add `test/source/motif_db/game_store_test.cpp`
  - [x] Update `test/CMakeLists.txt` so `motif_db_test` covers both Story 1.2 contracts and the new game store tests
  - [x] Test insert/get round-trip, player deduplication, event deduplication, duplicate rejection, and deletion semantics
  - [x] Use only in-memory SQLite instances in tests

- [x] Validate build, tests, and sanitizers (AC: #4)
  - [x] `cmake --preset=dev && cmake --build build/dev`
  - [x] `ctest --test-dir build/dev`
  - [x] `cmake --preset=dev-sanitize && cmake --build build/dev-sanitize`
  - [x] `ctest --test-dir build/dev-sanitize`

### Review Findings

- [x] [Review][Decision] Clarify `skip_duplicates` contract — removed `insert_options` struct and the `opts` parameter entirely; `insert()` always returns `error_code::duplicate` on conflict; callers decide whether to treat it as fatal.
- [x] [Review][Decision] Split or approve packaging/install changes for this story — approved by user as necessary support work for Story 1.3.
- [x] [Review][Patch] Check transaction start/commit failures before reporting insert success [source/motif/db/game_store.cpp:35] — `txn_guard` now stores BEGIN result in `m_began`; `insert()` returns `io_failure` if `began()` is false; `commit()` returns bool and only sets `m_committed` on success.
- [x] [Review][Patch] Validate stored move blobs before reconstructing `game::moves` [source/motif/db/game_store.cpp:451] — `get()` now checks `blob_bytes % sizeof(uint16_t) != 0` and returns `io_failure` if misaligned.
- [x] [Review][Patch] Make tag retrieval deterministic and fail on tag-scan errors [source/motif/db/game_store.cpp:462] — tag query now orders by `gt.rowid` to preserve insertion order; loop captures step result and returns `io_failure` if final step is not `SQLITE_DONE`.
- [x] [Review][Patch] Declare SQLite for vcpkg consumers [vcpkg.json:4] — added `sqlite3` to the shared dependency manifest so `find_package(SQLite3 REQUIRED)` is satisfied off the Nix path too.
- [x] [Review][Patch] Preserve `extra_tags` insertion order in `get()` [source/motif/db/game_store.cpp:476] — retrieval now orders by `game_tag` insertion order and the tag round-trip test uses a non-alphabetical sequence.
- [x] [Review][Patch] Fail `create_schema()` if foreign-key enforcement was not actually enabled [source/motif/db/game_store.cpp:213] — `create_schema()` now enables `PRAGMA foreign_keys` separately, verifies it took effect, and returns `io_failure` if SQLite ignored the pragma.

## Dev Notes

### Architecture Notes

- `motif_db` is the owning module for schema, CRUD, shared types, and database lifecycle concerns. `game_store` belongs in `source/motif/db/` and must remain Qt-free.
- Architecture already reserves `source/motif/db/game_store.hpp` and `source/motif/db/game_store.cpp` for this work. Do not create a separate library or move shared types out of `motif_db`.
- SQLite table names should be singular `lower_snake_case`: `game`, `player`, `event`, `tag`, `game_tag`. Foreign keys should use `<table>_id` naming.
- This story is SQLite-only. Do not pull DuckDB position indexing, manifest handling, scratch base, or database bundle lifecycle work into Story 1.3.

### API and Implementation Guidance

- Story 1.2 established `motif::db::error_code`, `result<T>`, shared domain types, and move codec wrappers. Reuse those directly.
- Keep the `game_store` API minimal and aligned with the acceptance criteria. The epics entry expects methods named around `insert`, `get`, and `remove`.
- Duplicate detection must be deterministic and explicit in code and tests. Favor a narrow contract based on game-identifying data already present in the `game` type rather than speculative heuristics.
- The move sequence stored in SQLite is the encoded 16-bit move blob. Retrieval must return the correct decoded move sequence in the public `game` struct.
- Do not invent new chess logic. If move encoding or decoding is needed, go through the Story 1.2 `move_codec` wrapper around `chesslib::codec`.

### Testing Guidance

- All tests for this story must use SQLite `:memory:` instances only.
- Keep Catch2 v3 style consistent with the existing repo and preserve Story 1.2 coverage rather than replacing it.
- Every public `game_store` function needs at least one test. The minimum useful matrix is:
  - insert and get round-trip
  - player deduplication
  - event deduplication
  - duplicate rejection with `skip_duplicates`
  - remove deletes `game` and `game_tag` rows but not `player` / `event`
- Sanitizer cleanliness is an explicit acceptance criterion, not optional follow-up work.

### Previous Story Intelligence

- Story 1.2 is complete and approved. It established the stable contracts this story should build on: `types.hpp`, `error.hpp`, `move_codec.hpp`, and focused `motif_db` tests.
- The current `motif_db` source tree still contains `motif_db.cpp` as the placeholder translation unit. Story 1.3 is the first step that should introduce a real store implementation file.
- Recent review work fixed packaging and install rules at the repo level. That context is not a reason to broaden Story 1.3 scope into more packaging work unless the new SQLite code makes a minimal CMake adjustment necessary.
- Cross-platform `vcpkg` alignment is intentionally deferred for now. Do not try to solve that in this story unless the user explicitly asks.

### Project Structure Notes

**Current source state:**
- `source/motif/db/` currently contains `CMakeLists.txt`, `motif_db.cpp`, `types.hpp`, `error.hpp`, and `move_codec.hpp`
- `test/source/motif_db/` currently contains `move_codec_test.cpp`

**Files to create:**
- `source/motif/db/game_store.hpp`
- `source/motif/db/game_store.cpp`
- `test/source/motif_db/game_store_test.cpp`

**Files likely to modify:**
- `source/motif/db/CMakeLists.txt`
- `source/motif/db/motif_db.cpp`
- `test/CMakeLists.txt`
- `test/source/motif_db/move_codec_test.cpp` if shared test helpers or fixture consolidation is useful

**Detected constraints:**
- Keep include paths rooted at `source/`, for example `#include "motif/db/game_store.hpp"`
- Preserve static-library structure and current module boundaries
- Avoid introducing `database_manager`, `schema`, `manifest`, or `scratch_base` files early unless a tiny internal helper is clearly justified by this story

### References

- [Source: `_bmad-output/planning-artifacts/epics.md` § "Story 1.3: SQLite Game Store"] — user story and acceptance criteria
- [Source: `_bmad-output/planning-artifacts/architecture.md` § "Project Structure & Boundaries"] — expected file layout for `game_store`
- [Source: `_bmad-output/planning-artifacts/architecture.md` § "Architectural Boundaries"] — module ownership and allowed dependencies
- [Source: `_bmad-output/planning-artifacts/architecture.md` § "P3 — Database Naming"] — singular `lower_snake_case` table and column naming
- [Source: `_bmad-output/project-context.md` § "Critical Don't-Miss Rules / Storage layer"] — move blob, deduplication, and SQLite rules
- [Source: `specs/001-database-schema/spec.md` § "SQLite Schema"] — authoritative schema intent for players, events, games, and tags
- [Source: `specs/001-database-schema/spec.md` § "Public API"] — prior design direction for game store responsibilities
- [Source: `_bmad-output/implementation-artifacts/1-2-domain-types-move-encoding-error-contracts.md`] — previous-story contracts and implementation learnings
- [Source: `source/motif/db/CMakeLists.txt`] — current `motif_db` build scaffold
- [Source: `test/CMakeLists.txt`] — current `motif_db_test` harness

## Dev Agent Record

### Agent Model Used

claude-sonnet-4-6

### Debug Log References

- Story created after Story 1.2 was approved and marked done.
- `llvm-prefer-static-over-anonymous-namespace` and `misc-use-anonymous-namespace` fire simultaneously on the same code (directly contradictory checks). Anonymous namespace is kept (C++ standard preference); NOLINT suppresses the LLVM-specific check on each affected free function.
- `bugprone-unchecked-optional-access` fires on `.value()` after a Catch2 `REQUIRE(opt.has_value())` because clang-tidy cannot model the REQUIRE macro's control flow. Suppressed with NOLINT on that single line.
- Duplicate detection key: `(white_id, black_id, COALESCE(event_id, -1), COALESCE(date, ''), result, moves)` — COALESCE is needed because SQLite treats NULL as distinct in UNIQUE indexes, so two games with NULL event_id would not be caught as duplicates without it.
- `find_or_insert_player` and `find_or_insert_event` deduplicate by name only; the first-inserted row's other fields win on subsequent inserts for the same name. This is documented behaviour.
- `PRAGMA foreign_keys = ON` is set in `create_schema()` and persists for the connection lifetime; `ON DELETE CASCADE` on `game_tag.game_id` handles tag cleanup in `remove()`.

### Completion Notes List

- All 7 tasks and all subtasks checked off.
- 15 new game_store tests added; 5 existing Story 1.2 tests preserved. All 20 pass under dev and dev-sanitize presets.
- Zero clang-tidy warnings in dev build; zero ASan/UBSan violations in sanitize build.
- SQLite found via CMake built-in `FindSQLite3` (provides `SQLite::SQLite3`); linked PUBLIC so test consumers get the include path transitively.
- `txn_guard` RAII class keeps `insert()` cognitive complexity well under the 25-node threshold.
- Move blob stored as raw `uint16_t` bytes (little-endian agnostic within the same process); round-trips correctly including empty move sequences.
- 2026-04-18: Resolved 5 review findings — ✅ removed `insert_options` (policy belongs at caller); ✅ `txn_guard` now propagates BEGIN/COMMIT failures; ✅ move blob alignment validated; ✅ tag scan ORDER BY + error propagation. All 20 tests pass, zero warnings/violations.

### File List

**Created:**
- `_bmad-output/implementation-artifacts/1-3-sqlite-game-store.md`
- `source/motif/db/game_store.hpp`
- `source/motif/db/game_store.cpp`
- `test/source/motif_db/game_store_test.cpp`

**Modified (review follow-ups):**
- `source/motif/db/game_store.hpp` — removed `insert_options` struct; `insert()` takes no options
- `source/motif/db/game_store.cpp` — `txn_guard` propagates BEGIN/COMMIT failures; blob alignment check; deterministic tag ORDER BY; tag scan error detection
- `test/source/motif_db/game_store_test.cpp` — removed `insert_options` argument from duplicate test

## Change Log

- 2026-04-17: Implemented SQLite game store — `game_store` class with `create_schema`, `insert`, `get`, `remove`; 5-table schema with expression UNIQUE index for deterministic duplicate detection; 15 Catch2 tests; zero warnings/sanitizer violations.
- 2026-04-18: Addressed 5 review findings — removed `insert_options`/`skip_duplicates` (callers handle duplicate policy); `txn_guard` now propagates BEGIN/COMMIT failures; move blob alignment validated before `memcpy`; tag query now `ORDER BY t.name` and fails on scan error.
