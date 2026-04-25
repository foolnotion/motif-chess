---
stepsCompleted: ['step-01-init', 'step-02-context', 'step-03-starter', 'step-04-decisions', 'step-05-patterns', 'step-06-structure', 'step-07-validation', 'step-08-complete']
status: 'complete'
completedAt: '2026-04-14'
inputDocuments:
  - '_bmad-output/planning-artifacts/prd.md'
  - '_bmad-output/project-context.md'
  - 'plans/design.md'
workflowType: 'architecture'
project_name: 'motif-chess'
user_name: 'Bogdan'
date: '2026-04-14'
---

# Architecture Decision Document

_This document builds collaboratively through step-by-step discovery. Sections are appended as we work through each architectural decision together._

## Project Context Analysis

### Requirements Overview

**Functional Requirements (45 FRs across 8 categories):**

| Category | FRs | Phase | Architectural implication |
|---|---|---|---|
| Game Database Management | FR01–FR08 | MVP | Dual-store schema layer; idempotent schema init; portable DB files |
| PGN Import | FR09–FR15 | MVP | Parallel worker pipeline with checkpoint/resume; configurable memory ceiling |
| Position Search & Opening Stats | FR16–FR20 | MVP | DuckDB hot-path queries; lazy opening tree traversal; auto-indexing on import |
| User Interface | FR21–FR28 | MVP (basic) | Qt 6 main window; chessboard widget; game tree widget; keyboard-first nav |
| Engine Integration | FR29–FR32 | Phase 2 | ucilib subprocess isolation; async info delivery to UI |
| Statistical Analysis | FR33–FR36 | Phase 2 | Metadata join queries across SQLite + DuckDB; graceful degradation without clock data |
| Repertoire Management | FR37–FR38 | Phase 2 | Annotated line storage; drill mode (separate from game DB) |
| AI Repertoire Construction | FR43–FR45 | Phase 3 | ML inference layer; corpus analysis pipeline; out of architecture scope for now |

**Non-Functional Requirements — architectural drivers:**

- **NFR01/02:** 100ms P99 position lookup, 500ms opening stats → DuckDB schema optimized for `UBIGINT` column scan; no joins on the hot path
- **NFR03/04:** 10M games in <20 min, <2GB memory → taskflow DAG with bounded worker concurrency; streaming import, not batch-in-memory
- **NFR05/06:** 100ms UI interaction, 3s startup → no blocking calls on the Qt main thread; all DB access off-thread
- **NFR07/08:** Crash-safe, always resumable → explicit SQLite transaction commits + DuckDB checkpoints as invariant in import loop
- **NFR09:** SQLite authoritative; DuckDB derived → one-directional consistency model; rebuild command must be a first-class operation
- **NFR20:** taskflow as preferred task graph library → import pipeline and future statistical queries modeled as taskflow DAGs

**Scale & Complexity:**

- Primary domain: native desktop, high-performance data pipeline
- Complexity: **medium-high** — large data volumes (400M+ DuckDB rows), strict latency budgets, dual-store consistency model, ML roadmap, well-bounded scope, no multi-tenancy/network
- FR count: 45 (20 MVP, 16 Phase 2, 4 Phase 3 config/system, 5 Phase 3 AI)
- Estimated architectural components: 4 internal modules + 3 external libraries + 1 serialization library

### Technical Constraints & Dependencies

| Constraint | Impact |
|---|---|
| C++20, Clang 21, NixOS | No MSVC-isms, no Windows-only APIs; all build config via CMake 4.1.2 |
| `tl::expected<T,E>` everywhere | Every module boundary returns `expected`; no exception propagation across components |
| `fmt::format` not `std::format` | No `<format>` includes anywhere |
| SQLite (WAL) + DuckDB (C++ API) | Not transactionally coupled; no two-phase commit; DuckDB single-writer during import |
| 16-bit move encoding via `chesslib::codec` | Move blob format fixed; no motif-chess-level re-encoding |
| Zobrist hash owned by `chesslib` | Position identity is chesslib's contract; motif-chess never derives hashes independently |
| Qt 6, Widgets/QML choice deferred | GUI layer must be isolatable; all business logic must be Qt-free |
| taskflow preferred for concurrency | Import DAG, future stats pipeline designed as taskflow task graphs |
| glaze for serialization | Database manifests, app config, repertoire trees, annotations, import checkpoint state |
| `pgnlib` prereqs before spec 002 | Import pipeline cannot begin until pgnlib milestone is complete |
| `chesslib` prereqs before spec 001/002 | Schema and import blocked until chesslib milestone (16-bit encode, Zobrist, SAN) is complete |

