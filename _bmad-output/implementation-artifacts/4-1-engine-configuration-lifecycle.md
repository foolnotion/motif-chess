# Story 4.1: Engine Configuration & Lifecycle

Status: done

## Story

As a user,
I want to configure one or more UCI chess engines by executable path and start/stop analysis on a FEN position,
So that any motif-chess frontend can get engine evaluations without managing engine processes directly.

## Acceptance Criteria

1. **Given** an engine executable path is registered with `engine_manager`
   **When** `list_engines()` is called
   **Then** the engine is returned with its name and path (FR29)

2. **Given** a valid engine path is registered and a valid FEN is provided
   **When** `start_analysis(params)` is called
   **Then** `engine_manager` starts a `uci::engine` subprocess via `ucilib`, sends the FEN, applies `go_params` from `analysis_params`, and returns an opaque `analysis_id` (FR30)
   **And** `ucilib` owns all subprocess lifecycle — `motif_engine` does not fork, exec, or reap processes directly (NFR18)

3. **Given** `subscribe(analysis_id, on_info, on_complete, on_error)` is called after `start_analysis`
   **When** the engine emits `info` lines
   **Then** `on_info` is invoked with depth, seldepth (if present), multipv, score (type+value), `pv_uci` (raw UCI moves), and `pv_san` (SAN strings where chesslib conversion succeeds)
   **And** when the engine sends `bestmove`, `on_complete` is invoked with `best_move_uci` and `ponder_uci`

4. **Given** analysis is active
   **When** `stop_analysis(analysis_id)` is called
   **Then** `uci::engine::stop()` is called, the session transitions to a terminal state, and subsequent `stop_analysis` calls return `error_code::analysis_already_terminal` (FR30)

5. **Given** `start_analysis` is called with a non-empty `engine` field that does not match any registered engine
   **When** the call is processed
   **Then** `error_code::engine_not_configured` is returned without launching any subprocess

6. **Given** `start_analysis` is called with no registered engines at all
   **When** the call is processed
   **Then** `error_code::engine_not_configured` is returned (preserves stub behavior for callers with no config)

7. **Given** the engine subprocess crashes or exits unexpectedly during analysis
   **When** `engine_manager` detects this via `ucilib`
   **Then** `on_error` is called with a descriptive message and the session transitions to a terminal state
   **And** `motif-chess` continues running normally — no exception propagates out of the callback path (FR32)

8. **Given** all changes are implemented
   **When** `cmake --preset=dev && cmake --build build/dev && ctest --test-dir build/dev` is run
   **Then** all tests pass with zero new clang-tidy or cppcheck warnings

9. **Given** all changes are implemented
   **When** `cmake --preset=dev-sanitize && cmake --build build/dev-sanitize && ctest --test-dir build/dev-sanitize` is run
   **Then** zero ASan/UBSan violations

## Tasks / Subtasks

- [x] Task 1: Wire ucilib and chesslib into `motif_engine` CMake target (AC: 2, 3, 8)
  - [x] Add `find_package(ucilib REQUIRED)` and link `ucilib::ucilib` in `source/motif/engine/CMakeLists.txt`
  - [x] Add `find_package(chesslib REQUIRED)` and link `chesslib::chesslib` for SAN conversion of PV moves
  - [x] Confirm build compiles with both new deps; fix any transitively missing find_package calls

- [x] Task 2: Add engine registry to `engine_manager` (AC: 1, 5, 6)
  - [x] Add `struct engine_config { std::string name; std::string path; }` in `engine_manager.hpp`
  - [x] Add `configure_engine(engine_config) -> tl::expected<void, error_code>` to `engine_manager`
    - Validates path is non-empty; returns `error_code::engine_not_configured` for empty path
    - Stores by name; overwrites on re-configure with same name
  - [x] Add `list_engines() -> std::vector<engine_config>` to `engine_manager`
  - [x] If `analysis_params::engine` is empty, use the first registered engine (if any); if no engine registered, return `engine_not_configured`
  - [x] If `analysis_params::engine` is non-empty, find by name; if not found, return `engine_not_configured`

