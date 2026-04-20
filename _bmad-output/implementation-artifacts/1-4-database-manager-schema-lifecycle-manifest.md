# Story 1.4: Database Manager, Schema Lifecycle & Manifest

Status: done

## Story

As a developer and as a user,
I want to create, open, and close a named chess database bundle at a user-specified path, with idempotent schema initialization and a machine-readable manifest,
so that database files are portable and the schema is always in a known, valid state regardless of how many times the database is opened.

## Acceptance Criteria

1. **Given** a valid directory path
   **When** `database_manager::create` is called
   **Then** a database bundle is created containing `games.db` (SQLite WAL) and `manifest.json` (glaze-serialized with name, schema version, game count = 0, created timestamp) (FR01, AR05)
   **And** the SQLite schema is initialized: `game`, `player`, `event`, `tag`, `game_tag`, `schema_migrations` tables created; `PRAGMA user_version` set to current schema version (FR07)

2. **Given** an existing database bundle
   **When** `database_manager::open` is called on its path
   **Then** the database is opened and `manifest.json` is deserialized; no tables are dropped or recreated (FR07)
   **And** `PRAGMA user_version` matches the expected schema version; if it does not, `error_code::schema_mismatch` is returned

3. **Given** a database bundle was created on machine A
   **When** the bundle directory is copied to machine B and opened with `database_manager::open`
   **Then** the database opens successfully and all game data is accessible (FR41)

4. **Given** any database bundle
   **When** `manifest_test` round-trips the manifest struct through glaze serialize→deserialize
   **Then** all fields match the original (AR05 round-trip requirement)
   **And** all tests pass under `dev-sanitize`

## Tasks / Subtasks

- [x] Add glaze dependency to build system (prerequisite — required before any implementation)
  - [x] Get explicit user approval to add `glaze` to `flake.nix` buildInputs (see dev notes — required by project rule)
  - [x] Add `glaze` to `vcpkg.json` dependencies
  - [x] Add `find_package(glaze REQUIRED)` and `find_package(fmt REQUIRED)` to `source/motif/db/CMakeLists.txt`
  - [x] Link `glaze::glaze` and `fmt::fmt` to `motif_db` in CMakeLists.txt

