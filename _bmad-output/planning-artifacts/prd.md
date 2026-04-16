---
stepsCompleted: ['step-01-init', 'step-02-discovery', 'step-02b-vision',
  'step-02c-executive-summary', 'step-03-success', 'step-04-journeys',
  'step-05-domain', 'step-06-innovation', 'step-07-project-type',
  'step-08-scoping', 'step-09-functional', 'step-10-nonfunctional', 'step-11-polish']
classification:
  projectType: desktop_app
  domain: chess_tooling
  complexity: medium-high
  projectContext: greenfield_on_foundation
inputDocuments:
  - 'specs/001-database-schema/spec.md'
  - 'specs/002-import-pipeline/spec.md'
  - 'specs/003-search/spec.md'
  - 'specs/004-qt-gui/spec.md'
  - 'plans/design.md'
  - '_bmad-output/project-context.md'
workflowType: 'prd'
---

# Product Requirements Document - motif-chess

**Author:** Bogdan  
**Date:** 2026-04-14

## Executive Summary

Motif-chess is a native desktop chess database and analysis platform for master-level players. It addresses the gap between expensive proprietary tools (ChessBase) and unmaintained or architecturally compromised open-source alternatives (scidb, scid, chessx) — none of which provide the ergonomics, analytical depth, or extensibility that serious players require.

The primary motivating use case is systematic opening preparation and performance analysis using a player's personal online game corpus (lichess, chess.com). The tool ingests thousands of games, indexes every position by Zobrist hash across a dual-store backend (SQLite for metadata, DuckDB for columnar position analytics), and delivers statistical analysis precise enough to identify specific problem areas — not just "you lose with the French" but "you consistently mismanage time in IQP middlegames after move 20 in games lasting over 3 hours."

The longer-term vision is an AI layer that closes the loop: analyzing a player's style, strengths, weaknesses, and theoretical gaps to generate a personalized opening repertoire — an autonomous preparation assistant rather than a passive database.

### What Makes This Special

Three differentiators that no existing tool combines:

1. **Statistical depth on personal data.** Performance correlated with game phase, time management, opponent rating, opening choice, and game duration. Problem area identification rather than raw statistics.
2. **AI-driven repertoire construction.** Not lookup, but generation — a repertoire designed around the specific player's current ability and style, updated as their game corpus grows.
3. **Master-level UX.** Lean, ergonomic, no hand-holding. Built by a FIDE Master for players who know what they want and need tools that keep pace with them.

The foundation — `chesslib` (board/SAN/Zobrist), `pgnlib` (PGN parsing), `ucilib` (UCI engine integration) — is already built and author-controlled, eliminating the most complex dependencies from the greenfield app scope.

### Project Classification

- **Type:** Native desktop application (C++20, Qt 6, Linux-first)
- **Domain:** Chess tooling / competitive games
- **Complexity:** Medium-high — deep domain specificity, high-performance requirements (10M+ game corpus, target on par with ChessBase Mega Database), ML roadmap
- **Context:** Greenfield application on a pre-existing in-house library foundation

## Success Criteria

### User Success

- A player can import their full online game history (lichess, chess.com PGN export) without manual intervention; malformed games are skipped and logged, not fatal
- From import to first meaningful position search: under 5 minutes for a 5,000-game corpus
- Statistical analysis surfaces actionable patterns in a player's opening and middlegame performance — specific enough to directly inform preparation decisions
- The tool is usable as a daily driver for opening preparation by a master-level player without resorting to ChessBase or external tools for core workflows
- UI is lean and non-intrusive: a player in analysis mode is never more than one click away from the board, the game tree, or the position database

### Business Success

This is a personal tool and open-source project; "business success" means:
- The author uses it as their primary chess preparation tool within 6 months of the first complete release
- The project is architecturally sound enough that other developers can contribute without being blocked by design debt (the core lesson from scidb)
- At least a small community of serious players adopts it as their primary database tool

### Technical Success

