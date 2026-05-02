#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <optional>
#include <ratio>
#include <string>
#include <string_view>
#include <vector>

#include "motif/search/opening_tree.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chesslib/board/board.hpp>
#include <chesslib/board/move_codec.hpp>
#include <chesslib/util/san.hpp>

#include "motif/db/database_manager.hpp"
#include "motif/db/types.hpp"
#include "motif/import/import_pipeline.hpp"
#include "motif/import/logger.hpp"
#include "motif/search/error.hpp"
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
        path = base / ("motif_opening_tree_test_" + suffix + "_" + std::to_string(tick));
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
        .provenance = {},
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
constexpr auto black_elo_other = std::int32_t {2200};
constexpr auto white_elo_low = std::int32_t {2100};
constexpr auto default_prefetch_depth = std::size_t {5};

auto perf_pgn_path() -> std::filesystem::path
{
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
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

TEST_CASE("opening_tree::open returns root with correct continuations at depth 1", "[motif-search][opening_tree]")
{
    tmp_dir const tdir {"depth1"};

    auto manager = motif::db::database_manager::create(tdir.path, "tree-db");
    REQUIRE(manager.has_value());

    auto const root_hash = hash_after_sans({"e4", "e5"});

    insert_games_and_rebuild(*manager,
                             {
                                 make_game({.sans = {"e4", "e5", "Nf3", "Nc6"},
                                            .result = "1-0",
                                            .white_elo = white_elo_high,
                                            .black_elo = black_elo_high,
                                            .eco = std::string {"C40"},
                                            .opening_name = std::string {"King's Knight Opening"}}),
                                 make_game({.sans = {"e4", "e5", "Nc3", "Nc6"},
                                            .result = "0-1",
                                            .white_elo = white_elo_low,
                                            .black_elo = black_elo_other,
                                            .eco = std::string {"C25"},
                                            .opening_name = std::string {"Vienna Game"}}),
                             });

    auto tree_res = motif::search::opening_tree::open(*manager, root_hash, default_prefetch_depth);
    REQUIRE(tree_res.has_value());

    auto const& root = tree_res->root;
    CHECK(root.zobrist_hash == root_hash);
    CHECK(root.is_expanded);
    REQUIRE(root.continuations.size() == 2);

    CHECK(root.continuations[0].san == "Nc3");
    CHECK(root.continuations[0].frequency == 1);
    CHECK(root.continuations[0].black_wins == 1);
    CHECK(root.continuations[0].result_hash == hash_after_sans({"e4", "e5", "Nc3"}));

    CHECK(root.continuations[1].san == "Nf3");
    CHECK(root.continuations[1].frequency == 1);
    CHECK(root.continuations[1].white_wins == 1);
    CHECK(root.continuations[1].result_hash == hash_after_sans({"e4", "e5", "Nf3"}));
}

TEST_CASE("opening_tree::open prefetches to configured depth", "[motif-search][opening_tree]")
{
    tmp_dir const tdir {"prefetch"};

    auto manager = motif::db::database_manager::create(tdir.path, "tree-db");
    REQUIRE(manager.has_value());

    // Use hash_after_sans({"e4"}) as root — this exists in the
    // position table (stored at ply=1).
    auto const root_hash = hash_after_sans({"e4"});

    insert_games_and_rebuild(*manager,
                             {
                                 make_game({.sans = {"e4", "e5", "Nf3", "Nc6", "Bb5"},
                                            .result = "1-0",
                                            .white_elo = white_elo_high,
                                            .black_elo = black_elo_high,
                                            .eco = std::string {"C60"},
                                            .opening_name = std::string {"Ruy Lopez"}}),
                             });

    // prefetch_depth=4: expand e5(depth1), Nf3(depth2), Nc6(depth3)
    // Bb5 is at depth4 = boundary → not expanded
    auto tree_res = motif::search::opening_tree::open(*manager, root_hash, 4);
    REQUIRE(tree_res.has_value());

    auto const& root = tree_res->root;
    CHECK(root.is_expanded);
    REQUIRE_FALSE(root.continuations.empty());

    auto const& e5_cont = root.continuations[0];
    CHECK(e5_cont.san == "e5");
    CHECK(e5_cont.subtree != nullptr);
    CHECK(e5_cont.subtree->is_expanded);

    REQUIRE_FALSE(e5_cont.subtree->continuations.empty());
    auto const& nf3_cont = e5_cont.subtree->continuations[0];
    CHECK(nf3_cont.san == "Nf3");
    CHECK(nf3_cont.subtree != nullptr);
    CHECK(nf3_cont.subtree->is_expanded);

    auto const& nc6_cont = nf3_cont.subtree->continuations[0];
    CHECK(nc6_cont.san == "Nc6");
    CHECK(nc6_cont.subtree != nullptr);
    CHECK(nc6_cont.subtree->is_expanded);

    auto const& bb5_cont = nc6_cont.subtree->continuations[0];
    CHECK(bb5_cont.san == "Bb5");
    CHECK(bb5_cont.subtree != nullptr);
    CHECK_FALSE(bb5_cont.subtree->is_expanded);
}

TEST_CASE("opening_tree::open leaves boundary nodes unexpanded", "[motif-search][opening_tree]")
{
    tmp_dir const tdir {"boundary"};

    auto manager = motif::db::database_manager::create(tdir.path, "tree-db");
    REQUIRE(manager.has_value());

    auto const root_hash = hash_after_sans({"e4"});

    insert_games_and_rebuild(*manager,
                             {
                                 make_game({.sans = {"e4", "e5", "Nf3", "Nc6"},
                                            .result = "1-0",
                                            .white_elo = white_elo_high,
                                            .black_elo = black_elo_high,
                                            .eco = std::string {"C40"},
                                            .opening_name = std::string {"King's Knight"}}),
                             });

    // prefetch_depth=2: e5 at depth1 is expanded; Nf3 at depth2 is
    // the boundary → its subtree is not expanded
    auto tree_res = motif::search::opening_tree::open(*manager, root_hash, 2);
    REQUIRE(tree_res.has_value());

    auto const& root = tree_res->root;
    CHECK(root.is_expanded);
    REQUIRE_FALSE(root.continuations.empty());

    auto const& e5_continuation = root.continuations[0];
    CHECK(e5_continuation.subtree->is_expanded);

    auto const& nf3 = e5_continuation.subtree->continuations[0];
    CHECK_FALSE(nf3.subtree->is_expanded);
}

TEST_CASE("opening_tree::expand populates children for unexpanded node", "[motif-search][opening_tree]")
{
    tmp_dir const tdir {"expand"};

    auto manager = motif::db::database_manager::create(tdir.path, "tree-db");
    REQUIRE(manager.has_value());

    auto const root_hash = hash_after_sans({"e4"});

    insert_games_and_rebuild(*manager,
                             {
                                 make_game({.sans = {"e4", "e5", "Nf3", "Nc6", "Bb5"},
                                            .result = "1-0",
                                            .white_elo = white_elo_high,
                                            .black_elo = black_elo_high,
                                            .eco = std::string {"C60"},
                                            .opening_name = std::string {"Ruy Lopez"}}),
                             });

    auto tree_res = motif::search::opening_tree::open(*manager, root_hash, 2);
    REQUIRE(tree_res.has_value());

    auto& root = tree_res->root;
    REQUIRE_FALSE(root.continuations.empty());
    auto& e5_continuation = root.continuations[0];
    REQUIRE_FALSE(e5_continuation.subtree->continuations.empty());
    auto& nf3 = e5_continuation.subtree->continuations[0];
    CHECK_FALSE(nf3.subtree->is_expanded);

    auto expand_res = motif::search::opening_tree::expand(*manager, *nf3.subtree);
    REQUIRE(expand_res.has_value());
    CHECK(nf3.subtree->is_expanded);
    REQUIRE_FALSE(nf3.subtree->continuations.empty());
    CHECK(nf3.subtree->continuations[0].san == "Nc6");
}

TEST_CASE("opening_tree::expand is no-op on already expanded node", "[motif-search][opening_tree]")
{
    tmp_dir const tdir {"noop"};

    auto manager = motif::db::database_manager::create(tdir.path, "tree-db");
    REQUIRE(manager.has_value());

    auto const root_hash = hash_after_sans({"e4", "e5"});

    insert_games_and_rebuild(*manager,
                             {
                                 make_game({.sans = {"e4", "e5", "Nf3"},
                                            .result = "1-0",
                                            .white_elo = white_elo_high,
                                            .black_elo = black_elo_high,
                                            .eco = std::string {"C40"},
                                            .opening_name = std::string {"King's Knight"}}),
                             });

    auto tree_res = motif::search::opening_tree::open(*manager, root_hash, default_prefetch_depth);
    REQUIRE(tree_res.has_value());

    auto& root = tree_res->root;
    REQUIRE_FALSE(root.continuations.empty());
    auto& nf3_cont = root.continuations[0];
    CHECK(nf3_cont.subtree->is_expanded);

    auto cont_count_before = nf3_cont.subtree->continuations.size();

    auto expand_res = motif::search::opening_tree::expand(*manager, *nf3_cont.subtree);
    REQUIRE(expand_res.has_value());

    CHECK(nf3_cont.subtree->is_expanded);
    CHECK(nf3_cont.subtree->continuations.size() == cont_count_before);
}

TEST_CASE("opening_tree::open with empty root returns empty tree", "[motif-search][opening_tree]")
{
    tmp_dir const tdir {"empty"};

    auto manager = motif::db::database_manager::create(tdir.path, "tree-db");
    REQUIRE(manager.has_value());

    insert_games_and_rebuild(*manager,
                             {
                                 make_game({.sans = {"d4", "Nf6"},
                                            .result = "1-0",
                                            .white_elo = white_elo_high,
                                            .black_elo = black_elo_high,
                                            .eco = std::string {"A06"},
                                            .opening_name = std::string {"Queen's Pawn"}}),
                             });

    auto const missing_hash = hash_after_sans({"c4", "e5", "Nc3"});
    auto tree_res = motif::search::opening_tree::open(*manager, missing_hash, default_prefetch_depth);
    REQUIRE(tree_res.has_value());
    CHECK(tree_res->root.continuations.empty());
    CHECK(tree_res->root.is_expanded);
}

TEST_CASE("opening_tree::open with custom prefetch_depth", "[motif-search][opening_tree]")
{
    tmp_dir const tdir {"custom_depth"};

    auto manager = motif::db::database_manager::create(tdir.path, "tree-db");
    REQUIRE(manager.has_value());

    auto const root_hash = hash_after_sans({"e4"});

    insert_games_and_rebuild(*manager,
                             {
                                 make_game({.sans = {"e4", "e5", "Nf3", "Nc6"},
                                            .result = "1-0",
                                            .white_elo = white_elo_high,
                                            .black_elo = black_elo_high,
                                            .eco = std::string {"C40"},
                                            .opening_name = std::string {"King's Knight"}}),
                             });

    // prefetch_depth=2: e5 at depth1 is expanded with continuations;
    // Nf3 at depth2 is boundary (not expanded)
    auto tree_res = motif::search::opening_tree::open(*manager, root_hash, 2);
    REQUIRE(tree_res.has_value());
    CHECK(tree_res->prefetch_depth == 2);

    auto const& root = tree_res->root;
    REQUIRE_FALSE(root.continuations.empty());
    auto const& e5_continuation = root.continuations[0];
    CHECK(e5_continuation.subtree->is_expanded);
    REQUIRE_FALSE(e5_continuation.subtree->continuations.empty());

    // depth 2 = boundary, so Nf3 subtree should NOT be expanded
    auto const& nf3 = e5_continuation.subtree->continuations[0];
    CHECK_FALSE(nf3.subtree->is_expanded);
}

TEST_CASE("opening_tree::open with zero prefetch leaves root lazy", "[motif-search][opening_tree]")
{
    tmp_dir const tdir {"zero_prefetch"};

    auto manager = motif::db::database_manager::create(tdir.path, "tree-db");
    REQUIRE(manager.has_value());

    auto const root_hash = hash_after_sans({"e4", "e5"});

    insert_games_and_rebuild(*manager,
                             {
                                 make_game({.sans = {"e4", "e5", "Nf3", "Nc6"},
                                            .result = "1-0",
                                            .white_elo = white_elo_high,
                                            .black_elo = black_elo_high,
                                            .eco = std::string {"C40"},
                                            .opening_name = std::string {"King's Knight"}}),
                             });

    auto tree_res = motif::search::opening_tree::open(*manager, root_hash, 0);
    REQUIRE(tree_res.has_value());
    CHECK(tree_res->prefetch_depth == 0);
    CHECK_FALSE(tree_res->root.is_expanded);
    CHECK(tree_res->root.continuations.empty());

    auto expand_res = motif::search::opening_tree::expand(*manager, tree_res->root);
    REQUIRE(expand_res.has_value());
    CHECK(tree_res->root.is_expanded);
    REQUIRE(tree_res->root.continuations.size() == 1);
    CHECK(tree_res->root.continuations[0].san == "Nf3");
}

TEST_CASE("opening_tree::open counts repeated root occurrences in one game", "[motif-search][opening_tree]")
{
    tmp_dir const tdir {"repeated_root"};

    auto manager = motif::db::database_manager::create(tdir.path, "tree-db");
    REQUIRE(manager.has_value());

    auto const root_hash = hash_after_sans({"Nf3", "Nf6"});

    insert_games_and_rebuild(*manager,
                             {
                                 make_game({.sans = {"Nf3", "Nf6", "Ng1", "Ng8", "Nf3", "Nf6", "Ng1", "Nc6"},
                                            .result = "1-0",
                                            .white_elo = white_elo_high,
                                            .black_elo = black_elo_high,
                                            .eco = std::string {"C40"},
                                            .opening_name = std::string {"King's Knight"}}),
                             });

    auto tree_res = motif::search::opening_tree::open(*manager, root_hash, 2);
    REQUIRE(tree_res.has_value());

    auto const& root = tree_res->root;
    REQUIRE(root.continuations.size() == 1);
    CHECK(root.continuations[0].san == "Ng1");
    CHECK(root.continuations[0].frequency == 2);
    REQUIRE(root.continuations[0].subtree != nullptr);
    REQUIRE(root.continuations[0].subtree->continuations.size() == 2);
    CHECK(root.continuations[0].subtree->continuations[0].san == "Nc6");
    CHECK(root.continuations[0].subtree->continuations[0].frequency == 1);
    CHECK(root.continuations[0].subtree->continuations[1].san == "Ng8");
    CHECK(root.continuations[0].subtree->continuations[1].frequency == 1);
}

TEST_CASE("opening_tree::open rejects oversized prefetch_depth", "[motif-search][opening_tree]")
{
    tmp_dir const tdir {"oversized_prefetch"};

    auto manager = motif::db::database_manager::create(tdir.path, "tree-db");
    REQUIRE(manager.has_value());

    auto const root_hash = hash_after_sans({"e4"});
    auto const oversized_depth = static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()) + 1U;

    auto tree_res = motif::search::opening_tree::open(*manager, root_hash, oversized_depth);
    REQUIRE_FALSE(tree_res.has_value());
    CHECK(tree_res.error() == motif::search::error_code::invalid_argument);
}

