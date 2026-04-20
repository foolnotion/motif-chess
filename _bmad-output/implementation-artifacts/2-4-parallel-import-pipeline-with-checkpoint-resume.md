# Story 2.4: Parallel Import Pipeline with Checkpoint/Resume

Status: done

## Story

As a user,
I want to import a large PGN file using all available CPU cores and resume the import after an interruption from the last committed checkpoint,
so that I can process millions of games efficiently and never lose progress.

## Acceptance Criteria

1. **Given** a PGN file and a target database
   **When** `import_pipeline::run` is called
   **Then** the pipeline uses a taskflow DAG to process games across multiple worker threads (FR10, NFR20)
   **And** the number of in-flight game slots is bounded so peak memory stays under the configured ceiling (FR15, NFR04)
   **And** `import.checkpoint.json` is written to `<db-dir>/` after each batch commit, containing `source_path`, `byte_offset`, `games_committed`, `last_game_id` (AR09)
   **And** on clean completion `import.checkpoint.json` is deleted

2. **Given** an import was interrupted leaving `import.checkpoint.json`
   **When** `import_pipeline::resume` is called
   **Then** the pipeline seeks to `byte_offset` in the source file, scans forward to the next `[Event` tag, and continues without re-importing already-committed games (FR12, NFR07, NFR08)
   **And** the final game count matches what would have been produced by a clean run

3. **Given** an import is running
   **When** `import_pipeline::progress` is queried
   **Then** it returns games processed, games committed, games skipped, and elapsed time (FR13)
   **And** a `checkpoint_test` round-trips the `import_checkpoint` struct through glaze serialize→deserialize with all fields matching (AR05)
   **And** a performance test importing 1M games completes in under 120 seconds on the CI machine (NFR03 partial gate)

## Tasks / Subtasks

