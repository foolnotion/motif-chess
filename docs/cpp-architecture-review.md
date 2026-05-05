# Modern C++ Architecture Review

**Date**: 2026-05-04
**Reviewer**: AI-assisted review from a modern C++ perspective (C++23, Clang 21)
**Scope**: Full codebase — all modules, build system, tests

---

## Strategic Context

### Why Qt, Not Web Frontend

The long-term product vision is a **Qt desktop application**, not a web frontend over a REST API. The HTTP server was built as an MVP to get something working fast, and the current TypeScript+Vite+Zustand frontend was a means to that end.

Chess database applications are inherently desktop workloads:

- **Multi-panel layouts** — board, opening tree, engine analysis, game list, statistics — all visible simultaneously. Qt dock widgets and QML layouts handle this trivially. Replicating a polished multi-panel chess UI in CSS is a disproportionate effort that doesn't compound.
- **GPU-accelerated board rendering** — QML/Qt Quick gives animated piece moves, drag-and-drop, and smooth zoom with far less effort than wrangling Canvas/SVG in a browser. A usable `ChessBoard` component is ~200 lines of QML.
- **Local filesystem access** — drag a PGN onto the window, it imports. No upload dialogs, no file size limits, no browser security sandbox.
- **Keyboard-driven workflows** — tree navigation with arrow keys, inline editing, keyboard shortcuts. Native widget toolkits handle this by default; web requires explicit keydown handling and focus management.
- **Single language, single build** — no TypeScript, no bundler, no node_modules, no CORS, no WebSocket plumbing. The entire stack is C++ with a declarative UI layer.

Lichess proves web chess *can* be good, but Lichess also has a team of 20+ frontend engineers and still can't match ChessBase/Scid for database-oriented workflows. The current web frontend isn't ugly because web tech is fundamentally incapable — it's ugly because building a polished chess UI in CSS/JS is a massive amount of work that doesn't accumulate toward the product's core value.

The web frontend is retained only for the MVP. The Qt desktop app is the target.

### Deployment Model: Local + Remote

The deployment model is:

- **Local (tournament laptop)**: Qt desktop app. Board visualization, game browsing, opening tree exploration. Local SQLite for small/personal databases. Lightweight local engine analysis (single core, limited depth). Works fully offline.
- **Remote (backend server)**: Large shared databases, heavy multi-core/GPU engine analysis, bulk imports, cross-device sync. The current HTTP server evolves into this backend.

This means the codebase needs a **repository/transport abstraction**: the Qt client must not know whether it's talking to local SQLite/DuckDB or a remote HTTP service. The same domain types and business logic work in both modes; only the data source changes.

The web frontend remains functional during the transition but receives no further investment.

---

## What You're Doing Well

- **`tl::expected` everywhere** — consistent, no exceptions crossing boundaries. The right call for a library with C interop.
- **Module boundary discipline** — `motif_import` and `motif_search` never touch SQLite/DuckDB directly. Clean layering.
- **Real DB tests, no mocks** — in-memory SQLite, real DuckDB. This is the CppCon advice (Kevlin Henney, the "mocks are a design smell" camp) in practice.
- **RAII discipline** — `unique_stmt`, `txn_guard`, `result_guard`, pImpl where it matters.
- **Build hardening** — aggressive warnings, sanitizers, clang-tidy essentially all-on. This is the floor, not the ceiling.

---

## Findings

### 1. No Repository Abstraction — The Critical Architectural Gap

The biggest architectural gap for the local/remote deployment model is that every module is tightly coupled to its storage backend. `game_store` takes `sqlite3*`. `position_store` takes `duckdb_connection`. `engine_manager` directly manages local UCI processes. There is no abstraction layer that would let the Qt client work against either local storage or a remote backend.

Today the call chain is: `Qt UI -> HTTP server -> database_manager -> game_store (sqlite3*)`. In the target architecture it needs to be: `Qt UI -> repository interface -> local or remote implementation`.

**What to do**: Introduce a `motif::repository` abstraction layer:

