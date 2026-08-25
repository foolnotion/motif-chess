#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "motif/db/opening_tree_index.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <chesslib/board/board.hpp>
#include <chesslib/board/move_codec.hpp>
#include <chesslib/util/san.hpp>

#include "motif/db/database_manager.hpp"
#include "motif/db/game_store.hpp"
#include "motif/db/types.hpp"

namespace
{

constexpr auto elo_tolerance = 0.001;

struct tmp_dir
{
    std::filesystem::path path;

    explicit tmp_dir(std::string const& suffix)
    {
        auto const base = std::filesystem::temp_directory_path();
        auto const tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = base / ("motif_opening_tree_index_test_" + suffix + "_" + std::to_string(tick));
        std::filesystem::create_directories(path);
    }

    ~tmp_dir() { std::filesystem::remove_all(path); }

    tmp_dir(tmp_dir const&) = delete;
    auto operator=(tmp_dir const&) -> tmp_dir& = delete;
    tmp_dir(tmp_dir&&) = delete;
    auto operator=(tmp_dir&&) -> tmp_dir& = delete;
};

auto hash_after_sans(std::initializer_list<char const*> sans) -> motif::db::zobrist_hash
{
    auto board = chesslib::board {};
    for (char const* const san : sans) {
        auto move = chesslib::san::from_string(board, san);
        REQUIRE(move.has_value());
        chesslib::move_maker maker {board, *move};
        maker.make();
    }
    return motif::db::zobrist_hash {board.hash()};
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
};

auto make_game(game_spec const& spec) -> motif::db::game
{
    static auto next_player_index = std::uint32_t {1};
    auto const player_index = next_player_index++;

    return motif::db::game {
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
        .eco = std::nullopt,
        .moves = encode_moves(spec.sans),
        .extra_tags = {},
        .provenance = {},
    };
}

// Sorts fixture and result rows by cont_encoded_move so query order is not
// part of the contract under test.
void sort_by_move(std::vector<motif::db::opening_stat_agg_row>& rows)
{
    std::ranges::sort(rows, {}, &motif::db::opening_stat_agg_row::cont_encoded_move);
}

void check_row_identity(motif::db::opening_stat_agg_row const& expected, motif::db::opening_stat_agg_row const& actual)
{
    CHECK(expected.cont_encoded_move == actual.cont_encoded_move);
    CHECK(expected.cont_hash == actual.cont_hash);
    CHECK(expected.root_ply == actual.root_ply);
    CHECK(expected.eco_sample_min == actual.eco_sample_min);
    CHECK(expected.eco_sample_max == actual.eco_sample_max);
}

void check_row_frequencies(motif::db::opening_stat_agg_row const& expected, motif::db::opening_stat_agg_row const& actual)
{
    CHECK(expected.frequency == actual.frequency);
    CHECK(expected.transposition_frequency == actual.transposition_frequency);
    CHECK(expected.white_wins == actual.white_wins);
    CHECK(expected.draws == actual.draws);
    CHECK(expected.black_wins == actual.black_wins);
}

void check_optional_close(std::optional<double> const& expected, std::optional<double> const& actual)
{
    REQUIRE(expected.has_value() == actual.has_value());
    if (expected.has_value()) {
        CHECK_THAT(actual.value_or(0.0), Catch::Matchers::WithinRel(expected.value_or(0.0), elo_tolerance));
    }
}

void check_row_elo_fields(motif::db::opening_stat_agg_row const& expected, motif::db::opening_stat_agg_row const& actual)
{
    check_optional_close(expected.avg_white_elo, actual.avg_white_elo);
    check_optional_close(expected.avg_black_elo, actual.avg_black_elo);
    check_optional_close(expected.elo_weighted_score, actual.elo_weighted_score);
}

void check_rows_equal(motif::db::opening_stat_agg_row const& expected, motif::db::opening_stat_agg_row const& actual)
{
    check_row_identity(expected, actual);
    check_row_frequencies(expected, actual);
    check_row_elo_fields(expected, actual);
}

// Checks the single deduped edge produced by the "dedup" fixture below:
// game A takes it once, game B twice within the same game (deduped to once).
// NOLINTBEGIN(bugprone-easily-swappable-parameters, readability-function-cognitive-complexity) -- swappable: caller passes these as named
// locals at each call site, not literals. complexity: Catch2's CHECK/REQUIRE macros inflate the reported complexity of any function with
// several of them.
void check_dedup_edge(motif::db::opening_stat_agg_row const& edge,
                      motif::db::zobrist_hash const h_hash,
                      std::int32_t const white_elo_a,
                      std::int32_t const white_elo_b,
                      std::int32_t const black_elo_a)
{
    CHECK(edge.cont_hash == h_hash);
    CHECK(edge.root_ply == 0);
    // 1 from game A + 1 (deduped) from game B, not 3.
    CHECK(edge.frequency == 2);
    CHECK(edge.white_wins == 1);
    CHECK(edge.black_wins == 1);
    CHECK(edge.draws == 0);
    // H is visited by both games (deduped per game: game B visits it twice
    // but counts once), so transposition_frequency == 2 here too.
    CHECK(edge.transposition_frequency == 2);

    constexpr auto elo_average_divisor = 2.0;
    auto const expected_avg_white_elo = (white_elo_a + white_elo_b) / elo_average_divisor;
    check_optional_close(std::optional<double> {expected_avg_white_elo}, edge.avg_white_elo);
    check_optional_close(std::optional<double> {static_cast<double>(black_elo_a)}, edge.avg_black_elo);
}

// NOLINTEND(bugprone-easily-swappable-parameters, readability-function-cognitive-complexity)

void check_stats_for_hash(motif::db::opening_tree_index const& index,
                          motif::db::zobrist_hash hash,
                          std::vector<motif::db::opening_stat_agg_row> expected)
{
    auto actual = index.query_opening_stats(hash);
    REQUIRE(actual.has_value());
    sort_by_move(expected);
    sort_by_move(*actual);
    REQUIRE(expected.size() == actual->size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        check_rows_equal(expected[i], (*actual)[i]);
    }
}

}  // namespace

