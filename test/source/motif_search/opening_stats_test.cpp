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
#include <string_view>
#include <vector>

#include "motif/search/opening_stats.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <chesslib/board/board.hpp>
#include <chesslib/board/move_codec.hpp>
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
        path = base / ("motif_opening_stats_test_" + suffix + "_" + std::to_string(tick));
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

auto encode_moves(std::initializer_list<char const*> sans) -> std::vector<std::uint16_t>
{
    auto board = chesslib::board {};
    auto moves = std::vector<std::uint16_t> {};
    moves.reserve(sans.size());

    for (char const* const san : sans) {
        auto move = chesslib::san::from_string(board, san);
        REQUIRE(move.has_value());
        moves.push_back(chesslib::codec::encode(*move));
        chesslib::move_maker maker {board, *move};
        maker.make();
    }

    return moves;
}

struct game_spec
{
    std::initializer_list<char const*> sans;
    std::string_view result;
    std::optional<std::int32_t> white_elo;
    std::optional<std::int32_t> black_elo;
    std::optional<std::string> eco;
    std::optional<std::string> opening_name;
};

auto make_game(game_spec const& spec) -> motif::db::game
{
    static auto next_player_index = std::uint32_t {1};
    auto const player_index = next_player_index++;

    auto game = motif::db::game {
        .white = {.name = "White Player " + std::to_string(player_index),
                  .elo = spec.white_elo,
                  .title = std::nullopt,
                  .country = std::nullopt},
        .black = {.name = "Black Player " + std::to_string(player_index),
                  .elo = spec.black_elo,
                  .title = std::nullopt,
                  .country = std::nullopt},
        .event_details = std::nullopt,
        .date = std::nullopt,
        .result = std::string {spec.result},
        .eco = spec.eco,
        .moves = encode_moves(spec.sans),
        .extra_tags = {},
    };

    if (spec.opening_name.has_value()) {
        game.extra_tags.emplace_back("Opening", *spec.opening_name);
    }

    return game;
}

void insert_games_and_rebuild(motif::db::database_manager& manager, std::initializer_list<motif::db::game> games)
{
    for (auto const& game : games) {
        auto inserted = manager.store().insert(game);
        REQUIRE(inserted.has_value());
    }

    auto rebuilt = manager.rebuild_position_store();
    REQUIRE(rebuilt.has_value());
}