```cpp
// Pure interfaces — no storage headers, no Qt, no network headers
namespace motif::repository {

class game_repository {
public:
    virtual ~game_repository() = default;
    virtual auto get(uint32_t id) -> db::result<db::game> = 0;
    virtual auto list(db::game_list_query const& q) -> db::result<std::vector<db::game_list_entry>> = 0;
    virtual auto count() -> db::result<int64_t> = 0;
    virtual auto patch(uint32_t id, db::game_patch const& p) -> db::result<void> = 0;
    virtual auto remove(uint32_t id) -> db::result<void> = 0;
};

class position_repository {
public:
    virtual ~position_repository() = default;
    virtual auto find(uint64_t hash, size_t limit, size_t offset) -> db::result<std::vector<db::position_match>> = 0;
    virtual auto opening_stats(uint64_t hash) -> search::result<search::opening_stats::stats> = 0;
    virtual auto tree(uint64_t root_hash, uint16_t max_depth) -> db::result<std::vector<db::tree_position_row>> = 0;
};

class analysis_service {
public:
    virtual ~analysis_service() = default;
    virtual auto start(engine::analysis_params const&) -> engine::result<std::string> = 0;
    virtual auto stop(std::string const& id) -> engine::result<void> = 0;
    virtual auto subscribe(std::string const& id, engine::info_callback, engine::complete_callback, engine::error_callback)
        -> engine::result<engine::subscription> = 0;
};

class import_service {
public:
    virtual ~import_service() = default;
    virtual auto start(std::filesystem::path const& pgn, import::import_config const&) -> import::result<std::string> = 0;
    virtual auto progress(std::string const& id) -> import::import_progress = 0;
    virtual auto cancel(std::string const& id) -> void = 0;
};

}
```

Then provide two implementations:

- **`local_repository`** — wraps `database_manager`, `engine_manager`, `import_pipeline` directly. Used in standalone desktop mode.
- **`remote_repository`** — wraps HTTP client calls to the backend. Used when connected to a remote server.

The Qt app selects the implementation at startup based on configuration. The current `motif_http` server becomes the remote backend that serves `remote_repository` clients.

**Why this matters now**: Every piece of new code written against `game_store` or `position_store` directly is code that will need to be refactored when the Qt client arrives. The repository interfaces define the stable contract that both local and remote implementations satisfy.

---

### 2. `motif::chess` Facade — Needed by Both Local and Remote Paths

This review originally found `chesslib` headers included across multiple
production modules with chess logic scattered through `server.cpp`,
`opening_stats.cpp`, `database_manager.cpp`, and `import_pipeline.cpp`.
That specific issue has since been addressed by introducing a `motif::chess`
facade module so production `chesslib` usage is centralized behind one entry
point.

For the local/remote split, the Qt client needs board operations (legal moves,
FEN, SAN, move validation) without any DB or network dependency. That is now
the role of the shipped `motif::chess` facade instead of exposing chesslib
directly to the rest of the codebase.

**What exists now**: `motif::chess` already centralizes the entry points motif
needs and keeps production chesslib usage behind one boundary:

```cpp
namespace motif::chess {
    // Value types — no chesslib headers leak
    class board { /* pImpl over chesslib::board */ };
    struct move_info {
        uint16_t encoded_move;
        std::string uci;
        std::string san;
        std::string from;
        std::string to;
        std::optional<std::string> promotion;
    };

    auto parse_fen(std::string_view fen) -> result<board>;
    auto write_fen(board const&) -> std::string;
    auto replay(std::span<uint16_t const>, uint16_t ply) -> result<board>;
    auto san(board const&, uint16_t encoded_move) -> std::string;
    auto apply_san(board&, std::string_view san_move) -> result<uint16_t>;
    auto legal_moves(board const&) -> std::vector<move_info>;
    auto apply_uci(board&, std::string_view uci_move) -> result<move_info>;
    auto apply_encoded_move(board&, uint16_t encoded_move) -> void;
}
```

This module has **no dependency on `motif_db`** and can be used by both the Qt client and the backend without pulling in SQLite/DuckDB. It also creates the seam needed to swap chesslib later if needed.

**What to do next**: Keep extending repository and client code in terms of this
facade rather than adding new direct chesslib dependencies.

---

### 3. Domain Types Need to Cross Network Boundaries

In the current architecture, types in `db/types.hpp` are used only within the process. In the local/remote model, these types are serialized/deserialized at the transport boundary. This has implications:

- **Error types must be serializable**. Today `error_code` is a simple enum, but it needs to travel across the network with context. See finding #6.
- **Types need stable serialization format**. glaze (JSON) works today, but the wire format needs to be versioned and backward-compatible. Consider a schema or version field in responses.
- **Date format inconsistency** is no longer just a bug risk — it's a wire protocol bug. `game_list_query` stores `YYYY-MM-DD` in the query but the DB uses `YYYY.MM.DD` (PGN convention). In a client/server model, the client sends dates and the server interprets them. This needs to be normalized at the type level, not left as a comment.

**What to do**: Define a `motif::protocol` or `motif::dto` namespace for types that cross boundaries. These types have validation, serialization, and versioning. Internal domain types (`db::game`, `db::position_row`) stay as they are — fast, aggregate, no overhead. The protocol types are the anti-corruption layer at the boundary.

---