TEST_CASE("opening_tree::open result_hash matches expected zobrist hash", "[motif-search][opening_tree]")
{
    tmp_dir const tdir {"result_hash"};

    auto manager = motif::db::database_manager::create(tdir.path, "tree-db");
    REQUIRE(manager.has_value());

    insert_games_and_rebuild(*manager,
                             {
                                 make_game({.sans = {"e4", "e5", "Nf3", "Nc6"},
                                            .result = "1-0",
                                            .white_elo = white_elo_high,
                                            .black_elo = black_elo_high,
                                            .eco = std::string {"C40"},
                                            .opening_name = std::string {"King's Knight"}}),
                             });

    auto const root_hash = hash_after_sans({"e4", "e5"});
    auto tree_res = motif::search::opening_tree::open(*manager, root_hash, default_prefetch_depth);
    REQUIRE(tree_res.has_value());

    auto const& root = tree_res->root;
    REQUIRE_FALSE(root.continuations.empty());

    // Only 1 game, only 1 continuation: Nf3
    auto const& nf3 = root.continuations[0];
    CHECK(nf3.san == "Nf3");
    CHECK(nf3.result_hash == hash_after_sans({"e4", "e5", "Nf3"}));
}

TEST_CASE("opening_tree::open from starting position aggregates first moves correctly", "[motif-search][opening_tree]")
{
    tmp_dir const tdir {"starting_position"};

    auto manager = motif::db::database_manager::create(tdir.path, "tree-db");
    REQUIRE(manager.has_value());

    // Three games from the initial position: two 1.e4 games (different outcomes),
    // one 1.d4 game.
    insert_games_and_rebuild(*manager,
                             {
                                 make_game({.sans = {"e4", "e5", "Nf3", "Nc6"},
                                            .result = "1-0",
                                            .white_elo = white_elo_high,
                                            .black_elo = black_elo_high,
                                            .eco = std::string {"C40"},
                                            .opening_name = std::string {"King's Knight Opening"}}),
                                 make_game({.sans = {"e4", "e5", "Nc3", "Nc6"},
                                            .result = "0-1",
                                            .white_elo = white_elo_low,
                                            .black_elo = black_elo_other,
                                            .eco = std::string {"C25"},
                                            .opening_name = std::string {"Vienna Game"}}),
                                 make_game({.sans = {"d4", "d5", "c4"},
                                            .result = "1/2-1/2",
                                            .white_elo = white_elo_high,
                                            .black_elo = black_elo_high,
                                            .eco = std::string {"D06"},
                                            .opening_name = std::string {"Queen's Gambit"}}),
                             });

    auto const starting_hash = hash_after_sans({});
    auto tree_res = motif::search::opening_tree::open(*manager, starting_hash, default_prefetch_depth);
    REQUIRE(tree_res.has_value());

    auto const& root = tree_res->root;
    CHECK(root.zobrist_hash == starting_hash);
    CHECK(root.is_expanded);

    // Continuations are sorted by frequency desc, then SAN asc.
    // e4: 2 games (1 white win + 1 black win); d4: 1 game (1 draw).
    REQUIRE(root.continuations.size() == 2);

    auto const& e4_cont = root.continuations[0];
    CHECK(e4_cont.san == "e4");
    CHECK(e4_cont.frequency == 2);
    CHECK(e4_cont.white_wins == 1);
    CHECK(e4_cont.black_wins == 1);
    CHECK(e4_cont.draws == 0);
    CHECK(e4_cont.result_hash == hash_after_sans({"e4"}));

    auto const& d4_cont = root.continuations[1];
    CHECK(d4_cont.san == "d4");
    CHECK(d4_cont.frequency == 1);
    CHECK(d4_cont.white_wins == 0);
    CHECK(d4_cont.black_wins == 0);
    CHECK(d4_cont.draws == 1);
    CHECK(d4_cont.result_hash == hash_after_sans({"d4"}));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("opening_tree::open performance on sorted position store", "[performance][motif-search][opening_tree]")
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

    auto manager = motif::db::database_manager::create(tdir.path / "db", "opening-tree-perf");
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

    std::vector<double> latencies_us;
    latencies_us.reserve(sample_hashes->size());

    for (auto const hash : *sample_hashes) {
        auto const start = std::chrono::steady_clock::now();
        auto result = motif::search::opening_tree::open(*manager, hash, default_prefetch_depth);
        auto const stop = std::chrono::steady_clock::now();

        REQUIRE(result.has_value());
        latencies_us.push_back(std::chrono::duration<double, std::micro>(stop - start).count());
    }

    std::ranges::sort(latencies_us);

    auto const count = latencies_us.size();
    auto total_ms = 0.0;
    for (auto const latency : latencies_us) {
        total_ms += latency;
    }
    total_ms /= us_per_ms;

    auto const p99_idx = std::min(count - 1, static_cast<std::size_t>(static_cast<double>(count) * 0.99));
    auto const p99_us = count > 0 ? latencies_us[p99_idx] : 0.0;

    std::cout << "\n=== opening_tree::open performance ===\n"
              << "  queries:      " << count << "\n"
              << "  total:        " << total_ms << " ms\n"
              << "  p99:          " << p99_us << " us\n";

    CHECK(p99_us < perf_p99_limit_us);

    auto const shutdown_result = motif::import::shutdown_logging();
    REQUIRE(shutdown_result.has_value());
}