- Import pipeline handles 10M+ games without memory exhaustion or corruption
- Position search (Zobrist lookup) returns results in under 100ms for a 10M-game corpus
- SQLite + DuckDB dual-store remains inspectable with standard CLI tools (`sqlite3`, `duckdb`)
- Zero ASan/UBSan violations across the full test suite
- clang-tidy and cppcheck report zero warnings on every build
- All acceptance criteria in specs 001–004 satisfied

### Measurable Outcomes

| Metric | Target |
|---|---|
| 10M-game corpus import time | < 20 minutes on reference hardware |
| Position lookup latency | P99 < 100ms |
| Opening statistics query | < 500ms for any position |
| Memory usage during import | < 2GB regardless of corpus size |

## Product Scope & Development Phases

### MVP Strategy

**Approach:** Problem-solving MVP — the minimum that makes the tool useful as a daily chess preparation driver. A player can import their games, navigate to any position, and see position statistics from a reference database.

**Resource profile:** Solo developer, evenings/weekends. Scope discipline is critical. Each spec is a self-contained deliverable; shipping spec 001 completely is better than half-finishing specs 001–004.

### Phase 1 — MVP

**Core user journeys supported:** Journey 1 (position lookup), Journey 2 (import + error recovery), Journey 3 (contributor onboarding)

| Spec | Scope |
|---|---|
| `spec-001` | SQLite + DuckDB schema, game CRUD, deduplication, move blob storage |
| `spec-002` | PGN import pipeline — parallel workers, malformed-game recovery, checkpoint/resume, import log |
| `spec-003` | Position search by Zobrist hash, opening statistics (win/draw/loss, move frequency) |
| `spec-004 (basic)` | Qt 6 GUI — chessboard widget, game tree widget, import dialog, position search panel |

**Explicit Phase 1 exclusions:** statistical analysis and visualization, UCI engine integration, online game fetch, repertoire manager, advanced filters.

### Phase 2 — Growth

- Statistical analysis and visualization — performance by opening, phase, time pressure, opponent rating
- UCI engine integration (analysis panel, eval graph)
- Advanced filtering and query builder
- Online game fetch from lichess/chess.com APIs
- Repertoire manager (build, annotate, drill)
- macOS and Windows ports + binary releases

### Phase 3 — Vision

- AI-driven repertoire construction from personal game corpus
- ML-based performance analysis (fatigue, time management, style profiling)
- Annotation assistant

### Risk Mitigation

**Technical risks:**
- *DuckDB at 400M+ rows (10M games × 40 avg ply):* Benchmark with 50M rows during spec 003 before locking in the schema. If columnar scan performance is inadequate, evaluate adding a Bloom filter or secondary index on `zobrist_hash`. Do not defer this benchmark to after MVP.
- *Dual-store consistency:* No two-phase commit across SQLite and DuckDB. DuckDB is derived data and can always be rebuilt from the SQLite game store. If they diverge, SQLite is authoritative.
- *In-house library API churn:* Since the author controls chesslib/pgnlib/ucilib, breaking changes are expected and manageable. Each spec documents its library prerequisites explicitly.
- *Clock data availability:* `%clk` PGN comments are present in lichess/chess.com exports but absent in many historical files. Time-management analysis must degrade gracefully when clock data is unavailable.

**Resource risks:**
- Solo developer: strictly one spec at a time, no parallel work. Each spec must be fully done (all ACs checked, tests passing, sanitizers clean) before the next begins.
- Scope creep: statistical analysis and engine integration are explicitly deferred. If they feel urgent, add a spec rather than expanding an existing one.

## User Journeys

### Journey 1: Opening Preparation Session (Primary — Happy Path)

**Bogdan, FIDE Master, returning to tournament play after two years.**

He hasn't played an OTB tournament since 2024. His repertoire as Black against 1.e4 used to be the French Defense, but he's suspicious it's gotten rusty — too many online blitz games where he played on autopilot without preparing.

He opens motif-chess. His lichess and chess.com games were already imported last week (3,400 games). He navigates to the opening tree, filters for his Black games against 1.e4, and selects the French Defense branch. The statistics panel shows him win/draw/loss rates at each branching point, annotated with average opponent rating and game phase error counts.