### Database Model

A **database** (in the ChessBase sense) is a named, portable directory bundle:

```
<db-name>/
  games.db          ← SQLite WAL (game metadata, move blobs, player/event entities)
  positions.duckdb  ← DuckDB (position index, opening statistics) — derived, rebuildable
  manifest.json     ← glaze-serialized (name, description, created, game count, schema version)
```

Two backing modes are supported:

- **Persistent** — directory bundle on disk; appears in recent-databases list
- **Ephemeral (scratch/clipbase)** — SQLite `":memory:"` + DuckDB in-memory; singleton, always available, never persisted, never in recent list, cleared on exit

The **database manager** is a first-class architectural component responsible for: create, open, close, list, validate integrity, and rebuild-DuckDB-from-SQLite operations. Copy-game between any two open databases (including to/from scratch base) is a database manager operation.

### Cross-Cutting Concerns

1. **Parallelism & thread safety** — DuckDB is single-writer; SQLite WAL allows concurrent readers. The import pipeline's taskflow DAG serializes DuckDB writes. The UI query layer never touches import transactions.

2. **Error handling uniformity** — `tl::expected<T,E>` is the universal error channel. No exceptions cross module boundaries.

3. **Memory budget enforcement** — Import pipeline operates under a configurable ceiling (FR15, NFR04). Streaming tokenization through pgnlib + bounded worker concurrency via taskflow; bulk materialization is forbidden.

4. **Opening tree laziness** — Traversal to depth 30+ with branching factor 10–30. Every UI-facing query is paginated/lazy; full materialization is a non-starter.

5. **Data integrity & observability** — Every skipped game logged with full context. The import log is a structured artifact. Crash-safe invariants enforced at every SQLite/DuckDB commit boundary.

6. **Qt-free business logic** — All game logic, import coordination, search, statistics, and database management live in Qt-free components. The Qt GUI is a rendering layer over them. This keeps core logic testable (Catch2) and platform-portable.

7. **Library boundary discipline** — `chesslib` owns all chess logic; `pgnlib` owns text parsing; `ucilib` owns engine subprocess lifecycle. Motif-chess never re-derives Zobrist hashes, SAN, or move legality.

## Starter Template Evaluation

### Primary Technology Domain

Native C++ desktop application — no framework generator applies. The project scaffold is hand-crafted and already established.

### Project Scaffold (Existing Foundation)

**Build system:** CMake 4.1.2 with preset architecture

| Preset | Type | Build dir | Purpose |
|---|---|---|---|
| `dev` | Debug | `build/dev` | Day-to-day development |
| `dev-sanitize` | Debug + ASan/UBSan | `build/dev-sanitize` | Sanitizer gate before story completion |

**Compiler toolchain:** Clang 21 via `llvmPackages_21` (Nix). clang-tidy + cppcheck on every `dev` build.

**Dependency acquisition:** `find_package()` only. All deps declared in `flake.nix` (Nix) and `vcpkg.json` (Windows, deferred).

**Code quality gates (pre-commit):**
- `clang-format` (4-space indent, 80-col, left pointer alignment)
- Zero clang-tidy warnings
- Zero cppcheck warnings
- `cmake --preset=dev-sanitize` test suite must be clean

**Architectural decisions the scaffold makes:**
- Static libraries only (`add_library(... STATIC ...)`)
- `CMAKE_CXX_EXTENSIONS=OFF` — standard C++20 only
- `#pragma once` headers, no modules
- Test layout mirrors source layout under `test/source/`

### Libraries to Add to Scaffold

| Library | Purpose | In flake.nix? |
|---|---|---|
| `chesslib` | Board, SAN, Zobrist | Yes (flake input) |
| `pgnlib` | PGN parsing | Yes (flake input) |
| `ucilib` | UCI engine wrapper | Yes (flake input) |
| SQLite | Metadata store | To add |
| DuckDB | Position analytics | To add |
| Qt 6 | GUI (spec 004) | To add |
| taskflow | Task graph / parallelism | To add |
| glaze | JSON serialization | To add |
| tl::expected | Error handling | To add |
| fmt | String formatting | To add |
| Catch2 v3 | Testing | To add |

