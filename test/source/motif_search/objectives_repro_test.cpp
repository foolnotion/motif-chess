// Reproduces the three optimization objectives on one corpus in one process:
// import time, tree navigation time, and database size on disk. Output is a
// stream of `OBJ key=value` lines so runs at different commits can be compared
// with grep. Select the configuration with MOTIF_REPRO_CONFIG=default|tree and
// the corpus with MOTIF_IMPORT_PERF_PGN (default: bench/data/twic-1m.pgn).
// Heavy: run one case per process, serialized, from a Release build.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <chesslib/board/board.hpp>
#include <fmt/base.h>

#include "motif/chess/chess.hpp"
#include "motif/db/database_manager.hpp"
#include "motif/db/game_store.hpp"
#include "motif/db/types.hpp"
#include "motif/import/import_pipeline.hpp"
#include "motif/import/logger.hpp"
#include "motif/search/opening_stats.hpp"
#include "motif_bench/bench_support.hpp"
#include "test_helpers.hpp"

namespace
{

constexpr std::size_t start_warm_repeats = 30;
constexpr double bytes_per_mib = 1048576.0;

// Shared with the benchmark suite so both sample the same positions.
using motif_bench::env_string;
using motif_bench::median_quantile;
using motif_bench::peak_rss_mib;
using motif_bench::percentile;
using motif_bench::sample_navigation_hashes;
using motif_bench::tail_quantile;

auto corpus_path() -> std::filesystem::path
{
    auto const fallback = (std::filesystem::path {MOTIF_PROJECT_SOURCE_DIR} / "bench" / "data" / "twic-1m.pgn").string();
    return env_string("MOTIF_IMPORT_PERF_PGN", fallback);
}

// percentile(), peak_rss_mib() and sample_navigation_hashes() live in
// motif_bench/bench_support.hpp.

auto time_query_ms(motif::db::database_manager const& manager, motif::db::zobrist_hash const hash) -> double
{
    auto const begin = std::chrono::steady_clock::now();
    auto stats = motif::search::opening_stats::query(manager, hash);
    auto const end = std::chrono::steady_clock::now();
    REQUIRE(stats.has_value());
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

void print(std::string_view const key, auto const& value)
{
    fmt::print("OBJ {}={}\n", key, value);
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one measured scenario, reported linearly
TEST_CASE("objectives: import time, navigation time, and disk size on the benchmark corpus", "[performance][objectives]")
{
    if (test_helpers::is_sanitized_build) {
        SKIP("performance checks are skipped in sanitize builds");
    }
#ifndef NDEBUG
    SKIP("performance checks run only in release builds");
#endif

    auto const pgn_file = corpus_path();
    if (!std::filesystem::exists(pgn_file)) {
        SKIP("PGN corpus not available");
    }

    auto const config_name = env_string("MOTIF_REPRO_CONFIG", "default");
    REQUIRE((config_name == "default" || config_name == "tree"));

    auto const work_dir =
        std::filesystem::path {env_string("MOTIF_REPRO_DIR", (std::filesystem::temp_directory_path() / "motif-objectives-repro").string())};
    std::filesystem::remove_all(work_dir);
    std::filesystem::create_directories(work_dir);

    auto manager = motif::db::database_manager::create(work_dir / "db", "objectives");
    REQUIRE(manager.has_value());
    auto init_log = motif::import::initialize_logging({.log_dir = work_dir / "logs"});
    REQUIRE(init_log.has_value());

    motif::import::import_config config {};
    config.build_opening_tree_index_after_import = config_name == "tree";

    motif::import::import_pipeline pipeline {*manager};
    auto summary = pipeline.run(pgn_file, config);
    REQUIRE(summary.has_value());
    REQUIRE(summary->committed > 0);

    print("config", config_name);
    print("corpus", pgn_file.filename().string());
    print("games_attempted", summary->total_attempted);
    print("games_committed", summary->committed);
    print("import_wall_ms", summary->elapsed.count());
    print("import_ingest_ms", summary->ingest_elapsed.count());
    print("import_dedup_ms", summary->dedup_elapsed.count());
    print("peak_rss_after_import_mib", peak_rss_mib());

    // Build-phase breakdown, available only where the pipeline reports it.
    if (auto const& metrics = summary->position_postings_metrics; metrics.has_value()) {
        print("x_postings_occurrences", metrics->occurrence_count);
        print("x_postings_distinct_hashes", metrics->distinct_hash_count);
        print("x_postings_spill_runs", metrics->spill_run_count);
        print("x_postings_posting_bytes", metrics->posting_bytes);
        print("x_postings_metadata_bytes", metrics->metadata_bytes);
        print("x_postings_directory_bytes", metrics->directory_bytes);
        print("x_postings_sparse_directory_bytes", metrics->sparse_directory_bytes);
        print("x_postings_peak_temp_bytes", metrics->peak_temp_bytes);
        print("x_postings_final_artifact_bytes", metrics->final_artifact_bytes);
        print("x_postings_replay_ms", metrics->replay_elapsed.count());
        print("x_postings_spill_ms", metrics->spill_elapsed.count());
        print("x_postings_merge_ms", metrics->merge_elapsed.count());
        print("x_postings_final_write_ms", metrics->final_write_elapsed.count());
    }
    if (auto const& metrics = summary->opening_tree_index_metrics; metrics.has_value()) {
        print("x_tree_edge_records", metrics->edge_record_count);
        print("x_tree_child_spill_runs", metrics->child_spill_run_count);
        print("x_tree_child_counts_external", metrics->child_counts_external);
        print("x_tree_replay_ms", metrics->replay_elapsed.count());
        print("x_tree_root_merge_ms", metrics->root_merge_elapsed.count());
        print("x_tree_edge_merge_ms", metrics->edge_merge_elapsed.count());
        print("x_tree_child_merge_ms", metrics->child_merge_elapsed.count());
        print("x_tree_index_write_ms", metrics->index_write_elapsed.count());
    }

    // Objective 3: bytes on disk, every file in the bundle. Close first so
    // SQLite checkpoints its WAL and the sizes describe the retained bundle.
    manager->close();
    std::uintmax_t total_bytes = 0;
    for (auto const& entry : std::filesystem::directory_iterator {work_dir / "db"}) {
        if (!entry.is_regular_file()) {
            continue;
        }
        auto const bytes = entry.file_size();
        total_bytes += bytes;
        fmt::print("OBJ file {}={}\n", entry.path().filename().string(), bytes);
    }
    print("size_total_bytes", total_bytes);
    print("size_total_mib", static_cast<double>(total_bytes) / bytes_per_mib);

    auto reopened = motif::db::database_manager::open(work_dir / "db");
    REQUIRE(reopened.has_value());

    // Objective 2: opening-explorer query latency through the public API.
    auto const start_hash = motif::db::zobrist_hash {chesslib::board {}.hash()};
    print("nav_start_first_ms", time_query_ms(*reopened, start_hash));
    std::vector<double> start_warm;
    start_warm.reserve(start_warm_repeats);
    for (std::size_t i = 0; i < start_warm_repeats; ++i) {
        start_warm.push_back(time_query_ms(*reopened, start_hash));
    }
    print("nav_start_warm_p50_ms", percentile(start_warm, median_quantile));

    auto const sample = sample_navigation_hashes(*reopened, start_hash);
    REQUIRE_FALSE(sample.empty());
    print("nav_other_sample_size", sample.size());
    for (auto const* const pass : {"cold", "warm"}) {
        std::vector<double> latencies;
        latencies.reserve(sample.size());
        for (auto const hash : sample) {
            latencies.push_back(time_query_ms(*reopened, hash));
        }
        print(std::string {"nav_other_"} + pass + "_p50_ms", percentile(latencies, median_quantile));
        print(std::string {"nav_other_"} + pass + "_p99_ms", percentile(latencies, tail_quantile));
    }

    print("peak_rss_mib", peak_rss_mib());

    auto const shutdown_result = motif::import::shutdown_logging();
    REQUIRE(shutdown_result.has_value());
    reopened->close();
    std::filesystem::remove_all(work_dir);
}