- [x] Define `db_manifest` struct and glaze serialization (AC: #4)
  - [x] Create `source/motif/db/manifest.hpp` with `db_manifest` struct in `motif::db`
  - [x] Fields: `name` (string), `schema_version` (uint32), `game_count` (uint64), `created_at` (string ISO 8601)
  - [x] Add serialize/deserialize free functions returning `result<void>` / `result<db_manifest>`
  - [x] No exceptions; use `tl::expected` throughout

- [x] Implement `schema` component (AC: #1, #2)
  - [x] Create `source/motif/db/schema.hpp` declaring `schema::initialize(sqlite3*)` → `result<void>`
  - [x] Create `source/motif/db/schema.cpp` with full DDL: all 6 tables (game, player, event, tag, game_tag, schema_migrations) and `PRAGMA user_version = 1`
  - [x] All table DDL uses `CREATE TABLE IF NOT EXISTS` — idempotent re-initialization must never fail or drop data
  - [x] Add `schema::version(sqlite3*)` → `result<uint32_t>` to query current PRAGMA user_version
  - [x] Set `PRAGMA foreign_keys = ON` and `PRAGMA journal_mode = WAL` in `initialize()`

- [x] Implement `database_manager` create and open (AC: #1, #2, #3)
  - [x] Create `source/motif/db/database_manager.hpp` with class `database_manager` in `motif::db`
  - [x] `create(std::filesystem::path const& dir, std::string const& name)` → `result<database_manager>`
  - [x] `open(std::filesystem::path const& dir)` → `result<database_manager>`
  - [x] `close()` releases SQLite connection; `~database_manager()` calls `close()`
  - [x] Expose `game_store& store()` accessor so callers can perform CRUD
  - [x] `create` must fail if bundle already exists (return `error_code::io_failure`)
  - [x] `open` must fail with `error_code::schema_mismatch` if PRAGMA user_version != expected

- [x] Wire build for new source files (AC: #1–#4)
  - [x] Add `database_manager.cpp`, `manifest.cpp`, `schema.cpp` to `motif_db` in `source/motif/db/CMakeLists.txt`
  - [x] Preserve existing `game_store.cpp` and all current link targets

- [x] Write tests (AC: #1–#4)
  - [x] `test/source/motif_db/database_manager_test.cpp` — create bundle, open bundle, schema_mismatch on wrong version, portability (copy to temp dir and reopen)
  - [x] `test/source/motif_db/manifest_test.cpp` — glaze round-trip: serialize all fields → deserialize → compare field by field
  - [x] `test/source/motif_db/schema_test.cpp` — initialize fresh connection, call initialize twice (idempotent), version query
  - [x] Add all three new test files to `motif_db_test` in `test/CMakeLists.txt`

- [x] Validate build, tests, sanitizers (AC: #4)
  - [x] `cmake --preset=dev && cmake --build build/dev`
  - [x] `ctest --test-dir build/dev`
  - [x] `cmake --preset=dev-sanitize && cmake --build build/dev-sanitize`
  - [x] `ctest --test-dir build/dev-sanitize`

### Review Findings

- [x] [Review][Patch] Update the repo language constraint to C++23 to match the Glaze-driven standard bump [CLAUDE.md:10]
- [x] [Review][Patch] Replace throwing `std::filesystem::exists(...)` calls in public APIs with non-throwing error-code handling [source/motif/db/database_manager.cpp:111]
- [x] [Review][Patch] Remove partially created bundle files when `database_manager::create()` fails after opening SQLite [source/motif/db/database_manager.cpp:121]
- [x] [Review][Patch] Make `store()` safe after `close()` or move instead of dereferencing an empty optional [source/motif/db/database_manager.cpp:199]
- [x] [Review][Patch] Derive manifest `schema_version` from the schema constant instead of hardcoding `1` [source/motif/db/manifest.cpp:35]

Review follow-up note: attempted local verification after applying fixes, but `cmake` and `ctest` are unavailable in the current shell environment, so build/test revalidation could not be run from this session.

---

## Dev Notes

### STOP: Dependency Approval Required First

**`glaze` is not yet in `flake.nix` buildInputs.** Before writing any implementation code:

1. Tell the user: "Story 1.4 requires adding `glaze` to `flake.nix` buildInputs and `vcpkg.json`. Do I have approval to modify `flake.nix`?"
2. Wait for explicit approval before modifying `flake.nix`.
3. Glaze 7.3.3 is in nixpkgs as `pkgs.glaze`. The foolnotion overlay may supply a newer version; consult `pkgs.glaze` as the default. Add it to the `buildInputs` list in the `packages.default` derivation alongside `fmt`.
4. `fmt` is already in flake.nix `buildInputs` but is not yet linked into `motif_db`. Add `find_package(fmt REQUIRED)` and `target_link_libraries(... fmt::fmt ...)` to `source/motif/db/CMakeLists.txt` — no flake.nix change needed for fmt.
5. Add `"glaze"` to `vcpkg.json` `dependencies` array (for future Windows port parity).

CMake find call for glaze: `find_package(glaze REQUIRED)` — the CMake target is `glaze::glaze`.

### Architecture Notes

- `database_manager` is defined in `source/motif/db/` and lives in `motif::db`. It is the public-facing entry point for the bundle lifecycle. All downstream code (import, search) gets a `database_manager&` rather than raw SQLite/DuckDB handles.
- The bundle directory layout at this story is:
  ```
  <bundle-dir>/
    games.db        ← SQLite WAL (created/opened by database_manager)
    manifest.json   ← glaze-serialized db_manifest (written on create; read on open)
  ```
  DuckDB (`positions.duckdb`) is NOT part of this story — it is added in Story 1.5. Do not create it here.
- `schema.hpp` is the authoritative owner of all SQLite DDL. `game_store::create_schema()` continues to exist for test fixtures (in-memory connections used in Story 1.3 tests) but is not called by `database_manager`. `database_manager` calls `schema::initialize()` instead.
- `schema::initialize()` re-uses the same `CREATE TABLE IF NOT EXISTS` pattern already established in `game_store::create_schema()`. The schema_migrations table is the one new addition:
  ```sql
  CREATE TABLE IF NOT EXISTS schema_migrations (
      name       TEXT NOT NULL PRIMARY KEY,
      applied_at TEXT NOT NULL
  );
  ```
- Current schema version constant: define `constexpr std::uint32_t k_schema_version = 1;` in `schema.hpp`. Both `initialize()` (which sets the pragma) and the mismatch check in `open` use this constant.

### `db_manifest` Struct and glaze Serialization

```cpp
// source/motif/db/manifest.hpp
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include "motif/db/error.hpp"

namespace motif::db {

struct db_manifest {
    std::string   name;
    std::uint32_t schema_version{1};
    std::uint64_t game_count{0};
    std::string   created_at;   // ISO 8601, e.g. "2026-04-18T14:30:00Z"
};

auto write_manifest(std::filesystem::path const& path,
                    db_manifest const& m) -> result<void>;

auto read_manifest(std::filesystem::path const& path)
    -> result<db_manifest>;

} // namespace motif::db
```

glaze serializes aggregates without metadata specializations. Use `glz::write_json` and `glz::read_json`. If glaze requires metadata (older API), use:
```cpp
template <>
struct glz::meta<motif::db::db_manifest> {
    using T = motif::db::db_manifest;
    static constexpr auto value = glz::object(
        "name",           &T::name,
        "schema_version", &T::schema_version,
        "game_count",     &T::game_count,
        "created_at",     &T::created_at
    );
};
```
Field names are `lower_snake_case` — they map directly to JSON keys (P6 in architecture). All fields are plain types; no enum fields in this struct, so no `glz::enumerate` needed here.

For the timestamp: use `fmt::format("{:%Y-%m-%dT%H:%M:%SZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()))`.

### `database_manager` API Shape

```cpp
// source/motif/db/database_manager.hpp
#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include "motif/db/error.hpp"
#include "motif/db/game_store.hpp"
#include "motif/db/manifest.hpp"

struct sqlite3;

namespace motif::db {

class database_manager {
public:
    ~database_manager();

    // Non-copyable; movable.
    database_manager(database_manager const&)                    = delete;
    auto operator=(database_manager const&) -> database_manager& = delete;
    database_manager(database_manager&&) noexcept;
    auto operator=(database_manager&&) noexcept -> database_manager&;

    // Factory methods (return by value via expected).
    static auto create(std::filesystem::path const& dir,
                       std::string const& name) -> result<database_manager>;

    static auto open(std::filesystem::path const& dir) -> result<database_manager>;

    // Access the underlying game store for CRUD operations.
    [[nodiscard]] auto store() noexcept -> game_store&;
    [[nodiscard]] auto store() const noexcept -> game_store const&;

    // Manifest accessor (read-only; update is an internal concern).
    [[nodiscard]] auto manifest() const noexcept -> db_manifest const&;

    void close() noexcept;

private:
    database_manager() = default;  // use factory methods

    struct impl;
    std::unique_ptr<impl> m_impl;
};

} // namespace motif::db
```

The `impl` pimpl holds: `sqlite3*` connection, `game_store` instance (initialized with the connection), `db_manifest`. This keeps the header clean of sqlite3.h exposure.

**`create` logic:**
1. Check that `dir` does not already contain `games.db` — return `error_code::io_failure` if so.
2. `std::filesystem::create_directories(dir)` if dir does not exist.
3. Open SQLite at `dir / "games.db"` with `sqlite3_open`.
4. Call `schema::initialize(conn)` — creates all 6 tables, sets WAL, sets foreign_keys, sets user_version.
5. Build `db_manifest{.name=name, .schema_version=1, .game_count=0, .created_at=<now>}`.
6. Call `write_manifest(dir / "manifest.json", manifest)`.
7. Construct and return the `database_manager` with all state set.

**`open` logic:**
1. Check that `dir / "games.db"` exists — return `error_code::not_found` if missing.
2. Check that `dir / "manifest.json"` exists — return `error_code::not_found` if missing.
3. Open SQLite at `dir / "games.db"`.
4. Read `PRAGMA user_version` via `schema::version(conn)`.
5. Compare to `k_schema_version` — return `error_code::schema_mismatch` if not equal.
6. Call `read_manifest(dir / "manifest.json")` → propagate error on failure.
7. Enable `PRAGMA foreign_keys = ON` (does NOT call `schema::initialize` — tables already exist).
8. Return the constructed `database_manager`.

### SQLite WAL Mode

Call `PRAGMA journal_mode = WAL` in `schema::initialize()` (only, not in `open`). WAL mode persists across re-opens once set; re-issuing the pragma on open is harmless but not required. For portability (AC3): WAL mode stores state in `games.db` itself; the `-wal` and `-shm` files are transient and do not need to be copied for portability.

### Clang-Tidy Suppressions (Inherited from Story 1.3)

Same contradictory checks will fire in anonymous namespaces:
- `llvm-prefer-static-over-anonymous-namespace` vs `misc-use-anonymous-namespace` — keep anonymous namespace; add `// NOLINT(llvm-prefer-static-over-anonymous-namespace)` to the namespace opening.
- `bugprone-unchecked-optional-access` fires after `REQUIRE(result.has_value())` in Catch2 tests — suppress with `// NOLINT(bugprone-unchecked-optional-access)` on the `.value()` call only.

### Test Guidance

**`manifest_test.cpp` — AC4 round-trip:**
```cpp
TEST_CASE("manifest: glaze round-trip preserves all fields", "[motif-db]") {
    motif::db::db_manifest const original{
        .name           = "test-db",
        .schema_version = 1,
        .game_count     = 42,
        .created_at     = "2026-04-18T12:00:00Z",
    };
    // Write to temp file, read back, compare field by field.
    // Use std::filesystem::temp_directory_path() / "manifest_roundtrip_test.json"
}
```

**`database_manager_test.cpp` — portability (AC3):**
```cpp
// Use two separate temp directories. Create bundle in dir_a, copy entire
// directory to dir_b (std::filesystem::copy_options::recursive), open in dir_b.
// Verify open succeeds and store().get(game_id) returns the game.
```

**`schema_test.cpp` — idempotency:**
```cpp
// Open :memory: SQLite, call schema::initialize() twice.
// Both calls must return result without error.
// Call schema::version() and verify it equals k_schema_version.
```

All tests must use temp directories (not `:memory:`) for database_manager tests because WAL + manifest require actual filesystem paths. Use `std::filesystem::temp_directory_path()` and clean up with RAII or `REQUIRE`-based teardown.

### File List

**Files to create:**
- `source/motif/db/database_manager.hpp`
- `source/motif/db/database_manager.cpp`
- `source/motif/db/manifest.hpp`
- `source/motif/db/manifest.cpp`
- `source/motif/db/schema.hpp`
- `source/motif/db/schema.cpp`
- `test/source/motif_db/database_manager_test.cpp`
- `test/source/motif_db/manifest_test.cpp`
- `test/source/motif_db/schema_test.cpp`

**Files to modify:**
- `source/motif/db/CMakeLists.txt` — add new source files; add `find_package(glaze REQUIRED)`, `find_package(fmt REQUIRED)`; link `glaze::glaze`, `fmt::fmt`
- `test/CMakeLists.txt` — add three new test files to `motif_db_test`
- `vcpkg.json` — add `"glaze"` to dependencies

**Files NOT to modify:**
- `source/motif/db/game_store.hpp` / `game_store.cpp` — Story 1.3 is done; `game_store::create_schema()` stays intact for test fixture use
- `source/motif/db/types.hpp`, `error.hpp`, `move_codec.hpp` — stable contracts

### Previous Story Intelligence (1.3 Learnings)

- `txn_guard` RAII pattern for SQLite transactions works well — apply the same pattern in `database_manager` if any write operations need transactions.
- `sqlite3_exec()` with a multi-statement SQL string is fine for DDL; wrap it in a small `exec(sqlite3*, char const*)` → `result<void>` helper (same as game_store's anonymous-namespace `exec` helper).
- `PRAGMA foreign_keys = ON` must be set per-connection and verified (SQLite can silently ignore it). Call it in both `initialize()` and `open()`.
- `game_store` tests operate on in-memory SQLite (`:memory:`) opened and schema-initialized independently. Story 1.4 tests use on-disk temp paths. These are entirely separate code paths — no conflict.
- `insert_options` / `skip_duplicates` was removed; `insert()` always returns `error_code::duplicate` on conflict. Story 1.4 does not change this.

### References

- [Epics: Story 1.4 acceptance criteria] — authoritative ACs
- [Architecture: "Database Model"] — bundle layout, dual-store model, create/open/close/rebuild responsibilities
- [Architecture: "Schema Versioning"] — PRAGMA user_version + schema_migrations table
- [Architecture: "P6 — Serialization (glaze)"] — glaze field naming, enum serialization, round-trip test requirement
- [Architecture: "Complete Project Directory Structure"] — expected file layout for database_manager, manifest, schema
- [Architecture: "Implementation Patterns P2"] — tl::expected error handling, monadic chains
- [Project context: "Storage layer"] — WAL mode, SQLite/DuckDB not transactionally coupled
- [Story 1.3 completed file] — game_store API, txn_guard pattern, exec helper, clang-tidy NOLINT patterns
