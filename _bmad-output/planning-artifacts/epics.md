---
stepsCompleted: ['step-01-validate-prerequisites', 'step-02-design-epics', 'step-03-create-stories', 'step-04-final-validation']
status: 'complete'
completedAt: '2026-05-06'
inputDocuments:
  - '_bmad-output/planning-artifacts/prd.md'
  - '_bmad-output/planning-artifacts/architecture.md'
---

# motif-chess — Epic Breakdown

## Overview

This document provides the complete epic and story breakdown for motif-chess, decomposing the requirements from the PRD and Architecture into implementable stories.

## Requirements Inventory

### Functional Requirements

**Game Database Management**
- FR01: The user can create and open a named game database stored at a user-specified file system path
- FR02: The user can insert a game with full metadata (players, event, site, date, result, ECO, extra PGN tags) and encoded move sequence
- FR03: The system deduplicates players and events on insert — the same name always resolves to the same record
- FR04: The system detects duplicate games on insert and skips or rejects them based on import configuration
- FR05: The user can retrieve a complete game (metadata + move sequence) by game ID
- FR06: The user can delete a game and all associated data (tags, position index entries, opening stats)
- FR07: The database schema is created idempotently — opening an existing database never overwrites or errors
- FR08: The user can inspect the database with standard CLI tools (`sqlite3`, `duckdb`) independent of motif-chess

**PGN Import**
- FR09: The user can import one or more PGN files into the active database
- FR10: The import pipeline processes multiple games concurrently using available CPU cores
- FR11: Malformed or unreadable games are skipped and logged with identifying context (game number, headers, error reason); import continues
- FR12: The user can interrupt an import and resume it from the last committed checkpoint without re-importing already-committed games
- FR13: The user can monitor import progress (games processed, games committed, games skipped, elapsed time)
- FR14: The import pipeline reports a structured summary on completion (total, committed, skipped, errors)
- FR15: The import pipeline enforces a configurable memory ceiling during processing

**Position Search & Opening Statistics**
- FR16: The user can search for any position by its Zobrist hash and retrieve all games that reached that position
- FR17: For any position, the user can view opening statistics: move frequency, win/draw/loss rates by color, average Elo of games
- FR18: The user can navigate an opening tree from any starting position, with per-node statistics
- FR19: The opening tree is traversed lazily — only nodes the user expands are loaded
- FR20: The position index is populated automatically during import

**User Interface (MVP)**
- FR21: The user can view a chessboard displaying any position from the active game
- FR22: The user can navigate through a game move by move (forward, backward, to start, to end)
- FR23: The user can view and navigate a game tree including variations
- FR24: The user can initiate a PGN import from within the UI and monitor its progress
- FR25: The user can search for a position from the current board state and view results
- FR26: The user can open, browse, and filter the game list in the active database
- FR27: The UI supports high-DPI displays
- FR28: The user can perform all core navigation actions via keyboard without requiring the mouse

**Engine Integration (Phase 2)**
- FR29: The user can configure one or more UCI-compatible chess engines by specifying their executable path
- FR30: The user can start and stop engine analysis on the current board position
- FR31: The user can view engine output (depth, score, principal variation) updating in real time
- FR32: The engine integration isolates engine crashes — an engine failure does not crash motif-chess

**Statistical Analysis (Phase 2)**
- FR33: The user can view their performance statistics by opening, broken down by result, color, and opponent rating range
- FR34: The user can view error distribution across game phases (opening, middlegame, endgame) for their personal game corpus
- FR35: The user can view time-management statistics for games where clock data is available in PGN
- FR36: Statistical analysis degrades gracefully when metadata (clock data, Elo) is absent from source games

**Repertoire Management (Phase 2)**
- FR37: The user can build and save an opening repertoire as a set of annotated lines
- FR38: The user can drill repertoire lines in a practice mode

**Configuration & System**
- FR39: The user can configure the active database directory, recently used databases, engine paths, and UI preferences
- FR40: Configuration is stored per-user and independently of database files
- FR41: Database files are portable — moving or copying them to another machine preserves all game data
- FR42: The application registers as a handler for `.pgn` files at the OS level

**AI Repertoire Construction (Phase 3)**
- FR43: The system can analyze a player's game corpus to identify playing style, opening strengths, and theoretical weaknesses
- FR44: The system can generate a personalized opening repertoire proposal based on the player's profile and corpus analysis
- FR45: The generated repertoire accounts for the player's current strength and preferred piece structures

### Non-Functional Requirements

**Performance**
- NFR01: Position search (Zobrist lookup) completes in under 100ms at P99 for a 10M-game corpus
- NFR02: Opening statistics query (move frequency, win rates) completes in under 500ms for any position
- NFR03: A 10M-game corpus imports in under 20 minutes on reference hardware (modern multi-core desktop)
- NFR04: Memory usage during import does not exceed 2GB regardless of corpus size
- NFR05: UI interactions (board navigation, move forward/back, opening tree expansion) respond within 100ms
- NFR06: The application starts and presents a usable window in under 3 seconds on first launch
- NFR20: CPU-intensive operations exploit task-level parallelism across available cores; taskflow is the preferred task graph library

**Reliability**
- NFR07: A crash or power loss during import leaves the database in a consistent, queryable state — no corruption, no partial game commits
- NFR08: Import can always be resumed from the last checkpoint after an interrupted session
- NFR09: The SQLite and DuckDB stores are eventually consistent: DuckDB is derived data and can always be rebuilt from SQLite; SQLite is authoritative
- NFR10: Malformed PGN input never causes a crash — all error paths are handled and logged
- NFR11: The test suite passes with zero errors under ASan and UBSan (`cmake --preset=dev-sanitize`)

**Code Quality**
- NFR12: Every build under the `dev` preset produces zero clang-tidy warnings
- NFR13: Every build under the `dev` preset produces zero cppcheck warnings
- NFR14: Every public API function has at least one Catch2 v3 test
- NFR15: All code is formatted with clang-format before commit; CI enforces this

**Integration**
- NFR16: PGN import correctly handles all legal PGN including recursive variations, NAGs, comments, and `%clk` clock annotations
- NFR17: FEN representation is standard and compatible with any FEN-consuming tool
- NFR18: UCI engine communication is compatible with Stockfish 17+, Leela Chess Zero, and Komodo Dragon
- NFR19: Exported or displayed SAN is unambiguous and valid per FIDE algebraic notation standard

### Additional Requirements (Architecture)

- **AR01:** Pre-spec-001 scaffold: remove template placeholder files (`source/lib.cpp`, `source/lib.hpp`, `source/main.cpp`); restructure root `CMakeLists.txt` to `add_subdirectory` per module; correct `cmake_minimum_required` to the minimum version required by features in use (audit `cmake/` helpers; CMakePresets.json requires ≥ 3.19)
- **AR02:** Per-module test executables (`motif_db_test`, `motif_import_test`, `motif_search_test`) with `catch_discover_tests` — replace monolithic `motif_test`
- **AR03:** Five static library modules with strict one-directional dependency graph: `motif_db` → `motif_import` → `motif_search` → `motif_engine` → `motif_app`; no Qt headers in any non-app module
- **AR04:** 16-bit move encoding via `chesslib::codec` (CPW convention) for `moves_blob` in the `game` table — no alternative encoding
- **AR05:** glaze serialization for: database manifest (`manifest.json`), app config (`~/.config/motif-chess/config.json`), import checkpoint (`import.checkpoint.json`); every serializable struct requires a round-trip test
- **AR06:** spdlog with async mode during import; rotating text sink always-on; optional JSON-lines sink via config flag; named loggers matching `motif.<module>` pattern
- **AR07:** Ephemeral scratch base (in-memory SQLite + DuckDB singleton) — always available, never persisted, never in recent list
- **AR08:** DuckDB `position` table schema: `(zobrist_hash UBIGINT, game_id UINTEGER, ply USMALLINT, result TINYINT, white_elo SMALLINT, black_elo SMALLINT)` — denormalized, no cross-store joins on hot path
- **AR09:** Import checkpoint (`import_checkpoint` struct) serialized via glaze to `<db-dir>/import.checkpoint.json`; deleted on clean completion; resume by seeking to `byte_offset` then scanning to next `[Event` tag
- **AR10:** Opening tree first-5-levels prefetch on open; configurable via `prefetch_depth` in app config (default: 5); beyond depth 5 all expansion is on-demand DuckDB query

### UX Design Requirements

_No UX Design document exists. GUI is deferred to spec 004 (Phase 1 MVP, later spec). UX requirements will be extracted when the UX design document is created prior to spec 004._

### FR Coverage Map

