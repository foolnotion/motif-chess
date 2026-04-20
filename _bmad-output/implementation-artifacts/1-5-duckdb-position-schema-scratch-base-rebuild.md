# Story 1.5: DuckDB Position Schema, Scratch Base & Rebuild

Status: done

## Story

As a developer,
I want the DuckDB position table defined, an ephemeral in-memory scratch base available, and a rebuild operation that re-derives the DuckDB store from SQLite,
so that the dual-store architecture is complete and recoverable from any DuckDB inconsistency.

## Acceptance Criteria

1. **Given** an open database bundle
   **When** `position_store::initialize_schema` is called
   **Then** the `position` table exists in DuckDB with columns: `zobrist_hash UBIGINT NOT NULL`, `game_id UINTEGER NOT NULL`, `ply USMALLINT NOT NULL`, `result TINYINT NOT NULL`, `white_elo SMALLINT`, `black_elo SMALLINT` (AR08)

2. **Given** the `position` table has been populated (manually in test)
   **When** `database_manager::rebuild_position_store` is called
   **Then** the DuckDB `position` table is dropped and repopulated from all games in SQLite (NFR09)
   **And** row count in DuckDB after rebuild equals the sum of move counts across all SQLite games

3. **Given** motif-chess starts
   **When** `scratch_base::instance()` is called
   **Then** a single in-memory database is returned (SQLite `:memory:` + DuckDB in-memory) that is never written to disk (AR07)
   **And** calling `scratch_base::instance()` twice returns the same instance

4. **Given** an existing database bundle
   **When** a user runs `sqlite3 games.db .tables` or `duckdb positions.duckdb "SELECT count(*) FROM position"`
   **Then** the schema is visible and queries execute successfully without motif-chess running (FR08)
   **And** all tests pass under `dev-sanitize`

## Tasks / Subtasks

- [x] Wire DuckDB into the build system
  - [x] Add `"duckdb"` to `vcpkg.json` dependencies (DuckDB is already in `flake.nix` line 78 — no flake change needed)
  - [x] Add `find_package(DuckDB REQUIRED)` to `source/motif/db/CMakeLists.txt`
  - [x] Link DuckDB target to `motif_db` in CMakeLists.txt; mark DuckDB includes SYSTEM to suppress header diagnostics
  - [x] Verify `cmake --preset=dev && cmake --build build/dev` succeeds after the change