He immediately spots it: in the Advance Variation (1.e4 e6 2.d4 d5 3.e5), his win rate drops from 52% to 31% after White plays f4. He's played this position 47 times and has a losing record. He clicks into the position — the position search panel shows how grandmasters have handled it in the reference database. He realizes he's been playing the wrong plan for years.

He marks the position as a preparation target, fires up the engine analysis panel, and spends 40 minutes working through the correct plan. He saves his analysis as annotations.

**Capabilities required:** PGN import, opening tree view with statistics, position search against reference database, engine integration, annotation saving.

---

### Journey 2: Bulk Import + Recovery (Primary — Edge Case)

**Bogdan downloads Caissabase (4.3M games, 2.1GB PGN) to use as a reference database.**

He starts the import. Halfway through — 2.1M games in — his laptop's power cuts out. When he restarts motif-chess, the import dialog shows the previous session: 2,134,847 games committed, last checkpoint at game 2,134,800. He clicks "Resume." The import picks up from the checkpoint, processes the remaining 2.16M games, and completes without re-importing the committed games.

One game in the source file is malformed (truncated mid-move). The import log shows: `[WARN] Skipped game 3,041,822: unexpected EOF at move 34. White: Carlsen, Magnus. Black: Nepomniachtchi, Ian. Event: WCC 2021.` He can investigate later; the import didn't stop.

**Capabilities required:** Resumable import with checkpointing, per-game error recovery, structured import log, progress display.

---

### Journey 3: Open-Source Contributor Onboarding

**A strong club player and C++ developer finds motif-chess on GitHub.**

He reads the README. The build instructions are three commands. He runs `nix develop` (or uses the direnv integration), then `cmake --preset=dev && cmake --build build/dev`. Everything compiles. He runs `ctest --test-dir build/dev` — all tests pass.

He wants to add a feature: filter games by time control. He reads `CONTRIBUTING.md`, finds the specs directory, understands the architecture from `plans/design.md` and the project context. He opens an issue, writes a spec stub, and submits a PR. The CI pipeline runs clang-tidy, cppcheck, and the test suite automatically.

He is never blocked by undocumented build steps or mysterious binary formats.

**Capabilities required:** Clean build from source, documented architecture, clear contribution pathway, CI enforcement of quality standards.

---

### Capability Coverage

| Capability | Journey |
|---|---|
| PGN import (parallel, resumable, error-recovering) | 1, 2 |
| Opening tree with per-position statistics | 1 |
| Position search against reference database | 1 |
| UCI engine integration | 1 |
| Game annotation and saving | 1 |
| Import progress display + structured log | 2 |
| Checkpoint/resume for large imports | 2 |
| Clean build from source + documented architecture | 3 |
| CI pipeline (clang-tidy, tests, cppcheck) | 3 |
| Binary/installer distribution (macOS, Windows) | Phase 2 |

## Domain-Specific Requirements

### Chess Data Format Interoperability

- **PGN** is the sole import format. PGN files in the wild are frequently malformed (missing headers, truncated games, non-standard tags, mixed encodings). The import pipeline must treat malformed games as skippable errors, never as fatal failures. Correct PGN handling is a correctness requirement, not a robustness nice-to-have.
- **FEN** is the canonical position representation. Every position stored or displayed must be derivable from a valid FEN string.
- **SAN** (Standard Algebraic Notation) is the move representation in PGN and in the UI. SAN parsing and generation is owned by `chesslib`; motif-chess must not reimplement or second-guess it.
- **UCI** (Universal Chess Interface) is the engine communication protocol. Major engines (Stockfish, Leela Chess Zero, Komodo) each have implementation quirks beyond the spec. `ucilib` must handle these gracefully.

### Technical Constraints

