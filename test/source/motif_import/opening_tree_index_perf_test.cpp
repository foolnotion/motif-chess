// Validates opening_tree_index::build()'s bounded-memory claim against a
// real corpus: imports games through the normal pipeline (SQLite only, no
// DuckDB rebuild -- irrelevant to this index), then builds the index and
// reports elapsed time, peak RSS, and output file size. Not a hard
// regression gate (no prior baseline exists to compare against -- this is
// the first version of the bounded-memory build) -- informational, like
// replay_throughput_test.cpp.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <numeric>
#include <vector>

#include "motif/db/opening_tree_index.hpp"

#include <catch2/catch_test_macros.hpp>
#include <fmt/core.h>

#include "motif/chess/chess.hpp"
#include "motif/db/database_manager.hpp"
#include "motif/import/import_pipeline.hpp"
#include "motif/import/logger.hpp"
#include "test_helpers.hpp"

using test_helpers::is_sanitized_build;
using test_helpers::peak_rss_sampler;

namespace
{

auto perf_pgn_path() -> std::filesystem::path
{
    // NOLINTNEXTLINE(concurrency-mt-unsafe) -- test-only env override read once
    auto const* const perf_pgn = std::getenv("MOTIF_IMPORT_PERF_PGN");
    if (perf_pgn != nullptr) {
        return std::filesystem::path {perf_pgn};
    }

    auto repo_local = std::filesystem::path {MOTIF_PROJECT_SOURCE_DIR} / "bench" / "data" / "twic-bench.pgn";
    if (std::filesystem::exists(repo_local)) {
        return repo_local;
    }

    repo_local = std::filesystem::path {MOTIF_PROJECT_SOURCE_DIR} / "bench" / "data" / "twic-1m.pgn";
    if (std::filesystem::exists(repo_local)) {
        return repo_local;
    }

    return "/data/chess/1m_games.pgn";
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- perf setup/reporting is branchy; Catch2 macros inflate this further.
TEST_CASE("opening_tree_index::build bounded-memory claim on a real corpus", "[performance][opening-tree-index-perf]")
{
    if (is_sanitized_build) {
        SKIP("performance checks are skipped in sanitize builds");
    }
#ifndef NDEBUG
    SKIP("performance checks run only in release builds");
#endif

    auto const pgn_path = perf_pgn_path();
    if (!std::filesystem::exists(pgn_path)) {
        SKIP("no perf corpus available");
    }

    auto const tmp = std::filesystem::temp_directory_path() / "opening_tree_index_perf";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto manager = motif::db::database_manager::create(tmp / "db", "opening-tree-index-perf");
    REQUIRE(manager.has_value());

    auto const perf_log_dir = std::filesystem::temp_directory_path() / "motif-opening-tree-index-perf-logs";
    std::filesystem::remove_all(perf_log_dir);
    auto init_log = motif::import::initialize_logging({.log_dir = perf_log_dir});
    REQUIRE(init_log.has_value());

    motif::import::import_pipeline pipeline {*manager};
    motif::import::import_config const import_cfg {
        .num_workers = 1,
        .num_lines = 1,
        .rebuild_positions_after_import = false,
    };
    auto import_summary = pipeline.run(pgn_path, import_cfg);
    auto const shutdown_result = motif::import::shutdown_logging();
    (void)shutdown_result;
    REQUIRE(import_summary.has_value());

    fmt::print("\n=== opening_tree_index perf: import ===\n"
               "  attempted:    {}\n"
               "  committed:    {}\n"
               "  skipped:      {}\n"
               "  errors:       {}\n"
               "  elapsed:      {} ms\n",
               import_summary->total_attempted, import_summary->committed, import_summary->skipped, import_summary->errors,
               import_summary->elapsed.count());

    auto const index_path = tmp / "opening_tree.idx";
    auto const rss_before = test_helpers::read_rss_bytes();
    auto sampler = peak_rss_sampler {};
    auto const started = std::chrono::steady_clock::now();
    auto build_res = motif::db::opening_tree_index::build(manager->store(), index_path);
    auto const elapsed = std::chrono::steady_clock::now() - started;
    auto const peak_rss = sampler.peak();
    REQUIRE(build_res.has_value());

    std::error_code size_err;
    auto const index_size = std::filesystem::file_size(index_path, size_err);
    std::error_code games_size_err;
    auto const games_db_size = std::filesystem::file_size(tmp / "db" / "games.db", games_size_err);

    constexpr auto bytes_per_mb = std::size_t {1024} * std::size_t {1024};

    fmt::print("=== opening_tree_index perf: build ===\n"
               "  committed games:  {}\n"
               "  elapsed:          {} ms\n"
               "  rss before:       {} MB\n"
               "  peak rss:         {} MB\n"
               "  index file size:  {} MB\n"
               "  games.db size:    {} MB\n"
               "  total bundle:     {} MB\n",
               import_summary->committed, std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
               rss_before / bytes_per_mb, peak_rss / bytes_per_mb, size_err ? 0 : index_size / bytes_per_mb,
               games_size_err ? 0 : games_db_size / bytes_per_mb,
               (size_err ? 0 : index_size / bytes_per_mb) + (games_size_err ? 0 : games_db_size / bytes_per_mb));

    auto opened = motif::db::opening_tree_index::open(index_path);
    REQUIRE(opened.has_value());

    // Sample real positions from a batch of committed games (root_ply <= 20,
    // the index's own cap), rather than synthetic hashes -- a representative
    // navigation workload: every ply a user would actually step through with
    // the opening explorer, not just the starting position.
    std::vector<motif::db::zobrist_hash> sample_hashes;
    constexpr auto sample_games = std::uint32_t {2000};
    constexpr auto max_ply_sampled = std::size_t {20};
    for (std::uint32_t id = 1; id <= sample_games && id <= static_cast<std::uint32_t>(import_summary->committed); ++id) {
        auto game_res = manager->store().get(motif::db::game_id {id});
        if (!game_res) {
            continue;
        }
        auto board = motif::chess::board {};
        sample_hashes.push_back(motif::db::zobrist_hash {board.hash()});
        auto const ply_limit = std::min(game_res->moves.size(), max_ply_sampled);
        for (std::size_t ply = 0; ply < ply_limit; ++ply) {
            motif::chess::apply_encoded_move(board, game_res->moves[ply]);
            sample_hashes.push_back(motif::db::zobrist_hash {board.hash()});
        }
    }
    REQUIRE(!sample_hashes.empty());

    std::vector<double> latencies_us;
    latencies_us.reserve(sample_hashes.size());
    std::size_t total_rows = 0;
    for (auto const hash : sample_hashes) {
        auto const query_started = std::chrono::steady_clock::now();
        auto stats = opened->query_opening_stats(hash);
        auto const query_elapsed = std::chrono::steady_clock::now() - query_started;
        REQUIRE(stats.has_value());
        total_rows += stats->size();
        latencies_us.push_back(std::chrono::duration<double, std::micro>(query_elapsed).count());
    }
    std::ranges::sort(latencies_us);
    auto const p50 = latencies_us[latencies_us.size() / 2];
    auto const p99_index = std::min(latencies_us.size() - 1, static_cast<std::size_t>(static_cast<double>(latencies_us.size()) * 0.99));
    auto const p99 = latencies_us[p99_index];
    // NOLINTNEXTLINE(boost-use-ranges) -- boost is not a project dependency
    auto const mean = std::accumulate(latencies_us.begin(), latencies_us.end(), 0.0) / static_cast<double>(latencies_us.size());

    fmt::print("=== opening_tree_index perf: query latency (tree navigation) ===\n"
               "  queries:      {}\n"
               "  total rows:   {}\n"
               "  mean:         {:.2f} us\n"
               "  p50:          {:.2f} us\n"
               "  p99:          {:.2f} us\n"
               "  max:          {:.2f} us\n",
               latencies_us.size(), total_rows, mean, p50, p99, latencies_us.back());

    std::filesystem::remove_all(tmp);
}