- [x] Task 3: Implement `start_analysis` with real ucilib wiring (AC: 2, 5, 6)
  - [x] Define a private `session` struct inside `engine_manager.cpp`:
    ```cpp
    struct session {
        std::string analysis_id;
        std::string start_fen;           // saved for PV SAN conversion
        uci::engine engine;
        analysis_params params;
        std::atomic<bool> terminal{false};
        info_callback on_info;
        complete_callback on_complete;
        error_callback on_error;
    };
    ```
  - [x] Generate `analysis_id` using `<random>` (same approach as existing HTTP stub — random hex string)
  - [x] Call `engine.start(path)` → on `std::error_code` failure, return `error_code::engine_start_failed`
  - [x] Call `engine.is_ready()` with default 5 s timeout → on failure, return `engine_start_failed`
  - [x] If `multipv > 1`, call `engine.set_option("MultiPV", std::to_string(multipv))`
  - [x] Call `engine.set_position(fen)` for non-startpos FEN, or `set_position_startpos()` for the starting position
  - [x] Register `engine.on_info(...)` and `engine.on_bestmove(...)` callbacks **before** calling `go()`
  - [x] Build `uci::go_params` from `analysis_params`: set `depth` or `movetime` (as `uci::milliseconds{n}`) exclusively
  - [x] Call `engine.go(go_params)` → on failure, return `engine_start_failed`
  - [x] Store session in a `std::unordered_map<std::string, std::unique_ptr<session>>` guarded by `std::mutex`
  - [x] Return the `analysis_id`

- [x] Task 4: Implement `stop_analysis` (AC: 4)
  - [x] Look up session by `analysis_id`; return `analysis_not_found` if missing
  - [x] If `session::terminal` is already true, return `analysis_already_terminal`
  - [x] Call `engine.stop()` on the session's engine
  - [x] Set `session::terminal = true`
  - [x] Return `tl::expected<void, error_code>{}` on success

- [x] Task 5: Implement `subscribe` with callback registration (AC: 3, 7)
  - [x] Look up session; return `analysis_not_found` if missing
  - [x] Store `on_info`, `on_complete`, `on_error` in the session
  - [x] The `on_info` ucilib callback (registered in `start_analysis`) should:
    - Map `uci::info` fields to `motif::engine::info_event`
    - Convert `pv` (UCI strings) to SAN using chesslib (see SAN Conversion note below)
    - Invoke the stored `on_info` if set — wrap in `try`/`catch(...)` to prevent subscriber exceptions crashing the engine thread
  - [x] The `on_bestmove` ucilib callback should:
    - Map `uci::best_move` to `motif::engine::complete_event`
    - Set `session::terminal = true`
    - Invoke stored `on_complete` if set — also wrap in `try`/`catch(...)`
  - [x] The crash/error path: ucilib does not expose an `on_error` callback in the current API (`uci::engine` only has `on_info` and `on_bestmove`). Crash detection is therefore not implemented in this story; a future story may add it when ucilib provides the mechanism.

- [x] Task 6: SAN conversion for PV (AC: 3)
  - [x] In `motif_engine.cpp`, add helper:
    ```cpp
    auto pv_to_san(std::string_view start_fen, std::vector<std::string> const& pv_uci)
        -> std::vector<std::string>
    ```
  - [x] Use `chesslib::fen::read(start_fen)` to get initial board
  - [x] For each UCI move string: `chesslib::uci::from_string(board, uci_str)` → if valid, `chesslib::san::to_string(board, move)`, then `chesslib::move_maker{board, move}.make()`
  - [x] If any move fails to parse, stop SAN conversion and return what was produced so far (partial SAN is valid per contract)
  - [x] Required headers: `<chesslib/board/board.hpp>`, `<chesslib/util/fen.hpp>`, `<chesslib/util/san.hpp>`, `<chesslib/util/uci.hpp>` (move_maker is in board.hpp — no separate header)

- [x] Task 7: Update existing tests and add new coverage (AC: 8, 9)
  - [x] Update `test/source/motif_engine/engine_manager_test.cpp`:
    - Keep existing stub tests but update them to reflect the new behavior when no engine is registered (they should still pass: `engine_not_configured` for `start_analysis`, `analysis_not_found` for `stop`/`subscribe`)
    - Add: `configure_engine` stores and lists engines
    - Add: `configure_engine` with empty path fails
    - Add: `start_analysis` with registered engine name not found returns `engine_not_configured`
    - Add: `start_analysis` with empty `engine` field and no registered engines returns `engine_not_configured`
    - Add: `pv_to_san` unit test — convert known starting position PV (e.g., `{"e2e4", "e7e5"}`) to `{"e4", "e5"}` using the starting FEN
    - Add: `pv_to_san` with an invalid UCI move mid-PV produces partial output without crashing
  - [x] **NOTE:** Tests that require launching a real engine subprocess should be skipped by default or guarded by a `[.slow]` tag — do not assume `stockfish` is present in the test environment
  - [x] Use `[motif-engine]` Catch2 tag for all new tests (consistent with existing)

