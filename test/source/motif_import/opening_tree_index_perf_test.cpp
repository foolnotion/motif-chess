// Compares ingest-only, exact postings, and opt-in shallow-index paths on one
// corpus and one revision. Informational, not a hard gate.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "motif/db/opening_tree_index.hpp"

#include <catch2/catch_test_macros.hpp>
#include <fmt/core.h>

#include "motif/chess/chess.hpp"
#include "motif/db/database_manager.hpp"
#include "motif/import/import_pipeline.hpp"
#include "motif/import/logger.hpp"
#include "motif/search/opening_stats.hpp"
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

    auto const perf_log_dir = std::filesystem::temp_directory_path() / "motif-opening-tree-index-perf-logs";
    constexpr auto bytes_per_mb = std::size_t {1024} * std::size_t {1024};

    struct import_measurement
    {
        motif::import::import_summary summary;
        std::chrono::milliseconds elapsed;
        std::size_t peak_rss;
        std::uintmax_t games_db_size;
        std::optional<std::uintmax_t> postings_size;
        std::optional<std::uintmax_t> index_size;
    };

    // NOLINTBEGIN(readability-function-cognitive-complexity) -- Catch2 assertions inflate isolated perf setup complexity.
    auto run_import = [&](std::string const& label,
                          bool const build_postings,
                          bool const build_index) -> import_measurement  // NOLINT(readability-function-cognitive-complexity) -- Catch2
                                                                         // assertions inflate isolated perf setup complexity.
    {
        auto const bundle_dir = tmp / label;
        auto manager = motif::db::database_manager::create(bundle_dir, label);
        REQUIRE(manager.has_value());
        std::filesystem::remove_all(perf_log_dir);
        auto init_log = motif::import::initialize_logging({.log_dir = perf_log_dir});
        REQUIRE(init_log.has_value());
        auto config = motif::import::import_config {};
        config.build_position_postings_after_import = build_postings;
        config.build_opening_tree_index_after_import = build_index;
        motif::import::import_pipeline pipeline {*manager};
        auto sampler = peak_rss_sampler {};
        auto const started = std::chrono::steady_clock::now();
        auto summary = pipeline.run(pgn_path, config);
        auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        auto const peak_rss = sampler.peak();
        auto const shutdown_result = motif::import::shutdown_logging();
        (void)shutdown_result;
        REQUIRE(summary.has_value());
        // Derived-index filenames are generation-qualified (see
        // publish_derived_index() in database_manager.cpp) -- capture them
        // from the manifest before close() rather than assuming a fixed name.
        auto const postings_filename = manager->manifest().position_postings
            ? std::optional<std::string> {manager->manifest().position_postings->filename}
            : std::nullopt;
        auto const index_filename = manager->manifest().opening_tree_index
            ? std::optional<std::string> {manager->manifest().opening_tree_index->filename}
            : std::nullopt;
        manager->close();
        std::error_code size_err;
        auto const games_db_size = std::filesystem::file_size(bundle_dir / "games.db", size_err);
        REQUIRE(!size_err);
        auto const file_size_if_present = [&](std::optional<std::string> const& filename) -> std::optional<std::uintmax_t>
        {
            if (!filename) {
                return std::nullopt;
            }
            auto const file_size = std::filesystem::file_size(bundle_dir / *filename, size_err);
            if (!size_err) {
                return file_size;
            }
            if (size_err == std::errc::no_such_file_or_directory) {
                size_err.clear();
                return std::nullopt;
            }
            return std::nullopt;
        };
        auto const postings_size = file_size_if_present(postings_filename);
        REQUIRE(!size_err);
        auto const index_size = file_size_if_present(index_filename);
        REQUIRE(!size_err);
        return {.summary = *summary,
                .elapsed = elapsed,
                .peak_rss = peak_rss,
                .games_db_size = games_db_size,
                .postings_size = postings_size,
                .index_size = index_size};
    };
    // NOLINTEND(readability-function-cognitive-complexity)

    auto const ingest_only = run_import("ingest-only", false, false);
    auto const postings = run_import("postings", true, false);
    auto const indexed = run_import("postings-index", true, true);
    REQUIRE(ingest_only.summary.total_attempted == postings.summary.total_attempted);
    REQUIRE(postings.summary.total_attempted == indexed.summary.total_attempted);
    REQUIRE(ingest_only.summary.committed == postings.summary.committed);
    REQUIRE(postings.summary.committed == indexed.summary.committed);
    REQUIRE(ingest_only.summary.skipped == postings.summary.skipped);
    REQUIRE(postings.summary.skipped == indexed.summary.skipped);
    REQUIRE(ingest_only.summary.errors == postings.summary.errors);
    REQUIRE(postings.summary.errors == indexed.summary.errors);
    REQUIRE_FALSE(ingest_only.postings_size.has_value());
    REQUIRE(postings.postings_size.has_value());
    REQUIRE(indexed.index_size.has_value());

    fmt::print("\n=== opening_tree_index perf: serialized 1M comparison ===\n"
               "  committed games:       {}\n"
               "  ingest-only elapsed:   {} ms\n"
               "  postings elapsed:      {} ms\n"
               "  postings delta:        {} ms\n"
               "  postings+index:        {} ms\n"
               "  shallow-index delta:   {} ms\n"
               "  ingest-only peak RSS:  {} MB\n"
               "  postings peak RSS:     {} MB\n"
               "  indexed peak RSS:      {} MB\n"
               "  games.db:              {} MB\n"
               "  positions.postings:    {} MB\n"
               "  opening_tree.idx:      {} MB\n",
               indexed.summary.committed, ingest_only.elapsed.count(), postings.elapsed.count(),
               postings.elapsed.count() - ingest_only.elapsed.count(), indexed.elapsed.count(),
               indexed.elapsed.count() - postings.elapsed.count(), ingest_only.peak_rss / bytes_per_mb,
               postings.peak_rss / bytes_per_mb, indexed.peak_rss / bytes_per_mb, indexed.games_db_size / bytes_per_mb,
               postings.postings_size.value_or(0U) / bytes_per_mb, indexed.index_size.value_or(0U) / bytes_per_mb);
    REQUIRE(postings.summary.position_postings_metrics.has_value());
    REQUIRE(indexed.summary.position_postings_metrics.has_value());
    REQUIRE(indexed.summary.opening_tree_index_metrics.has_value());
    auto const posting_metrics = postings.summary.position_postings_metrics.value_or(motif::db::position_postings_build_metrics {});
    auto const indexed_posting_metrics = indexed.summary.position_postings_metrics.value_or(motif::db::position_postings_build_metrics {});
    auto const tree_metrics = indexed.summary.opening_tree_index_metrics.value_or(motif::db::opening_tree_index_build_metrics {});
    auto const postings_builder_elapsed = posting_metrics.replay_elapsed + posting_metrics.spill_elapsed + posting_metrics.merge_elapsed
        + posting_metrics.metadata_write_elapsed + posting_metrics.directory_write_elapsed + posting_metrics.final_write_elapsed;
    auto const indexed_postings_builder_elapsed = indexed_posting_metrics.replay_elapsed + indexed_posting_metrics.spill_elapsed
        + indexed_posting_metrics.merge_elapsed + indexed_posting_metrics.metadata_write_elapsed
        + indexed_posting_metrics.directory_write_elapsed + indexed_posting_metrics.final_write_elapsed;
    auto const tree_builder_elapsed = tree_metrics.replay_elapsed + tree_metrics.root_merge_elapsed + tree_metrics.edge_merge_elapsed
        + tree_metrics.child_merge_elapsed + tree_metrics.index_write_elapsed;
    auto const ingest_only_accounted = ingest_only.summary.ingest_elapsed + ingest_only.summary.dedup_elapsed;
    auto const postings_derived_wall = postings.summary.elapsed - postings.summary.ingest_elapsed - postings.summary.dedup_elapsed;
    auto const indexed_derived_wall = indexed.summary.elapsed - indexed.summary.ingest_elapsed - indexed.summary.dedup_elapsed;
    fmt::print("=== opening_tree_index perf: import components ===\n"
               "  ingest-only: ingest {} dedup {} remainder {} total {} ms\n"
               "  postings: ingest {} dedup {} derived-wall {} measured-builder {} remainder {} total {} ms\n"
               "  postings+tree: ingest {} dedup {} derived-wall {} postings-builder {} tree-builder {} remainder {} total {} ms\n",
               ingest_only.summary.ingest_elapsed.count(),
               ingest_only.summary.dedup_elapsed.count(),
               (ingest_only.summary.elapsed - ingest_only_accounted).count(),
               ingest_only.summary.elapsed.count(),
               postings.summary.ingest_elapsed.count(),
               postings.summary.dedup_elapsed.count(),
               postings_derived_wall.count(),
               postings_builder_elapsed.count(),
               (postings_derived_wall - postings_builder_elapsed).count(),
               postings.summary.elapsed.count(),
               indexed.summary.ingest_elapsed.count(),
               indexed.summary.dedup_elapsed.count(),
               indexed_derived_wall.count(),
               indexed_postings_builder_elapsed.count(),
               tree_builder_elapsed.count(),
               (indexed_derived_wall - indexed_postings_builder_elapsed - tree_builder_elapsed).count(),
               indexed.summary.elapsed.count());
    fmt::print("=== opening_tree_index perf: build phases ===\n"
               "  postings input rows:   {}\n"
               "  postings occurrences:  {}\n"
               "  postings hashes:       {}\n"
               "  postings spill runs:   {}\n"
               "  postings replay:       {} ms\n"
               "  postings spills:       {} ms\n"
               "  postings merge:        {} ms\n"
               "  postings metadata:     {} ms\n"
               "  postings directory:    {} ms\n"
               "  postings final write:  {} ms\n"
               "  postings bytes:        payload {} metadata {} directory {} sparse {}\n"
               "  directory fields:     headers {} bitmaps {} hash-encoding {} length {} occurrence-exceptions {} games {} min-ply {} max-ply {}\n"
               "  directory patterns:   equal occurrence/game {} hash-delta >32b {} >40b {} >48b {}\n"
               "  postings temp peak:    {} MB\n"
               "  tree root records:     {}\n"
               "  tree edge records:     {}\n"
               "  tree child visits:     {}\n"
               "  external child counts: {}\n"
               "  child lookups/loaded:  {} / {}\n"
               "  tree spill runs:       roots {} edges {} child {}\n"
               "  tree replay:           {} ms\n"
               "  tree merges:           roots {} edges {} child {} ms\n"
               "  child frequency load:  {} ms\n"
               "  tree index write:      {} ms\n",
               posting_metrics.input_occurrence_count, posting_metrics.occurrence_count, posting_metrics.distinct_hash_count,
               posting_metrics.spill_run_count, posting_metrics.replay_elapsed.count(), posting_metrics.spill_elapsed.count(),
               posting_metrics.merge_elapsed.count(), posting_metrics.metadata_write_elapsed.count(),
               posting_metrics.directory_write_elapsed.count(), posting_metrics.final_write_elapsed.count(), posting_metrics.posting_bytes,
                posting_metrics.metadata_bytes, posting_metrics.directory_bytes, posting_metrics.sparse_directory_bytes,
                posting_metrics.directory_block_header_bytes,
                posting_metrics.directory_bitmap_bytes,
                posting_metrics.directory_hash_encoding_bytes,
                posting_metrics.directory_posting_length_bytes,
                posting_metrics.directory_occurrence_count_bytes,
                posting_metrics.directory_distinct_game_count_bytes,
                posting_metrics.directory_min_ply_bytes,
                posting_metrics.directory_max_ply_bytes,
                posting_metrics.directory_equal_occurrence_game_count,
                posting_metrics.directory_hash_delta_over_32_bits,
                posting_metrics.directory_hash_delta_over_40_bits,
                posting_metrics.directory_hash_delta_over_48_bits,
                posting_metrics.peak_temp_bytes / bytes_per_mb, tree_metrics.root_record_count,
                tree_metrics.edge_record_count, tree_metrics.child_visit_count, tree_metrics.child_counts_external,
               tree_metrics.child_frequency_lookup_count, tree_metrics.child_frequency_loaded_count,
               tree_metrics.root_spill_run_count, tree_metrics.edge_spill_run_count, tree_metrics.child_spill_run_count,
                tree_metrics.replay_elapsed.count(),
                tree_metrics.root_merge_elapsed.count(), tree_metrics.edge_merge_elapsed.count(), tree_metrics.child_merge_elapsed.count(),
                tree_metrics.child_frequency_load_elapsed.count(),
                tree_metrics.index_write_elapsed.count());

    // Reopen the bundle first: the derived-index filename is
    // generation-qualified (see publish_derived_index() in
    // database_manager.cpp), so it must be read from the reopened manifest
    // rather than assumed.
    auto indexed_manager = motif::db::database_manager::open(tmp / "postings-index");
    REQUIRE(indexed_manager.has_value());
    REQUIRE(indexed_manager->manifest().opening_tree_index.has_value());
    auto const index_path = tmp / "postings-index"
        / indexed_manager->manifest().opening_tree_index.value_or(motif::db::derived_index_manifest_entry {}).filename;

    auto opened = motif::db::opening_tree_index::open(index_path);
    REQUIRE(opened.has_value());

    // Sample real positions from a batch of committed games (root_ply <= 20,
    // the index's own cap), rather than synthetic hashes -- a representative
    // navigation workload: every ply a user would actually step through with
    // the opening explorer, not just the starting position.
    std::vector<motif::db::zobrist_hash> sample_hashes;
    constexpr auto sample_games = std::uint32_t {2000};
    constexpr auto max_ply_sampled = std::size_t {20};
    for (std::uint32_t id = 1; id <= sample_games && id <= static_cast<std::uint32_t>(indexed.summary.committed); ++id) {
        auto game_res = indexed_manager->store().get(motif::db::game_id {id});
        if (game_res) {
            auto board = motif::chess::board {};
            sample_hashes.push_back(motif::db::zobrist_hash {board.hash()});
            auto const ply_limit = std::min(game_res->moves.size(), max_ply_sampled);
            for (std::size_t ply = 0; ply < ply_limit; ++ply) {
                motif::chess::apply_encoded_move(board, game_res->moves[ply]);
                sample_hashes.push_back(motif::db::zobrist_hash {board.hash()});
            }
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

    std::ranges::sort(sample_hashes);
    auto const unique_hashes_end = std::ranges::unique(sample_hashes);
    sample_hashes.erase(unique_hashes_end.begin(), unique_hashes_end.end());
    auto const starting_hash = motif::db::zobrist_hash {motif::chess::board {}.hash()};

    auto time_public_query = [&](motif::db::zobrist_hash const hash) -> double
    {
        auto const started = std::chrono::steady_clock::now();
        auto const stats = motif::search::opening_stats::query(*indexed_manager, hash);
        auto const elapsed = std::chrono::steady_clock::now() - started;
        REQUIRE(stats.has_value());
        return std::chrono::duration<double, std::micro>(elapsed).count();
    };

    auto const starting_first_us = time_public_query(starting_hash);
    constexpr auto starting_repeat_count = std::size_t {5};
    auto starting_repeats_us = std::vector<double> {};
    starting_repeats_us.reserve(starting_repeat_count);
    for (std::size_t index = 0; index < starting_repeat_count; ++index) {
        starting_repeats_us.push_back(time_public_query(starting_hash));
    }
    std::ranges::sort(starting_repeats_us);

    constexpr auto public_sample_limit = std::size_t {512};
    auto public_hashes = std::vector<motif::db::zobrist_hash> {};
    public_hashes.reserve(std::min(public_sample_limit, sample_hashes.size()));
    for (auto const hash : sample_hashes) {
        if (hash != starting_hash) {
            public_hashes.push_back(hash);
        }
        if (public_hashes.size() == public_sample_limit) {
            break;
        }
    }
    REQUIRE(!public_hashes.empty());

    auto measure_public_sample = [&](std::span<motif::db::zobrist_hash const> const hashes) -> std::vector<double>
    {
        auto measured = std::vector<double> {};
        measured.reserve(hashes.size());
        for (auto const hash : hashes) {
            measured.push_back(time_public_query(hash));
        }
        std::ranges::sort(measured);
        return measured;
    };
    auto const public_first_us = measure_public_sample(public_hashes);
    auto const public_warm_us = measure_public_sample(public_hashes);
    auto print_public_distribution = [](std::string_view const label, std::vector<double> const& measured) -> void
    {
        auto const p50_value = measured[measured.size() / 2U];
        auto const p99_index_value = std::min(measured.size() - 1U, static_cast<std::size_t>(static_cast<double>(measured.size()) * 0.99));
        // NOLINTNEXTLINE(boost-use-ranges) -- boost is not a project dependency
        auto const mean_value = std::accumulate(measured.begin(), measured.end(), 0.0) / static_cast<double>(measured.size());
        fmt::print("  {}: queries {} mean {:.2f} us p50 {:.2f} us p99 {:.2f} us max {:.2f} us\n",
                   label,
                   measured.size(),
                   mean_value,
                   p50_value,
                   measured[p99_index_value],
                   measured.back());
    };
    fmt::print(
        "=== opening explorer public API latency ===\n" "  starting position: first {:.2f} us warm-p50 {:.2f} us warm-max {:.2f} us\n",
        starting_first_us,
        starting_repeats_us[starting_repeats_us.size() / 2U],
        starting_repeats_us.back());
    print_public_distribution("sample first pass", public_first_us);
    print_public_distribution("sample warm pass", public_warm_us);

    indexed_manager->close();
    std::filesystem::remove_all(tmp);
}