- FR01: Epic 1 — create/open named database at user-specified path
- FR02: Epic 1 — insert game with full metadata and encoded move sequence
- FR03: Epic 1 — deduplicate players and events on insert
- FR04: Epic 1 — detect and skip/reject duplicate games on insert
- FR05: Epic 1 — retrieve complete game by game ID | Epic 4b — HTTP endpoint GET /games/{id}
- FR06: Epic 1 — delete game and all associated data
- FR07: Epic 1 — idempotent schema creation
- FR08: Epic 1 — database inspectable with standard CLI tools
- FR09: Epic 2 — import one or more PGN files | Epic 4b — HTTP endpoint POST /imports
- FR10: Epic 2 — parallel multi-game processing
- FR11: Epic 2 — skip and log malformed games
- FR12: Epic 2 — interrupt and resume import from last checkpoint
- FR13: Epic 2 — monitor import progress | Epic 4b — SSE progress stream
- FR14: Epic 2 — structured completion summary | Epic 4b — SSE final event
- FR15: Epic 2 — configurable memory ceiling
- FR16: Epic 3 — search position by Zobrist hash | Epic 4b — HTTP endpoint GET /positions/{hash}
- FR17: Epic 3 — view opening statistics per position | Epic 4b — HTTP endpoint GET /openings/{hash}/stats
- FR18: Epic 3 — navigate opening tree with per-node statistics
- FR19: Epic 3 — lazy opening tree traversal
- FR20: Epic 2 — position index populated automatically during import
- FR21: Epic 7 — chessboard widget displaying current position | Epic 4d — legal moves and move validation endpoints for interactive board rendering
- FR22: Epic 7 — move-by-move game navigation
- FR23: Epic 7 — game tree view with variations
- FR24: Epic 7 — initiate PGN import and monitor progress from UI
- FR25: Epic 7 — position search from current board state
- FR26: Epic 7 — game list browser with filtering | Epic 4b — HTTP endpoint GET /games
- FR27: Epic 7 — high-DPI display support
- FR28: Epic 7 — full keyboard navigation
- FR29: Epic 4 — configure UCI engine by executable path | Epic 4d — engine API contract
- FR30: Epic 4 — start/stop engine analysis | Epic 4d — HTTP start/stop contract
- FR31: Epic 4 — view real-time engine output | Epic 4d — SSE stream contract
- FR32: Epic 4 — engine crash isolation
- FR33: Epic 5 — performance statistics by opening
- FR34: Epic 5 — error distribution by game phase
- FR35: Epic 5 — time-management statistics
- FR36: Epic 5 — graceful degradation without clock/Elo data
- FR37: Epic 6 — build and save annotated repertoire lines
- FR38: Epic 6 — repertoire drill mode
- FR39: Epic 7 — configure database directory, recent DBs, engine paths, UI preferences
- FR40: Epic 7 — per-user config independent of database files
- FR41: Epic 1 — database portability (move/copy preserves all data)
- FR42: Epic 7 — .pgn file handler registration at OS level
- FR43: Epic 8 — corpus analysis for style/strength/weakness profiling
- FR44: Epic 8 — generate personalized repertoire proposal
- FR45: Epic 8 — repertoire accounts for player strength and piece structures

**AR Coverage:**
- AR01–AR03: Epic 1 Story 1.1 (scaffold setup)
- AR04: Epic 1 (16-bit move encoding)
- AR05: Epic 1 (manifest glaze) + Epic 2 (checkpoint glaze)
- AR06: Epic 2 (spdlog async logging)
- AR07: Epic 1 (scratch base)
- AR08: Epic 1 (DuckDB schema) + Epic 3 (queries)
- AR09: Epic 2 (import checkpoint struct and resume)
- AR10: Epic 3 (opening tree 5-level prefetch)

**Epic 4b Note:** No new ARs — Epic 4b is a thin HTTP adapter over existing stable APIs. All architectural constraints (module boundaries, dependency direction, no Qt in non-app modules) continue to apply.

## Epic List

### Epic 1: Game Database Foundation
Users can create and manage a chess game database — storing games with full metadata, deduplicating players and events, retrieving and deleting games, and inspecting the raw database files with standard CLI tools. Includes project scaffold setup.
**FRs covered:** FR01–FR08, FR41
**ARs covered:** AR01–AR05, AR07, AR08
**NFRs covered:** NFR07, NFR09, NFR11–NFR15, NFR17, NFR19

### Epic 2: PGN Import Pipeline
Users can import thousands of PGN games in parallel, survive a power loss mid-import and resume without re-importing committed games, and receive a structured log of any skipped or malformed games. The position index is built automatically as games are committed.
**FRs covered:** FR09–FR15, FR20
**ARs covered:** AR06, AR09, AR10 (index population)
**NFRs covered:** NFR03, NFR04, NFR07, NFR08, NFR10, NFR16, NFR20

### Epic 3: Position Search & Opening Statistics
Users can find any board position by Zobrist hash, view move frequency and win/draw/loss statistics, and explore a lazily-loaded opening tree with per-node performance data. First 5 levels of the tree are prefetched for fast click-through.
**FRs covered:** FR16–FR19
**ARs covered:** AR10 (prefetch)
**NFRs covered:** NFR01, NFR02

### Epic 4b: HTTP API Layer
An HTTP API layer between motif-chess and any future frontend (Qt, web, or experimental), exposing the core database, search, and import capabilities as REST endpoints. The OpenAPI spec becomes a versioned interface boundary.
**FRs covered:** FR05, FR09, FR13, FR14, FR16, FR17, FR26
**NFRs covered:** NFR01, NFR02, NFR10
**Dependencies:** All met — `position_search`, `opening_stats`, `opening_tree`, `import_pipeline`, `database_manager` are stable and tested.

### Epic 4d: Web Frontend API Prerequisites
Before interactive web UI work depends on the local HTTP API, the backend exposes the missing chessboard, engine-analysis, and personal-game management prerequisites: legal moves and move validation/application for a FEN position, a stable HTTP/SSE contract for UCI engine analysis, a domain-native CRUD API for user-added games, and browser-compatible import and game-count endpoints.
**FRs covered:** FR02, FR05, FR06, FR09, FR21, FR24, FR26, FR29–FR31
**NFRs covered:** NFR05, NFR09, NFR10, NFR18, NFR19
**Dependencies:** Epic 4c complete; `chesslib` owns move legality/SAN/FEN; `ucilib` owns UCI subprocess lifecycle.

### Epic 7: Desktop Application *(DEFERRED — API prerequisites take priority)*
Users interact with the full database through a native Qt 6 desktop GUI — browsing games, navigating positions on a chessboard, running imports, searching positions, and configuring the application — all keyboard-accessible on high-DPI displays.
**FRs covered:** FR21–FR28, FR39–FR40, FR42
**NFRs covered:** NFR05, NFR06

### Epic 4: Engine Integration *(Phase 2)*
Users can configure UCI engines and run real-time analysis on any position through backend-owned engine lifecycle APIs; engine crashes are isolated and never affect the application or connected frontend.
**FRs covered:** FR29–FR32
**NFRs covered:** NFR18

### Epic 5: Statistical Analysis *(Phase 2)*
Users view personal performance correlated across openings, game phases, opponent rating ranges, and time management — degrading gracefully when clock data or Elo is absent.
**FRs covered:** FR33–FR36

### Epic 6: Repertoire Management *(Phase 2)*
Users build and save annotated opening repertoire lines and drill them in practice mode.
**FRs covered:** FR37–FR38

### Epic 8: AI Repertoire Construction *(Phase 3)*
Users receive a personalized opening repertoire generated from analysis of their corpus, playing style, and theoretical gaps.
**FRs covered:** FR43–FR45

---

## Epic 1: Game Database Foundation

Users can create and manage a chess game database — storing games with full metadata, deduplicating players and events, retrieving and deleting games, and inspecting the raw database files with standard CLI tools. Includes project scaffold setup.

### Story 1.1: Project Scaffold Restructure

As a developer,
I want the project build system restructured into per-module static libraries with per-module test executables,
So that each module can be developed, built, and tested independently without touching unrelated code.

**Acceptance Criteria:**

**Given** the current root `CMakeLists.txt` declares `VERSION 3.14` and contains a monolithic template library
**When** the scaffold restructure story is complete
**Then** `cmake_minimum_required` is set to the minimum version required by features actually in use (audit `cmake/` helpers; CMakePresets.json requires ≥ 3.19)
**And** `source/lib.cpp`, `source/lib.hpp`, `source/main.cpp` are removed
**And** `source/motif/db/`, `source/motif/import/`, `source/motif/search/`, `source/motif/engine/` directories exist, each with a stub `CMakeLists.txt` defining an empty static library target
**And** `test/CMakeLists.txt` defines per-module test executables: `motif_db_test`, `motif_import_test`, `motif_search_test`, each with `catch_discover_tests`
**And** `cmake --preset=dev && cmake --build build/dev` succeeds with zero clang-tidy and zero cppcheck warnings
**And** `ctest --test-dir build/dev` passes (no tests yet, but the harness runs)

### Story 1.2: Domain Types, Move Encoding & Error Contracts

As a developer,
I want the core domain types, move encoding, and error handling contracts defined in `motif_db`,
So that all other modules have a stable, shared foundation for representing chess games without duplicating or reinventing these definitions.

**Acceptance Criteria:**

**Given** the `motif_db` module scaffold exists
**When** the domain types story is complete
**Then** `source/motif/db/types.hpp` defines `game`, `player`, `event`, and `position` structs in the `motif::db` namespace with `lower_snake_case` members and `#pragma once`
**And** `source/motif/db/error.hpp` defines `motif::db::error_code` as an `enum class` covering at minimum: `ok`, `not_found`, `schema_mismatch`, `io_failure`, `duplicate`
**And** `source/motif/db/move_codec.hpp` wraps `chesslib::codec` encode/decode for `uint16_t` moves; tests verify round-trip encode→decode for all move types (quiet, capture, promotion, castling, en passant)
**And** all public functions return `tl::expected<T, motif::db::error_code>`; no exceptions thrown
**And** `motif_db_test` contains at least one test per public function; all pass under `dev-sanitize`

