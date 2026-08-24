#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>

#include "motif/db/database_manager.hpp"
#include "motif/import/error.hpp"

namespace motif::import
{

struct import_checkpoint;

// Parallelism default that scales with the host instead of assuming a fixed
// machine size: half of hardware_concurrency(), floor of 1. Chosen over a
// flat cap after measuring diminishing returns above ~16-24 workers on a
// 32-core box (half of that, 16, sits inside the diminishing-returns range
// rather than past it) -- using half leaves the other half free for the
// OS/UI thread and other work on any size of machine, rather than tuning a
// constant to the one box this was measured on.
[[nodiscard]] auto default_worker_count() noexcept -> std::size_t;

struct import_config
{
    static constexpr std::size_t default_lines_per_worker = 16;
    static constexpr std::size_t default_batch_size = 500;

    std::size_t num_workers {default_worker_count()};
    std::size_t num_lines {num_workers * default_lines_per_worker};
    bool rebuild_positions_after_import {true};
    bool sort_positions_by_zobrist_after_rebuild {true};
    std::size_t batch_size {default_batch_size};
};

struct import_summary
{
    std::size_t total_attempted {};
    std::size_t committed {};
    std::size_t skipped {};
    std::size_t errors {};
    // Nonzero only for a fresh import, which ingests raw (no per-row identity
    // check) and removes exact duplicates in one pass afterward; included in
    // `skipped`.
    std::size_t duplicates_removed {};
    // Includes PGN parsing, game writes, and inline position generation/writes.
    std::chrono::milliseconds ingest_elapsed {};
    // Nonzero only for a fresh import (see duplicates_removed).
    std::chrono::milliseconds dedup_elapsed {};
    // Nonzero only when positions are rebuilt from the persisted game store.
    std::chrono::milliseconds position_replay_elapsed {};
    std::chrono::milliseconds sort_elapsed {};
    std::chrono::milliseconds rollup_elapsed {};
    std::chrono::milliseconds elapsed {};
};

enum class import_phase : std::uint8_t
{
    idle,  // before the pipeline starts
    ingesting,  // reading and inserting games into SQLite
    deduplicating,  // fresh imports only: removing exact duplicates from raw ingest
    rebuilding,  // rebuilding and sorting the DuckDB position store
};

struct import_progress
{
    std::size_t games_processed {};
    std::size_t games_committed {};
    std::size_t games_skipped {};
    std::size_t errors {};
    std::size_t total_games {};
    std::chrono::milliseconds elapsed {};
    import_phase phase {import_phase::idle};
};

class import_pipeline
{
  public:
    explicit import_pipeline(motif::db::database_manager& dbm) noexcept;

    // Fresh import of pgn_path into the bound database.
    [[nodiscard]] auto run(std::filesystem::path const& pgn_path, import_config const& config = {}) -> result<import_summary>;

    // Resume from <db-dir>/import.checkpoint.json. Returns io_failure if
    // absent.
    [[nodiscard]] auto resume(std::filesystem::path const& pgn_path, import_config const& config = {}) -> result<import_summary>;

    // Thread-safe snapshot of current progress.
    [[nodiscard]] auto progress() const noexcept -> import_progress;

    // Signal a running import to stop after the current batch.
    void request_stop() noexcept;

  private:
    [[nodiscard]] auto resume_from_checkpoint(std::filesystem::path const& pgn_path,
                                              import_checkpoint const& checkpoint,
                                              import_config const& config) -> result<import_summary>;

    [[nodiscard]] auto run_from(std::filesystem::path const& pgn_path,
                                std::size_t start_offset,
                                std::int64_t pre_committed,
                                std::int64_t pre_last_game_id,
                                import_config const& config) -> result<import_summary>;

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    motif::db::database_manager& db_;

    std::atomic<std::size_t> games_processed_ {0};
    std::atomic<std::size_t> games_committed_ {0};
    std::atomic<std::size_t> games_skipped_ {0};
    std::atomic<std::size_t> games_errored_ {0};
    std::atomic<std::size_t> total_games_ {0};
    std::atomic<std::int64_t> start_time_ns_ {0};
    std::atomic<bool> stop_requested_ {false};
    std::atomic<import_phase> phase_ {import_phase::idle};
};

}  // namespace motif::import
