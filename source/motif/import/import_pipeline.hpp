// NOLINTNEXTLINE(portability-avoid-pragma-once)
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <thread>

#include "motif/db/database_manager.hpp"
#include "motif/import/error.hpp"

namespace motif::import
{

struct import_config
{
    static constexpr std::size_t k_default_lines = 64;
    static constexpr std::size_t k_default_batch_size = 500;

    // Taskflow executor threads; default = hardware_concurrency (≥1).
    std::size_t num_workers {std::max(1U, std::thread::hardware_concurrency())};
    // In-flight pipeline slots — bounds peak memory (each slot holds one
    // pgn::game).
    std::size_t num_lines {k_default_lines};
    // Write checkpoint every batch_size games committed.
    std::size_t batch_size {k_default_batch_size};
};

struct import_summary
{
    std::size_t total_attempted {};
    std::size_t committed {};
    std::size_t skipped {};
    std::size_t errors {};
    std::chrono::milliseconds elapsed {};
};

struct import_progress
{
    std::size_t games_processed {};
    std::size_t games_committed {};
    std::size_t games_skipped {};
    std::chrono::milliseconds elapsed {};
};

class import_pipeline
{
  public:
    explicit import_pipeline(motif::db::database_manager& dbm) noexcept;

    // Fresh import of pgn_path into the bound database.
    [[nodiscard]] auto run(std::filesystem::path const& pgn_path,
                           import_config const& config = {})
        -> result<import_summary>;

    // Resume from <db-dir>/import.checkpoint.json. Returns io_failure if
    // absent.
    [[nodiscard]] auto resume(std::filesystem::path const& pgn_path,
                              import_config const& config = {})
        -> result<import_summary>;

    // Thread-safe snapshot of current progress.
    [[nodiscard]] auto progress() const noexcept -> import_progress;

  private:
    [[nodiscard]] auto run_from(std::filesystem::path const& pgn_path,
                                std::size_t start_offset,
                                std::int64_t pre_committed,
                                std::int64_t pre_last_game_id,
                                import_config const& config)
        -> result<import_summary>;

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    motif::db::database_manager& db_;

    std::atomic<std::size_t> games_processed_ {0};
    std::atomic<std::size_t> games_committed_ {0};
    std::atomic<std::size_t> games_skipped_ {0};
    std::atomic<std::int64_t> start_time_ns_ {0};
};

}  // namespace motif::import