### 4. Raw Handle Types in Public APIs

`game_store` takes `sqlite3*` and `position_store` takes `duckdb_connection` (which is `void*`). These are non-owning raw pointers with no lifetime documentation beyond comments. `game_store` stores 8 `sqlite3_stmt*` members as raw pointers and manually manages them with `finalize_stmt()` in the destructor and move operations.

The `unique_stmt` RAII wrapper in `sqlite_util.hpp` is used for *one-shot* queries but the 8 cached statements are still raw. This is exactly the kind of inconsistency that leads to leaks on early returns.

**What to do**: Make `unique_stmt` the standard for *all* statements, including cached ones. The cached statements should be `std::array<unique_stmt, 8>` or named `unique_stmt` members. This eliminates the manual `finalize_stmt()` calls and makes the move constructor/destructor trivially correct. For `position_store`, wrap `duckdb_connection` in a proper RAII type with ownership semantics documented in the type system.

This is a quick win that makes the storage layer safer regardless of whether it's accessed locally or through a repository.

---

### 5. `position_store` Builds SQL by String Concatenation

`position_store.cpp` has 8+ queries built via `fmt::format` with direct integer interpolation. These are integers, not user strings, so it's not an injection vulnerability today. But it's fragile and — critically — you're already paying the cost of a C API without getting the safety benefit. DuckDB's C API supports prepared statements (`duckdb_prepare` / `duckdb_bind_*`).

More importantly, the SQL is invisible to static analysis. No tool can verify that the column list in your SELECT matches the `by_zobrist_col` constants. One day someone adds a column to the query and forgets to update the constant, and you get silent data corruption.

**What to do**: Create a `duckdb_prepared_stmt` RAII wrapper (analogous to `sqlite_util.hpp`'s `unique_stmt`), bind parameters properly, and move all SQL into `constexpr` raw string literals like you already do for schema creation.

---

### 6. Error Types Must Carry Context and Be Transportable

Every module has a 4-6 value `error_code` enum. When `position_store::insert_batch` fails, you return `error_code::io_failure`. No DuckDB error message, no SQLite error code, no path. The information is discarded at the module boundary.

In the local/remote model this gets worse: a remote repository call fails, and the client sees `error_code::io_failure` with no indication of whether the server is down, the DB is corrupt, or the query was malformed.

**What to do**:

```cpp
// In each module's error.hpp:
struct error {
    error_code code;
    std::string message;          // human-readable, safe to show/log
    // Optionally: source_location for debug builds
};

template<typename T>
using result = tl::expected<T, error>;
```

Errors are logged at the site where they occur (before the original context is lost) *and* propagated with enough information for the caller to act on them. For the remote path, errors are serialized into JSON responses so the Qt client can display meaningful messages.

---

### 7. `game_store` Responsibilities Should Be Separated

`game_store` is 1217 lines handling schema creation, entity caching, CRUD, pagination, patching, and provenance. The insert path (entity caches, prepared statements, transaction management) shares no state with the query paths (read-only selects, list/filter). In the repository model, reads go through one implementation and writes through another (remote vs local).

**What to do**: Extract:

- An `entity_cache` for the player/event/tag ID resolution caches
- A `game_writer` for the insert+transaction path (used only by `local_repository` and the import pipeline)
- Keep `game_store` as the read/query interface

This aligns naturally with the repository split — `local_repository` uses both reader and writer, `remote_repository` sends write requests over HTTP and doesn't need the writer at all.

---

### 8. Two-Engine Storage with No Transactional Coordination

SQLite and DuckDB are independent stores. The `position_index_dirty` crash recovery flag is pragmatic but incomplete. If `delete_by_game_id` succeeds but `game_store::remove` fails, you have orphaned position rows right now, not just after a crash.

In the remote backend context, this matters less (the server is long-lived, rebuilds are rare) but for the local desktop context (laptop at a tournament, might sleep/wake, might run out of disk), it matters more.

**What to do**:

- Document the consistency model explicitly (eventual consistency via rebuild)
- Add a `verify_consistency()` method that cross-checks game counts
- Consider making the position store purely derived (always rebuilt from SQLite) rather than incrementally maintained

---

### 9. `opening_stats::query` Does Too Much Work in One Function

It makes 3 separate DB calls, replays board positions, resolves ECO codes, builds SAN, and sorts results. In the repository model, this orchestration should happen on the backend (where the data lives) but the function structure makes it hard to remote — the client can't call `count_distinct_games_by_zobrist`, `query_opening_stats`, `get_game_contexts`, and then do board replay locally (it doesn't have the data).

The board replay in `find_root_board` is O(candidates x ply) with no caching. For the backend, this is a hot path.