Adding any library requires updating both `flake.nix` (explicit approval required) and `vcpkg.json`. Resolving all MVP dependencies is the first spec 001 prerequisite story.

## Core Architectural Decisions

### Module Structure

Five static libraries with a strict one-directional dependency graph:

```
motif_db      — database manager, schema CRUD, migrations, scratch base
motif_import  — PGN import pipeline (taskflow DAG, workers, checkpoint/resume)
motif_search  — position search, opening statistics, lazy opening tree
motif_engine  — UCI engine integration via ucilib (Phase 2; stub only in Phase 1)
motif_app     — Qt 6 GUI; the only CMake target that links Qt
motif_ml      — AI/ML layer (Phase 3; not scaffolded until Phase 3 begins)
```

**Dependency direction:**
```
motif_app → motif_db, motif_import, motif_search, motif_engine
motif_import → motif_db
motif_search → motif_db
motif_engine → (ucilib only)
motif_db → (SQLite, DuckDB, glaze, chesslib)
```

No circular dependencies. `motif_import`, `motif_search`, and `motif_engine` are Qt-free. All business logic is testable without Qt overhead.

### GUI↔Backend Communication

**Qt worker threads + signals/slots (option A).** Backend operations run on `QThread` workers; results are emitted as typed Qt signals back to the main thread. Simple, idiomatic Qt, sufficient for a solo project at this scale.

### Schema Versioning

- **SQLite:** `PRAGMA user_version` as an integer schema version; a `schema_migrations` table records applied migration names as strings. Migration applied = insert name; migration needed = name not present.
- **DuckDB:** Schema version stored in the glaze `manifest.json`. DuckDB is derived data — schema mismatch triggers a full rebuild from SQLite, not a migration.

### DuckDB PositionIndex Schema

```sql
CREATE TABLE position_index (
    zobrist_hash  UBIGINT   NOT NULL,   -- 64-bit position identity (chesslib)
    game_id       UINTEGER  NOT NULL,   -- 32-bit; covers up to 4.3B games
    ply           USMALLINT NOT NULL,   -- half-move number within game
    result        TINYINT   NOT NULL,   -- 1=white wins, -1=black wins, 0=draw
    white_elo     SMALLINT,             -- NULL if not available in PGN
    black_elo     SMALLINT              -- NULL if not available in PGN
);
```

**Rationale:** Denormalized to avoid cross-store joins. Opening stats queries (`win%, move frequency, avg Elo`) are fully answerable from a single DuckDB column scan. `game_id UINTEGER` (vs UBIGINT) saves 4 bytes/row; at 50M games × 40 avg ply = 2B rows, estimated on-disk size ~10–15GB with DuckDB columnar compression.

### Opening Tree Traversal

**On-demand DuckDB query per node expansion** (fully lazy). Exception: when the user opens the opening tree, the first **5 levels** are prefetched eagerly in a single query to support the common workflow of clicking through the main line rapidly without per-move latency. Beyond depth 5, every expansion triggers an individual query. Prefetch depth is configurable in app config (default: 5).

### Import Checkpoint

glaze-serialized to `<db-dir>/import.checkpoint.json` during active import; deleted on clean completion:

```cpp
struct import_checkpoint {
    std::string  source_path;
    std::size_t  byte_offset;       // seek target in source PGN file
    std::int64_t games_committed;   // games fully written to both stores
    std::int64_t last_game_id;      // last SQLite game_id committed
};
```

On resume: seek to `byte_offset`, scan forward to next `[Event` tag, continue.

### C4 Diagrams

L1 (System Context), L2 (Container), and L3 (Component — import pipeline and search layer) produced as Mermaid C4 syntax (`C4Context`, `C4Container`, `C4Component`). Renders natively on GitHub and in VSCode without external tooling.

### Deferred Decisions

| Decision | Deferred Until |
|---|---|
| Qt Widgets vs QML vs hybrid | spec 004 |
| `motif_ml` module design | Phase 3 |
| Windows vcpkg port | Post-Linux feature-complete |
| macOS Nix build | Post-Linux feature-complete |

## Implementation Patterns & Consistency Rules

### P1 — Header Organization

- **`#pragma once`** in every header (no traditional include guards)
- **Include order** (enforced by clang-format):
  1. Standard library (`<algorithm>`, `<vector>`, …)
  2. Third-party (`<fmt/format.h>`, `<tl/expected.hpp>`, `<spdlog/spdlog.h>`, …)
  3. Project headers (`"motif/db/types.hpp"`, …)