### Story 1.3: SQLite Game Store

As a developer,
I want a fully-tested SQLite game store that can insert, retrieve, and delete chess games with player/event deduplication,
So that the database layer correctly persists game data with referential integrity.

**Acceptance Criteria:**

**Given** domain types from Story 1.2 exist
**When** a game is inserted via `game_store::insert`
**Then** the game is persisted to the `game`, `player`, `event`, and `game_tag` tables
**And** inserting a game where the white player name already exists reuses the existing `player` row (FR03)
**And** inserting a game where the event name already exists reuses the existing `event` row (FR03)
**And** inserting a duplicate game returns `error_code::duplicate` when `skip_duplicates` is set (FR04)

**Given** a game has been inserted with a known game ID
**When** `game_store::get` is called with that ID
**Then** the returned `game` struct contains the original metadata and the correct decoded move sequence (FR05)

**Given** a game has been inserted
**When** `game_store::remove` is called with that game's ID
**Then** the game row and all associated `game_tag` rows are deleted (FR06)
**And** the associated `player` and `event` rows are NOT deleted (they may be referenced by other games)

**Given** any test scenario
**Then** all tests use an in-memory SQLite instance (`:memory:`) — no on-disk files
**And** all tests pass under `cmake --preset=dev-sanitize` with zero ASan/UBSan violations (NFR11)

### Story 1.4: Database Manager, Schema Lifecycle & Manifest

As a developer and as a user,
I want to create, open, and close a named chess database bundle at a user-specified path, with idempotent schema initialization and a machine-readable manifest,
So that database files are portable and the schema is always in a known, valid state regardless of how many times the database is opened.

**Acceptance Criteria:**

**Given** a valid directory path
**When** `database_manager::create` is called
**Then** a database bundle is created containing `games.db` (SQLite WAL) and `manifest.json` (glaze-serialized with name, schema version, game count = 0, created timestamp) (FR01, AR05)
**And** the SQLite schema is initialized: `game`, `player`, `event`, `tag`, `game_tag`, `schema_migrations` tables created; `PRAGMA user_version` set to current schema version (FR07)

**Given** an existing database bundle
**When** `database_manager::open` is called on its path
**Then** the database is opened and `manifest.json` is deserialized; no tables are dropped or recreated (FR07)
**And** `PRAGMA user_version` matches the expected schema version; if it does not, `error_code::schema_mismatch` is returned

**Given** a database bundle was created on machine A
**When** the bundle directory is copied to machine B and opened with `database_manager::open`
**Then** the database opens successfully and all game data is accessible (FR41)

**Given** any database bundle
**When** `manifest_test` round-trips the manifest struct through glaze serialize→deserialize
**Then** all fields match the original (AR05 round-trip requirement)
**And** all tests pass under `dev-sanitize`

### Story 1.5: DuckDB Position Schema, Scratch Base & Rebuild

As a developer,
I want the DuckDB position table defined, an ephemeral in-memory scratch base available, and a rebuild operation that re-derives the DuckDB store from SQLite,
So that the dual-store architecture is complete and recoverable from any DuckDB inconsistency.

**Acceptance Criteria:**

**Given** an open database bundle
**When** `position_store::initialize_schema` is called
**Then** the `position` table exists in DuckDB with columns: `zobrist_hash UBIGINT NOT NULL`, `game_id UINTEGER NOT NULL`, `ply USMALLINT NOT NULL`, `result TINYINT NOT NULL`, `white_elo SMALLINT`, `black_elo SMALLINT` (AR08)

**Given** the `position` table has been populated (manually in test)
**When** `database_manager::rebuild_position_store` is called
**Then** the DuckDB `position` table is dropped and repopulated from all games in SQLite (NFR09)
**And** row count in DuckDB after rebuild equals the sum of move counts across all SQLite games

**Given** motif-chess starts
**When** `scratch_base::instance()` is called
**Then** a single in-memory database is returned (SQLite `:memory:` + DuckDB in-memory) that is never written to disk (AR07)
**And** calling `scratch_base::instance()` twice returns the same instance

**Given** an existing database bundle
**When** a user runs `sqlite3 games.db .tables` or `duckdb positions.duckdb "SELECT count(*) FROM position"`
**Then** the schema is visible and queries execute successfully without motif-chess running (FR08)
**And** all tests pass under `dev-sanitize`

---

## Epic 2: PGN Import Pipeline

Users can import thousands of PGN games in parallel, survive a power loss mid-import and resume without re-importing committed games, and receive a structured log of any skipped or malformed games. The position index is built automatically as games are committed.

### Story 2.1: spdlog Logger Infrastructure

As a developer,
I want a project-wide spdlog logger initialized once at startup with rotating text and optional JSON-lines sinks,
So that all modules can emit structured, levelled log output without duplicating logger setup or linking Qt.

**Acceptance Criteria:**

**Given** the logger infrastructure is initialized (before the import pipeline is first used)
**When** the logger is set up
**Then** a rotating text sink writes to `logs/motif-chess.log` (5 MB × 3 files); a JSON-lines sink writes to `logs/motif-chess.jsonl` only when `logging.json_sink: true` in app config; both sinks may be active simultaneously (AR06)
**And** named loggers `motif.db`, `motif.import`, `motif.search` are retrievable via `spdlog::get`
**And** the async thread pool is initialized before any worker threads start
**And** spdlog may be linked by `motif_db`, `motif_import`, and `motif_search`; no Qt headers are included in logger setup; Qt logging APIs are restricted to `motif_app` only
**And** a test verifies that emitting a log line at each level (`trace` through `critical`) does not crash under `dev-sanitize`

### Story 2.2: PGN Reader & pgnlib Adapter

As a developer,
I want a `pgn_reader` adapter over pgnlib that streams `pgn::game` structs one at a time from a PGN file,
So that the import pipeline can process games without loading the entire file into memory.

**Acceptance Criteria:**

**Given** a valid PGN file with N games
**When** `pgn_reader::next` is called repeatedly
**Then** each call returns the next `pgn::game` struct until the file is exhausted, then returns `tl::unexpected(error_code::eof)`
**And** memory usage does not grow proportionally with file size (streaming, not bulk load) (NFR04)

**Given** a PGN file containing a malformed game (truncated headers, invalid move)
**When** `pgn_reader::next` is called on that game
**Then** the malformed game is skipped; `next` returns `tl::unexpected(error_code::parse_error)` with the game number and available header context (FR11)
**And** calling `next` again successfully returns the following valid game

**Given** a valid PGN file
**When** `pgn_reader::seek_to_offset` is called with a byte offset followed by `next`
**Then** reading resumes from the next `[Event` tag at or after that offset (AR09 resume logic)
**And** all tests pass under `dev-sanitize` including the malformed-game case (NFR10)

### Story 2.3: Import Worker — Game Processing Unit

As a developer,
I want a single-game import worker that converts a `pgn::game` into a stored `game` row plus position index entries,
So that the pipeline has a correct, tested unit of work before parallelism is introduced.

**Acceptance Criteria:**

**Given** a valid `pgn::game` struct
**When** `import_worker::process` is called
**Then** the game is inserted into the SQLite game store (via `game_store::insert`) with all metadata fields populated
**And** for each half-move, one row is inserted into the DuckDB `position` table with the correct `zobrist_hash` (from chesslib), `game_id`, `ply`, `result`, `white_elo`, `black_elo` (FR20, AR08)
**And** SAN→move conversion uses `chesslib` exclusively; motif-chess does not reimplement move parsing (NFR19)

**Given** a `pgn::game` where the player name already exists in the database
**When** `import_worker::process` is called
**Then** the existing player record is reused — no duplicate player rows (FR03)

**Given** a `pgn::game` that is a duplicate of an already-committed game and `skip_duplicates` is set
**When** `import_worker::process` is called
**Then** the game is skipped; the function returns `error_code::duplicate` without inserting any rows (FR04)

**Given** a `pgn::game` with a `%clk` annotation in move comments
**When** `import_worker::process` is called
**Then** the clock data is stored in the move blob or metadata as available; absence of `%clk` does not cause an error (NFR16)
**And** all tests pass under `dev-sanitize`

### Story 2.4: Parallel Import Pipeline with Checkpoint/Resume

As a user,
I want to import a large PGN file using all available CPU cores and resume the import after an interruption from the last committed checkpoint,
So that I can process millions of games efficiently and never lose progress.

**Acceptance Criteria:**

**Given** a PGN file and a target database
**When** `import_pipeline::run` is called
**Then** the pipeline uses a taskflow DAG to process games across multiple worker threads (FR10, NFR20)
**And** the number of workers is bounded so memory usage stays under the configured ceiling (FR15, NFR04)
**And** `import.checkpoint.json` is written to `<db-dir>/` after each batch commit, containing `source_path`, `byte_offset`, `games_committed`, `last_game_id` (AR09)
**And** on clean completion `import.checkpoint.json` is deleted

**Given** an import was interrupted leaving `import.checkpoint.json`
**When** `import_pipeline::resume` is called
**Then** the pipeline seeks to `byte_offset` in the source file, scans forward to the next `[Event` tag, and continues without re-importing already-committed games (FR12, NFR07, NFR08)
**And** the final game count matches what would have been produced by a clean run