- [x] Task 8: Validate and record results (AC: 8, 9)
  - [x] Run `cmake --preset=dev && cmake --build build/dev`
  - [x] Run `ctest --test-dir build/dev`
  - [x] Run `cmake --preset=dev-sanitize && cmake --build build/dev-sanitize`
  - [x] Run `ctest --test-dir build/dev-sanitize`
  - [x] Apply clang-format to all touched source files

### Review Findings

- [x] [Review][Defer] AC7 crash/error handling requires upstream `ucilib` support — deferred, reason: we control `ucilib` upstream, and the correct fix is to expose crash/error notification there rather than adding a downstream workaround in `motif_engine`.
- [x] [Review][Patch] Fast completion before `subscribe()` loses `bestmove` permanently [source/motif/engine/motif_engine.cpp:289]
- [x] [Review][Patch] Default engine selection is not first-registered [source/motif/engine/motif_engine.cpp:134]
- [x] [Review][Patch] `analysis_params` invariants are not validated before launching the engine [source/motif/engine/motif_engine.cpp:177]
- [x] [Review][Patch] Some ucilib failures are ignored instead of mapped [source/motif/engine/motif_engine.cpp:160]
- [x] [Review][Patch] Random `analysis_id` collisions are not handled [source/motif/engine/motif_engine.cpp:295]
- [x] [Review][Patch] Required lifecycle behavior lacks real-engine or guarded integration test coverage [test/source/motif_engine/engine_manager_test.cpp:113]
- [x] [Review][Patch] OpenAPI documents `player` query for opening stats without implementation [docs/api/openapi.yaml:235]

## Dev Notes

### Scope Boundary