- **Include paths** are relative to the module's `source/` root.
  Example: `#include "motif/db/game_store.hpp"` from within `motif_import`.
- No transitive includes — every `.cpp` includes what it directly uses.

### P2 — Error Handling

Two-tier model:

```cpp
// Tier 1 — fast, allocation-free error codes (per-module enum class)
namespace motif::db {
enum class error_code { ok, not_found, schema_mismatch, io_failure };
}

// Tier 2 — rich diagnostic struct (expensive path, human-readable only)
struct error_info {
    std::string        message;
    std::source_location location = std::source_location::current();
};
```

- **All public API functions** return `tl::expected<T, error_code>` (Tier 1).
- Tier 2 is used only for logging and user-facing error display; never as an `expected` error type.
- Monadic chains (`.and_then`, `.or_else`, `.transform`) preferred over explicit `if (!result)` guards.
- `result.value()` without a prior check is forbidden — clang-tidy will catch this via `bugprone-unchecked-optional-access`.
- Error code enums are module-scoped: `motif::db::error_code`, `motif::import::error_code`, etc. — no shared global enum.

### P3 — Database Naming

**SQLite tables:** singular `lower_snake_case` nouns.
Examples: `game`, `player`, `event`, `tag`, `game_tag` (junction).

**DuckDB tables:** singular `lower_snake_case`.
Example: `position` (not `position_index`, not `positions`).

**Column names:** `lower_snake_case`.
Examples: `zobrist_hash`, `game_id`, `white_elo`.

**Foreign keys:** `<referenced_table>_id`.
Example: `player_id` referencing `player(id)`.

**Indexes:** `idx_<table>_<column(s)>`.
Example: `idx_position_zobrist_hash`.

**SQLite schema version:** tracked in `PRAGMA user_version` (integer) + `schema_migrations` table (applied migration names as strings).

**DuckDB schema version:** stored in `manifest.json` (glaze-serialized). DuckDB is derived; schema mismatch → full rebuild, no migration.

### P4 — Qt Signals & Slots

- **Signals** follow project convention: `lower_snake_case`.
  Example: `Q_SIGNAL void import_progress(int games_done, int total);`
- **Slots** follow `on_<signal_name>` convention.
  Example: `Q_SLOT void on_import_progress(int games_done, int total);`
- MOC-generated files live in `build/` and are excluded from clang-tidy — no suppressions required in source headers.
- All DB and import operations run on `QThread` workers; results delivered to main thread via typed signals. **No blocking calls on the Qt main thread.**

### P5 — Logging

- **Library:** spdlog (async mode during import; sync acceptable for startup/shutdown).
- **Thread pool** for async logger initialized once at application startup (before any worker threads start); ownership lives in the application entry point.
- **Sinks:**
  - Primary: rotating file sink (`logs/motif-chess.log`, 5 MB × 3 files).
  - Optional JSON-lines sink: enabled via a boolean in app config (`logging.json_sink: true`); writes to `logs/motif-chess.jsonl`. Both sinks may be active simultaneously.
- **Log levels:** `trace` (hot-path debug), `debug`, `info` (operational milestones), `warn` (skipped games, schema warnings), `error` (recoverable failures), `critical` (unrecoverable; triggers flush + exit).
- **Format — text sink:** `[%Y-%m-%dT%H:%M:%S.%e] [%l] [%n] %v`
- **Format — JSON sink:** `{"ts":"%Y-%m-%dT%H:%M:%S.%e","level":"%l","logger":"%n","msg":"%v"}`
- **Logger names** match module namespaces: `motif.db`, `motif.import`, `motif.search`.
- spdlog is a Qt-free dependency; `motif_app` is the only target that may call Qt logging APIs.

### P6 — Serialization (glaze)

- **Scope:** database `manifest.json`, app config, repertoire trees, annotations, import checkpoint (`import.checkpoint.json`).
- **Field names in glaze structs:** `lower_snake_case`; map directly to JSON keys with no aliases.
- **Enum serialization:** specialize `glaze::meta` to emit string names, not integers.

  ```cpp
  template <>
  struct glaze::meta<motif::db::schema_version_status> {
      static constexpr auto value = glaze::enumerate(
          "ok",      motif::db::schema_version_status::ok,
          "outdated", motif::db::schema_version_status::outdated
      );
  };
  ```