constexpr auto us_per_ms = 1000.0;
constexpr auto perf_sample_hashes = std::size_t {100};
constexpr auto perf_sample_seed = std::uint64_t {42};
constexpr auto perf_p99_limit_us = 500000.0;
constexpr auto white_elo_high = std::int32_t {2500};
constexpr auto black_elo_high = std::int32_t {2400};
constexpr auto white_elo_mid = std::int32_t {2300};
constexpr auto black_elo_other = std::int32_t {2200};
constexpr auto white_elo_low = std::int32_t {2100};
constexpr auto white_elo_sicilian = std::int32_t {2200};
constexpr auto black_elo_sicilian = std::int32_t {2150};
constexpr auto elo_average_high = 2400.0;
constexpr auto elo_average_other = 2200.0;
constexpr auto elo_average_low = 2100.0;

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
        auto results = motif::search::opening_stats::query(manager, hash);
        auto const stop = std::chrono::steady_clock::now();

        REQUIRE(results.has_value());

        total_rows += results->continuations.size();
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void run_opening_stats_perf_test()
{
    if (is_sanitized_build) {
        SKIP("performance checks are skipped in sanitize builds");
    }

#ifndef NDEBUG
    SKIP("performance checks run only in release builds");
#endif

    // Perf fixture setup is intentionally end-to-end.
    auto const pgn_file = perf_pgn_path();
    if (!std::filesystem::exists(pgn_file)) {
        SKIP("PGN corpus not available");
    }

    tmp_dir const tdir {"perf"};

    auto manager = motif::db::database_manager::create(tdir.path / "db", "opening-stats-perf");
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

    auto const result = measure_query_latencies(*manager, *sample_hashes, "opening_stats::query on sorted position store");

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

TEST_CASE("opening_stats::query aggregates continuation statistics", "[motif-search][opening_stats]")
{
    tmp_dir const tdir {"aggregate"};

    auto manager = motif::db::database_manager::create(tdir.path, "search-db");
    REQUIRE(manager.has_value());

    insert_games_and_rebuild(*manager,
                             {
                                 make_game({.sans = {"e4", "e5", "Nf3", "Nc6"},
                                            .result = "1-0",
                                            .white_elo = white_elo_high,
                                            .black_elo = black_elo_high,
                                            .eco = std::string {"C40"},
                                            .opening_name = std::string {"King's Knight Opening"}}),
                                 make_game({.sans = {"e4", "e5", "Nf3", "d6"},
                                            .result = "1/2-1/2",
                                            .white_elo = white_elo_mid,
                                            .black_elo = std::nullopt,
                                            .eco = std::string {"C40"},
                                            .opening_name = std::string {"King's Knight Opening"}}),
                                 make_game({.sans = {"e4", "e5", "Nc3", "Nc6"},
                                            .result = "0-1",
                                            .white_elo = std::nullopt,
                                            .black_elo = black_elo_other,
                                            .eco = std::string {"C25"},
                                            .opening_name = std::string {"Vienna Game"}}),
                             });

    auto stats = motif::search::opening_stats::query(*manager, hash_after_sans({"e4", "e5"}));
    REQUIRE(stats.has_value());
    REQUIRE(stats->continuations.size() == 2);

    auto const& first = stats->continuations[0];
    auto const& second = stats->continuations[1];

    CHECK(first.san == "Nf3");
    CHECK(first.frequency == 2);
    CHECK(first.white_wins == 1);
    CHECK(first.draws == 1);
    CHECK(first.black_wins == 0);
    REQUIRE(first.average_white_elo.has_value());
    auto const first_average_white_elo = first.average_white_elo.value_or(0.0);
    CHECK_THAT(first_average_white_elo, Catch::Matchers::WithinRel(elo_average_high));
    REQUIRE(first.average_black_elo.has_value());
    auto const first_average_black_elo = first.average_black_elo.value_or(0.0);
    CHECK_THAT(first_average_black_elo, Catch::Matchers::WithinRel(elo_average_high));
    CHECK(first.eco == std::optional<std::string> {"C40"});
    CHECK(first.opening_name == std::optional<std::string> {"King's Knight Opening"});

    CHECK(second.san == "Nc3");
    CHECK(second.frequency == 1);
    CHECK(second.white_wins == 0);
    CHECK(second.draws == 0);
    CHECK(second.black_wins == 1);
    CHECK_FALSE(second.average_white_elo.has_value());
    REQUIRE(second.average_black_elo.has_value());
    auto const second_average_black_elo = second.average_black_elo.value_or(0.0);
    CHECK_THAT(second_average_black_elo, Catch::Matchers::WithinRel(elo_average_other));
    CHECK(second.eco == std::optional<std::string> {"C25"});
    CHECK(second.opening_name == std::optional<std::string> {"Vienna Game"});
}

TEST_CASE("opening_stats::query ignores null Elo values in averages", "[motif-search][opening_stats]")
{
    tmp_dir const tdir {"elo"};

    auto manager = motif::db::database_manager::create(tdir.path, "search-db");
    REQUIRE(manager.has_value());

    insert_games_and_rebuild(*manager,
                             {
                                 make_game({.sans = {"d4", "Nf6", "c4"},
                                            .result = "1-0",
                                            .white_elo = white_elo_low,
                                            .black_elo = std::nullopt,
                                            .eco = std::string {"A46"},
                                            .opening_name = std::string {"Queen's Pawn Game"}}),
                                 make_game({.sans = {"d4", "Nf6", "c4"},
                                            .result = "1/2-1/2",
                                            .white_elo = std::nullopt,
                                            .black_elo = std::nullopt,
                                            .eco = std::string {"A46"},
                                            .opening_name = std::string {"Queen's Pawn Game"}}),
                             });

    auto stats = motif::search::opening_stats::query(*manager, hash_after_sans({"d4", "Nf6"}));
    REQUIRE(stats.has_value());
    REQUIRE(stats->continuations.size() == 1);

    auto const& continuation = stats->continuations.front();
    CHECK(continuation.san == "c4");
    REQUIRE(continuation.average_white_elo.has_value());
    auto const continuation_average_white_elo = continuation.average_white_elo.value_or(0.0);
    CHECK_THAT(continuation_average_white_elo, Catch::Matchers::WithinRel(elo_average_low));
    CHECK_FALSE(continuation.average_black_elo.has_value());
}

TEST_CASE("opening_stats::query returns empty statistics for missing positions", "[motif-search][opening_stats]")
{
    tmp_dir const tdir {"missing"};

    auto manager = motif::db::database_manager::create(tdir.path, "search-db");
    REQUIRE(manager.has_value());

    insert_games_and_rebuild(*manager,
                             {
                                 make_game({.sans = {"e4", "c5", "Nf3"},
                                            .result = "1-0",
                                            .white_elo = white_elo_sicilian,
                                            .black_elo = black_elo_sicilian,
                                            .eco = std::string {"B20"},
                                            .opening_name = std::string {"Sicilian Defense"}}),
                             });

    auto stats = motif::search::opening_stats::query(*manager, hash_after_sans({"c4", "e5", "Nc3"}));
    REQUIRE(stats.has_value());
    CHECK(stats->continuations.empty());
}

TEST_CASE("opening_stats::query skips orphaned rows and returns remaining stats", "[motif-search][opening_stats]")
{
    tmp_dir const tdir {"orphaned-row"};

    auto manager = motif::db::database_manager::create(tdir.path, "search-db");
    REQUIRE(manager.has_value());

    auto orphaned_game_id = manager->store().insert(make_game({.sans = {"e4", "e5", "Nf3", "Nc6"},
                                                               .result = "1-0",
                                                               .white_elo = white_elo_high,
                                                               .black_elo = black_elo_high,
                                                               .eco = std::string {"C40"},
                                                               .opening_name = std::string {"King's Knight Opening"}}));
    REQUIRE(orphaned_game_id.has_value());

    auto surviving_game_id = manager->store().insert(make_game({.sans = {"e4", "e5", "Nc3", "Nc6"},
                                                                .result = "0-1",
                                                                .white_elo = white_elo_low,
                                                                .black_elo = black_elo_other,
                                                                .eco = std::string {"C25"},
                                                                .opening_name = std::string {"Vienna Game"}}));
    REQUIRE(surviving_game_id.has_value());

    auto rebuilt = manager->rebuild_position_store();
    REQUIRE(rebuilt.has_value());

    auto removed = manager->store().remove(*orphaned_game_id);
    REQUIRE(removed.has_value());

    auto stats = motif::search::opening_stats::query(*manager, hash_after_sans({"e4", "e5"}));
    REQUIRE(stats.has_value());
    REQUIRE(stats->continuations.size() == 1);

    auto const& continuation = stats->continuations.front();
    CHECK(continuation.san == "Nc3");
    CHECK(continuation.frequency == 1);
    CHECK(continuation.white_wins == 0);
    CHECK(continuation.draws == 0);
    CHECK(continuation.black_wins == 1);
    CHECK(continuation.eco == std::optional<std::string> {"C25"});
    CHECK(continuation.opening_name == std::optional<std::string> {"Vienna Game"});
}

TEST_CASE("opening_stats::query performance on sorted position store", "[performance][motif-search][opening_stats]")
{
    run_opening_stats_perf_test();
}