// Expected aggregate literals are the independent oracle contract; naming each
// value would obscure the field-by-field fixtures.
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate the reported complexity of test bodies.
TEST_CASE("opening_tree_index::build dedupes a within-game repeated edge and aggregates across games", "[motif-db][opening_tree_index]")
{
    tmp_dir const tdir {"dedup"};

    auto manager = motif::db::database_manager::create_scratch();
    REQUIRE(manager.has_value());

    // Game A: a single ply "Nf3" -- the shallowest possible edge into H
    // (position after 1.Nf3, black to move).
    constexpr auto white_elo_a = std::int32_t {2000};
    constexpr auto black_elo_a = std::int32_t {1900};
    auto inserted_a =
        manager->insert_game(make_game({.sans = {"Nf3"}, .result = "1-0", .white_elo = white_elo_a, .black_elo = black_elo_a}));
    REQUIRE(inserted_a.has_value());

    // Game B: both knights shuffle out and home (Nf3 Nc6 Ng1 Nb8), landing
    // back on the exact starting position at ply 4, then plays "Nf3" again,
    // reaching H a second time. The edge (start, Nf3, H) therefore occurs
    // twice within this one game (root_ply 0 and root_ply 4) and must be
    // counted once, with root_ply = 0 (the minimum).
    constexpr auto white_elo_b = std::int32_t {2200};
    auto inserted_b = manager->insert_game(
        make_game({.sans = {"Nf3", "Nc6", "Ng1", "Nb8", "Nf3"}, .result = "0-1", .white_elo = white_elo_b, .black_elo = std::nullopt}));
    REQUIRE(inserted_b.has_value());

    // Sanity: the shuffle really does return to the start position at ply 4,
    // and both games really do reach the same hash H at their respective
    // final ply. If either of these fail, the test's premise is broken, not
    // the code under test.
    REQUIRE(hash_after_sans({"Nf3", "Nc6", "Ng1", "Nb8"}) == hash_after_sans({}));
    REQUIRE(hash_after_sans({"Nf3"}) == hash_after_sans({"Nf3", "Nc6", "Ng1", "Nb8", "Nf3"}));

    auto const index_path = tdir.path / "opening_tree.idx";
    auto build_res = motif::db::opening_tree_index::build(manager->store(), index_path);
    REQUIRE(build_res.has_value());

    auto opened = motif::db::opening_tree_index::open(index_path);
    REQUIRE(opened.has_value());

    auto const start_hash = hash_after_sans({});
    auto const h_hash = hash_after_sans({"Nf3"});

    auto stats = opened->query_opening_stats(start_hash);
    REQUIRE(stats.has_value());
    REQUIRE(stats->size() == 1);
    REQUIRE(opened->game_count(start_hash).has_value());
    CHECK(*opened->game_count(start_hash) == 2);
    REQUIRE(opened->game_count(h_hash).has_value());
    CHECK(*opened->game_count(h_hash) == 2);

    check_dedup_edge(stats->front(), h_hash, white_elo_a, white_elo_b, black_elo_a);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate the reported complexity of test bodies.
TEST_CASE("opening_tree_index::open is stable across repeated reads of the same file", "[motif-db][opening_tree_index]")
{
    tmp_dir const tdir {"roundtrip"};

    auto manager = motif::db::database_manager::create_scratch();
    REQUIRE(manager.has_value());
    auto inserted =
        manager->insert_game(make_game({.sans = {"e4", "e5"}, .result = "1/2-1/2", .white_elo = std::nullopt, .black_elo = std::nullopt}));
    REQUIRE(inserted.has_value());

    auto const index_path = tdir.path / "opening_tree.idx";
    auto build_res = motif::db::opening_tree_index::build(manager->store(), index_path);
    REQUIRE(build_res.has_value());

    auto opened_first = motif::db::opening_tree_index::open(index_path);
    REQUIRE(opened_first.has_value());
    auto opened_second = motif::db::opening_tree_index::open(index_path);
    REQUIRE(opened_second.has_value());

    auto const start_hash = hash_after_sans({});
    auto first_stats = opened_first->query_opening_stats(start_hash);
    auto second_stats = opened_second->query_opening_stats(start_hash);
    REQUIRE(first_stats.has_value());
    REQUIRE(second_stats.has_value());
    REQUIRE(first_stats->size() == second_stats->size());
    for (std::size_t i = 0; i < first_stats->size(); ++i) {
        check_rows_equal((*first_stats)[i], (*second_stats)[i]);
    }

    auto const terminal_hash = hash_after_sans({"e4", "e5"});
    auto terminal_stats = opened_first->query_opening_stats(terminal_hash);
    REQUIRE(terminal_stats.has_value());
    CHECK(terminal_stats->empty());
    REQUIRE(opened_first->game_count(terminal_hash).has_value());
    CHECK(*opened_first->game_count(terminal_hash) == 1);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate the reported complexity of test bodies.
TEST_CASE("opening_tree_index::query_opening_stats matches deterministic aggregate fixtures", "[motif-db][opening_tree_index]")
{
    tmp_dir const tdir {"equivalence"};

    auto manager = motif::db::database_manager::create(tdir.path / "db", "opening-tree-index-equivalence");
    REQUIRE(manager.has_value());

    constexpr auto white_elo_high = std::int32_t {2500};
    constexpr auto black_elo_high = std::int32_t {2400};
    constexpr auto white_elo_mid = std::int32_t {2300};
    constexpr auto black_elo_other = std::int32_t {2200};

    for (auto const& game :
         {
             make_game({.sans = {"e4", "e5", "Nf3", "Nc6"}, .result = "1-0", .white_elo = white_elo_high, .black_elo = black_elo_high}),
             make_game({.sans = {"e4", "e5", "Nf3", "d6"}, .result = "1/2-1/2", .white_elo = white_elo_mid, .black_elo = std::nullopt}),
             make_game({.sans = {"e4", "e5", "Nc3", "Nc6"}, .result = "0-1", .white_elo = std::nullopt, .black_elo = black_elo_other}),
         })
    {
        auto inserted = manager->insert_game(game);
        REQUIRE(inserted.has_value());
    }

    auto const index_path = tdir.path / "opening_tree.idx";
    auto build_res = motif::db::opening_tree_index::build(manager->store(), index_path);
    REQUIRE(build_res.has_value());
    auto opened = motif::db::opening_tree_index::open(index_path);
    REQUIRE(opened.has_value());

    auto const e4_move = encode_moves({"e4"}).back();
    auto const nf3_move = encode_moves({"e4", "e5", "Nf3"}).back();
    auto const nc3_move = encode_moves({"e4", "e5", "Nc3"}).back();
    check_stats_for_hash(*opened,
                         hash_after_sans({}),
                         {{.cont_encoded_move = e4_move,
                           .cont_hash = hash_after_sans({"e4"}),
                           .root_ply = 0,
                           .frequency = 3,
                           .transposition_frequency = 3,
                           .white_wins = 1,
                           .draws = 1,
                           .black_wins = 1,
                           .avg_white_elo = 2400.0,
                           .avg_black_elo = 2300.0,
                           .eco_sample_min = motif::db::game_id {1},
                           .eco_sample_max = motif::db::game_id {3},
                           .elo_weighted_score = 1.0}});
    check_stats_for_hash(*opened,
                         hash_after_sans({"e4", "e5"}),
                         {{.cont_encoded_move = nf3_move,
                           .cont_hash = hash_after_sans({"e4", "e5", "Nf3"}),
                           .root_ply = 2,
                           .frequency = 2,
                           .transposition_frequency = 2,
                           .white_wins = 1,
                           .draws = 1,
                           .black_wins = 0,
                           .avg_white_elo = 2400.0,
                           .avg_black_elo = 2400.0,
                           .eco_sample_min = motif::db::game_id {1},
                           .eco_sample_max = motif::db::game_id {2},
                           .elo_weighted_score = 1.0},
                          {.cont_encoded_move = nc3_move,
                           .cont_hash = hash_after_sans({"e4", "e5", "Nc3"}),
                           .root_ply = 2,
                           .frequency = 1,
                           .transposition_frequency = 1,
                           .white_wins = 0,
                           .draws = 0,
                           .black_wins = 1,
                           .avg_white_elo = std::nullopt,
                           .avg_black_elo = 2200.0,
                           .eco_sample_min = motif::db::game_id {3},
                           .eco_sample_max = motif::db::game_id {3},
                           .elo_weighted_score = std::nullopt}});
}

TEST_CASE("opening_tree_index::build preserves aggregates across external spill runs", "[motif-db][opening_tree_index]")
{
    tmp_dir const tdir {"external-spill"};
    auto manager = motif::db::database_manager::create_scratch();
    REQUIRE(manager.has_value());

    for (auto const& spec : {game_spec {.sans = {"e4", "e5", "Nf3"}, .result = "1-0", .white_elo = 2500, .black_elo = 2400},
                             game_spec {.sans = {"e4", "c5", "Nf3"}, .result = "0-1", .white_elo = 2300, .black_elo = 2200},
                             game_spec {.sans = {"d4", "d5", "c4"}, .result = "1/2-1/2", .white_elo = 2100, .black_elo = 2000},
                             game_spec {.sans = {"e4", "e5", "Nc3"}, .result = "1-0", .white_elo = 2400, .black_elo = 2300}})
    {
        REQUIRE(manager->insert_game(make_game(spec)).has_value());
    }
    auto const index_path = tdir.path / "opening_tree.idx";
    auto const options = motif::db::opening_tree_index::build_options {.max_root_ply = 20U, .spill_threshold = 2U};
    REQUIRE(motif::db::opening_tree_index::build(manager->store(), index_path, options).has_value());
    auto index = motif::db::opening_tree_index::open(index_path);
    REQUIRE(index.has_value());

    check_stats_for_hash(*index,
                         hash_after_sans({}),
                         {{.cont_encoded_move = encode_moves({"e4"}).back(),
                           .cont_hash = hash_after_sans({"e4"}),
                           .root_ply = 0,
                           .frequency = 3,
                           .transposition_frequency = 3,
                           .white_wins = 2,
                           .draws = 0,
                           .black_wins = 1,
                           .avg_white_elo = 2400.0,
                           .avg_black_elo = 2300.0,
                           .eco_sample_min = motif::db::game_id {1},
                           .eco_sample_max = motif::db::game_id {4},
                           .elo_weighted_score = 2550.0 / 7050.0},
                          {.cont_encoded_move = encode_moves({"d4"}).back(),
                           .cont_hash = hash_after_sans({"d4"}),
                           .root_ply = 0,
                           .frequency = 1,
                           .transposition_frequency = 1,
                           .white_wins = 0,
                           .draws = 1,
                           .black_wins = 0,
                           .avg_white_elo = 2100.0,
                           .avg_black_elo = 2000.0,
                           .eco_sample_min = motif::db::game_id {3},
                           .eco_sample_max = motif::db::game_id {3},
                           .elo_weighted_score = 0.0}});
    check_stats_for_hash(*index,
                         hash_after_sans({"e4"}),
                         {{.cont_encoded_move = encode_moves({"e4", "e5"}).back(),
                           .cont_hash = hash_after_sans({"e4", "e5"}),
                           .root_ply = 1,
                           .frequency = 2,
                           .transposition_frequency = 2,
                           .white_wins = 2,
                           .draws = 0,
                           .black_wins = 0,
                           .avg_white_elo = 2450.0,
                           .avg_black_elo = 2350.0,
                           .eco_sample_min = motif::db::game_id {1},
                           .eco_sample_max = motif::db::game_id {4},
                           .elo_weighted_score = 1.0},
                          {.cont_encoded_move = encode_moves({"e4", "c5"}).back(),
                           .cont_hash = hash_after_sans({"e4", "c5"}),
                           .root_ply = 1,
                           .frequency = 1,
                           .transposition_frequency = 1,
                           .white_wins = 0,
                           .draws = 0,
                           .black_wins = 1,
                           .avg_white_elo = 2300.0,
                           .avg_black_elo = 2200.0,
                           .eco_sample_min = motif::db::game_id {2},
                           .eco_sample_max = motif::db::game_id {2},
                           .elo_weighted_score = -1.0}});
    check_stats_for_hash(*index,
                         hash_after_sans({"d4"}),
                         {{.cont_encoded_move = encode_moves({"d4", "d5"}).back(),
                           .cont_hash = hash_after_sans({"d4", "d5"}),
                           .root_ply = 1,
                           .frequency = 1,
                           .transposition_frequency = 1,
                           .white_wins = 0,
                           .draws = 1,
                           .black_wins = 0,
                           .avg_white_elo = 2100.0,
                           .avg_black_elo = 2000.0,
                           .eco_sample_min = motif::db::game_id {3},
                           .eco_sample_max = motif::db::game_id {3},
                           .elo_weighted_score = 0.0}});
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate the reported complexity of test bodies.
TEST_CASE("database_manager::rebuild_opening_tree_index reuses exact postings child counts", "[motif-db][opening_tree_index]")
{
    tmp_dir const tdir {"postings-child-counts"};
    auto manager = motif::db::database_manager::create(tdir.path / "db", "postings-child-counts");
    REQUIRE(manager.has_value());

    REQUIRE(manager->insert_game(make_game({.sans = {"Nf3"}, .result = "1-0", .white_elo = 2200, .black_elo = 2100})).has_value());
    REQUIRE(
        manager
            ->insert_game(make_game({.sans = {"Nf3", "Nc6", "Ng1", "Nb8", "Nf3"}, .result = "0-1", .white_elo = 2000, .black_elo = 1900}))
            .has_value());
    REQUIRE(manager->rebuild_position_postings().has_value());

    auto metrics = motif::db::opening_tree_index_build_metrics {};
    REQUIRE(manager->rebuild_opening_tree_index(&metrics).has_value());
    CHECK(metrics.child_counts_external);
    CHECK(metrics.child_visit_count == 0U);
    CHECK(metrics.child_spill_run_count == 0U);

    REQUIRE(manager->manifest().opening_tree_index.has_value());
    auto manager_index = motif::db::opening_tree_index::open(
        manager->dir() / manager->manifest().opening_tree_index.value_or(motif::db::derived_index_manifest_entry {}).filename);
    REQUIRE(manager_index.has_value());

    check_stats_for_hash(*manager_index,
                         hash_after_sans({}),
                         {{.cont_encoded_move = encode_moves({"Nf3"}).back(),
                           .cont_hash = hash_after_sans({"Nf3"}),
                           .root_ply = 0,
                           .frequency = 2,
                           .transposition_frequency = 2,
                           .white_wins = 1,
                           .draws = 0,
                           .black_wins = 1,
                           .avg_white_elo = 2100.0,
                           .avg_black_elo = 2000.0,
                           .eco_sample_min = motif::db::game_id {1},
                           .eco_sample_max = motif::db::game_id {2},
                           .elo_weighted_score = 200.0 / 4100.0}});
}

TEST_CASE("database_manager::rebuild_opening_tree_index rejects a postings game-count mismatch", "[motif-db][opening_tree_index]")
{
    tmp_dir const tdir {"stale-postings"};
    auto manager = motif::db::database_manager::create(tdir.path / "db", "stale-postings");
    REQUIRE(manager.has_value());
    REQUIRE(manager->insert_game(make_game({.sans = {"e4"}, .result = "1-0", .white_elo = 2200, .black_elo = 2100})).has_value());
    REQUIRE(manager->rebuild_position_postings().has_value());

    REQUIRE(manager->insert_game(make_game({.sans = {"d4"}, .result = "0-1", .white_elo = 2000, .black_elo = 1900})).has_value());
    auto const rebuilt = manager->rebuild_opening_tree_index();
    REQUIRE_FALSE(rebuilt.has_value());
    CHECK(rebuilt.error() == motif::db::error_code::invalid_argument);
    CHECK_FALSE(manager->manifest().opening_tree_index.has_value());
}

TEST_CASE("database_manager::rebuild_opening_tree_index rejects postings invalidated before a canonical mutation",
          "[motif-db][opening_tree_index]")
{
    tmp_dir const tdir {"invalidated-postings"};
    auto manager = motif::db::database_manager::create(tdir.path / "db", "invalidated-postings");
    REQUIRE(manager.has_value());
    REQUIRE(manager->insert_game(make_game({.sans = {"e4"}, .result = "1-0", .white_elo = 2200, .black_elo = 2100})).has_value());
    REQUIRE(manager->rebuild_position_postings().has_value());

    REQUIRE(manager->prepare_canonical_mutation().has_value());
    REQUIRE(manager->insert_game(make_game({.sans = {"d4"}, .result = "0-1", .white_elo = 2000, .black_elo = 1900})).has_value());
    CHECK_FALSE(manager->manifest().position_postings.has_value());
    auto const rebuilt = manager->rebuild_opening_tree_index();
    REQUIRE_FALSE(rebuilt.has_value());
    CHECK(rebuilt.error() == motif::db::error_code::invalid_argument);
}

TEST_CASE("opening_tree_index::build rejects an external child-count stream missing a referenced hash", "[motif-db][opening_tree_index]")
{
    tmp_dir const tdir {"missing-external-child"};
    auto manager = motif::db::database_manager::create_scratch();
    REQUIRE(manager.has_value());
    REQUIRE(manager->insert_game(make_game({.sans = {"e4"}, .result = "1-0", .white_elo = 2200, .black_elo = 2100})).has_value());

    auto stream = motif::db::opening_tree_child_count_stream {
        [](motif::db::opening_tree_child_count_visitor const& visitor) -> motif::db::result<void>
        { return visitor(hash_after_sans({}), 1U); }};
    auto const built =
        motif::db::opening_tree_index::build(manager->store(), tdir.path / "missing-child.idx", {}, nullptr, std::move(stream));
    REQUIRE_FALSE(built.has_value());
    CHECK(built.error() == motif::db::error_code::io_failure);
}

TEST_CASE("opening_tree_index::build rejects a nonascending external child-count stream", "[motif-db][opening_tree_index]")
{
    tmp_dir const tdir {"nonascending-external-counts"};
    auto manager = motif::db::database_manager::create_scratch();
    REQUIRE(manager.has_value());

    auto stream = motif::db::opening_tree_child_count_stream {
        [](motif::db::opening_tree_child_count_visitor const& visitor) -> motif::db::result<void>
        {
            if (auto first = visitor(motif::db::zobrist_hash {2U}, 1U); !first) {
                return first;
            }
            return visitor(motif::db::zobrist_hash {1U}, 1U);
        }};
    auto const built =
        motif::db::opening_tree_index::build(manager->store(), tdir.path / "nonascending.idx", {}, nullptr, std::move(stream));
    REQUIRE_FALSE(built.has_value());
    CHECK(built.error() == motif::db::error_code::io_failure);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertions and the deliberately malformed stream inflate this test.
TEST_CASE("opening_tree_index::build rejects an external child count below direct edge frequency", "[motif-db][opening_tree_index]")
{
    tmp_dir const tdir {"external-count-too-small"};
    auto manager = motif::db::database_manager::create_scratch();
    REQUIRE(manager.has_value());
    REQUIRE(manager->insert_game(make_game({.sans = {"e4"}, .result = "1-0", .white_elo = 2200, .black_elo = 2100})).has_value());
    REQUIRE(manager->insert_game(make_game({.sans = {"e4"}, .result = "0-1", .white_elo = 2000, .black_elo = 1900})).has_value());

    auto const start_hash = hash_after_sans({});
    auto const e4_hash = hash_after_sans({"e4"});
    auto stream = motif::db::opening_tree_child_count_stream {
        [start_hash, e4_hash](motif::db::opening_tree_child_count_visitor const& visitor) -> motif::db::result<void>
        {
            auto const first_hash = std::min(start_hash, e4_hash);
            auto const second_hash = std::max(start_hash, e4_hash);
            if (auto first = visitor(first_hash, first_hash == e4_hash ? 1U : 2U); !first) {
                return first;
            }
            return visitor(second_hash, second_hash == e4_hash ? 1U : 2U);
        }};
    auto const built =
        motif::db::opening_tree_index::build(manager->store(), tdir.path / "count-too-small.idx", {}, nullptr, std::move(stream));
    REQUIRE_FALSE(built.has_value());
    CHECK(built.error() == motif::db::error_code::io_failure);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertions and the callback loop inflate this test.
TEST_CASE("opening_tree_index::build resolves external child counts through the disk fallback", "[motif-db][opening_tree_index]")
{
    tmp_dir const tdir {"external-count-disk-fallback"};
    auto manager = motif::db::database_manager::create_scratch();
    REQUIRE(manager.has_value());
    REQUIRE(manager->insert_game(make_game({.sans = {"e4", "e5"}, .result = "1-0", .white_elo = 2200, .black_elo = 2100})).has_value());

    auto hashes = std::vector<motif::db::zobrist_hash> {hash_after_sans({}), hash_after_sans({"e4"}), hash_after_sans({"e4", "e5"})};
    std::ranges::sort(hashes);
    auto stream = motif::db::opening_tree_child_count_stream {
        [hashes](motif::db::opening_tree_child_count_visitor const& visitor) -> motif::db::result<void>
        {
            for (auto const hash : hashes) {
                if (auto result = visitor(hash, 1U); !result) {
                    return result;
                }
            }
            return {};
        }};
    auto const options = motif::db::opening_tree_index::build_options {
        .max_root_ply = 20U,
        .spill_threshold = motif::db::opening_tree_index_default_spill_threshold,
        .child_frequency_memory_limit_bytes = 0U,
    };
    auto metrics = motif::db::opening_tree_index_build_metrics {};
    auto const built =
        motif::db::opening_tree_index::build(manager->store(), tdir.path / "disk-fallback.idx", options, &metrics, std::move(stream));
    REQUIRE(built.has_value());
    CHECK(metrics.child_counts_external);
    CHECK(metrics.child_frequency_lookup_count > 0U);
    CHECK(metrics.child_frequency_loaded_count == 0U);
}

TEST_CASE("opening_tree_index_builder latches an accumulate failure", "[motif-db][opening_tree_index]")
{
    tmp_dir const tdir {"latched-accumulate-failure"};
    auto const index_path = tdir.path / "missing" / "opening_tree.idx";
    auto const options = motif::db::opening_tree_index::build_options {.max_root_ply = 20U, .spill_threshold = 1U};
    auto builder = motif::db::opening_tree_index_builder {index_path, options};
    auto const game = motif::db::replay_game_record {
        .id = motif::db::game_id {1U},
        .result = "1-0",
        .white_elo = 2200,
        .black_elo = 2100,
        .moves = encode_moves({"e4"}),
    };

    auto const first = builder.accumulate(game);
    REQUIRE_FALSE(first.has_value());
    auto const second = builder.accumulate(game);
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error() == first.error().code);
    auto const finalized = builder.finalize();
    REQUIRE_FALSE(finalized.has_value());
    CHECK(finalized.error() == first.error().code);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate the reported complexity of test bodies.
TEST_CASE("opening_tree_index marks the starting position complete and aggregates edges taken after max_root_ply",
          "[motif-db][opening_tree_index]")
{
    tmp_dir const tdir {"full-coverage-start"};

    auto manager = motif::db::database_manager::create_scratch();
    REQUIRE(manager.has_value());

    // Six repetitions of a fully-reversible knight shuffle (24 plies) return
    // to the exact starting position, then "e4" is played as the 25th
    // half-move -- root_ply 24, past the default max_root_ply of 20. A
    // build that still caps the starting position's edges at max_root_ply
    // would never see this "e4" edge.
    auto const shuffle_and_break_out =
        make_game({.sans = {"Nf3", "Nc6", "Ng1", "Nb8", "Nf3", "Nc6", "Ng1", "Nb8", "Nf3", "Nc6", "Ng1", "Nb8", "Nf3",
                            "Nc6", "Ng1", "Nb8", "Nf3", "Nc6", "Ng1", "Nb8", "Nf3", "Nc6", "Ng1", "Nb8", "e4"},
                   .result = "1-0",
                   .white_elo = 2200,
                   .black_elo = 2100});
    REQUIRE(manager->insert_game(shuffle_and_break_out).has_value());

    // A second, ordinary game reaching H = 1.Nf3 at root_ply 0, so the
    // starting position also has a normal shallow edge to compare against.
    REQUIRE(manager->insert_game(make_game({.sans = {"Nf3"}, .result = "0-1", .white_elo = 2000, .black_elo = 1900})).has_value());

    // Sanity: the shuffle really does return to the start position after 24
    // plies, so the "e4" move played next really is root_ply 24. If this
    // fails, the test's premise is broken, not the code under test.
    REQUIRE(hash_after_sans({"Nf3", "Nc6", "Ng1", "Nb8", "Nf3", "Nc6", "Ng1", "Nb8", "Nf3", "Nc6", "Ng1", "Nb8",
                             "Nf3", "Nc6", "Ng1", "Nb8", "Nf3", "Nc6", "Ng1", "Nb8", "Nf3", "Nc6", "Ng1", "Nb8"})
            == hash_after_sans({}));

    auto const index_path = tdir.path / "opening_tree.idx";
    REQUIRE(motif::db::opening_tree_index::build(manager->store(), index_path).has_value());
    auto opened = motif::db::opening_tree_index::open(index_path);
    REQUIRE(opened.has_value());

    auto const start_hash = hash_after_sans({});
    auto const deep_hash = hash_after_sans({"Nf3"});

    CHECK(opened->is_complete(start_hash));
    // A node other than the canonical starting position must not be marked
    // complete -- only the starting position gets full-depth aggregation.
    CHECK_FALSE(opened->is_complete(deep_hash));

    REQUIRE(opened->game_count(start_hash).has_value());
    CHECK(*opened->game_count(start_hash) == 2);

    auto stats = opened->query_opening_stats(start_hash);
    REQUIRE(stats.has_value());
    REQUIRE(stats->size() == 2);

    auto const e4_hash = hash_after_sans({"e4"});
    auto const e4_it = std::ranges::find(*stats, e4_hash, &motif::db::opening_stat_agg_row::cont_hash);
    REQUIRE(e4_it != stats->end());
    CHECK(e4_it->root_ply == 24);
    CHECK(e4_it->frequency == 1);

    check_stats_for_hash(*opened,
                         start_hash,
                         {{.cont_encoded_move = encode_moves({"Nf3"}).back(),
                           .cont_hash = deep_hash,
                           .root_ply = 0,
                           .frequency = 2,
                           .transposition_frequency = 2,
                           .white_wins = 1,
                           .draws = 0,
                           .black_wins = 1,
                           .avg_white_elo = 2100.0,
                           .avg_black_elo = 2000.0,
                           .eco_sample_min = motif::db::game_id {1},
                           .eco_sample_max = motif::db::game_id {2},
                           .elo_weighted_score = 200.0 / 4100.0},
                          {.cont_encoded_move = encode_moves({"e4"}).back(),
                           .cont_hash = e4_hash,
                           .root_ply = 24,
                           .frequency = 1,
                           .transposition_frequency = 1,
                           .white_wins = 1,
                           .draws = 0,
                           .black_wins = 0,
                           .avg_white_elo = 2200.0,
                           .avg_black_elo = 2100.0,
                           .eco_sample_min = motif::db::game_id {1},
                           .eco_sample_max = motif::db::game_id {1},
                           .elo_weighted_score = 1.0}});
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

namespace
{

constexpr auto byte_mask = std::uint32_t {0xFFU};
constexpr auto bits_per_byte = std::uint32_t {8U};
constexpr auto bits_per_u32 = std::uint32_t {32U};
constexpr auto bits_per_u64 = std::uint32_t {64U};

void write_u16_le(std::ofstream& out, std::uint16_t const value)
{
    out.put(static_cast<char>(value & byte_mask));
    out.put(static_cast<char>((static_cast<std::uint32_t>(value) >> bits_per_byte) & byte_mask));
}

void write_u32_le(std::ofstream& out, std::uint32_t const value)
{
    for (std::uint32_t shift = 0; shift < bits_per_u32; shift += bits_per_byte) {
        out.put(static_cast<char>((value >> shift) & byte_mask));
    }
}

void write_u64_le(std::ofstream& out, std::uint64_t const value)
{
    for (std::uint32_t shift = 0; shift < bits_per_u64; shift += bits_per_byte) {
        out.put(static_cast<char>((value >> shift) & byte_mask));
    }
}

constexpr auto test_format_version = std::uint32_t {5};
constexpr auto test_max_root_ply = std::uint16_t {20};
constexpr auto arbitrary_hash_a = std::uint64_t {0xAAAAAAAAAAAAAAAAULL};
constexpr auto arbitrary_hash_b = std::uint64_t {0xBBBBBBBBBBBBBBBBULL};
constexpr auto arbitrary_encoded_move = std::uint16_t {0x1234};
constexpr auto varint_continuation_byte = static_cast<char>(0xFF);
constexpr auto malformed_varint_prefix_bytes = 9;
constexpr auto single_continuation_varint_byte = static_cast<char>(0x01);
constexpr auto overflowing_varint_final_byte = static_cast<char>(0x02);

}  // namespace

TEST_CASE("opening_tree_index::open rejects a corrupt node_count instead of throwing", "[motif-db][opening_tree_index]")
{
    tmp_dir const tdir {"corrupt-node-count"};
    auto const index_path = tdir.path / "corrupt.idx";

    {
        std::ofstream out {index_path, std::ios::binary | std::ios::trunc};
        REQUIRE(out.good());
        out << "MOTIFOT1";  // magic
        write_u32_le(out, test_format_version);
        write_u16_le(out, test_max_root_ply);
        write_u64_le(out, 1);  // source_game_count
        // node_count: an absurd value that would previously be handed
        // straight to vector::reserve() before any node bytes are read.
        write_u64_le(out, std::numeric_limits<std::uint64_t>::max());
        // No node bytes follow -- the file is truncated relative to what
        // node_count claims.
    }

    auto opened = motif::db::opening_tree_index::open(index_path);
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error() == motif::db::error_code::io_failure);
}

TEST_CASE("opening_tree_index::open rejects a malformed overflowing varint instead of truncating it", "[motif-db][opening_tree_index]")
{
    tmp_dir const tdir {"corrupt-varint"};
    auto const index_path = tdir.path / "corrupt.idx";

    {
        std::ofstream out {index_path, std::ios::binary | std::ios::trunc};
        REQUIRE(out.good());
        out << "MOTIFOT1";
        write_u32_le(out, test_format_version);
        write_u16_le(out, test_max_root_ply);
        write_u64_le(out, 1);  // source_game_count
        write_u64_le(out, 1);  // node_count: one node

        write_u64_le(out, arbitrary_hash_a);  // node hash
        out.put(single_continuation_varint_byte);  // game_count varint: 1
        out.put(single_continuation_varint_byte);  // continuation_count varint: 1
        out.put(static_cast<char>(0x00));  // complete varint: false

        write_u16_le(out, arbitrary_encoded_move);
        write_u64_le(out, arbitrary_hash_b);  // child_hash

        // root_ply as a malformed 10-byte varint: nine continuation bytes
        // with all payload bits set, then a 10th byte whose payload has bit
        // 1 set -- only bit 0 fits in a 64-bit value at this shift, so this
        // encodes a value that overflows u64 and must be rejected rather
        // than silently truncated.
        for (int i = 0; i < malformed_varint_prefix_bytes; ++i) {
            out.put(varint_continuation_byte);
        }
        out.put(overflowing_varint_final_byte);
    }

    auto opened = motif::db::opening_tree_index::open(index_path);
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error() == motif::db::error_code::io_failure);
}

TEST_CASE("opening_tree_index::open rejects a complete flag on a non-starting node", "[motif-db][opening_tree_index]")
{
    tmp_dir const tdir {"invalid-complete-node"};
    auto const index_path = tdir.path / "invalid-complete.idx";

    {
        std::ofstream out {index_path, std::ios::binary | std::ios::trunc};
        REQUIRE(out.good());
        out << "MOTIFOT1";
        write_u32_le(out, test_format_version);
        write_u16_le(out, test_max_root_ply);
        write_u64_le(out, 1);  // source_game_count
        write_u64_le(out, 1);  // node_count
        write_u64_le(out, arbitrary_hash_a);
        out.put(single_continuation_varint_byte);  // game_count: 1
        out.put(static_cast<char>(0x00));  // continuation_count: 0
        out.put(single_continuation_varint_byte);  // complete: true
    }

    auto opened = motif::db::opening_tree_index::open(index_path);
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error() == motif::db::error_code::io_failure);
}

TEST_CASE("opening_tree_index::open rejects trailing bytes after a valid index", "[motif-db][opening_tree_index]")
{
    tmp_dir const tdir {"trailing-data"};
    auto manager = motif::db::database_manager::create_scratch();
    REQUIRE(manager.has_value());
    REQUIRE(manager->insert_game(make_game({.sans = {"e4"}, .result = "1-0", .white_elo = 2000, .black_elo = 1900})).has_value());

    auto const index_path = tdir.path / "opening_tree.idx";
    REQUIRE(motif::db::opening_tree_index::build(manager->store(), index_path).has_value());
    {
        std::ofstream output {index_path, std::ios::binary | std::ios::app};
        REQUIRE(output.good());
        output.put('\0');
    }

    auto const opened = motif::db::opening_tree_index::open(index_path);
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error() == motif::db::error_code::io_failure);
}

TEST_CASE("opening_tree_index::build writes without continuation staging files", "[motif-db][opening_tree_index]")
{
    tmp_dir const tdir {"continuation-staging"};
    auto manager = motif::db::database_manager::create_scratch();
    REQUIRE(manager.has_value());
    REQUIRE(
        manager->insert_game(make_game({.sans = {"e4", "e5", "Nf3"}, .result = "1-0", .white_elo = 2000, .black_elo = 1900})).has_value());

    auto const index_path = tdir.path / "opening_tree.idx";
    REQUIRE(motif::db::opening_tree_index::build(manager->store(), index_path).has_value());
    CHECK(std::filesystem::exists(index_path));
    CHECK_FALSE(std::filesystem::exists(index_path.string() + ".continuations.tmp"));
}