**Given** an import is running
**When** `import_pipeline::progress` is queried
**Then** it returns games processed, games committed, games skipped, and elapsed time (FR13)
**And** a `checkpoint_test` round-trips the `import_checkpoint` struct through glaze serialize→deserialize with all fields matching (AR05)
**And** a performance test importing 1M games completes in under 120 seconds on the CI machine (NFR03 partial gate)

### Story 2.5: Import Completion Summary & Error Logging

As a user,
I want a structured import summary on completion and a log entry for every skipped game with enough context to investigate later,
So that I can trust the import was complete and can audit any data that was excluded.

**Acceptance Criteria:**

**Given** an import has completed (clean or with skipped games)
**When** `import_pipeline::run` returns
**Then** the returned `import_summary` struct contains: total games attempted, games committed, games skipped, error count, and elapsed time (FR14)

**Given** a PGN file where game N is malformed (truncated, invalid SAN, missing result tag)
**When** the import pipeline processes game N
**Then** a `WARN`-level log entry is emitted via `motif.import` logger containing: game number in file, available PGN header fields (White, Black, Event), and the error reason (FR11)
**And** the import continues with game N+1 — the pipeline never aborts on a single bad game (FR11, NFR10)
**And** the skipped game count in the final summary increments by 1

**Given** a PGN file containing only malformed games
**When** the import pipeline runs
**Then** `import_summary.committed == 0` and `import_summary.skipped == N`
**And** no crash, no ASan/UBSan violations (NFR10, NFR11)

### Story 2.6: Adopt Upstream chesslib SAN Optimization

As a developer,
I want to adopt the upstream chesslib SAN resolution optimization,
so that the import pipeline benefits from the reported large isolated gains in SAN parsing speed.

### Story 2.7: Integrate pgnlib Import Stream into Import Pipeline

As a developer,
I want to integrate the upstream pgnlib `import_stream` API into the import pipeline,
so that PGN parsing and materialization gains are realized in the hot path.

### Story 2.8: Reprofile and Tune Remaining Import/Storage Path

As a developer,
I want to reprofile the import pipeline after the chesslib and pgnlib optimizations are adopted,
so that remaining bottlenecks in the SQLite/DuckDB storage path are identified and addressed.

### Story 2.9: Import Pipeline Performance Optimization

As a developer,
I want to optimize the import pipeline's performance to close the NFR03 gap,
by addressing memory locality, pipeline batching friction, and allocation overhead
identified through Story 2.8 profiling and cache miss analysis.

**Acceptance Criteria:**

**Given** the import pipeline memory hotspots are identified
**When** optimization changes are implemented
**Then** cache miss rate reduces by at least 15% (from 47% to ≤40%)
**And** this reduction is verified through perf stat on 10k games fixture

**Given** the taskflow pipeline batching friction is identified
**When** optimization changes are implemented
**Then** futex contention time reduces by at least 50% (from 8% to ≤4% of wall time)
**And** this reduction is verified through perf stat on 10k games fixture

**Given** the allocation pressure in hot paths is identified
**When** optimization changes are implemented
**Then** malloc/free rate in prepare_game and position generation reduces by at least 60%
**And** this reduction is verified through perf stat on 10k games fixture

**Given** all optimizations are implemented
**When** benchmarked on 10k games fixture with deferred position index build
**Then** wall time improves by at least 20% compared to post-2.8 baseline (13.5s)
**Or** extrapolated time for 10M games reaches ≤25 minutes (down from 3.7 hours)

**Given** any downstream tuning is applied
**Then** all Epic 2 correctness guarantees remain intact: malformed inputs never crash (NFR10); resume behavior is correct (NFR08); structured summary and enriched logging are unchanged (FR14, AR06); all tests pass under `dev-sanitize` with zero ASan/UBSan violations (NFR11)

**Given** the architectural boundary between import pipeline and storage is preserved
**When** optimizations are implemented
**Then** import pipeline continues to use motif_db::position_store public API only
**And** no direct DuckDB C API calls bypass motif_db abstraction

---

## Epic 3: Position Search & Opening Statistics

Users can find any board position by Zobrist hash, view move frequency and win/draw/loss statistics, and explore a lazily-loaded opening tree with per-node performance data. First 5 levels of the tree are prefetched for fast click-through.

### Story 3.1: Position Search by Zobrist Hash

As a user,
I want to look up any chess position by its Zobrist hash and retrieve all games in the database that reached that position,
So that I can instantly find every game where a specific position occurred.

**Acceptance Criteria:**

**Given** the DuckDB `position` table is populated
**When** `position_search::find` is called with a `zobrist_hash`
**Then** the function returns a list of `(game_id, ply, result, white_elo, black_elo)` rows matching that hash (FR16)
**And** the query completes in under 100ms at P99 for a database of 10M games (NFR01)

**Given** a Zobrist hash with no matching rows
**When** `position_search::find` is called
**Then** an empty result set is returned (not an error)

**Given** any search
**Then** Zobrist hash computation uses `chesslib` exclusively — motif-chess never derives hashes independently
**And** all tests pass under `dev-sanitize`

### Story 3.2: Opening Statistics per Position

As a user,
I want to see move frequency, win/draw/loss rates by color, and average Elo for all moves played from any position,
So that I can immediately understand how a position has been handled in practice.

**Acceptance Criteria:**

**Given** a position with multiple continuations in the database
**When** `opening_stats::query` is called with a `zobrist_hash`
**Then** the returned `opening_stats` struct contains for each continuation move: move (SAN), frequency (count), white wins, draws, black wins, average white Elo, average black Elo (FR17)
**And** the query completes in under 500ms for any position (NFR02)

**Given** a position where Elo data is absent for some games
**When** `opening_stats::query` is called
**Then** average Elo is computed only over games where Elo is non-null; positions with no Elo data return null for that field

**Given** a position not present in the database
**When** `opening_stats::query` is called
**Then** an empty `opening_stats` is returned with zero continuations
**And** all tests pass under `dev-sanitize`

### Story 3.3: Lazy Opening Tree with Prefetch

As a user,
I want to navigate an opening tree where the first 5 levels are prefetched on open and deeper nodes are loaded on demand,
So that I can click through the main line of an opening rapidly without per-move latency.

**Acceptance Criteria:**

**Given** a user opens the opening tree from the starting position
**When** `opening_tree::open` is called
**Then** the first 5 levels of the tree are prefetched in a single DuckDB query and held in memory (AR10)
**And** each node contains the statistics from Story 3.2 (move frequency, win/draw/loss, avg Elo, ECO code, opening name)

**Given** the user expands a node beyond depth 5
**When** `opening_tree::expand` is called on that node
**Then** a single on-demand DuckDB query fetches the children of that node only (FR19)
**And** nodes already in the prefetch cache are not re-queried

**Given** `prefetch_depth` is set to a non-default value in app config
**When** `opening_tree::open` is called
**Then** prefetching uses the configured depth instead of the default 5 (AR10)

**Given** any tree traversal
**Then** the tree is never fully materialized in memory — only expanded nodes are held (FR19)
**And** all tests pass under `dev-sanitize`

---

## Epic 4b: HTTP API Layer

An HTTP API layer between motif-chess and any future frontend (Qt, web, or experimental), exposing the core database, search, and import capabilities as REST endpoints. Provides a low-stakes experimentation space for features too risky or speculative for motif-chess proper. The OpenAPI spec becomes a versioned interface boundary.

**Pre-conditions (from Epic 3 retrospective):**
- Triage all deferred items — close or promote before Epic 4b starts
- Document `dominant_eco` tie-break rule in `opening_stats.hpp`

### Story 4b.1: HTTP Server Scaffold

As a developer,
I want an HTTP server scaffold using cpp-httplib with CORS support, database configuration at startup, and a health endpoint,
So that all subsequent endpoint stories have a running server with common infrastructure ready.

**Acceptance Criteria:**

**Given** the `motif_http` module is configured
**When** the server starts
**Then** it listens on a configured port (default: 8080) and exposes a `GET /health` endpoint returning HTTP 200 with `{ "status": "ok" }`
**And** CORS headers are present on all responses allowing cross-origin requests from any host during development
**And** the server opens or creates a database bundle at a path provided via CLI argument or environment variable
**And** a new `motif_http` static library module is created with the same one-directional dependency pattern as existing modules
**And** `cmake --preset=dev && cmake --build build/dev` succeeds with zero warnings
**And** `ctest --test-dir build/dev` passes including a test that verifies the health endpoint returns 200

### Story 4b.2: Position Search Endpoint

As a user,
I want to search for a chess position via HTTP and receive a list of matching games,
So that I can find every game where a specific position occurred without a GUI.

**Acceptance Criteria:**

**Given** the server is running and a database is loaded
**When** `GET /api/positions/{zobrist_hash}` is called
**Then** the response contains a JSON array of games that reached that position, each with `game_id`, `ply`, `result`, `white_elo`, `black_elo` (FR16)
**And** the endpoint responds in under 100ms for a 10M-game corpus (NFR01)

**Given** a Zobrist hash with no matches
**When** the endpoint is called
**Then** HTTP 200 is returned with an empty array

**Given** an invalid Zobrist hash (non-numeric, negative)
**When** the endpoint is called
**Then** HTTP 400 is returned with an error message (NFR10)