**What to do**: Keep `opening_stats::query` as a single backend operation (it should be one remote call, not three). But decompose the internal logic into testable pieces and cache board replay results within a single query invocation.

---

### 10. Thread Safety Is Ad-Hoc

`server.cpp` uses a single `database_mutex` for all DB access. The import pipeline has its own cooperative cancellation. The engine manager has its own mutex and callback threading. No unified model.

For the Qt desktop app, concurrency is different: the UI thread must never block on DB or engine calls. The `local_repository` needs to be async or return futures. The `remote_repository` already deals with network latency.

The import pipeline's 6 atomic counters have no snapshot consistency — `progress()` reads them one at a time.

**What to do**:

- Define the concurrency contract for repository interfaces: are they thread-safe? Do they return futures?
- The import progress should use a single atomic snapshot (`std::atomic<std::shared_ptr<import_progress>>` or seqlock)
- The Qt app will likely need a thread pool + signal/slot bridge between repository calls and the UI thread

---

### 11. Types Are Bags of Fields with No Invariants

Every struct in `types.hpp` is an aggregate with all-public members and no validation. `game_patch` has 11 `std::optional` fields. `game_list_query` has date filters that silently return wrong results if you pass `YYYY-MM-DD` but the DB stores `YYYY.MM.DD`. `analysis_params` says "Exactly one of depth or movetime_ms must be set" — but this is a comment, not an invariant.

In the local/remote model, these types cross the network boundary. Invalid data from the client silently corrupts queries.

**What to do**: Types that cross API boundaries need validation:

- `analysis_params` should have a factory or validated constructor
- `game_list_query` should normalize date formats
- `game_patch` should validate that at least one field is set
- Consider a `validated<T>` wrapper that enforces invariants at construction time

---

### 12. Strong Typing for Domain Concepts

`game_store::get` takes `uint32_t` — any uint32_t, not a valid game ID. `encoded_move` is `uint16_t` everywhere. `zobrist_hash` is `uint64_t`. These are "primitive obsession" smells that make it easy to pass the wrong value to the wrong parameter.

In the repository model, the interfaces need to be self-documenting so that Qt client code can't accidentally pass a `game_id` where a `zobrist_hash` belongs.

**What to do**: Strong typedefs or named aliases for domain IDs:

```cpp
struct game_id { uint32_t value; };
struct zobrist_hash { uint64_t value; };
struct encoded_move { uint16_t value; };
```

Even without full wrapper types, `using` aliases with documentation make APIs self-documenting and catch parameter-order bugs at the type level.

---

## Revised Priority Ranking

Prioritized for the local-desktop + remote-backend deployment model. Items that support the repository abstraction are weighted higher.

| # | Item | Impact | Effort | When |
|---|------|--------|--------|------|
| 1 | Repository abstraction | Critical | High | **Foundation — do first** |
| 2 | Error types with context | High | Medium | **With repository work** |
| 3 | Domain type invariants + DTO layer | High | Medium | **With repository work** |
| 4 | RAII-wrap all cached statements | Medium | Low | **Quick win, anytime** |
| 5 | Parameterized DuckDB queries | Medium | Medium | **Safety win, anytime** |
| 6 | Strong typedefs for domain IDs | Medium | Low | **With repository interfaces** |
| 7 | Split `game_store` (reader/writer) | Medium | Medium | **Before repository work** |
| 8 | `opening_stats` decomposition | Low | Medium | When performance matters |
| 9 | Consistency model documentation | Low | Low | Write it down |
| 10 | Thread safety model | Medium | High | With Qt client work |

### Recommended Sequence

**Phase 1 — Foundation (before Qt work)**:
1. RAII-wrap cached statements (#5) — quick win, reduces risk
2. Error types with context (#3) — needed by both local and remote paths
3. Strong typedefs (#7) — cheap, clarifies all interfaces
4. Split `game_store` (#8) — separates read/write paths
5. Keep new board/FEN/SAN work behind `motif::chess` rather than adding fresh chesslib call sites

**Phase 2 — Repository Layer**:
6. Define repository interfaces (#1) — the stable contract
7. Implement `local_repository` — wraps existing modules
8. Domain type invariants + DTO layer (#4) — boundary validation
9. `remote_repository` client + backend evolution

**Phase 3 — Qt Desktop**:
10. Thread safety model (#11) — async repository calls, UI thread bridge
11. Performance tuning (#9, #10) as needed

### Explicitly Deferred

- **Server.cpp decomposition** — the server evolves into the remote backend naturally; don't over-invest now
- **HTTP test file split** — follows server evolution
- **Parameterized DuckDB queries (#6)** — important but orthogonal to the architecture work