- **Zobrist hash correctness:** Position identity across the entire database depends on Zobrist hash collision resistance. Hash computation is owned by `chesslib`; motif-chess must use it consistently and never derive hashes independently.
- **DuckDB hot path:** The `PositionIndex` table will reach 400M+ rows at 10M games × 40 avg ply. Position search (lookup by `zobrist_hash UBIGINT`) is the performance-critical query. Schema design and query patterns must be optimized for single-column `UBIGINT` scans on a columnar store.
- **Opening tree traversal:** Rendering an opening tree involves recursive position traversal to depth 30+ with branching factors of 10–30 at master-level positions. Tree construction must be lazy/paginated — never fully materialized in memory.
- **Thread safety:** SQLite (WAL mode) supports concurrent readers; DuckDB is single-writer during import. The import pipeline and the UI query layer must not share write transactions across the two stores.

### Data Source Constraints (Phase 2)

- **lichess API:** Fully open, CC0 licensed. Game export via `https://lichess.org/api/games/user/{username}`. No attribution requirement.
- **chess.com API:** Rate-limited; exported games carry terms-of-service restrictions on redistribution. Suitable for personal use; do not bundle chess.com games in any distributed database.

### Data Integrity Risks

- **Database corruption on crash:** SQLite WAL mode provides crash safety for the metadata store. DuckDB import must use explicit transaction commits with checkpoints so a crash mid-import leaves the database in a consistent, resumable state — not a corrupt one.
- **Silent data loss from bad PGN:** Every skipped game must be logged with enough context (game number, headers, error reason) to allow manual investigation. Silent skip without logging is a data integrity failure.
- **Zobrist collision:** Statistically negligible with 64-bit hashes, documented as a known limitation. Collision detection is not implemented — it would destroy query performance.

## Innovation & Novel Patterns

### Differentiating Capabilities

**1. AI-driven personalized repertoire construction**

No existing chess tool generates an opening repertoire *for you* based on analysis of your own game corpus, playing style, and theoretical gaps. ChessBase, Scid, and equivalents are passive databases — you look things up, they don't advise you. The repertoire builder in motif-chess inverts this: given a player's games, strengths, weaknesses, and the theoretical landscape of their preferred openings, it generates a repertoire proposal. This is a genuinely novel capability in the chess tooling space.

**2. Statistical analysis correlated with metadata**

Existing tools count win/draw/loss by opening. Motif-chess targets a qualitatively different insight level: correlating performance with time management, game phase, opponent rating band, game duration, and error distribution. The insight "you lose in IQP positions when the game exceeds 40 moves and you're in time pressure" is not available from any current tool — it requires joining metadata (time control, game duration, clock data from PGN comments) with position-level statistics, enabled by the dual-store architecture.

**3. Dual-store columnar architecture for chess**

Using DuckDB for position analytics alongside SQLite for metadata is an architectural choice that no open-source chess database has made. DuckDB's columnar storage is exceptionally well-suited for the position index query pattern (scanning a single `UBIGINT` column over hundreds of millions of rows).

### Competitive Landscape

| Tool | Strengths | Weaknesses |
|---|---|---|
| ChessBase | Industry standard, feature-rich | Proprietary, expensive, Windows-only, closed format |
| Scid/Scid vs PC | Open-source, functional | Aging codebase, Tcl/Tk UI, no statistical depth |
| scidb | Ambitious architecture | Unmaintained, reimplements STL, complex binary format |
| ChessX | Qt-based, cross-platform | Primitive, minimal features |
| lichess (web) | Excellent free analysis | Web-only, no local database, no personal stats depth |

The gap: no open-source, ergonomic, high-performance chess database with deep personal statistical analysis exists. Motif-chess occupies this space.

### Validation Approach