### Story 4b.3: Opening Statistics Endpoint

As a user,
I want to retrieve opening statistics for a position via HTTP,
So that I can see move frequency and win/draw/loss breakdowns for any position.

**Acceptance Criteria:**

**Given** the server is running and a populated database is loaded
**When** `GET /api/openings/{zobrist_hash}/stats` is called
**Then** the response contains for each continuation move: move (SAN), frequency, white wins, draws, black wins, average white Elo, average black Elo (FR17)
**And** the endpoint responds in under 500ms (NFR02)

**Given** a position with no continuations
**When** the endpoint is called
**Then** HTTP 200 is returned with an empty continuations array

### Story 4b.4: Import Trigger & SSE Progress Stream

As a user,
I want to trigger a PGN import via HTTP and receive real-time progress updates via Server-Sent Events,
So that I can import games from any frontend and monitor progress without polling.

**Acceptance Criteria:**

**Given** the server is running and a database is loaded
**When** `POST /api/imports` is called with `{ "path": "/path/to/file.pgn" }`
**Then** the import starts asynchronously and the response returns HTTP 202 with `{ "import_id": "<uuid>" }` (FR09)

**Given** an import is running
**When** `GET /api/imports/{import_id}/progress` is called (SSE endpoint)
**Then** events are streamed containing `games_processed`, `games_committed`, `games_skipped`, `elapsed_seconds` (FR13)
**And** a final event is sent with the complete `import_summary` on completion (FR14)

**Given** an import fails to start (file not found, unreadable)
**When** `POST /api/imports` is called
**Then** HTTP 400 is returned with an error message (NFR10)

**Given** an import is running
**When** `DELETE /api/imports/{import_id}` is called
**Then** the import is gracefully stopped and checkpoint is saved for resume

### Story 4b.5: Game List Endpoint

As a user,
I want to browse the list of games in the database via HTTP with optional filtering,
So that I can find specific games from any frontend.

**Acceptance Criteria:**

**Given** the server is running and a populated database is loaded
**When** `GET /api/games` is called
**Then** a paginated JSON array of games is returned with fields: `id`, `white`, `black`, `result`, `event`, `date`, `eco` (FR26)

**Given** query parameters `?player=<name>&result=<result>`
**When** `GET /api/games?player=Carlsen&result=1-0` is called
**Then** the list is filtered by player name (white or black) and result (FR26)

**Given** a large database
**When** the endpoint is called with `?offset=100&limit=50`
**Then** pagination is applied; default limit is 50; maximum limit is 200

### Story 4b.6: Single Game Retrieval Endpoint

As a user,
I want to retrieve a complete game by ID via HTTP,
So that I can view full game metadata and move data from any frontend.

**Acceptance Criteria:**

**Given** the server is running and a populated database is loaded
**When** `GET /api/games/{id}` is called with a valid game ID
**Then** the response contains the full game: metadata (players, event, site, date, result, ECO, tags) and encoded move sequence (FR05)

**Given** a game ID that does not exist
**When** the endpoint is called
**Then** HTTP 404 is returned with `{ "error": "not_found" }`

**Given** an invalid game ID format
**When** the endpoint is called
**Then** HTTP 400 is returned with an error message

### Story 4b.7: OpenAPI Specification

As a developer integrating with the motif-chess HTTP API,
I want a versioned OpenAPI 3.1 specification document,
So that I have a machine-readable and human-readable contract for all endpoints that serves as the epic's exit artifact.

**Acceptance Criteria:**

**Given** all six Epic 4b endpoints are implemented
**When** the spec file at `docs/api/openapi.yaml` is loaded by any OpenAPI 3.1-compliant tool (e.g., Swagger UI, Redoc, openapi-generator)
**Then** it validates without errors and documents all 8 routes: `GET /health`, `GET /api/positions/{zobrist_hash}`, `GET /api/openings/{zobrist_hash}/stats`, `POST /api/imports`, `GET /api/imports/{import_id}/progress`, `DELETE /api/imports/{import_id}`, `GET /api/games`, `GET /api/games/{id}`

**Given** each documented endpoint
**When** the spec is reviewed
**Then** every route includes: HTTP method, path, summary, all path/query parameters with types and constraints, request body schema (where applicable), all documented response codes with schema, and at least one example

**Given** the spec is committed
**When** `cmake --preset=dev && cmake --build build/dev && ctest --test-dir build/dev` is run
**Then** all existing tests continue to pass (no regressions)

---

## Epic 4c: HTTP API Contract Hardening

Before a separate web frontend consumes the local HTTP API, the API contract must be hardened around frontend-safe data representation and consistent formatting. This is a follow-up to Epic 4b, not the start of the deferred Qt desktop epic.

### Story 4c.1: HTTP API Contract Hardening

As a developer building a separate web frontend against the motif-chess HTTP API,
I want hash values and formatting behavior to be consistent and frontend-safe,
So that API clients can consume the contract without integer precision loss or representation drift.

**Acceptance Criteria:**

**Given** any HTTP response field exposes a Zobrist or resulting-position hash
**When** JSON is serialized
**Then** the hash is emitted as a JSON string, regardless of numeric magnitude
**And** internal C++ domain/search types continue to store hashes as `std::uint64_t`

**Given** `GET /api/openings/{zobrist_hash}/stats` returns continuations
**When** the response contains `result_hash`
**Then** every `result_hash` value is serialized as a decimal JSON string
**And** tests assert the quoted string form for a known computed hash

**Given** `docs/api/openapi.yaml` is used by the web frontend
**When** the OpenAPI schemas and examples are reviewed
**Then** every externally exposed hash field is documented as `type: string`
**And** examples use quoted decimal strings

**Given** HTTP production code or HTTP tests intentionally format strings or write console output
**When** the code is reviewed
**Then** it uses `fmt::format` for string construction and `fmt::print` for stdout/stderr output
**And** new or touched HTTP files do not use `std::cout`, `std::cerr`, `std::ostringstream`, or `std::to_string`

---

## Epic 4d: Web Frontend API Prerequisites

Before the separate web frontend implements interactive board movement or engine analysis panels, the local HTTP API must expose two missing backend surfaces: legal move generation plus move validation/application from a FEN position, and an engine analysis contract that lets the browser receive streamed UCI output through the motif-chess backend.

### Story 4d.1: Legal Moves and Move Validation Endpoints

As a developer building an interactive web chessboard,
I want HTTP endpoints that return legal moves for a FEN position and validate/apply a candidate move,
So that a display-only frontend board can highlight legal destinations and update only after the backend confirms the move.

**Acceptance Criteria:**

**Given** the server is running
**When** `GET /api/positions/legal-moves?fen=<url-encoded-fen>` is called with a valid FEN
**Then** HTTP 200 is returned with the canonical FEN and a `legal_moves` array
**And** each move contains at minimum `uci`, `san`, `from`, `to`, and optional `promotion`
**And** SAN/FEN parsing and move legality are delegated to `chesslib`; motif-chess does not reimplement chess rules (NFR19)

**Given** the frontend board has a FEN and a candidate drag/drop move
**When** `POST /api/positions/apply-move` is called with the FEN and candidate move in UCI notation
**Then** a legal move returns HTTP 200 with the accepted `uci`, `san`, and resulting FEN
**And** an illegal move returns HTTP 400 with a JSON error response
**And** the frontend can keep the board renderer free of chess logic by updating only from the returned FEN

**Given** the FEN is missing, malformed, or illegal
**When** the endpoint is called
**Then** HTTP 400 is returned with a JSON error response
**And** no undefined behavior, exception escape, or process crash occurs (NFR10)

**Given** `docs/api/openapi.yaml` is updated
**When** the OpenAPI document is reviewed
**Then** the legal moves and apply-move routes, query/body constraints, response schemas, error schemas, and examples are documented
**And** C++ integration tests cover initial position moves, castling availability, promotion moves, invalid FEN, and check-constrained moves
**And** apply-move tests cover legal moves, illegal moves, promotions, and resulting-FEN correctness

### Story 4d.2: Engine Analysis API Contract

As a developer building a browser-based engine analysis panel,
I want a documented HTTP/SSE contract for starting, streaming, and stopping UCI engine analysis,
So that frontend work can proceed against a stable backend boundary before engine implementation details are finalized.

**Acceptance Criteria:**

**Given** the API contract is documented
**When** a frontend wants to start analysis
**Then** `POST /api/engine/analyses` accepts a JSON body containing FEN, engine selection or default engine, `multipv` in the range 1–5, and exactly one bounded analysis limit (`depth` or `movetime_ms`)
**And** the response returns HTTP 202 with an opaque `analysis_id`

**Given** analysis has started
**When** `GET /api/engine/analyses/{analysis_id}/stream` is called
**Then** the endpoint is documented as `text/event-stream`
**And** normal events include depth, optional seldepth, multipv index, score (`cp` or `mate`), `pv_uci`, and `pv_san`
**And** final `complete` and `error` event shapes are documented
**And** the frontend consumption model states that engine streaming should run from a Web Worker or equivalent off-main-thread client path

**Given** analysis is active
**When** `DELETE /api/engine/analyses/{analysis_id}` is called
**Then** the contract documents cancellation semantics and the expected success/error responses