- Every serializable struct must have a round-trip test (serialize → deserialize → compare).

### Namespace Conventions

- Namespaces mirror CMake target names: `motif::db`, `motif::import`, `motif::search`, `motif::engine`, `motif::app`.
- All public symbols live inside the module's namespace. No `using namespace` in headers.
- Anonymous namespaces for TU-local helpers in `.cpp` files.

### Shared Type Ownership

- Types shared across modules (`game`, `player`, `event`, `position`) are defined in `motif_db` — the base dependency that every other module already links.
- Other modules `#include "motif/db/types.hpp"` (or a more specific header) to access them.
- Forward declarations (e.g., `struct game;`) are used in headers of other modules when only a pointer or reference is needed, avoiding full inclusion.
- **No separate `motif_types` library.** All shared types ride with `motif_db`.

### Enforcement Summary

All agents MUST:

1. Return `tl::expected<T, motif::<module>::error_code>` from every public API function.
2. Name all identifiers `lower_snake_case` — including Qt signals. `CamelCase` is for template parameters only.
3. Use `#pragma once` in every header. No traditional include guards.
4. Place shared domain types in `motif_db`; include via `"motif/db/types.hpp"`.
5. Use `motif::<module>` namespace in every public header. Never `using namespace` in a header.
6. Serialize enums as strings (glaze `enumerate` specialization); include round-trip test.
7. Initialize spdlog async thread pool once at startup; use named loggers matching `motif.<module>` pattern.
8. Never call Qt APIs from `motif_db`, `motif_import`, `motif_search`, or `motif_engine`.

## Project Structure & Boundaries

### Complete Project Directory Structure

```
motif-chess/
├── CMakeLists.txt                    (root; adds subdirectories per module)
├── CMakePresets.json                 (dev, dev-sanitize presets)
├── cmake/                            (existing cmake helpers)
│   ├── dev-mode.cmake
│   ├── lint.cmake
│   ├── lint-targets.cmake
│   └── ...
├── source/
│   ├── motif/
│   │   ├── db/                       (motif_db — base dependency)
│   │   │   ├── CMakeLists.txt
│   │   │   ├── types.hpp             (game, player, event — shared domain types)
│   │   │   ├── error.hpp             (motif::db::error_code enum)
│   │   │   ├── database_manager.hpp
│   │   │   ├── database_manager.cpp
│   │   │   ├── game_store.hpp        (SQLite CRUD for game/player/event)
│   │   │   ├── game_store.cpp
│   │   │   ├── position_store.hpp    (DuckDB position index CRUD)
│   │   │   ├── position_store.cpp
│   │   │   ├── schema.hpp            (migrations, version check)
│   │   │   ├── schema.cpp
│   │   │   ├── manifest.hpp          (glaze-serialized bundle manifest)
│   │   │   └── scratch_base.hpp      (ephemeral in-memory DB singleton)
│   │   ├── import/                   (motif_import → motif_db)
│   │   │   ├── CMakeLists.txt
│   │   │   ├── error.hpp             (motif::import::error_code enum)
│   │   │   ├── import_pipeline.hpp   (taskflow DAG orchestrator)
│   │   │   ├── import_pipeline.cpp
│   │   │   ├── import_worker.hpp     (per-game processing unit)
│   │   │   ├── import_worker.cpp
│   │   │   ├── pgn_reader.hpp        (pgnlib adapter)
│   │   │   ├── pgn_reader.cpp
│   │   │   └── checkpoint.hpp        (import_checkpoint struct + glaze)
│   │   ├── search/                   (motif_search → motif_db)
│   │   │   ├── CMakeLists.txt
│   │   │   ├── error.hpp             (motif::search::error_code enum)
│   │   │   ├── position_search.hpp   (Zobrist-based position lookup)
│   │   │   ├── position_search.cpp
│   │   │   ├── opening_stats.hpp     (win%, frequency, avg Elo)
│   │   │   ├── opening_stats.cpp
│   │   │   ├── opening_tree.hpp      (lazy tree + 5-level prefetch)
│   │   │   └── opening_tree.cpp
│   │   ├── engine/                   (motif_engine — Phase 2 stub only)
│   │   │   ├── CMakeLists.txt
│   │   │   ├── error.hpp             (motif::engine::error_code enum)
│   │   │   ├── engine_manager.hpp
│   │   │   └── engine_manager.cpp
│   │   └── app/                      (motif_app — NOT scaffolded until spec 004)
├── test/
│   ├── CMakeLists.txt
│   └── source/
│       ├── motif_db/
│       │   ├── database_manager_test.cpp
│       │   ├── game_store_test.cpp
│       │   ├── position_store_test.cpp
│       │   ├── schema_test.cpp
│       │   └── manifest_test.cpp     (glaze round-trip)
│       ├── motif_import/
│       │   ├── import_pipeline_test.cpp
│       │   ├── pgn_reader_test.cpp
│       │   └── checkpoint_test.cpp   (glaze round-trip)
│       └── motif_search/
│           ├── position_search_test.cpp
│           ├── opening_stats_test.cpp
│           └── opening_tree_test.cpp
├── specs/
│   ├── 001-database-schema/spec.md
│   ├── 002-import-pipeline/spec.md
│   ├── 003-search/spec.md
│   └── 004-qt-gui/spec.md
├── plans/
│   └── design.md
├── docs/
│   └── devlog/
├── flake.nix
├── vcpkg.json
├── .clang-format
├── .clang-tidy
└── CLAUDE.md
```