This story implements `motif_engine`'s real engine lifecycle using ucilib. It does **not** include:
- HTTP route wiring (the `POST /api/engine/analyzes` stub in `server.cpp` already returns HTTP 202; the SSE stream and real HTTP wiring belong to Story 4.2)
- Qt UI or config.json persistence for engine paths (that belongs to Story 7.1's `app_config` and `engine_paths` field)
- Calling the HTTP server from `motif_engine` — the boundary goes the other way

After this story, `engine_manager` works as a pure C++ backend: it can be given a path, start a real subprocess, and fire callbacks. Story 4.2 connects it to the SSE HTTP layer.

### ucilib API — Read From Installed Headers

The actual ucilib API is in `/nix/store/.../include/ucilib/`. **Do not guess the API from generic UCI documentation.**

CMake:
```cmake
find_package(ucilib REQUIRED)
target_link_libraries(motif_engine PUBLIC ucilib::ucilib)
```

Key classes and methods (`ucilib/engine.hpp`, `ucilib/types.hpp`):

```cpp
#include <ucilib/engine.hpp>   // uci::engine, uci::go_params
#include <ucilib/types.hpp>    // uci::info, uci::best_move, uci::score, uci::score_type, uci::errc

// Lifecycle
auto start(std::string const& path) -> tl::expected<engine_id, std::error_code>;
auto quit()                         -> tl::expected<void, std::error_code>;
auto is_ready(milliseconds timeout) -> tl::expected<void, std::error_code>;  // default 5000ms

// UCI protocol
auto set_option(std::string_view name, std::string_view value) -> tl::expected<void, std::error_code>;
auto set_position(std::string_view fen, std::vector<std::string> const& moves = {})
    -> tl::expected<void, std::error_code>;
auto set_position_startpos(std::vector<std::string> const& moves = {})
    -> tl::expected<void, std::error_code>;

// Search
auto go(go_params const& params) -> tl::expected<void, std::error_code>;
auto stop()                      -> tl::expected<void, std::error_code>;

// Callbacks (must be registered BEFORE calling go())
void on_info(info_callback cb);        // std::function<void(info const&)>
void on_bestmove(bestmove_callback cb); // std::function<void(best_move const&)>

auto running() const -> bool;
```

`uci::info` fields (all optional except `pv` which is `std::vector<std::string>`):
- `depth`, `seldepth`, `multipv` — `std::optional<int>`
- `score` — `std::optional<uci::score>` where `score.type` is `uci::score_type::cp` or `::mate`
- `nodes` — `std::optional<std::int64_t>`
- `nps`, `time_ms`, `hashfull` — `std::optional<int>`
- `pv` — `std::vector<std::string>` (UCI move strings)

`uci::best_move` fields:
- `move` — `std::string`
- `ponder` — `std::optional<std::string>`

`uci::errc` enum: `engine_not_running`, `uci_handshake_timeout`, `ready_timeout`, `write_failed`, `engine_crashed`, `invalid_argument`.

ucilib transitively uses `reproc++` (process management) and `fmt::fmt`. These come through automatically via the CMake import — no separate `find_package` needed.

### chesslib SAN Conversion Pattern

Established patterns from `source/motif/http/server.cpp` (lines ~537–558):

```cpp
#include <chesslib/board/board.hpp>
#include <chesslib/util/fen.hpp>
#include <chesslib/util/san.hpp>
#include <chesslib/util/uci.hpp>

auto board = chesslib::fen::read(start_fen);  // returns optional<chesslib::board>
if (!board) { /* invalid FEN — return empty SAN */ }

for (auto const& uci_str : pv_uci) {
    auto move_result = chesslib::uci::from_string(*board, uci_str);
    if (!move_result) break;   // stop on first illegal move
    san_list.push_back(chesslib::san::to_string(*board, *move_result));
    chesslib::move_maker {*board, *move_result}.make();
}
```

chesslib CMake target: `chesslib::chesslib`. It is already linked by `motif_db` and `motif_http`; add it to `motif_engine` for PV conversion.

### Thread Safety Requirements

`engine_manager` will be called from the HTTP server's thread pool (multiple concurrent analysis sessions). Use a single `std::mutex` guarding the `sessions_` map. Callbacks are invoked from ucilib's internal reader thread — do **not** hold the mutex inside callback bodies (deadlock risk). Instead, capture all needed session data before releasing the lock when registering callbacks.

The `session::terminal` flag should be `std::atomic<bool>` to avoid data races between the callback thread setting it and the HTTP thread checking it.

### info_event / complete_event Mapping

Current `engine_manager.hpp` types must not change (HTTP stub depends on them):

| `uci::info` field | `motif::engine::info_event` field |
|---|---|
| `*depth` | `depth` (int, use 0 if absent) |
| `seldepth` | `seldepth` (optional<int>) |
| `multipv` value or 1 | `multipv` |
| `score->type == cp` → `"cp"`, `::mate` → `"mate"` | `score.type` (string) |
| `score->value` | `score.value` |
| `pv` | `pv_uci` |
| SAN conversion of `pv` | `pv_san` (optional<vector<string>>) |
| `nodes` | `nodes` (optional<int64_t>) |
| `nps` | `nps` (optional<int>) |
| `time_ms` | `time_ms` (optional<int>) |

`complete_event` maps directly from `uci::best_move`.

### Error Handling Discipline

- All `ucilib` calls return `tl::expected<T, std::error_code>`. Map `std::error_code` failures to `motif::engine::error_code` values at the module boundary; do not let `std::error_code` escape `motif_engine`.
- `engine_start_failed` covers: `start()` failure, `is_ready()` timeout, `go()` failure.
- Never throw or propagate exceptions from callback invocations. Wrap subscriber-provided callbacks in `try { cb(event); } catch (...) { /* log but do not rethrow */ }`.
- Use `fmt::print(stderr, ...)` for logging within engine_manager until spdlog is wired.

### Architecture Compliance

- `motif_engine` must remain Qt-free. No `<Q*>` headers, no Qt CMake targets.
- All identifiers (including any new methods) use `lower_snake_case`.
- No `using namespace` in any header.
- `#pragma once` on all new headers.
- Full include paths: `"motif/engine/engine_manager.hpp"`.
- Public fallible methods return `tl::expected<T, motif::engine::error_code>`.
- `engine_manager` is move-only (no copy) — this matches the existing deleted copy constructor/assignment. The `session` struct holds a `uci::engine` which is also move-only.

### Suggested File Structure

No new files required — the existing files get real implementations:

```text
source/motif/engine/
  CMakeLists.txt         ← add find_package(ucilib) and find_package(chesslib), link both
  error.hpp              ← unchanged
  engine_manager.hpp     ← add configure_engine(), list_engines(), engine_config struct
  motif_engine.cpp       ← replace stubs with real implementations

test/source/motif_engine/
  engine_manager_test.cpp  ← expand existing stub tests + new unit tests
```

### Previous Story Intelligence

- Story 4d.2 defined the HTTP/SSE contract and `motif_engine` stub. The `engine_manager` public API in `engine_manager.hpp` is the **contract** Story 4d.2 locked in; the HTTP server already compiles against it. Do not rename or remove existing public methods.
- The existing three stub methods `start_analysis`, `stop_analysis`, `subscribe` must remain with the same signatures. You are replacing bodies, not signatures.
- The stub test at `test/source/motif_engine/engine_manager_test.cpp` tests the stub behavior. The first two stub tests remain valid after this story (no engine registered → `engine_not_configured`; unknown id → `analysis_not_found`). The third stub test (subscribe unknown id) should still pass.
- Architecture note G5 says "no tests expected until Phase 2 begins" — that was the Phase 1 state. Phase 2 begins with this story, so tests are now required.
- `chesslib` SAN/UCI patterns are established in `server.cpp`; replicate the same pattern (not a reinvention).
- `fmt::format`/`fmt::print` for all string ops — not `std::format`, `std::to_string`, `std::cout`.

### References

- [Source: `_bmad-output/planning-artifacts/epics.md` — Epic 4, Stories 4.1 and 4.2]
- [Source: `_bmad-output/planning-artifacts/architecture.md` — motif_engine module, Qt-free rule, ucilib boundary]
- [Source: `_bmad-output/implementation-artifacts/4d-2-engine-analysis-api-contract.md` — locked API contract]
- [Source: `source/motif/engine/engine_manager.hpp` — existing public interface (do not break)]
- [Source: `source/motif/engine/motif_engine.cpp` — stubs to be replaced]
- [Source: `source/motif/http/server.cpp` — chesslib SAN/UCI patterns, fmt usage patterns]
- [Source: `CONVENTIONS.md` — naming, error handling, fmt, testing, module boundaries]
- [Installed: `/nix/store/4mg6mh4aclbzykpv7mb9a62yx59f5vzq-ucilib/include/ucilib/engine.hpp`]
- [Installed: `/nix/store/4mg6mh4aclbzykpv7mb9a62yx59f5vzq-ucilib/include/ucilib/types.hpp`]
- [Installed: `/nix/store/4mg6mh4aclbzykpv7mb9a62yx59f5vzq-ucilib/lib/cmake/ucilib/ucilibTargets.cmake`]

## Dev Agent Record

### Agent Model Used

claude-sonnet-4-5

### Debug Log References

- clang-format must not be run on CMakeLists.txt (corrupts CMake syntax); only run on C++ source files
- `engine_manager::impl` is a private forward-declared struct; free functions in anonymous namespace cannot reference it by name — resolved by passing `impl_->engines` (the map type alias) directly instead of `impl&`
- `chesslib/board/move_maker.hpp` does not exist as a separate header; `move_maker` is defined in `chesslib/board/board.hpp`
- `chesslib::fen::read()` returns `tl::expected<board, parse_error>`, not `optional<board>` as story notes indicated
- ucilib does not expose an `on_error` callback; AC 7 crash detection is deferred to a future story

### Completion Notes List

- Replaced the Phase 2 stub in `motif_engine.cpp` with full ucilib + chesslib wiring
- PIMPL added to `engine_manager` to keep ucilib/chesslib headers out of the public interface
- `session` struct declares `uci::engine` last so its destructor (joins reader thread) runs before callbacks are destroyed
- Per-session `callback_mutex` guards concurrent subscribe/callback-fire access; main `mutex` guards the sessions map — no circular lock ordering
- `pv_to_san` exposed as public free function in `motif::engine` namespace for unit testability
- AC 7 (crash detection): ucilib `engine` has no `on_error` callback in the installed API; crash detection is not implemented — noted in task 5 above
- Code review fixes added deterministic default engine ordering, analysis parameter validation, late-subscriber completion replay, ucilib failure mapping, collision-safe session insertion, and hermetic fake-UCI lifecycle tests
- 237/237 tests pass (dev), 237/237 pass (dev-sanitize), zero ASan/UBSan violations

### File List

- source/motif/engine/CMakeLists.txt
- source/motif/engine/engine_manager.hpp
- source/motif/engine/motif_engine.cpp
- test/source/motif_engine/engine_manager_test.cpp

## Change Log

- 2026-04-29: Implemented engine configuration lifecycle — replaced Phase 2 stubs with real ucilib + chesslib wiring; added engine registry, pv_to_san, PIMPL, thread-safe callback dispatch; 13 unit tests added (Date: 2026-04-29)