- **Statistical analysis validity:** Cross-check per-position and per-game statistics against manual calculation on a known dataset (e.g., 3,400 lichess games cross-referenced against lichess's own stats)
- **AI repertoire quality:** Use the generated repertoire for actual preparation; track OTB/online results over time — the ultimate real-world test
- **Performance at scale:** Import Caissabase (~4M games) during spec 003 to verify position search latency meets the <100ms P99 target before committing to the 10M-game architecture

## Desktop Application Requirements

### Platform Support

- **Primary:** Linux (NixOS). All development and initial release targets Linux exclusively.
- **Phase 2:** macOS (Nix build) and Windows (vcpkg) once the Linux version is feature-complete. Binary releases and installers planned for both.
- **Qt 6** as the UI framework ensures platform portability.
- No web or mobile targets.

### System Integration

- `.pgn` file association (OS-level handler — `.desktop` file on Linux, bundle metadata on macOS)
- All database files stored in a user-specified directory; no hardcoded paths
- UCI engines are external processes launched by `ucilib`; engine paths configured by the user, not bundled

### Update Strategy

- No in-app auto-update — updates delivered via Nix flake, GitHub releases, or distro packages
- Version displayed in About dialog

### Offline Capabilities

- Fully offline for all core features. Network access is optional, scoped to Phase 2 (lichess/chess.com game fetch).

### Configuration

- Per-user config (`~/.config/motif-chess/`) for: database directory, recent databases, engine paths, UI preferences (board theme, piece set)
- Configuration never stored inside database files — databases remain portable

### Implementation Notes

- **UI framework:** Qt 6. The choice between Widgets, QML, or hybrid is deferred to spec 004. QML is well-suited for the chessboard widget, piece animations, eval graph, and board overlays; Qt Model/View (`QAbstractItemModel`) for data-heavy panels (game lists, opening tree, filter panels).
- **Concurrency:** Parallelism is a first-class architectural concern. `taskflow` is the preferred task graph library for CPU-intensive pipelines (import workers, position indexing, statistical analysis); alternatives may be considered but should be raised before implementation begins on spec 002.
- High-DPI display support required
- Keyboard-driven navigation is first-class — power users should never be forced to reach for the mouse

## Functional Requirements

### Game Database Management

- **FR01:** The user can create and open a named game database stored at a user-specified file system path
- **FR02:** The user can insert a game with full metadata (players, event, site, date, result, ECO, extra PGN tags) and encoded move sequence
- **FR03:** The system deduplicates players and events on insert — the same name always resolves to the same record
- **FR04:** The system detects duplicate games on insert and skips or rejects them based on import configuration
- **FR05:** The user can retrieve a complete game (metadata + move sequence) by game ID
- **FR06:** The user can delete a game and all associated data (tags, position index entries, opening stats)
- **FR07:** The database schema is created idempotently — opening an existing database never overwrites or errors
- **FR08:** The user can inspect the database with standard CLI tools (`sqlite3`, `duckdb`) independent of motif-chess

### PGN Import

- **FR09:** The user can import one or more PGN files into the active database
- **FR10:** The import pipeline processes multiple games concurrently using available CPU cores
- **FR11:** Malformed or unreadable games are skipped and logged with identifying context (game number, headers, error reason); import continues
- **FR12:** The user can interrupt an import and resume it from the last committed checkpoint without re-importing already-committed games
- **FR13:** The user can monitor import progress (games processed, games committed, games skipped, elapsed time)
- **FR14:** The import pipeline reports a structured summary on completion (total, committed, skipped, errors)
- **FR15:** The import pipeline enforces a configurable memory ceiling during processing

### Position Search & Opening Statistics

- **FR16:** The user can search for any position by its Zobrist hash and retrieve all games that reached that position
- **FR17:** For any position, the user can view opening statistics: move frequency, win/draw/loss rates by color, average Elo of games
- **FR18:** The user can navigate an opening tree from any starting position, with per-node statistics
- **FR19:** The opening tree is traversed lazily — only nodes the user expands are loaded
- **FR20:** The position index is populated automatically during import

### User Interface

- **FR21:** The user can view a chessboard displaying any position from the active game
- **FR22:** The user can navigate through a game move by move (forward, backward, to start, to end)
- **FR23:** The user can view and navigate a game tree including variations
- **FR24:** The user can initiate a PGN import from within the UI and monitor its progress
- **FR25:** The user can search for a position from the current board state and view results
- **FR26:** The user can open, browse, and filter the game list in the active database
- **FR27:** The UI supports high-DPI displays
- **FR28:** The user can perform all core navigation actions via keyboard without requiring the mouse

### Engine Integration *(Phase 2)*

- **FR29:** The user can configure one or more UCI-compatible chess engines by specifying their executable path
- **FR30:** The user can start and stop engine analysis on the current board position
- **FR31:** The user can view engine output (depth, score, principal variation) updating in real time
- **FR32:** The engine integration isolates engine crashes — an engine failure does not crash motif-chess

### Statistical Analysis *(Phase 2)*

- **FR33:** The user can view their performance statistics by opening, broken down by result, color, and opponent rating range
- **FR34:** The user can view error distribution across game phases (opening, middlegame, endgame) for their personal game corpus
- **FR35:** The user can view time-management statistics for games where clock data is available in PGN
- **FR36:** Statistical analysis degrades gracefully when metadata (clock data, Elo) is absent from source games

### Repertoire Management *(Phase 2)*

- **FR37:** The user can build and save an opening repertoire as a set of annotated lines
- **FR38:** The user can drill repertoire lines in a practice mode

### Configuration & System

- **FR39:** The user can configure the active database directory, recently used databases, engine paths, and UI preferences
- **FR40:** Configuration is stored per-user and independently of database files
- **FR41:** Database files are portable — moving or copying them to another machine preserves all game data
- **FR42:** The application registers as a handler for `.pgn` files at the OS level

### AI Repertoire Construction *(Phase 3)*

- **FR43:** The system can analyze a player's game corpus to identify playing style, opening strengths, and theoretical weaknesses
- **FR44:** The system can generate a personalized opening repertoire proposal based on the player's profile and corpus analysis
- **FR45:** The generated repertoire accounts for the player's current strength and preferred piece structures

## Non-Functional Requirements

### Performance

- **NFR01:** Position search (Zobrist lookup) completes in under 100ms at P99 for a 10M-game corpus
- **NFR02:** Opening statistics query (move frequency, win rates) completes in under 500ms for any position
- **NFR03:** A 10M-game corpus imports in under 20 minutes on reference hardware (modern multi-core desktop)
- **NFR04:** Memory usage during import does not exceed 2GB regardless of corpus size
- **NFR05:** UI interactions (board navigation, move forward/back, opening tree expansion) respond within 100ms
- **NFR06:** The application starts and presents a usable window in under 3 seconds on first launch
- **NFR20:** CPU-intensive operations (PGN import, position indexing, statistical analysis) exploit task-level parallelism across available cores; `taskflow` is the preferred task graph library

### Reliability

- **NFR07:** A crash or power loss during import leaves the database in a consistent, queryable state — no corruption, no partial game commits
- **NFR08:** Import can always be resumed from the last checkpoint after an interrupted session
- **NFR09:** The SQLite and DuckDB stores are eventually consistent: DuckDB is derived data and can always be rebuilt from SQLite; SQLite is the authoritative source
- **NFR10:** Malformed PGN input never causes a crash — all error paths are handled and logged
- **NFR11:** The test suite passes with zero errors under ASan and UBSan (`cmake --preset=dev-sanitize`)

### Code Quality

- **NFR12:** Every build under the `dev` preset produces zero clang-tidy warnings
- **NFR13:** Every build under the `dev` preset produces zero cppcheck warnings
- **NFR14:** Every public API function has at least one Catch2 v3 test
- **NFR15:** All code is formatted with clang-format before commit; CI enforces this

### Integration

- **NFR16:** PGN import correctly handles all legal PGN as defined by the PGN standard, including recursive variations, NAGs, comments, and `%clk` clock annotations
- **NFR17:** FEN representation is standard and compatible with any FEN-consuming tool (engines, web boards)
- **NFR18:** UCI engine communication is compatible with Stockfish 17+, Leela Chess Zero, and Komodo Dragon without engine-specific workarounds in the caller
- **NFR19:** Exported or displayed SAN is unambiguous and valid per the FIDE algebraic notation standard