**Runtime paths (not in-repo):**

```
<db-bundle-dir>/               (user-chosen location)
  games.db                     (SQLite WAL — authoritative)
  positions.duckdb             (DuckDB — derived, rebuildable)
  manifest.json                (glaze: name, version, game count)
  import.checkpoint.json       (present only during active import)

~/.config/motif-chess/
  config.json                  (glaze: logging, prefetch_depth, recent_dbs)

logs/
  motif-chess.log              (rotating text, always on)
  motif-chess.jsonl            (JSON-lines, opt-in via config)
```

### Architectural Boundaries

**Module boundaries and ownership:**

| Module | Owns | May depend on |
|---|---|---|
| `motif_db` | Schema, CRUD, shared types, DB bundle lifecycle | SQLite, DuckDB, chesslib, glaze, tl::expected, fmt |
| `motif_import` | PGN parsing, taskflow DAG, checkpoint | `motif_db`, pgnlib, chesslib, taskflow, spdlog |
| `motif_search` | Position lookup, opening stats, lazy tree | `motif_db`, chesslib, spdlog |
| `motif_engine` | UCI engine subprocess lifecycle | ucilib |
| `motif_app` | Qt GUI (spec 004) | All above modules + Qt 6 |

**Strict prohibitions:**
- `motif_db`, `motif_import`, `motif_search`, `motif_engine` must never include any Qt header.
- `motif_import` and `motif_search` must never access DuckDB or SQLite directly — all storage goes through `motif_db` APIs.
- No module may include headers from another module it does not explicitly depend on.

### FR Category → Module Mapping

| FR Category | Module | Key files |
|---|---|---|
| Game Database Management (FR01–FR08) | `motif_db` | `database_manager`, `game_store`, `schema`, `manifest` |
| PGN Import (FR09–FR15) | `motif_import` | `import_pipeline`, `import_worker`, `pgn_reader`, `checkpoint` |
| Position Search & Opening Stats (FR16–FR20) | `motif_search` | `position_search`, `opening_stats`, `opening_tree` |
| User Interface (FR21–FR28) | `motif_app` | (spec 004) |
| Engine Integration (FR29–FR32) | `motif_engine` | `engine_manager` |
| Statistical Analysis (FR33–FR36) | `motif_search` | `opening_stats` extensions |
| Repertoire Management (FR37–FR38) | `motif_db` + `motif_app` | TBD at spec 004 |

### Include Path Convention

- `source/` is the include root for all targets.
- Includes always use the full path from root: `#include "motif/db/types.hpp"`.
- No bare `#include "types.hpp"` within a module — always fully qualified.
- Forward declarations in headers: `namespace motif::db { struct game; }` when only pointer/reference needed.

### Test Organization

- Test executable per module: `motif_db_test`, `motif_import_test`, `motif_search_test`.
- Test files mirror source paths: `test/source/motif_db/game_store_test.cpp` ↔ `source/motif/db/game_store.cpp`.
- Test naming: `TEST_CASE("game_store: insert deduplicates players", "[motif-db]")`.
- All tests use real in-memory SQLite (`:memory:`) and DuckDB instances — no mocks for storage layer.

