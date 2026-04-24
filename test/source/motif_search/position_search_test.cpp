#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <ratio>
#include <string>
#include <vector>

#include "motif/search/position_search.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chesslib/board/board.hpp>
#include <chesslib/util/san.hpp>

#include "motif/db/database_manager.hpp"
#include "motif/db/types.hpp"
#include "motif/import/import_pipeline.hpp"
#include "motif/import/logger.hpp"
#include "test_helpers.hpp"

namespace
{

using test_helpers::is_sanitized_build;

struct tmp_dir
{
    std::filesystem::path path;

    explicit tmp_dir(std::string const& suffix)
    {
        auto const base = std::filesystem::temp_directory_path();
        auto const tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = base / ("motif_search_test_" + suffix + "_" + std::to_string(tick));
    }

    ~tmp_dir() { std::filesystem::remove_all(path); }

    tmp_dir(tmp_dir const&) = delete;
    auto operator=(tmp_dir const&) -> tmp_dir& = delete;
    tmp_dir(tmp_dir&&) = delete;
    auto operator=(tmp_dir&&) -> tmp_dir& = delete;
};

auto hash_after_sans(std::initializer_list<char const*> sans) -> std::uint64_t
{
    auto board = chesslib::board {};
    for (char const* const san : sans) {
        auto move = chesslib::san::from_string(board, san);
        REQUIRE(move.has_value());
        chesslib::move_maker maker {board, *move};
        maker.make();
    }
    return board.hash();
}

constexpr auto us_per_ms = 1000.0;
constexpr auto perf_sample_hashes = std::size_t {200};
constexpr auto perf_sample_seed = std::uint64_t {42};
constexpr auto perf_p99_limit_us = 100000.0;

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

struct query_latency_result
{
    std::string variant_name;
    std::size_t num_queries {};
    double total_ms {};
    double p50_us {};
    double p99_us {};
    double min_us {};
    double max_us {};
    std::size_t total_rows_returned {};
};

auto measure_query_latencies(motif::db::database_manager const& manager,
                             std::vector<std::uint64_t> const& sample_hashes,
                             std::string_view const variant_name) -> query_latency_result
{
    std::vector<double> latencies_us;
    latencies_us.reserve(sample_hashes.size());

    auto total_rows = std::size_t {};
    for (auto const hash : sample_hashes) {
        auto const start = std::chrono::steady_clock::now();
        auto results = motif::search::position_search::find(manager, hash);
        auto const stop = std::chrono::steady_clock::now();

        REQUIRE(results.has_value());

        total_rows += results->size();
        latencies_us.push_back(std::chrono::duration<double, std::micro>(stop - start).count());
    }

    std::ranges::sort(latencies_us);

    auto const count = latencies_us.size();
    auto total_ms = 0.0;
    for (auto const latency : latencies_us) {
        total_ms += latency;
    }
    total_ms /= us_per_ms;

    auto const p50_idx = std::min(count - 1, static_cast<std::size_t>(static_cast<double>(count) * 0.50));
    auto const p99_idx = std::min(count - 1, static_cast<std::size_t>(static_cast<double>(count) * 0.99));

    return query_latency_result {
        .variant_name = std::string {variant_name},
        .num_queries = count,
        .total_ms = total_ms,
        .p50_us = count > 0 ? latencies_us[p50_idx] : 0.0,
        .p99_us = count > 0 ? latencies_us[p99_idx] : 0.0,
        .min_us = count > 0 ? latencies_us.front() : 0.0,
        .max_us = count > 0 ? latencies_us.back() : 0.0,
        .total_rows_returned = total_rows,
    };
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch assertions make the perf setup inherently branchy
void run_position_search_perf_test()
{
    if (is_sanitized_build) {
        SKIP("performance checks are skipped in sanitize builds");
    }

#ifndef NDEBUG
    SKIP("performance checks run only in release builds");
#endif

    auto const pgn_file = perf_pgn_path();
    if (!std::filesystem::exists(pgn_file)) {
        SKIP("PGN corpus not available");
    }

    tmp_dir const tdir {"perf"};

    auto manager = motif::db::database_manager::create(tdir.path / "db", "search-perf");
    REQUIRE(manager.has_value());

    auto init_log = motif::import::initialize_logging({.log_dir = tdir.path / "logs"});
    REQUIRE(init_log.has_value());

    motif::import::import_pipeline pipeline {*manager};
    auto summary = pipeline.run(pgn_file, motif::import::import_config {});
    REQUIRE(summary.has_value());
    REQUIRE(summary->committed > 0);

    auto sample_hashes = manager->positions().sample_zobrist_hashes(perf_sample_hashes, perf_sample_seed);
    REQUIRE(sample_hashes.has_value());
    REQUIRE_FALSE(sample_hashes->empty());

    auto const result = measure_query_latencies(*manager, *sample_hashes, "position_search sorted by zobrist");

    std::cout << "\n=== " << result.variant_name << " ===\n"
              << "  queries:      " << result.num_queries << "\n"
              << "  total:        " << result.total_ms << " ms\n"
              << "  p50:          " << result.p50_us << " us\n"
              << "  p99:          " << result.p99_us << " us\n"
              << "  min:          " << result.min_us << " us\n"
              << "  max:          " << result.max_us << " us\n"
              << "  total rows:   " << result.total_rows_returned << "\n";

    CHECK(result.p99_us < perf_p99_limit_us);

    auto const shutdown_result = motif::import::shutdown_logging();
    REQUIRE(shutdown_result.has_value());
}

}  // namespace

TEST_CASE("position_search::find returns matching position rows", "[motif-search][position_search]")
{
    tmp_dir const tdir {"find_hits"};

    auto manager = motif::db::database_manager::create(tdir.path, "search-db");
    REQUIRE(manager.has_value());

    auto const target_hash = hash_after_sans({"e4", "e5"});

    std::vector<motif::db::position_row> const rows {
        {.zobrist_hash = target_hash,
         .game_id = 11,
         .ply = 2,
         .result = 1,
         .white_elo = std::int16_t {2700},
         .black_elo = std::int16_t {2650}},
        {.zobrist_hash = target_hash, .game_id = 42, .ply = 2, .result = 0, .white_elo = std::int16_t {2500}, .black_elo = std::nullopt},
        {.zobrist_hash = hash_after_sans({"d4"}),
         .game_id = 99,
         .ply = 1,
         .result = -1,
         .white_elo = std::int16_t {2400},
         .black_elo = std::int16_t {2450}},
    };

    REQUIRE(manager->positions().insert_batch(rows).has_value());

    auto found = motif::search::position_search::find(*manager, target_hash);
    REQUIRE(found.has_value());
    REQUIRE(found->size() == 2);

    std::ranges::sort(*found, {}, &motif::db::position_match::game_id);

    auto const& first = (*found)[0];
    auto const& second = (*found)[1];

    CHECK(first.game_id == 11);
    CHECK(first.ply == 2);
    CHECK(first.result == 1);
    CHECK(first.white_elo == std::optional<std::int16_t> {2700});
    CHECK(first.black_elo == std::optional<std::int16_t> {2650});

    CHECK(second.game_id == 42);
    CHECK(second.ply == 2);
    CHECK(second.result == 0);
    CHECK(second.white_elo == std::optional<std::int16_t> {2500});
    CHECK_FALSE(second.black_elo.has_value());
}

TEST_CASE("position_search::find returns empty result for missing hash", "[motif-search][position_search]")
{
    tmp_dir const tdir {"find_miss"};

    auto manager = motif::db::database_manager::create(tdir.path, "search-db");
    REQUIRE(manager.has_value());

    std::vector<motif::db::position_row> const rows {
        {.zobrist_hash = hash_after_sans({"e4"}),
         .game_id = 7,
         .ply = 1,
         .result = 1,
         .white_elo = std::int16_t {2200},
         .black_elo = std::int16_t {2100}},
    };
    REQUIRE(manager->positions().insert_batch(rows).has_value());

    auto found = motif::search::position_search::find(*manager, hash_after_sans({"c4", "e5", "Nc3"}));
    REQUIRE(found.has_value());
    CHECK(found->empty());
}

TEST_CASE("position_search::find performance on sorted position store", "[performance][motif-search][position_search]")
{
    run_position_search_perf_test();
}