**Given** this story is complete
**When** implementation stories are created
**Then** they preserve module ownership: `motif_engine` uses `ucilib` for subprocess lifecycle, `motif_http` owns HTTP/SSE session management, and `chesslib` owns PV conversion to SAN
**And** they preserve local deployment constraints: API target `localhost:8080`, no auth, no multi-tenancy, no CDN, CORS for the frontend dev origin, and no data leaving the machine

### Story 4d.3: Personal Games CRUD API and LAN Support

As a developer building motif-chess-web as a domain-native replacement for a required CRUD exercise,
I want backend endpoints for user-added personal games plus simple trusted-LAN configuration,
So that the web frontend can add, list/search, open, edit, and delete chess games without inventing a non-chess TODO feature.

**Acceptance Criteria:**

**Given** the server is running with an open database
**When** `POST /api/games` is called with pasted single-game PGN text
**Then** the PGN is parsed through the existing PGN/chess libraries, persisted through the SQLite/DuckDB storage path, and HTTP 201 returns the created game ID plus normalized metadata
**And** the created game is marked as user/manual provenance for safe future edits/deletes
**And** malformed PGN or invalid SAN returns a user-readable HTTP 400 JSON error

**Given** game responses are returned by `GET /api/games` or `GET /api/games/{id}`
**When** the OpenAPI/API response is reviewed
**Then** user-added game provenance is represented in API responses and imported/reference games are distinguishable from manual games

**Given** a user-added game exists
**When** `PATCH /api/games/{id}` is called with valid editable metadata/status fields
**Then** HTTP 200 returns the updated game
**And** editable fields are limited to supported metadata/status fields, including `review_status` values `new`, `needs_review`, `studied`, and `archived`
**And** invalid IDs/fields/status values return 400 and missing games return 404

**Given** a non-user-added imported/reference game exists
**When** `PATCH /api/games/{id}` or `DELETE /api/games/{id}` is called
**Then** HTTP 409 is returned and no imported/reference corpus data is changed

**Given** a user-added game exists
**When** `DELETE /api/games/{id}` is called
**Then** HTTP 204 is returned, SQLite game/tag rows are removed, and DuckDB derived position data is removed or rebuilt so position search/opening stats no longer include the deleted game

**Given** motif-chess-web may run on a trusted LAN during development
**When** the HTTP server starts
**Then** bind host/port and explicit CORS allowed origins are configurable while default local development remains `localhost:8080`
**And** no authentication, multi-user support, public-network hardening, engine analysis, or frontend UI is added in this story

**Given** `docs/api/openapi.yaml` is updated
**When** the document is reviewed
**Then** it documents create, patch, delete, provenance fields, schemas, error cases, and examples
**And** integration tests cover create/read/list/patch/delete/error cases, imported/reference mutation rejection, PGN parse errors, and SQLite/DuckDB consistency

### Story 4d.4: File Upload Import and Game Count Endpoints

As a developer building a web frontend import panel,
I want an endpoint that accepts PGN file bytes directly and another that returns the total game count,
So that the browser can import games without a server-side path and display the database total after an import completes.

**Acceptance Criteria:**

**Given** a browser user selects or drags a PGN file
**When** `POST /api/imports/upload` is called with `multipart/form-data` containing a `file` field
**Then** HTTP 202 is returned with an `import_id` matching the format of `POST /api/imports`
**And** the backend writes the bytes to a temporary file, runs the same import pipeline, and deletes the temp file when the import finishes or fails
**And** the returned `import_id` works with the existing `GET /api/imports/{import_id}/progress` SSE stream

**Given** the `file` field is absent from the multipart body
**When** `POST /api/imports/upload` is called
**Then** HTTP 400 is returned with a JSON error response

**Given** an import is already running
**When** `POST /api/imports/upload` is called
**Then** HTTP 409 is returned with a JSON error response, matching the conflict behavior of `POST /api/imports`

**Given** the server is running with an open database
**When** `GET /api/games/count` is called
**Then** HTTP 200 is returned with `{"count": <integer>}` reflecting the total number of games in the database
**And** the count is 0 on an empty database and increments correctly after an import completes

**Given** `docs/api/openapi.yaml` is updated
**When** the document is reviewed
**Then** `POST /api/imports/upload` is documented with its multipart schema, 202/400/409 responses, and an `ImportStartResponse` reference
**And** `GET /api/games/count` is documented with a `GameCountResponse` schema
**And** integration tests cover: missing field → 400, valid upload → 202, concurrent upload → 409, empty count → 0, count after import → correct total

---

## Epic 7: Desktop Application *(DEFERRED — API prerequisites take priority)*

Users interact with the full database through a native Qt 6 desktop GUI — browsing games, navigating positions on a chessboard, running imports, searching positions, and configuring the application — all keyboard-accessible on high-DPI displays.

### Story 7.1: Database Workspace - Create, Open, Recent & Scratch

As a user,
I want motif-chess to start with a first-class database workspace where I can create, open, reopen, or use a scratch database,
So that every game list, import, search, and analysis workflow has an explicit active database context.

**Acceptance Criteria:**

**Given** the application is launched for the first time
**When** the main window appears
**Then** startup time from launch to usable window is under 3 seconds (NFR06)
**And** a database workspace/start screen is shown before game list, import, search, or analysis workflows
**And** `~/.config/motif-chess/config.json` is created with defaults for database directory, empty recent list, default engine paths, and default UI preferences (FR39, FR40)

**Given** the database workspace is visible
**When** the user creates a named database at a chosen directory
**Then** a portable database bundle is created containing `games.db`, `positions.duckdb`, and `manifest.json` (FR01, FR41)
**And** the new database becomes the active database
**And** the active database name and path are visible in the shell

**Given** the database workspace is visible
**When** the user opens an existing database bundle
**Then** the bundle is validated and opened without dropping or recreating data (FR07)
**And** invalid, missing, schema-mismatched, or corrupt bundles are reported through the shell without crashing
**And** failed opens do not replace the previous active database

**Given** the user has previously opened databases
**When** the application starts
**Then** the recent databases list is populated from `config.json`; config is never stored inside database files (FR39, FR40)
**And** missing recent paths are shown as unavailable and can be removed
**And** successfully opened databases move to the front of the recent list without duplicates

**Given** the database workspace is visible
**When** the user selects the scratch database option
**Then** the in-memory scratch database becomes active (AR07)
**And** the shell clearly labels it as temporary
**And** scratch is never persisted to disk and never added to recent databases

**Given** no active database exists
**When** the user attempts to browse games, import PGN, search positions, or start analysis
**Then** the shell prompts the user to create, open, or use scratch first
**And** backend workflows are not started without an active database context

**Given** the application is launched with one or more `.pgn` file paths
**When** the main window appears
**Then** those paths are captured as a queued import list
**And** if no active database exists, the workspace asks the user to choose/create/use scratch before import can proceed

**Given** the application is installed on Linux
**When** the `.desktop` file is registered
**Then** double-clicking a `.pgn` file in the file manager opens motif-chess with that file queued for import (FR42)
**And** all Qt code is confined to `motif_app`; no Qt headers appear in `motif_db`, `motif_import`, `motif_search`, `motif_engine`, or `motif_http`

### Story 7.2: Chessboard Widget & Game Navigation

As a user,
I want to view a chessboard displaying the current position and navigate through a game move by move using the keyboard,
So that I can step through any game in my database without reaching for the mouse.

**Acceptance Criteria:**

**Given** a game is loaded
**When** the chessboard widget renders
**Then** the correct position for the current move is displayed with standard piece graphics on a high-DPI display (FR21, FR27)

**Given** a game is loaded
**When** the user presses the forward/backward navigation key (or clicks)
**Then** the board advances or retreats one half-move and updates within 100ms (FR22, NFR05)
**And** pressing Home/End jumps to the start or end of the game (FR22)

**Given** a game with variations
**When** the game tree widget renders
**Then** variations are displayed as branching lines; the user can navigate into and out of variations via keyboard (FR23)

**Given** high-DPI display scaling is active
**When** the board renders
**Then** pieces and squares are crisp with no blurry scaling artifacts (FR27)

### Story 7.3: Game List Browser

As a user,
I want to browse the full list of games in my active database, filter by player name or result, and open any game for board replay,
So that I can find and review specific games from my collection without external tools.

**Acceptance Criteria:**

**Given** a database with games is open
**When** the game list panel is displayed
**Then** all games are listed with columns: White, Black, Result, Event, Date, ECO; the list renders within 100ms for databases up to 100K games (FR26, NFR05)

**Given** the game list is displayed
**When** the user types in the filter field
**Then** the list filters in real time by player name (white or black) and result; no mouse required (FR26, FR28)

**Given** a game is selected in the list
**When** the user presses Enter or double-clicks
**Then** the selected game loads into the chessboard widget and game tree widget; transition completes within 100ms (FR22, NFR05)

### Story 7.4: PGN Import Dialog

As a user,
I want to initiate a PGN import from within the UI, monitor its real-time progress, and see the final summary when it completes,
So that I never need to use a command line to add games to my database.

**Acceptance Criteria:**

**Given** the user selects "Import PGN" from the menu
**When** the import dialog opens and a PGN file is selected
**Then** the import starts; the dialog shows games processed, games committed, games skipped, and elapsed time updating in real time (FR24, FR13)
**And** the import runs on a `QThread` worker — the main thread and UI remain responsive during the entire import (NFR05)