- [x] Extend `types.hpp` with `position_row` struct (AC: #1, #2)
  - [x] Add `position_row` struct to `source/motif/db/types.hpp` in `motif::db` namespace
  - [x] Fields: `zobrist_hash` (uint64_t), `game_id` (uint32_t), `ply` (uint16_t), `result` (int8_t), `white_elo` (optional<int16_t>), `black_elo` (optional<int16_t>)
  - [x] Do NOT remove the existing `position` struct — it may be used elsewhere

- [x] Implement `position_store` (AC: #1, #2)
  - [x] Create `source/motif/db/position_store.hpp` with class `position_store` in `motif::db`
  - [x] Constructor takes `duckdb_connection` (C API handle — see Dev Agent Record)
  - [x] `initialize_schema() -> result<void>` — creates `position` table and `idx_position_zobrist_hash` index (idempotent)
  - [x] `insert_batch(std::span<position_row const>) -> result<void>` — uses DuckDB C appender for fast bulk insert
  - [x] `row_count() -> result<std::int64_t>` — for test assertions
  - [x] Create `source/motif/db/position_store.cpp` with implementation

- [x] Extend `database_manager` with DuckDB lifecycle and rebuild (AC: #1, #2, #4)
  - [x] Add `duckdb_database` and `duckdb_connection` members to `database_manager` private state
  - [x] Update `database_manager::create` to open `positions.duckdb` and call `position_store::initialize_schema`
  - [x] Update `database_manager::open` to open existing `positions.duckdb`; `initialize_schema` is idempotent
  - [x] Update `database_manager::close` to disconnect and close DuckDB
  - [x] Add `position_store& positions() noexcept` accessor (const overload too) to `database_manager`
  - [x] Implement `database_manager::rebuild_position_store() -> result<void>`
  - [x] Declare `rebuild_position_store` in `source/motif/db/database_manager.hpp`

- [x] Implement `scratch_base` singleton (AC: #3)
  - [x] Create `source/motif/db/scratch_base.hpp` with class `scratch_base` in `motif::db`
  - [x] `static auto instance() -> scratch_base&` — Meyer's singleton
  - [x] Delete copy/move constructors and assignment operators
  - [x] Expose `game_store& store() noexcept` and `position_store& positions() noexcept`
  - [x] Create `source/motif/db/scratch_base.cpp` — constructor opens SQLite `:memory:`, calls `schema::initialize`, calls `position_store::initialize_schema`; DuckDB opened with `nullptr` path for in-memory mode

- [x] Update CMakeLists.txt for new source files
  - [x] Add `position_store.cpp` and `scratch_base.cpp` to `motif_db` in `source/motif/db/CMakeLists.txt`

- [x] Write tests (AC: #1–#4)
  - [x] `test/source/motif_db/position_store_test.cpp`:
    - `initialize_schema` creates the table (verify via `row_count()` == 0)
    - `initialize_schema` is idempotent (call twice, no error)
    - `insert_batch` inserts rows; `row_count()` matches
    - Null elo columns accepted (optional fields)
    - Round-trip: insert row, query DuckDB directly, verify columns
  - [x] Extend `test/source/motif_db/database_manager_test.cpp`:
    - `create` produces `positions.duckdb` in the bundle dir
    - `rebuild_position_store` on empty DB returns 0 rows
    - `rebuild_position_store` after inserting N-move game returns N rows
    - `rebuild_position_store` is idempotent (second call produces same row count)
  - [x] `test/source/motif_db/scratch_base_test.cpp`:
    - `instance()` returns a valid object
    - `instance()` called twice returns same address
    - `store().create_schema()` succeeds on scratch base
    - `positions().initialize_schema()` succeeds on scratch base
  - [x] Add `position_store_test.cpp` and `scratch_base_test.cpp` to `motif_db_test` in `test/CMakeLists.txt`

- [x] Validate build, tests, sanitizers
  - [x] `cmake --preset=dev && cmake --build build/dev`
  - [x] `ctest --test-dir build/dev` — 52/52 passed
  - [x] `cmake --preset=dev-sanitize && cmake --build build/dev-sanitize`
  - [x] `ctest --test-dir build/dev-sanitize` — 52/52 passed

### Review Findings

- [x] [Review][Patch] Ensure `database_manager::create()` cleans up partial bundle state when DuckDB schema initialization fails [source/motif/db/database_manager.cpp:233]
- [x] [Review][Patch] Validate `rebuild_position_store()` narrowing conversions before storing ELO and ply values in `int16_t`/`uint16_t` fields [source/motif/db/database_manager.cpp:396]
- [x] [Review][Patch] Avoid double-closing the SQLite handle on DuckDB open/init failure paths in `create()` and `open()` [source/motif/db/database_manager.cpp:252]
- [x] [Review][Patch] Detect SQLite iteration errors in `rebuild_position_store()` instead of treating partial `SELECT id FROM game` reads as success [source/motif/db/database_manager.cpp:407]
- [x] [Review][Patch] Make `rebuild_position_store()` atomic so failures do not leave DuckDB empty or partially rebuilt after `DELETE FROM position` [source/motif/db/database_manager.cpp:386]

---

## Dev Notes

### CRITICAL: DuckDB is Already in flake.nix

DuckDB appears in `flake.nix` at line 78 (`duckdb`). **No flake.nix modification needed and no user approval required.** Only `vcpkg.json` and the CMakeLists.txt need updating.

### DuckDB CMake Package Name

The exact CMake package name from the Nix DuckDB derivation must be verified by running `cmake --preset=dev` after adding a `find_package(...)` call and observing the error. Common candidates:

```cmake
find_package(duckdb REQUIRED)      # target: duckdb
find_package(DuckDB REQUIRED)      # target: DuckDB::duckdb or DuckDB
```

If neither works, use pkg-config fallback:
```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(duckdb REQUIRED IMPORTED_TARGET duckdb)
# then link: PkgConfig::duckdb
```

The DuckDB C++ header is `<duckdb.hpp>`. If the Nix package places it differently, adjust the include path via `target_include_directories`.

### Table Name: `position`, Not `position_index`

The architecture section titled "DuckDB PositionIndex Schema" uses `position_index` in the SQL DDL example — this is a naming error in the document. The authoritative names are:
- **P3 naming convention:** `position` (singular, no `_index` suffix)
- **AR08:** explicitly says `position` table

Use `position` everywhere. The index is named `idx_position_zobrist_hash` per P3 convention.

```sql
CREATE TABLE IF NOT EXISTS position (
    zobrist_hash  UBIGINT   NOT NULL,
    game_id       UINTEGER  NOT NULL,
    ply           USMALLINT NOT NULL,
    result        TINYINT   NOT NULL,
    white_elo     SMALLINT,
    black_elo     SMALLINT
);
CREATE INDEX IF NOT EXISTS idx_position_zobrist_hash ON position(zobrist_hash);
```

### `position_store` API Shape

```cpp
// source/motif/db/position_store.hpp
#pragma once
#include <cstdint>
#include <span>
#include "motif/db/error.hpp"
#include "motif/db/types.hpp"

namespace duckdb { class Connection; }

namespace motif::db {

class position_store {
public:
    explicit position_store(duckdb::Connection& con) noexcept;

    auto initialize_schema() -> result<void>;
    auto insert_batch(std::span<position_row const> rows) -> result<void>;
    auto row_count() -> result<std::int64_t>;

private:
    duckdb::Connection* m_con;  // non-owning
};

} // namespace motif::db
```

DuckDB `Appender` is the correct tool for bulk insert:
```cpp
#include <duckdb.hpp>

auto position_store::insert_batch(std::span<position_row const> rows) -> result<void> {
    try {
        duckdb::Appender appender(*m_con, "position");
        for (auto const& row : rows) {
            appender.BeginRow();
            appender.Append<uint64_t>(row.zobrist_hash);
            appender.Append<uint32_t>(row.game_id);
            appender.Append<uint16_t>(row.ply);
            appender.Append<int8_t>(row.result);
            if (row.white_elo.has_value()) {
                appender.Append<int16_t>(*row.white_elo);
            } else {
                appender.AppendNull();
            }
            if (row.black_elo.has_value()) {
                appender.Append<int16_t>(*row.black_elo);
            } else {
                appender.AppendNull();
            }
            appender.EndRow();
        }
        appender.Close();
        return {};
    } catch (std::exception const& e) {
        return tl::unexpected{error_code::io_failure};
    }
}
```

The `try/catch` is correct here: DuckDB's C++ API throws on error. This is the module boundary where exceptions are caught and converted to `tl::expected`. Do not propagate DuckDB exceptions across this boundary.

### `position_row` Struct

Add to `source/motif/db/types.hpp` (after the existing `position` struct — do not remove `position`):

```cpp
struct position_row {
    std::uint64_t             zobrist_hash{};
    std::uint32_t             game_id{};
    std::uint16_t             ply{};
    std::int8_t               result{};         // 1=white wins, 0=draw, -1=black wins
    std::optional<std::int16_t> white_elo;
    std::optional<std::int16_t> black_elo;
};
```

### `database_manager` Extension

The `database_manager` header currently has direct members (not pimpl). Extend with DuckDB members:

```cpp
// database_manager.hpp additions
#include "motif/db/position_store.hpp"
namespace duckdb { class DuckDB; class Connection; }

class database_manager {
public:
    // ... existing API ...
    [[nodiscard]] auto positions() noexcept -> position_store&;
    [[nodiscard]] auto positions() const noexcept -> position_store const&;
    auto rebuild_position_store() -> result<void>;

private:
    // ... existing members ...
    std::unique_ptr<duckdb::DuckDB>       m_duck_db;
    std::unique_ptr<duckdb::Connection>   m_duck_con;
    std::optional<position_store>         m_positions;
};
```

Use `unique_ptr` for DuckDB objects to keep `#include <duckdb.hpp>` out of the header (forward-declare `duckdb::DuckDB` and `duckdb::Connection`). Only include `<duckdb.hpp>` in `database_manager.cpp`.

**`create` updates:**
After writing manifest, add:
```cpp
m_duck_db  = std::make_unique<duckdb::DuckDB>((dir / "positions.duckdb").string());
m_duck_con = std::make_unique<duckdb::Connection>(*m_duck_db);
m_positions.emplace(*m_duck_con);
auto schema_result = m_positions->initialize_schema();
if (!schema_result) return tl::unexpected{schema_result.error()};
```

**`open` updates:**
Open `positions.duckdb`; if it doesn't exist, create it and call `initialize_schema`. Tolerate its absence (portability: the SQLite bundle may have been copied without the DuckDB file).
```cpp
m_duck_db  = std::make_unique<duckdb::DuckDB>((dir / "positions.duckdb").string());
m_duck_con = std::make_unique<duckdb::Connection>(*m_duck_db);
m_positions.emplace(*m_duck_con);
// Always call initialize_schema on open — it's idempotent
if (auto r = m_positions->initialize_schema(); !r) return tl::unexpected{r.error()};
```

**`close` updates:**
```cpp
m_positions.reset();
m_duck_con.reset();
m_duck_db.reset();
// then existing: sqlite3_close(m_conn); m_conn = nullptr;
```

### `rebuild_position_store` Algorithm

```
1. Truncate: run DuckDB query "DELETE FROM position" (faster than DROP+recreate — schema preserved)
2. Query all game IDs from SQLite: "SELECT id FROM game ORDER BY id"
3. For each game_id:
   a. game_store::get(game_id)  → game struct
   b. Map game.result string to int8_t:
        "1-0"       → 1
        "0-1"       → -1
        "1/2-1/2"   → 0
        "*" or else → 0
   c. Extract white_elo = game.white.elo (map int32_t → int16_t if present)
      Extract black_elo = game.black.elo (map int32_t → int16_t if present)
   d. Initialize chesslib board to starting position
   e. For each move in game.moves (index i, 0-based):
        board.apply(move)           ← advance board state
        position_row row{
            .zobrist_hash = board.zobrist_hash(),
            .game_id      = game_id,
            .ply          = static_cast<uint16_t>(i + 1),
            .result       = result_code,
            .white_elo    = white_elo,
            .black_elo    = black_elo,
        };
        batch.push_back(row);
4. After accumulating all rows (or per-batch), call position_store::insert_batch(batch)
5. Return result<void>{}
```

**Batch size:** Accumulate all rows per game before inserting, or use a fixed batch (e.g., 10,000 rows) to avoid unbounded memory. For the story's test scope (small games), per-game batches are fine.

**chesslib API for Zobrist hash:** Consult `chesslib` headers in the Nix store. Typical pattern:
```cpp
#include <chesslib/board.hpp>  // or similar — verify the actual header path
chesslib::Board board;         // default-constructed = starting position
board.apply_move(encoded_move); // may be apply(), make_move(), or similar
uint64_t hash = board.zobrist_hash(); // or board.hash()
```
Find the exact API with: `find $(nix eval --raw '.#packages.x86_64-linux.default.inputDerivation') -name "*.hpp" | head -20` (adapt to actual Nix output path), or grep `source/` for existing chesslib usage from Story 1.2.

### `scratch_base` Implementation

```cpp
// source/motif/db/scratch_base.hpp
#pragma once
#include <memory>
#include "motif/db/error.hpp"
#include "motif/db/game_store.hpp"
#include "motif/db/position_store.hpp"

namespace duckdb { class DuckDB; class Connection; }
struct sqlite3;

namespace motif::db {

class scratch_base {
public:
    static auto instance() -> scratch_base&;

    scratch_base(scratch_base const&)                    = delete;
    auto operator=(scratch_base const&) -> scratch_base& = delete;
    scratch_base(scratch_base&&)                         = delete;
    auto operator=(scratch_base&&) -> scratch_base&      = delete;

    [[nodiscard]] auto store() noexcept -> game_store&;
    [[nodiscard]] auto positions() noexcept -> position_store&;

private:
    scratch_base();
    ~scratch_base();

    sqlite3*                              m_conn{nullptr};
    std::unique_ptr<duckdb::DuckDB>       m_duck_db;
    std::unique_ptr<duckdb::Connection>   m_duck_con;
    std::optional<game_store>             m_store;
    std::optional<position_store>         m_positions;
};

} // namespace motif::db
```

```cpp
// source/motif/db/scratch_base.cpp
#include "motif/db/scratch_base.hpp"
#include <duckdb.hpp>
#include <sqlite3.h>
#include "motif/db/schema.hpp"

namespace motif::db {

// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
namespace {

// Helper: assert a result and terminate if failed (scratch base init must not fail).
void must(result<void> r) {
    if (!r) std::terminate();
}

} // namespace

scratch_base::scratch_base() {
    sqlite3_open(":memory:", &m_conn);
    must(schema::initialize(m_conn));
    m_store.emplace(m_conn);

    m_duck_db  = std::make_unique<duckdb::DuckDB>(nullptr); // in-memory
    m_duck_con = std::make_unique<duckdb::Connection>(*m_duck_db);
    m_positions.emplace(*m_duck_con);
    must(m_positions->initialize_schema());
}

scratch_base::~scratch_base() {
    m_positions.reset();
    m_duck_con.reset();
    m_duck_db.reset();
    m_store.reset();
    if (m_conn) {
        sqlite3_close(m_conn);
        m_conn = nullptr;
    }
}

auto scratch_base::instance() -> scratch_base& {
    static scratch_base s_instance;
    return s_instance;
}

auto scratch_base::store() noexcept -> game_store& {
    return *m_store;
}

auto scratch_base::positions() noexcept -> position_store& {
    return *m_positions;
}

} // namespace motif::db
```

`std::terminate()` in the constructor is intentional: scratch base failing to open in-memory storage is an unrecoverable startup error. No exception thrown across the boundary.

### Result Encoding

Map `game.result` (std::string from PGN) to `int8_t` for the DuckDB column:

```cpp
auto map_result(std::string const& pgn_result) -> std::int8_t {
    if (pgn_result == "1-0")     return 1;
    if (pgn_result == "0-1")     return -1;
    return 0;  // "1/2-1/2", "*", or anything else
}
```

Place this as a TU-local function in `database_manager.cpp`.

### Clang-Tidy Notes

- DuckDB's C++ API may trigger `cppcoreguidelines-pro-type-reinterpret-cast` or similar. Suppress on the include line or add DuckDB headers to the system-header suppress list in `.clang-tidy` if needed.
- The `try/catch` in `insert_batch` is the correct exception-to-expected boundary; no suppression needed.
- The `// NOLINT(llvm-prefer-static-over-anonymous-namespace)` suppression used in game_store.cpp applies here too for any anonymous namespace in `scratch_base.cpp`.

### Testing Approach for DuckDB Tests

All DuckDB tests use in-memory mode — never write to disk:
```cpp
duckdb::DuckDB db(nullptr);  // in-memory
duckdb::Connection con(db);
position_store store(con);
```

For `row_count()` in tests, use DuckDB's query API:
```cpp
auto res = con.Query("SELECT count(*) FROM position");
auto count = res->GetValue<int64_t>(0, 0);
```

Or use `position_store::row_count()` directly.

### File List

**Files to create:**
- `source/motif/db/position_store.hpp`
- `source/motif/db/position_store.cpp`
- `source/motif/db/scratch_base.hpp`
- `source/motif/db/scratch_base.cpp`
- `test/source/motif_db/position_store_test.cpp`
- `test/source/motif_db/scratch_base_test.cpp`

**Files to modify:**
- `source/motif/db/types.hpp` — add `position_row` struct
- `source/motif/db/database_manager.hpp` — add DuckDB members, `positions()` accessor, `rebuild_position_store()`
- `source/motif/db/database_manager.cpp` — open `positions.duckdb` in create/open/close; implement `rebuild_position_store`
- `source/motif/db/CMakeLists.txt` — add `find_package(duckdb ...)`, link DuckDB target, add new `.cpp` files
- `test/CMakeLists.txt` — add `position_store_test.cpp`, `scratch_base_test.cpp` to `motif_db_test`
- `vcpkg.json` — add `"duckdb"` to dependencies

**Files NOT to modify:**
- `source/motif/db/game_store.hpp` / `game_store.cpp` — Story 1.3 is done; add no methods
- `source/motif/db/schema.hpp` / `schema.cpp` — SQLite DDL; DuckDB DDL lives in `position_store`
- `source/motif/db/manifest.hpp` / `manifest.cpp` — no changes needed
- `source/motif/db/types.hpp` `position` struct — do not remove, only add `position_row`

### Previous Story Intelligence (1.4 Learnings)

- `database_manager` went with direct members (not pimpl) despite the story 1.4 dev notes suggesting pimpl. Follow the existing pattern: add DuckDB members directly.
- `sqlite3_close` is called in `close()` and the destructor guards with nullptr check. Apply the same guard to DuckDB cleanup.
- `// NOLINT(portability-avoid-pragma-once)` is the header guard suppression used in all existing headers — add it to new headers.
- `bugprone-unchecked-optional-access` fires after `REQUIRE(result.has_value())` in Catch2 tests — suppress with `// NOLINT(...)` on the `.value()` call only.
- The story 1.4 review added non-throwing filesystem checks (`std::error_code` overloads). Apply the same discipline in `database_manager.cpp` additions.
- Build/test verification after applying review fixes could not run due to cmake unavailability in the review session. Make sure to run `cmake --preset=dev && ctest --test-dir build/dev` as the final verification step.

### References

- [Epics: Story 1.5 acceptance criteria] — authoritative ACs
- [Architecture: "Database Model"] — bundle layout, `positions.duckdb` filename, scratch base semantics
- [Architecture: "DuckDB PositionIndex Schema"] — column types (use `position` table name, not `position_index`)
- [Architecture: "P3 — Database Naming"] — authoritative table/column/index naming rules
- [Architecture: "P2 — Error Handling"] — tl::expected, exception catch at module boundary
- [Architecture: "Module Structure"] — `motif_db` links DuckDB; no DuckDB headers in `motif_import` or `motif_search`
- [Story 1.4 completed file] — database_manager structure, clang-tidy patterns, review fix patterns
- [Story 1.3 completed file] — game_store API, txn_guard pattern, Catch2 test style

---

## Dev Agent Record

### Implementation Notes

**DuckDB C API (not C++) was used throughout.** DuckDB 1.5.1-dev's `profiling_utils.hpp` has an incomplete-type bug: `unique_ptr<ActiveTimer>` is used before `ActiveTimer` is fully defined. Clang 21 rejects this with "invalid application of sizeof to an incomplete type". Marking headers SYSTEM did not help (clang still instantiates templates). The definitive fix was to use the pure-C `<duckdb.h>` API exclusively — no C++ template instantiation, no issue.

**Consequence:** `position_store`, `database_manager`, and `scratch_base` all store raw C API handles (`duckdb_database`, `duckdb_connection`) as direct members, with explicit `duckdb_disconnect`/`duckdb_close` calls guarded by null checks in `close()` and destructors.

**chesslib API confirmed:** `chesslib::board` default-constructs to starting position; `chesslib::move_maker{board, decoded}.make()` applies a move in place; `board.hash()` returns the `uint64_t` Zobrist hash; `chesslib::codec::decode(encoded)` decodes a 16-bit encoded move.

**C++23 aggregate rule hit in tests:** `duck_fixture` had a user-declared destructor, making it non-aggregate in C++23. Added `duck_fixture() = default;` and initialized `con` via a helper function `make_duck_con(duckdb_database&)` so member initializers run in declaration order.

### Files Created
- `source/motif/db/position_store.hpp`
- `source/motif/db/position_store.cpp`
- `source/motif/db/scratch_base.hpp`
- `source/motif/db/scratch_base.cpp`
- `test/source/motif_db/position_store_test.cpp`
- `test/source/motif_db/scratch_base_test.cpp`

### Files Modified
- `source/motif/db/types.hpp` — added `position_row` struct
- `source/motif/db/database_manager.hpp` — added DuckDB C API members, `positions()` accessor, `rebuild_position_store()`
- `source/motif/db/database_manager.cpp` — DuckDB lifecycle, `rebuild_position_store()` implementation
- `source/motif/db/CMakeLists.txt` — `find_package(DuckDB REQUIRED)`, SYSTEM include, new sources
- `test/CMakeLists.txt` — added `position_store_test.cpp`, `scratch_base_test.cpp`
- `vcpkg.json` — added `"duckdb"` dependency

### Completion Status
- All 4 ACs satisfied
- 52/52 tests pass (`dev` build)
- 52/52 tests pass (`dev-sanitize` build — ASan + UBSan clean)

---

## Change Log

| Date       | Version | Description                                      | Author     |
|------------|---------|--------------------------------------------------|------------|
| 2026-04-18 | 1.0     | Initial implementation — all ACs complete        | Dev Agent  |