## Architecture Validation Results

### Coherence Validation

**Decision compatibility:** All technology choices are mutually consistent. spdlog uses fmt internally, aligning with the fmt-only string formatting rule. tl::expected and glaze are both Nix-provided, no version conflicts. SQLite WAL + DuckDB single-writer model is consistent; the architecture explicitly prohibits two-phase commit across them. taskflow is Qt-free, consistent with the Qt-boundary rule.

**Pattern consistency:** `lower_snake_case` everywhere is enforced by clang-tidy and reflected in naming patterns for DB columns, Qt signals, logger names (`motif.db`), glaze fields, and namespace names (`motif::db`). No contradictions found.

**Structure alignment:** `source/motif/db/` mirrors `motif::db` namespace. Test layout mirrors source. Static libraries only, no SHARED targets.

### Requirements Coverage

| FR Group | Status | Module |
|---|---|---|
| FR01–FR08 DB Management | Covered | `motif_db`: database_manager, game_store, schema, scratch_base |
| FR09–FR15 PGN Import | Covered | `motif_import`: import_pipeline (taskflow), checkpoint, pgn_reader |
| FR16–FR20 Search/Stats | Covered | `motif_search`: position_search, opening_stats, opening_tree |
| FR21–FR28 GUI | Deferred | `motif_app` scaffolded at spec 004 |
| FR29–FR32 Engine | Phase 2 stub | `motif_engine`: engine_manager (minimal) |
| FR33–FR36 Stats | Covered | `motif_search` opening_stats extensions |
| FR37–FR38 Repertoire | Deferred | TBD at spec 004 |

NFR coverage: NFR01/02 (latency) → DuckDB denormalized schema; NFR03/04 (throughput/memory) → streaming taskflow pipeline; NFR05/06 (UI latency) → QThread workers; NFR07/08 (crash-safe) → import_checkpoint; NFR09 (SQLite authoritative) → rebuild in database_manager; NFR20 (taskflow) → import_pipeline.

### Gaps — Must Address Before Spec 001

**G1 — `cmake_minimum_required` floor is wrong.**
The existing root `CMakeLists.txt` declares `VERSION 3.14`. The floor must be set to the minimum that covers all CMake features actually in use: `CMakePresets.json` alone requires 3.19; the cmake helper files may require higher. The correct action is to audit `cmake/` helpers and set the floor to the highest feature requirement found — not to the installed version.

**G2 — Remove template placeholder files.**
`source/lib.cpp`, `source/lib.hpp`, and `source/main.cpp` are scaffold artifacts. They must be removed and the root `CMakeLists.txt` restructured (`add_subdirectory` per module) as part of spec 001's first story.

**G3 — `test/CMakeLists.txt` is monolithic.**
Currently defines a single `motif_test` executable. Must be restructured into per-module test executables (`motif_db_test`, `motif_import_test`, `motif_search_test`) each with `catch_discover_tests`, as part of spec 001.

### Gaps — No Blocker

**G4 — C4 diagrams not yet generated.**
Committed to in D7. Should be authored in `docs/architecture/` as a standalone docs task (not blocking any spec).

**G5 — `motif_engine` has no test files.**
Correct for a Phase 2 stub. No test coverage expected until Phase 2 begins.

### Architecture Completeness Checklist

- [x] Technology stack fully specified with versions
- [x] Core decisions documented (D1–D7) with rationale
- [x] Implementation patterns defined (P1–P6, namespace convention, shared type ownership)
- [x] Module dependency graph (one-directional, no cycles)
- [x] FR → module mapping explicit
- [x] NFR → architectural mechanism mapping explicit
- [x] Complete directory structure with file-level detail
- [x] Qt-boundary rule concrete and enforceable
- [x] Error handling uniform contract
- [x] Logging strategy concrete
- [x] Serialization strategy concrete
- [x] Test strategy concrete (Catch2 v3, real DBs, sanitizer gate)
- [ ] cmake_minimum_required floor (G1 — fix before spec 001)
- [ ] C4 diagrams (G4 — standalone docs task)

### Readiness Assessment

**Overall:** Ready for spec 001 implementation, subject to resolving G1–G3 in the spec 001 first story.

**Confidence:** High — all decision points have explicit choices; the patterns section eliminates the main AI-agent drift vectors (namespace convention, shared type ownership, error handling, logging, serialization); module boundary rules are concrete and testable.