**Given** an import is running
**When** the user closes the dialog or clicks Cancel
**Then** the import is gracefully stopped and `import.checkpoint.json` is written so the import can be resumed later (FR12)

**Given** the import completes
**When** the summary is displayed
**Then** the dialog shows total attempted, committed, skipped, and error count (FR14)
**And** the game list panel refreshes to include the newly imported games

### Story 7.5: Position Search Panel

As a user,
I want to search the database for the position currently on the board and view matching games and opening statistics inline,
So that I can immediately see how any position has been played without leaving the analysis view.

**Acceptance Criteria:**

**Given** a position is displayed on the chessboard
**When** the user triggers "Search position" (keyboard shortcut or button)
**Then** the position search panel shows all games in the database that reached that position, with columns: White, Black, Result, Elo, Move number (FR25, FR16)
**And** the search completes and results appear within 100ms (NFR05)

**Given** the position search panel is showing results
**When** the user selects a game from the results
**Then** that game loads into the board at the searched position

**Given** the position search panel is open
**When** the opening statistics tab is selected
**Then** the move frequency and win/draw/loss breakdown from `opening_stats::query` is displayed (FR17)
**And** all UI interactions in this story respond within 100ms (NFR05)

---

## Epic 4: Engine Integration *(Phase 2)*

Users can configure UCI engines and run real-time analysis on any position; engine crashes are isolated and never affect the application.

### Story 4.1: Engine Configuration & Lifecycle

As a user,
I want to configure one or more UCI chess engines by executable path and start/stop analysis on a FEN position,
So that any motif-chess frontend can get engine evaluations without managing engine processes directly.

**Acceptance Criteria:**

**Given** the user opens engine settings
**When** they specify an executable path for a UCI engine
**Then** the engine path is saved to `config.json` and the engine is listed as available (FR29)

**Given** an engine is configured and a valid FEN is provided by a frontend
**When** analysis starts
**Then** `engine_manager` launches the engine subprocess via `ucilib`, sends the FEN, applies bounded analysis parameters, and begins receiving info lines (FR30)
**And** `ucilib` owns all engine subprocess lifecycle — motif-chess does not interact with the engine process directly (NFR18)

**Given** the frontend stops analysis or the analysis limit is reached
**When** stop is triggered
**Then** the engine subprocess receives a `stop` command, output ceases, and the analysis session transitions to a terminal state (FR30)

### Story 4.2: Real-Time Engine Output & Crash Isolation

As a user,
I want to see engine depth, score, and principal variation updating in real time through a streaming backend API, and have engine crashes never crash motif-chess,
So that I can analyze positions safely from desktop or web frontends even with unstable third-party engines.

**Acceptance Criteria:**

**Given** engine analysis is running
**When** the engine emits `info` lines
**Then** depth, score (centipawns/mate), multipv index, and principal variation are emitted as normalized analysis updates
**And** principal variations are exposed in UCI form and SAN form where SAN conversion succeeds (FR31, NFR19)
**And** the HTTP adapter streams those updates via the contract defined in Story 4d.2

**Given** the engine process crashes or exits unexpectedly
**When** `engine_manager` detects the crash
**Then** the session emits an error state; motif-chess continues running normally (FR32)
**And** the user can restart analysis without relaunching the application or backend (FR32)

---

## Epic 5: Statistical Analysis *(Phase 2)*

Users view personal performance correlated across openings, game phases, opponent rating ranges, and time management — degrading gracefully when clock data or Elo is absent.

### Story 5.1: Performance Statistics by Opening & Color

As a user,
I want to view my win/draw/loss rates by opening, color, and opponent rating range,
So that I can identify which openings are working and which need attention.

**Acceptance Criteria:**

**Given** a personal game corpus is imported
**When** the statistics panel is opened
**Then** performance by opening (ECO code or position) broken down by result, color, and opponent rating band is displayed (FR33)
**And** the query completes within 500ms (NFR02)

**Given** Elo data is absent for some games
**When** statistics are computed
**Then** those games contribute to result counts but not to rating-band breakdowns; the UI labels absent-Elo bins clearly (FR36)

### Story 5.2: Game Phase & Time Management Statistics

As a user,
I want to see where in the game I tend to make errors and how my performance correlates with time pressure,
So that I can target the specific phase or time situation that costs me the most points.

**Acceptance Criteria:**

**Given** a personal game corpus is imported
**When** the phase analysis view is opened
**Then** error distribution across opening/middlegame/endgame phases is displayed (FR34)

**Given** games with `%clk` PGN annotations exist
**When** time management statistics are displayed
**Then** performance correlation with time pressure (low clock time) is shown (FR35)

**Given** no games with `%clk` annotations exist
**When** time management statistics are requested
**Then** the panel displays a clear "clock data not available" message rather than empty charts (FR36)

---

## Epic 6: Repertoire Management *(Phase 2)*

Users build and save annotated opening repertoire lines and drill them in practice mode.

### Story 6.1: Build & Save Annotated Repertoire Lines

As a user,
I want to build an opening repertoire as a set of annotated lines and save it within the application,
So that I have a structured preparation reference I can return to.

**Acceptance Criteria:**

**Given** a position is on the board
**When** the user adds it to the repertoire with a chosen move and optional annotation
**Then** the line is saved as part of the repertoire (FR37)
**And** the repertoire is stored separately from game databases; it does not modify `games.db`

**Given** a saved repertoire
**When** the user reopens the application
**Then** the repertoire is loaded and all annotated lines are accessible (FR37)

### Story 6.2: Repertoire Drill Mode

As a user,
I want to drill my repertoire lines in a practice mode where the application plays the opponent's moves and I respond,
So that I can memorize my preparation through active recall.

**Acceptance Criteria:**

**Given** a repertoire is loaded
**When** the user starts drill mode
**Then** the board presents positions from the repertoire; the application plays the expected opponent moves; the user must respond with the correct repertoire move (FR38)

**Given** the user plays a move not in the repertoire
**When** drill mode evaluates the response
**Then** the application highlights the deviation and shows the correct repertoire move (FR38)

---

## Epic 8: AI Repertoire Construction *(Phase 3)*

Users receive a personalized opening repertoire generated from analysis of their corpus, playing style, and theoretical gaps.

### Story 8.1: Corpus Analysis & Player Profiling

As a user,
I want the system to analyze my game corpus and produce a profile of my playing style, strengths, and theoretical weaknesses,
So that I have a data-driven foundation for personalised repertoire recommendations.

**Acceptance Criteria:**

**Given** a personal game corpus of at least 500 games is imported
**When** corpus analysis runs
**Then** a player profile is generated identifying: predominant openings, win/loss patterns by position type, theoretical gaps (positions with poor results and high frequency) (FR43)

### Story 8.2: Personalized Repertoire Generation

As a user,
I want the system to generate a personalized opening repertoire proposal based on my player profile,
So that I receive concrete preparation recommendations tailored to my current ability and preferred piece structures.

**Acceptance Criteria:**

**Given** a player profile from Story 8.1 exists
**When** repertoire generation runs
**Then** a repertoire proposal is produced covering both colors, accounting for the player's current strength, preferred piece structures, and identified theoretical gaps (FR44, FR45)

**Given** the player's corpus grows with new games
**When** the user re-runs repertoire generation
**Then** the proposal is updated to reflect the new data (FR44)

---

## Epic 3b: Search Filters & Filtered Opening Explorer

Users can filter the game list and the opening explorer by player, color, Elo range, result, ECO prefix, and position (Zobrist hash), enabling opponent preparation from a filtered game list and opening study from a position-centric filtered view with Elo-weighted statistics and a continuous Elo distribution per continuation.

**FRs covered:** FR01–FR09 (game search & list), FR11–FR16 (filtered opening explorer), FR17–FR20 (sync toggle — stateless API contract; Qt manages client state), FR21–FR23 (HTTP, deferrable)
**NFRs covered:** NFR01–NFR08, NFR09–NFR12
**Dependencies:** Epic 3 complete (`position_search`, `opening_stats`, `opening_tree` stable and tested)
**Note on FR10:** Board navigation on game select is a Qt concern — covered by Epic 7 Story 7.3, not here.
**Note on Story 3b.5:** HTTP adapter is deferrable; may move to `motif-chess-web`. Core Qt workflows use `motif_search` and `motif_db` directly.

### Story 3b.1: Search Filter Model & Filtered Game List

As a user,
I want to filter the game list by player name, color, Elo range, result, ECO prefix, and/or position (Zobrist hash),
So that I can find relevant games for opponent preparation and opening study without browsing the entire database.

**Acceptance Criteria:**

**Given** the `motif_db` module
**When** the search filter model story is complete
**Then** `source/motif/db/types.hpp` defines `motif::db::search_filter` with optional fields: `player_name` (string), `player_color` (enum: white | black | either), `min_elo` (int), `max_elo` (int), `result` (string), `eco_prefix` (string), `position` (zobrist_hash), `offset` (int, default 0), `limit` (int, default 100, max 500)
**And** a `game_list_result` struct is defined with `std::vector<game_list_entry> games` and `std::int64_t total_count`

**Given** `game_store` with a `search_filter` where `player_name` is set
**When** `game_store::find_games(search_filter)` is called
**Then** only games where the matching player name is a case-insensitive partial match against white or black player are returned
**And** when `player_color` is set to `white`, only games where the matching player is the white player are returned; `black` restricts to black; `either` matches either side