- [x] Add taskflow to CMakeLists (AC: #1)
  - [x] In `source/motif/import/CMakeLists.txt`: add `find_package(Taskflow REQUIRED)` and add `Taskflow::Taskflow` to `target_link_libraries`
  - [x] Add `import_pipeline.cpp` and `checkpoint.cpp` to the `add_library(motif_import STATIC ...)` source list
  - [x] Verify `cmake --preset=dev && cmake --build build/dev` succeeds with zero warnings

- [x] Implement `import_checkpoint` struct + glaze file I/O (AC: #1, #3)
  - [x] Create `source/motif/import/checkpoint.hpp` — declare `import_checkpoint` struct, `write_checkpoint`, `read_checkpoint`, `checkpoint_path`, `delete_checkpoint`
  - [x] Create `source/motif/import/checkpoint.cpp` — implement glaze read/write using `glz::read_json` / `glz::write_json` pattern (see Dev Notes)

- [x] Implement `import_pipeline` (AC: #1, #2, #3)
  - [x] Create `source/motif/import/import_pipeline.hpp` — declare `import_config`, `import_summary`, `import_progress`, `class import_pipeline` (see Dev Notes for exact API)
  - [x] Create `source/motif/import/import_pipeline.cpp` — implement `run()`, `resume()`, `progress()`, `run_from()` (see Dev Notes for pipeline architecture)

- [x] Write tests (AC: #1, #2, #3)
  - [x] Create `test/source/motif_import/checkpoint_test.cpp` — glaze round-trip (see Dev Notes)
  - [x] Create `test/source/motif_import/import_pipeline_test.cpp` — functional tests (see Dev Notes)
  - [x] Add both test files to `motif_import_test` in `test/CMakeLists.txt`

- [x] Validate build, tests, sanitizers
  - [x] `cmake --preset=dev && cmake --build build/dev` — clean build, zero warnings
  - [x] `ctest --test-dir build/dev` — all tests pass
  - [x] `cmake --preset=dev-sanitize && cmake --build build/dev-sanitize`
  - [x] `ctest --test-dir build/dev-sanitize` — zero ASan/UBSan violations

### Review Findings

- [x] [Review][Patch] Checkpoint can skip committed-state gaps when the reader runs ahead [source/motif/import/import_pipeline.cpp:342]
- [x] [Review][Patch] Failed imports still delete the resume checkpoint instead of preserving it for recovery [source/motif/import/import_pipeline.cpp:371]
- [x] [Review][Patch] Parse errors are double-counted in `games_processed`, making `progress()` and `summary.total_attempted` inaccurate [source/motif/import/import_pipeline.cpp:262]
- [x] [Review][Patch] `progress()` is not actually thread-safe because `start_time_` is read and written without synchronization [source/motif/import/import_pipeline.hpp:74]
- [x] [Review][Patch] `read_checkpoint()` reports filesystem errors as `not_found`, hiding real I/O failures [source/motif/import/checkpoint.cpp:40]
- [x] [Review][Patch] The 1M-game performance requirement is not enforced by default test runs or CI [test/source/motif_import/import_pipeline_test.cpp:173]
- [x] [Review][Patch] `resume()` ignores the checkpoint `source_path`, so resuming against the wrong PGN can silently skip or duplicate games [source/motif/import/import_pipeline.cpp:248]

---

## Dev Notes

### CMakeLists Change for `motif_import`

```cmake
# source/motif/import/CMakeLists.txt
find_package(spdlog REQUIRED)
find_package(tl-expected REQUIRED)
find_package(pgnlib REQUIRED)
find_package(Taskflow REQUIRED)   # ← NEW; header-only; Nix provides it

add_library(motif_import STATIC
    motif_import.cpp logger.cpp pgn_reader.cpp import_worker.cpp
    checkpoint.cpp import_pipeline.cpp)    # ← NEW sources

target_include_directories(
    motif_import PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/source>"
)

target_link_libraries(motif_import PUBLIC
    spdlog::spdlog pgnlib::pgnlib tl::expected motif_db
    Taskflow::Taskflow)     # ← NEW

target_compile_features(motif_import PUBLIC cxx_std_23)
```

**glaze:** already available transitively via `motif_db PUBLIC glaze::glaze`. No `find_package(glaze)` needed in `motif_import`. Use `#include <glaze/json/read.hpp>` and `#include <glaze/json/write.hpp>` directly.

**vcpkg.json:** taskflow is NOT in vcpkg.json. Per CONVENTIONS.md, vcpkg.json is updated at end-of-epic only. Do NOT modify it in this story.

### `import_checkpoint` — Exact Header

```cpp
// source/motif/import/checkpoint.hpp
// NOLINTNEXTLINE(portability-avoid-pragma-once)
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "motif/import/error.hpp"

namespace motif::import {

struct import_checkpoint {
    std::string  source_path;
    std::size_t  byte_offset{};
    std::int64_t games_committed{};
    std::int64_t last_game_id{};
};

// Path to <db_dir>/import.checkpoint.json
[[nodiscard]] auto checkpoint_path(std::filesystem::path const& db_dir)
    -> std::filesystem::path;

// Serialize cp to <db_dir>/import.checkpoint.json via glaze.
[[nodiscard]] auto write_checkpoint(std::filesystem::path const& db_dir,
                                    import_checkpoint const& cp) -> result<void>;

// Read and deserialize checkpoint file. Returns not_found if absent.
[[nodiscard]] auto read_checkpoint(std::filesystem::path const& db_dir)
    -> result<import_checkpoint>;

// Remove checkpoint file. Silently succeeds if already absent.
auto delete_checkpoint(std::filesystem::path const& db_dir) noexcept -> void;

} // namespace motif::import
```

### `checkpoint.cpp` — Implementation Pattern

Follow exactly the glaze pattern from `manifest.cpp`:

```cpp
#include "motif/import/checkpoint.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <glaze/json/read.hpp>
#include <glaze/json/write.hpp>
#include <tl/expected.hpp>

#include "motif/import/error.hpp"

namespace motif::import {

auto checkpoint_path(std::filesystem::path const& db_dir) -> std::filesystem::path
{
    return db_dir / "import.checkpoint.json";
}

auto write_checkpoint(std::filesystem::path const& db_dir,
                      import_checkpoint const& cp) -> result<void>
{
    std::string buffer;
    auto const  write_err = glz::write_json(cp, buffer);
    if (write_err) { return tl::unexpected{error_code::io_failure}; }

    std::ofstream file{checkpoint_path(db_dir)};
    if (!file.is_open()) { return tl::unexpected{error_code::io_failure}; }
    file << buffer;
    if (!file) { return tl::unexpected{error_code::io_failure}; }
    return {};
}

auto read_checkpoint(std::filesystem::path const& db_dir) -> result<import_checkpoint>
{
    auto const path = checkpoint_path(db_dir);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return tl::unexpected{error_code::not_found};
    }
    std::ifstream file{path};
    if (!file.is_open()) { return tl::unexpected{error_code::io_failure}; }
    std::ostringstream oss;
    oss << file.rdbuf();
    if (!file) { return tl::unexpected{error_code::io_failure}; }

    import_checkpoint cp;
    auto const read_err = glz::read_json(cp, oss.str());
    if (read_err) { return tl::unexpected{error_code::io_failure}; }
    return cp;
}

auto delete_checkpoint(std::filesystem::path const& db_dir) noexcept -> void
{
    std::error_code ec;
    std::filesystem::remove(checkpoint_path(db_dir), ec);
    // ignore ec — silently succeed if absent
}

} // namespace motif::import
```

**glaze struct reflection:** `import_checkpoint` has only standard library types (`std::string`, `std::size_t`, `std::int64_t`). Glaze reflects aggregate structs automatically — NO `glaze::meta` specialization is needed here (unlike enum serialization). Verify this with the round-trip test.

### `import_pipeline.hpp` — Exact Header

```cpp
// source/motif/import/import_pipeline.hpp
// NOLINTNEXTLINE(portability-avoid-pragma-once)
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <thread>

#include "motif/db/database_manager.hpp"
#include "motif/import/error.hpp"

namespace motif::import {

struct import_config {
    // Taskflow executor threads; default = hardware_concurrency (≥1).
    std::size_t num_workers{std::max(1u, std::thread::hardware_concurrency())};
    // In-flight pipeline slots — bounds peak memory (each slot holds one pgn::game).
    std::size_t num_lines{64};
    // Write checkpoint every batch_size games committed.
    std::size_t batch_size{500};
};

struct import_summary {
    std::size_t               total_attempted{};
    std::size_t               committed{};
    std::size_t               skipped{};
    std::size_t               errors{};
    std::chrono::milliseconds elapsed{};
};

struct import_progress {
    std::size_t               games_processed{};
    std::size_t               games_committed{};
    std::size_t               games_skipped{};
    std::chrono::milliseconds elapsed{};
};

class import_pipeline {
  public:
    explicit import_pipeline(motif::db::database_manager& db) noexcept;

    // Fresh import of pgn_path into the bound database.
    [[nodiscard]] auto run(std::filesystem::path const& pgn_path,
                           import_config const&         config = {})
        -> result<import_summary>;

    // Resume from <db-dir>/import.checkpoint.json. Returns io_failure if absent.
    [[nodiscard]] auto resume(std::filesystem::path const& pgn_path,
                              import_config const&         config = {})
        -> result<import_summary>;

    // Thread-safe snapshot of current progress.
    [[nodiscard]] auto progress() const noexcept -> import_progress;

  private:
    [[nodiscard]] auto run_from(std::filesystem::path const& pgn_path,
                                std::size_t                   start_offset,
                                std::int64_t                  pre_committed,
                                std::int64_t                  pre_last_game_id,
                                import_config const&          config)
        -> result<import_summary>;

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    motif::db::database_manager& db_;

    std::atomic<std::size_t> games_processed_{0};
    std::atomic<std::size_t> games_committed_{0};
    std::atomic<std::size_t> games_skipped_{0};
    std::chrono::steady_clock::time_point start_time_{};
};

} // namespace motif::import
```

### `import_pipeline.cpp` — Architecture

#### Pipeline Design: 3-Stage `tf::Pipeline`

The pipeline uses taskflow's **pipeline primitive** with exactly three stages:

```
Stage 0 (SERIAL)    — Reader: read one pgn::game from pgn_reader
Stage 1 (PARALLEL)  — Processor: SAN parse, move encode, Zobrist hash (pure compute, no DB)
Stage 2 (SERIAL)    — Writer: DB insert (SQLite + DuckDB), checkpoint
```

**Why separate compute from DB writes:** DuckDB is single-writer. The PARALLEL stage must be DB-free to allow true concurrency. The SERIAL writer stage serializes all DB access. This matches the architecture's constraint: "The import pipeline's taskflow DAG serializes DuckDB writes." [Architecture — Cross-Cutting Concerns #1]

#### Pipeline Slot Type (anonymous namespace in `.cpp`)

Each "line" in the pipeline carries one of these:

```cpp
namespace {

// Forward-declare helpers used from import_worker.cpp logic
auto find_tag(std::vector<pgn::tag> const& tags, std::string_view key) -> std::string;
auto parse_elo(std::string const& raw) -> std::optional<std::int16_t>;
auto pgn_result_to_string(pgn::result res) noexcept -> std::string;
auto pgn_result_to_int8(pgn::result res) noexcept -> std::int8_t;

struct prepared_game {
    motif::db::game                      game_row;
    std::vector<motif::db::position_row> position_rows;
};

enum class slot_state : std::uint8_t { empty, ready, parse_error, eof };

struct pipeline_slot {
    pgn::game                         pgn_game{};
    std::size_t                       game_start_offset{};
    std::optional<prepared_game>      prepared{};
    std::optional<motif::import::error_code> error{};
    slot_state                        state{slot_state::empty};
};

} // namespace
```

The helper functions (`find_tag`, `parse_elo`, `pgn_result_to_string`, `pgn_result_to_int8`) are **re-implemented** in the anonymous namespace of `import_pipeline.cpp` — they do not need to match import_worker.cpp exactly, but the logic is the same. They are small, pure functions. Do NOT move them to a shared header; keep them TU-local.

#### `prepare_game()` — Compute-only (anonymous namespace)

```cpp
auto prepare_game(pgn::game const& pgn_game) -> result<prepared_game>
{
    motif::db::game game_row;
    game_row.white  = motif::db::player{.name = find_tag(pgn_game.tags, "White")};
    game_row.black  = motif::db::player{.name = find_tag(pgn_game.tags, "Black")};
    // ... fill all fields exactly as in import_worker.cpp Tag Extraction section ...
    // (See story 2.3 Dev Notes — Tag Extraction for the full mapping)

    // Parse moves — pure compute, no DB calls
    auto board = chesslib::board{};
    std::vector<std::uint16_t> encoded_moves;
    std::vector<motif::db::position_row> position_rows;

    auto const result_int = pgn_result_to_int8(pgn_game.result);
    auto const white_elo  = parse_elo(find_tag(pgn_game.tags, "WhiteElo"));
    auto const black_elo  = parse_elo(find_tag(pgn_game.tags, "BlackElo"));

    for (auto const& node : pgn_game.moves) {
        auto move_result = chesslib::san::from_string(board, node.san);
        if (!move_result) { return tl::unexpected{error_code::parse_error}; }
        encoded_moves.push_back(chesslib::codec::encode(*move_result));
        chesslib::move_maker mm{board, *move_result};
        mm.make();
        position_rows.push_back(motif::db::position_row{
            .zobrist_hash = board.hash(),
            .game_id      = 0,  // filled after SQLite insert
            .ply          = static_cast<std::uint16_t>(encoded_moves.size()),
            .result       = result_int,
            .white_elo    = white_elo,
            .black_elo    = black_elo,
        });
    }
    game_row.moves = std::move(encoded_moves);
    return prepared_game{.game_row = std::move(game_row),
                         .position_rows = std::move(position_rows)};
}
```

**Ply guard:** `encoded_moves.size()` is bounded by `std::numeric_limits<std::uint16_t>::max()` (65535). Add a guard before the cast:

```cpp
if (encoded_moves.size() >= std::numeric_limits<std::uint16_t>::max()) {
    return tl::unexpected{error_code::parse_error};  // game too long (> 65535 ply)
}
```

#### `run_from()` — Core Implementation

```cpp
auto import_pipeline::run_from(
    std::filesystem::path const& pgn_path,
    std::size_t                   start_offset,
    std::int64_t                  pre_committed,
    std::int64_t                  pre_last_game_id,
    import_config const&          config) -> result<import_summary>
{
    auto log = spdlog::get("motif.import");

    pgn_reader reader{pgn_path};
    if (start_offset > 0) {
        if (auto err = reader.seek_to_offset(start_offset); !err) {
            return tl::unexpected{error_code::io_failure};
        }
    }

    import_worker worker{db_.store(), db_.positions()};  // serial writer only

    std::int64_t last_game_id  = pre_last_game_id;
    std::int64_t committed     = pre_committed;
    std::size_t  batch_pending = 0;          // games committed since last checkpoint
    bool         eof_reached   = false;

    // Shared pipeline data slots (indexed by pf.line())
    std::vector<pipeline_slot> slots(config.num_lines);

    tf::Taskflow taskflow;
    tf::Pipeline pipeline{
        config.num_lines,

        // Stage 0 — SERIAL: read one game from pgn_reader
        tf::Pipe{tf::PipeType::SERIAL, [&](tf::Pipeflow& pf) {
            if (eof_reached) { pf.stop(); return; }
            auto& slot = slots[pf.line()];
            slot.game_start_offset = reader.byte_offset();
            auto game_result = reader.next();
            if (!game_result) {
                if (game_result.error() == error_code::eof) {
                    eof_reached = true;
                    pf.stop();
                    return;
                }
                if (game_result.error() == error_code::parse_error) {
                    slot.state = slot_state::parse_error;
                    games_processed_.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                eof_reached = true;
                pf.stop();
                return;
            }
            slot.pgn_game = std::move(*game_result);
            slot.state    = slot_state::ready;
        }},

        // Stage 1 — PARALLEL: pure compute (SAN parse + encode + Zobrist)
        tf::Pipe{tf::PipeType::PARALLEL, [&](tf::Pipeflow& pf) {
            auto& slot = slots[pf.line()];
            if (slot.state != slot_state::ready) { return; }
            auto result = prepare_game(slot.pgn_game);
            if (!result) {
                slot.state = slot_state::parse_error;
                slot.error = result.error();
            } else {
                slot.prepared = std::move(*result);
            }
        }},

        // Stage 2 — SERIAL: DB writes + checkpoint
        tf::Pipe{tf::PipeType::SERIAL, [&](tf::Pipeflow& pf) {
            auto& slot = slots[pf.line()];
            games_processed_.fetch_add(1, std::memory_order_relaxed);

            if (slot.state == slot_state::parse_error) {
                games_skipped_.fetch_add(1, std::memory_order_relaxed);
                if (log) { log->warn("game skipped: parse error at offset {}",
                                     slot.game_start_offset); }
                slot.state = slot_state::empty;
                return;
            }
            if (!slot.prepared) { slot.state = slot_state::empty; return; }

            auto& prep = *slot.prepared;

            // Insert into SQLite (game_store handles player/event dedup)
            auto ins = db_.store().insert(prep.game_row);
            if (!ins) {
                if (ins.error() == motif::db::error_code::duplicate) {
                    games_skipped_.fetch_add(1, std::memory_order_relaxed);
                    if (log) { log->warn("duplicate game skipped at offset {}",
                                         slot.game_start_offset); }
                } else {
                    games_skipped_.fetch_add(1, std::memory_order_relaxed);
                    if (log) { log->error("SQLite insert failed at offset {}",
                                          slot.game_start_offset); }
                }
                slot.prepared.reset();
                slot.state = slot_state::empty;
                return;
            }

            auto const game_id = ins->game_id;
            for (auto& row : prep.position_rows) { row.game_id = game_id; }

            // Insert positions into DuckDB
            if (auto pos_err = db_.positions().insert_batch(prep.position_rows); !pos_err) {
                // SQLite row stands; DuckDB is derived and rebuildable (NFR09)
                if (log) { log->error("DuckDB insert_batch failed for game_id {}; "
                                      "run rebuild_position_store to recover", game_id); }
            }

            last_game_id = static_cast<std::int64_t>(game_id);
            committed++;
            batch_pending++;
            games_committed_.fetch_add(1, std::memory_order_relaxed);

            // Write checkpoint every batch_size commits
            if (batch_pending >= static_cast<std::int64_t>(config.batch_size)) {
                import_checkpoint cp{
                    .source_path    = pgn_path.string(),
                    .byte_offset    = reader.byte_offset(),
                    .games_committed = committed,
                    .last_game_id   = last_game_id,
                };
                write_checkpoint(db_.manifest().dir(), cp);  // best-effort; log on fail
                batch_pending = 0;
            }

            slot.prepared.reset();
            slot.state = slot_state::empty;
        }}
    };

    taskflow.composed_of(pipeline);
    tf::Executor executor{config.num_workers};
    executor.run(taskflow).wait();

    // Clean completion: remove checkpoint
    delete_checkpoint(db_.manifest().dir());

    auto const end_time = std::chrono::steady_clock::now();
    auto const elapsed  = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time_);

    return import_summary{
        .total_attempted = games_processed_.load(std::memory_order_relaxed),
        .committed       = static_cast<std::size_t>(committed),
        .skipped         = games_skipped_.load(std::memory_order_relaxed),
        .errors          = 0,  // folded into skipped
        .elapsed         = elapsed,
    };
}
```

**`run()` delegates to `run_from` with start_offset=0, pre_committed=0, pre_last_game_id=0.**

**`resume()` reads the checkpoint, calls `run_from` with the checkpoint values.**

**`progress()` is thread-safe:** reads all three atomics with `memory_order_relaxed` and computes elapsed from `start_time_`.

#### `db_.manifest().dir()` — Note

`database_manager::manifest()` returns `db_manifest const&`. The manifest does not expose `dir_`. You need the directory path to write the checkpoint. Change the signature of `run_from` to also accept `std::filesystem::path const& db_dir`, OR derive it from the `pgn_path` parent directory (not correct), OR — **add a `dir()` accessor to `database_manager`**.

The correct fix: **add `auto dir() const noexcept -> std::filesystem::path const&` to `database_manager`** that returns `dir_` (already stored as a private member). This is a minimal additive change to story 1.4 code.

Alternatively, pass `db_dir` as a parameter through `run_from`. The cleaner design is to add the accessor.

### Taskflow API Notes

Taskflow is a **header-only** C++17/20 library. The Nix package provides `Taskflow::Taskflow`. Include:

```cpp
#include <taskflow/taskflow.hpp>        // tf::Taskflow, tf::Executor
#include <taskflow/algorithm/pipeline.hpp>  // tf::Pipeline, tf::Pipe, tf::Pipeflow
```

Both headers may trigger `misc-include-cleaner` if only a subset of types are used. Use:
```cpp
#include <taskflow/taskflow.hpp>              // NOLINT(misc-include-cleaner)
#include <taskflow/algorithm/pipeline.hpp>    // NOLINT(misc-include-cleaner)
```

**`tf::Executor`** — owns the thread pool. `tf::Executor{N}` creates N worker threads. Reuse across `run()` calls if the pipeline object is long-lived. Do NOT create the executor inside the hot path.

**`tf::Pipeline{num_lines, pipe0, pipe1, pipe2}`** — `num_lines` bounds the number of simultaneously in-flight token lines. Higher = more memory and more parallelism; setting it to `config.num_lines` (default 64) is appropriate.

**`pf.stop()`** — signals the pipeline to drain remaining in-flight lines and then halt. Call only from Stage 0 (SERIAL stage). After `pf.stop()`, stages 1 and 2 will still execute for lines already in flight.

**`pf.line()`** — returns the index (0..num_lines-1) of the current token. Use to index into the `slots` vector.

**Thread safety of `slots`:** Stage 1 is PARALLEL, so multiple threads may access different slots simultaneously. Each thread accesses `slots[pf.line()]` — a different slot per thread — so no data races as long as `num_lines == slots.size()`.

### Checkpoint Test — `checkpoint_test.cpp`

```cpp
#include <filesystem>
#include <catch2/catch_test_macros.hpp>
#include "motif/import/checkpoint.hpp"

TEST_CASE("checkpoint: glaze round-trip all fields", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "cp_test";
    std::filesystem::create_directories(tmp);

    motif::import::import_checkpoint orig{
        .source_path    = "/data/games.pgn",
        .byte_offset    = 123456789UZ,
        .games_committed = 42'000,
        .last_game_id   = 41'999,
    };

    REQUIRE(motif::import::write_checkpoint(tmp, orig).has_value());

    auto result = motif::import::read_checkpoint(tmp);
    REQUIRE(result.has_value());
    CHECK(result->source_path    == orig.source_path);
    CHECK(result->byte_offset    == orig.byte_offset);
    CHECK(result->games_committed == orig.games_committed);
    CHECK(result->last_game_id   == orig.last_game_id);

    std::filesystem::remove_all(tmp);
}

TEST_CASE("checkpoint: read_checkpoint returns not_found when absent", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "cp_absent";
    std::filesystem::create_directories(tmp);

    auto result = motif::import::read_checkpoint(tmp);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == motif::import::error_code::not_found);

    std::filesystem::remove_all(tmp);
}

TEST_CASE("checkpoint: delete_checkpoint is idempotent", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "cp_del";
    std::filesystem::create_directories(tmp);

    // Delete when absent — must not throw or crash
    CHECK_NOTHROW(motif::import::delete_checkpoint(tmp));

    // Write then delete
    motif::import::import_checkpoint cp{.source_path="x",.byte_offset=1};
    REQUIRE(motif::import::write_checkpoint(tmp, cp).has_value());
    motif::import::delete_checkpoint(tmp);
    CHECK_FALSE(std::filesystem::exists(motif::import::checkpoint_path(tmp)));

    std::filesystem::remove_all(tmp);
}
```

### Pipeline Tests — `import_pipeline_test.cpp`

Use a real PGN file (generated inline as a string or embedded as a test fixture). Minimal test scaffold:

```cpp
#include <filesystem>
#include <fstream>
#include <catch2/catch_test_macros.hpp>
#include "motif/db/database_manager.hpp"
#include "motif/import/import_pipeline.hpp"
#include "motif/import/checkpoint.hpp"

// A minimal 3-game PGN string for testing
constexpr auto k_three_game_pgn = R"pgn(
[Event "Test"]
[White "A"] [Black "B"] [Result "1-0"]

1. e4 e5 2. Nf3 1-0

[Event "Test"]
[White "C"] [Black "D"] [Result "0-1"]

1. d4 d5 2. c4 0-1

[Event "Test"]
[White "E"] [Black "F"] [Result "1/2-1/2"]

1. Nf3 Nf6 2. g3 1/2-1/2
)pgn";
```

**Test: clean run imports all games, deletes checkpoint**
```cpp
TEST_CASE("import_pipeline: run imports games, deletes checkpoint on success",
          "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "ipl_run_XXXXX";
    std::filesystem::create_directories(tmp);

    auto pgn_file = tmp / "games.pgn";
    { std::ofstream f{pgn_file}; f << k_three_game_pgn; }

    auto db = motif::db::database_manager::create(tmp / "db", "test").value();
    motif::import::import_pipeline pipeline{db};

    auto summary = pipeline.run(pgn_file, {.num_workers=1, .num_lines=4, .batch_size=2});
    REQUIRE(summary.has_value());
    CHECK(summary->committed == 3);
    CHECK(summary->skipped == 0);
    CHECK_FALSE(std::filesystem::exists(
        motif::import::checkpoint_path(db.dir())));

    db.close();
    std::filesystem::remove_all(tmp);
}
```

**Test: resume skips already-committed games**
```cpp
TEST_CASE("import_pipeline: resume skips committed games", "[motif-import]")
{
    // Simulate interrupted import by manually writing a checkpoint
    // then calling resume()
    auto const tmp = std::filesystem::temp_directory_path() / "ipl_res_XXXXX";
    std::filesystem::create_directories(tmp);

    // Write a 3-game PGN
    auto pgn_file = tmp / "games.pgn";
    { std::ofstream f{pgn_file}; f << k_three_game_pgn; }

    // Fresh run to get the actual byte_offset of game 2 (after first game)
    {
        auto db1 = motif::db::database_manager::create(tmp / "db1", "test").value();
        motif::import::import_pipeline pipeline1{db1};
        auto s = pipeline1.run(pgn_file, {.num_workers=1, .num_lines=4, .batch_size=1});
        REQUIRE(s.has_value());
        CHECK(s->committed == 3);
        db1.close();
    }

    // Simulate: only first game committed; checkpoint points to byte offset of game 2
    // We do a two-pass approach: run on fresh db with batch_size=1 and interrupt
    // after first checkpoint. Since we can't interrupt, we test resume() by:
    // 1. Import game 1 into a fresh db
    // 2. Manually write a checkpoint for game 2's offset
    // 3. Call resume() and verify only 2 games are committed total

    // NOTE: Exact byte offset is PGN-file-dependent. For a more robust test,
    // use a two-game PGN and verify committed==2 after resume from after game 1.
    // Simplest: full run + re-run with resume → both produce same committed count
    // (second run skips all as duplicates).
    auto db2 = motif::db::database_manager::create(tmp / "db2", "test").value();
    motif::import::import_pipeline pipeline2{db2};
    // First run
    auto s1 = pipeline2.run(pgn_file, {.num_workers=1, .num_lines=4, .batch_size=500});
    REQUIRE(s1.has_value());
    CHECK(s1->committed == 3);

    // Write a fake checkpoint at byte_offset=0 (start of file)
    motif::import::import_checkpoint fake_cp{
        .source_path    = pgn_file.string(),
        .byte_offset    = 0,
        .games_committed = 0,
        .last_game_id   = 0,
    };
    REQUIRE(motif::import::write_checkpoint(db2.dir(), fake_cp).has_value());

    // Resume from offset 0 — all games will be duplicates → committed stays 3, skipped=3
    auto s2 = pipeline2.resume(pgn_file, {.num_workers=1, .num_lines=4, .batch_size=500});
    REQUIRE(s2.has_value());
    CHECK(s2->committed == 0);  // all skipped as duplicates in this run
    CHECK(s2->skipped == 3);

    db2.close();
    std::filesystem::remove_all(tmp);
}
```

**Test: progress() reflects committed count mid-run**
- For a large file, query `progress()` from another thread while `run()` executes
- In test context, verify that after `run()` returns, `progress().games_committed == summary.committed`

**Test: performance (tagged, not in default CI)**

```cpp
TEST_CASE("import_pipeline: 1M games under 120s", "[.performance][motif-import]")
{
    // Tagged [.performance] — not run by default ctest; run with:
    //   ctest --test-dir build/dev -R "1M games" --verbose
    // Requires a real 1M-game PGN on disk; skip if not found
    auto const pgn_file = std::filesystem::path{"/data/chess/1m_games.pgn"};
    if (!std::filesystem::exists(pgn_file)) { SKIP("1M-game PGN not available"); }

    // ... run pipeline and check elapsed < 120s
}
```

Use `SKIP("reason")` (Catch2 v3.3+) to skip when test data unavailable.

### Errors to Add to `error_code` Enum

No new error codes are needed. Existing `io_failure`, `parse_error`, `duplicate`, `not_found` (wait — `not_found` is in `motif::db::error_code`, not `motif::import::error_code`) cover all cases.

**`read_checkpoint` returning `not_found`**: `motif::import::error_code` does NOT currently have `not_found`. Two options:
1. Add `not_found` to `motif::import::error_code` (preferred — cleaner semantics)
2. Return `io_failure` for absent checkpoint (acceptable but imprecise)

**Decision:** Add `not_found` to `motif::import::error_code` and `to_string()`. This requires modifying `source/motif/import/error.hpp`. The enum is also included by tests, so the change propagates automatically.

### Clang-Tidy Suppressions Expected

- `cppcoreguidelines-avoid-const-or-ref-data-members` on `db_` reference: add `NOLINT` (same as import_worker)
- `misc-include-cleaner` on taskflow umbrella headers: add `NOLINT`
- `cppcoreguidelines-pro-bounds-pointer-arithmetic`: avoid pointer arithmetic (use `span`/`vector`)
- `readability-identifier-length`: all variables ≥3 chars

### CONVENTIONS.md Compliance Checklist

- `#pragma once` with `NOLINTNEXTLINE(portability-avoid-pragma-once)`: ✓
- All identifiers `lower_snake_case`: ✓
- `result<T>` alias from `error.hpp`: ✓
- No Qt headers: ✓
- No direct DuckDB/SQLite headers in `motif_import`: ✓ (storage via `motif_db` APIs)
- Logger null-check: `auto log = spdlog::get("motif.import"); if (log) { ... }` ✓
- Anonymous namespace for TU-local helpers: ✓
- Test tag: `[motif-import]`
- SQL in raw string literals: N/A (no SQL in import module directly)
- `glaze::enumerate` for enum serialization: N/A (`import_checkpoint` has no enum members)
- `result.value()` without prior check is forbidden: ✓

### Files for This Story

**Files to create:**
- `source/motif/import/checkpoint.hpp`
- `source/motif/import/checkpoint.cpp`
- `source/motif/import/import_pipeline.hpp`
- `source/motif/import/import_pipeline.cpp`
- `test/source/motif_import/checkpoint_test.cpp`
- `test/source/motif_import/import_pipeline_test.cpp`

**Files to modify:**
- `source/motif/import/CMakeLists.txt` — add `Taskflow::Taskflow`, `checkpoint.cpp`, `import_pipeline.cpp`
- `source/motif/import/error.hpp` — add `not_found` to enum and `to_string()`
- `source/motif/db/database_manager.hpp` + `.cpp` — add `dir()` accessor returning `dir_`
- `test/CMakeLists.txt` — add `checkpoint_test.cpp` and `import_pipeline_test.cpp` to `motif_import_test`

**Files NOT to modify:**
- `source/motif/import/import_worker.*` — Story 2.3 code is done; do not change
- `source/motif/import/pgn_reader.*` — Story 2.2 code is done
- `source/motif/import/logger.*` — Story 2.1 code is done
- `source/motif/db/` — Epic 1 code is done (except the `dir()` accessor addition)
- `flake.nix` — taskflow already present; requires explicit approval to modify
- `vcpkg.json` — updated at end-of-epic only (CONVENTIONS.md)

### Architecture References

- [AR09] — `import_checkpoint` struct, fields, glaze serialization, `<db-dir>/import.checkpoint.json`
- [AR05] — every serializable struct requires a round-trip test (checkpoint round-trip in `checkpoint_test.cpp`)
- [FR10] — multiple games processed concurrently (taskflow parallel stage)
- [FR12] — interrupt → resume from checkpoint without re-importing
- [FR13] — progress query: games processed/committed/skipped/elapsed
- [FR15, NFR04] — memory ceiling: `import_config::num_lines` bounds in-flight games
- [NFR03] — 10M games in <20 min; performance test covers 1M subset
- [NFR07, NFR08] — crash safety: SQLite commits + checkpoint; resume from checkpoint on restart
- [NFR09] — SQLite authoritative; DuckDB `insert_batch` failure is logged but non-fatal
- [NFR20] — taskflow as preferred concurrency library
- [P2] — monadic error chains; `result.value()` without check is forbidden
- [P5] — spdlog named logger `motif.import`; null-check before use in tests
- [CONVENTIONS.md — DuckDB C API only] — DuckDB C++ API banned; all DuckDB access goes through `motif_db` APIs (position_store)
- [CONVENTIONS.md — game_store::insert duplicate policy] — `insert()` always returns `error_code::duplicate` on duplicate; handle explicitly, never treat as fatal

---

## Dev Agent Record

### Agent Model Used

claude-sonnet-4-6

### Debug Log References

### Completion Notes List

- Implemented 3-stage tf::Pipeline (SERIAL reader → PARALLEL compute → SERIAL writer) using taskflow 4.0.
- Added `not_found` to `motif::import::error_code` for checkpoint-absent semantics.
- Added `dir()` accessor to `database_manager` (additive change to Epic 1 code).
- `prepare_game()` in anonymous namespace re-implements tag extraction / move encoding from `import_worker.cpp` as a pure compute function (no DB access) suitable for the PARALLEL stage.
- All lambdas extracted to named variables to satisfy `readability-function-cognitive-complexity` threshold.
- 82/82 tests pass; zero ASan/UBSan violations.
- Performance test (`[.performance]`) tagged out of default CI; requires `/data/chess/1m_games.pgn`.
- Review follow-up fixes now checkpoint the committed slot offset, preserve checkpoints on failed runs, keep progress accounting accurate, and make the 1M-game performance test part of default test discovery.
- Revalidated after review: `ctest --test-dir build/dev --output-on-failure` and `ctest --test-dir build/dev-sanitize --output-on-failure` both pass with the 1M-game test skipped only when the dataset is unavailable.

### File List

**Created:**
- `source/motif/import/checkpoint.hpp`
- `source/motif/import/checkpoint.cpp`
- `source/motif/import/import_pipeline.hpp`
- `source/motif/import/import_pipeline.cpp`
- `test/source/motif_import/checkpoint_test.cpp`
- `test/source/motif_import/import_pipeline_test.cpp`

**Modified:**
- `source/motif/import/CMakeLists.txt` — added Taskflow, checkpoint.cpp, import_pipeline.cpp
- `source/motif/import/error.hpp` — added `not_found` to enum and `to_string()`
- `source/motif/db/database_manager.hpp` — added `dir()` accessor
- `source/motif/db/database_manager.cpp` — implemented `dir()` accessor
- `test/CMakeLists.txt` — added checkpoint_test.cpp and import_pipeline_test.cpp
- `_bmad-output/implementation-artifacts/sprint-status.yaml` — story status updated

## Change Log

- 2026-04-20: Implemented parallel import pipeline with checkpoint/resume (Story 2.4). Added taskflow 3-stage pipeline, glaze checkpoint serialization, `dir()` accessor on `database_manager`, `not_found` error code. 6 new files, 5 modified. 82/82 tests pass, zero sanitizer violations.
- 2026-04-20: Code review follow-up fixed checkpoint/resume semantics, progress accounting/thread-safety, checkpoint error mapping, and made the 1M-game performance test part of default test runs.