**Given** a `search_filter` with `min_elo` and/or `max_elo` set
**When** `game_store::find_games` is called
**Then** only games where both white and black Elo are within the range (both endpoints inclusive) are returned
**And** games without Elo data are excluded when an Elo filter is active

**Given** a `search_filter` with `result` set to "1-0", "0-1", or "1/2-1/2"
**When** `game_store::find_games` is called
**Then** only games with the matching result are returned

**Given** a `search_filter` with `eco_prefix` set to "B9"
**When** `game_store::find_games` is called
**Then** games with ECO codes B90–B99 are returned; prefix "B" matches all B-code games

**Given** a `search_filter` with `position` set to a Zobrist hash
**When** `game_store::find_games` is called
**Then** the query first retrieves matching game IDs from DuckDB `position` table, then applies remaining metadata filters in SQLite via an IN clause or temp table join
**And** no cross-store join occurs on the hot path — DuckDB returns game IDs; SQLite resolves metadata

**Given** multiple fields set in `search_filter`
**When** `game_store::find_games` is called
**Then** all active filters are ANDed; a game must satisfy all conditions to appear in results

**Given** an empty `search_filter` (all fields nullopt)
**When** `game_store::find_games` is called
**Then** all games are returned paginated; this is not treated as an error

**Given** any filter combination on a 4M-game corpus
**When** `game_store::find_games` is called
**Then** the query completes in under 100ms (NFR01)
**And** player name search in SQLite completes in under 50ms before DuckDB intersection (NFR05)

**Given** any test scenario
**Then** filtered results exactly match manually counted results from the test dataset (NFR06)
**And** all tests use real in-memory SQLite and DuckDB instances — no mocks
**And** all tests pass under `cmake --preset=dev-sanitize` with zero ASan/UBSan violations (NFR12)

### Story 3b.2: Filtered Opening Stats & ELO-Weighted Ranking

As a user,
I want opening statistics at any position to reflect only games matching my current filter, with continuations ranked by an Elo-weighted score,
So that I can see how strong players handle each position and identify which moves are objectively best rather than just most popular.

**Acceptance Criteria:**

**Given** `opening_stats::query` with an optional `search_filter` parameter
**When** the filter is set
**Then** all statistics (frequency, white_wins, draws, black_wins, average_white_elo, average_black_elo) are computed over the filtered game set only
**And** the `opening_stat_cont` (or equivalent continuation struct) gains a `double elo_weighted_score` field

**Given** an empty or absent `search_filter`
**When** `opening_stats::query` is called
**Then** behavior is identical to the pre-3b implementation — no regression (FR12, NFR08)

**Given** the ELO-weighted score
**When** it is computed for a continuation
**Then** the formula is: score = Σ(result_value(g) × avg_elo(g)) / Σ avg_elo(g), where result_value is +1 (side to move wins), 0 (draw), −1 (side to move loses), and avg_elo(g) = (white_elo + black_elo) / 2 for games where both are available; games with missing Elo data are excluded from the weighted score computation but included in frequency counts
**And** the score is returned as a field for the caller to use as a sort key — the API does not sort; sort order is the caller's responsibility (FR13, FR14)

**Given** two continuations with equal ELO-weighted score
**When** the caller sorts by score
**Then** ties are broken by frequency (descending), then SAN alphabetically (NFR07)

**Given** a position with a known game set
**When** `opening_stats::query` is called with a filter
**Then** the returned stats exactly match manually computed values from the filtered game set (NFR06)

**Given** any filter combination on a 4M-game corpus
**When** `opening_stats::query` is called
**Then** the query completes in under 200ms (NFR02)

**Given** any test scenario
**Then** all new public functions have at least one Catch2 v3 test (NFR09)
**And** tests use real in-memory DuckDB — no mocks (NFR10)
**And** all tests pass under `cmake --preset=dev-sanitize` (NFR12)
**And** zero new clang-tidy or cppcheck warnings (NFR11)

### Story 3b.3: Continuous ELO Distribution per Continuation

As a user,
I want to see win/draw/loss rates as a continuous distribution over Elo for each continuation,
So that I can tell whether a move is objectively sound or only underperforms because it is difficult to play correctly below a certain Elo threshold.

**Acceptance Criteria:**

**Given** `position_store` (in `motif_db`)
**When** the ELO distribution story is complete
**Then** a new method `position_store::query_elo_distribution(zobrist_hash, search_filter, bucket_width)` exists, returning a map from continuation encoded_move to a vector of `elo_distribution_bucket { int elo_bucket_floor; uint32_t white_wins; uint32_t draws; uint32_t black_wins; uint32_t game_count; }`

**Given** a position with games spanning Elo 1400–2800 and bucket_width = 25
**When** `query_elo_distribution` is called
**Then** buckets are returned at 25-Elo intervals covering the full range of matching games
**And** empty buckets (no games in that interval) are included as zeros so the frontend has a contiguous range without gaps (FR15)
**And** the floor boundary for each bucket is `floor(avg_elo / bucket_width) * bucket_width`

**Given** the distribution query
**When** it executes
**Then** a single DuckDB query handles all continuations at the position — not one query per continuation (NFR04)

**Given** bucket totals for a continuation
**When** summed across all buckets
**Then** total game_count equals the frequency from Story 3b.2 filtered stats for the same filter (correctness invariant)

**Given** any filter combination on a 4M-game corpus
**When** `query_elo_distribution` is called
**Then** it completes within the 200ms budget shared with filtered opening stats (NFR02)

**Given** any test scenario
**Then** bucket sum invariant is verified by tests
**And** empty bucket inclusion is verified for a known sparse dataset
**And** all tests pass under `cmake --preset=dev-sanitize` (NFR12)
**And** zero new clang-tidy or cppcheck warnings (NFR11)

### Story 3b.4: Filtered Opening Tree Traversal

As a user,
I want the opening tree to reflect only positions and statistics from games matching my filter,
So that when I filter by opponent, the tree shows their actual repertoire rather than the full database.

**Acceptance Criteria:**

**Given** `opening_tree::open` with an optional `search_filter` parameter
**When** the filter is set
**Then** all tree node statistics (frequency, white_wins, draws, black_wins, average_white_elo, average_black_elo, elo_weighted_score) are computed over the filtered game set
**And** the filter is passed through to all underlying `position_store` and `opening_stats` calls
**And** ELO-weighted score (from Story 3b.2) is included in each `node_continuation`

**Given** `opening_tree::expand` with an optional `search_filter` parameter
**When** a lazy node expansion is triggered
**Then** the same filter is applied to the on-demand DuckDB query for that node's children (FR16)

**Given** an empty or absent `search_filter`
**When** `opening_tree::open` or `expand` is called
**Then** behavior is identical to the pre-3b implementation — no regression

**Given** a filtered tree opened with `prefetch_depth = 3` on a 4M-game corpus
**When** `opening_tree::open` is called
**Then** the call completes in under 500ms (NFR03)

**Given** a filter that matches zero games at a node
**When** that node is included in the tree
**Then** the node has zero continuations and is marked `is_expanded = true` — not an error

**Given** any test scenario
**Then** filtered tree node statistics match manually computed values from the filtered game set
**And** all tests pass under `cmake --preset=dev-sanitize` (NFR12)
**And** zero new clang-tidy or cppcheck warnings (NFR11)

### Story 3b.5: HTTP Search & Filter API *(deferrable — may move to motif-chess-web)*

As a developer building a frontend against the motif-chess HTTP API,
I want game list, opening statistics, and opening tree endpoints to accept filter query parameters,
So that the Qt or web frontend can drive all search and filtered explorer workflows over HTTP without direct library access.

**Acceptance Criteria:**

**Given** `GET /api/games`
**When** called with any combination of query parameters: `player` (string), `color` (white|black|either), `min_elo` (int), `max_elo` (int), `result` (string), `eco` (string), `position_hash` (uint64 as string), `offset` (int), `limit` (int, max 500)
**Then** the response includes a paginated game list and `total_count` reflecting the full filtered result set (FR21)
**And** the response game objects include `white_elo` and `black_elo` fields
**And** absent parameters apply no filter for that field; all absent = unfiltered

**Given** `GET /api/positions/{hash}/stats`
**When** called with optional filter params (player, color, min_elo, max_elo, result, eco)
**Then** the response reflects filtered opening statistics including `elo_weighted_score` per continuation (FR22)

**Given** `GET /api/positions/{hash}/tree`
**When** called with optional filter params
**Then** the tree nodes reflect filtered statistics (FR23)

**Given** the sync toggle (FR17–FR20)
**When** the API design is reviewed
**Then** the API is explicitly stateless — each request carries its own complete filter; no session, no sync state is held server-side; sync toggle behavior is a Qt (or web) client responsibility implemented by sharing filter state between panels in the UI layer

**Given** any invalid filter parameter (non-numeric Elo, unrecognized color, invalid result string)
**When** the endpoint is called
**Then** HTTP 400 is returned with a descriptive JSON error; the server does not crash (NFR10-equivalent)

**Given** `docs/api/openapi.yaml`
**When** updated for this story
**Then** all new query parameters are documented with types, constraints, and examples
**And** integration tests cover filter combinations, edge cases, and the stateless-sync contract documentation
**And** all tests pass under `cmake --preset=dev-sanitize`
