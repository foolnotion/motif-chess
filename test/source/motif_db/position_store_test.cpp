#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "motif/db/position_store.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <duckdb.h>
#include <fmt/base.h>

#include "motif/db/types.hpp"

namespace
{

// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto make_duck_con(duckdb_database& duck_db) -> duckdb_connection
{
    duckdb_open(nullptr, &duck_db);
    duckdb_connection con {nullptr};
    duckdb_connect(duck_db, &con);
    return con;
}

struct duck_fixture
{
    duckdb_database duck_db {nullptr};
    duckdb_connection con {make_duck_con(duck_db)};
    motif::db::position_store store {con};

    duck_fixture() = default;
    duck_fixture(duck_fixture const&) = delete;
    auto operator=(duck_fixture const&) = delete;
    duck_fixture(duck_fixture&&) = delete;
    auto operator=(duck_fixture&&) = delete;

    ~duck_fixture()
    {
        if (con != nullptr) {
            duckdb_disconnect(&con);
        }
        if (duck_db != nullptr) {
            duckdb_close(&duck_db);
        }
    }
};

constexpr auto opening_root_hash = 100U;
constexpr auto first_child_hash = 200U;
constexpr auto first_grandchild_hash = 300U;
constexpr auto second_child_hash = 201U;
constexpr auto second_grandchild_hash = 301U;
constexpr auto first_move = 10U;
constexpr auto second_move = 11U;
constexpr auto third_move = 20U;
constexpr auto fourth_move = 21U;
constexpr auto first_white_elo = 2500;
constexpr auto first_black_elo = 2400;
constexpr auto second_white_elo = 2200;
constexpr auto second_black_elo = 2100;
constexpr auto score_tolerance = 0.001;

auto make_opening_rows() -> std::vector<motif::db::position_row>
{
    return {
        {.zobrist_hash = motif::db::zobrist_hash {opening_root_hash},
         .game_id = motif::db::game_id {1U},
         .ply = 0,
         .encoded_move = 0U,
         .result = 1,
         .white_elo = first_white_elo,
         .black_elo = first_black_elo},
        {.zobrist_hash = motif::db::zobrist_hash {first_child_hash},
         .game_id = motif::db::game_id {1U},
         .ply = 1,
         .encoded_move = first_move,
         .result = 1,
         .white_elo = first_white_elo,
         .black_elo = first_black_elo},
        {.zobrist_hash = motif::db::zobrist_hash {first_grandchild_hash},
         .game_id = motif::db::game_id {1U},
         .ply = 2,
         .encoded_move = second_move,
         .result = 1,
         .white_elo = first_white_elo,
         .black_elo = first_black_elo},
        {.zobrist_hash = motif::db::zobrist_hash {opening_root_hash},
         .game_id = motif::db::game_id {2U},
         .ply = 0,
         .encoded_move = 0U,
         .result = -1,
         .white_elo = second_white_elo,
         .black_elo = second_black_elo},
        {.zobrist_hash = motif::db::zobrist_hash {second_child_hash},
         .game_id = motif::db::game_id {2U},
         .ply = 1,
         .encoded_move = third_move,
         .result = -1,
         .white_elo = second_white_elo,
         .black_elo = second_black_elo},
        {.zobrist_hash = motif::db::zobrist_hash {second_grandchild_hash},
         .game_id = motif::db::game_id {2U},
         .ply = 2,
         .encoded_move = fourth_move,
         .result = -1,
         .white_elo = second_white_elo,
         .black_elo = second_black_elo},
    };
}

auto opening_rollups_exist(duckdb_connection con) -> bool
{
    duckdb_result result {};
    auto const status = duckdb_query(
        con,
        "SELECT COUNT(*) = 1 FROM information_schema.tables "
        "WHERE table_schema = 'main' "
        "AND table_name = 'opening_continuation'",
        &result);
    if (status == DuckDBError) {
        duckdb_destroy_result(&result);
        return false;
    }
    auto const exists = duckdb_value_boolean(&result, 0, 0);
    duckdb_destroy_result(&result);
    return exists;
}

}  // namespace

TEST_CASE("position_store::initialize_schema creates table with zero rows", "[motif-db][position_store]")
{
    duck_fixture fix;
    auto res = fix.store.initialize_schema();
    REQUIRE(res.has_value());

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto count = fix.store.row_count();
    REQUIRE(count.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(*count == 0);
}

TEST_CASE("position_store::initialize_schema is idempotent", "[motif-db][position_store]")
{
    duck_fixture fix;
    REQUIRE(fix.store.initialize_schema().has_value());
    REQUIRE(fix.store.initialize_schema().has_value());
}

TEST_CASE("position_store::insert_batch increases row_count", "[motif-db][position_store]")
{
    duck_fixture fix;
    REQUIRE(fix.store.initialize_schema().has_value());

    static constexpr std::uint64_t hash_a = 0xDEADBEEF'CAFEBABE;
    static constexpr std::uint64_t hash_b = 0x0123456789ABCDEF;
    static constexpr std::int16_t elo_white = 2800;
    static constexpr std::int16_t elo_black = 2750;

    std::vector<motif::db::position_row> rows {
        {.zobrist_hash = motif::db::zobrist_hash {hash_a},
         .game_id = motif::db::game_id {1},
         .ply = 1,
         .result = 1,
         .white_elo = {elo_white},
         .black_elo = {elo_black}},
        {.zobrist_hash = motif::db::zobrist_hash {hash_b},
         .game_id = motif::db::game_id {1},
         .ply = 2,
         .result = 1,
         .white_elo = {elo_white},
         .black_elo = {elo_black}},
    };

    auto ins_res = fix.store.insert_batch(rows);
    REQUIRE(ins_res.has_value());

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto count = fix.store.row_count();
    REQUIRE(count.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(*count == 2);
}

TEST_CASE("position_store::insert_batch accepts null elo columns", "[motif-db][position_store]")
{
    duck_fixture fix;
    REQUIRE(fix.store.initialize_schema().has_value());

    static constexpr std::uint64_t hash_null_elo = 0xABCDABCDABCDABCD;

    std::vector<motif::db::position_row> rows {
        {.zobrist_hash = motif::db::zobrist_hash {hash_null_elo},
         .game_id = motif::db::game_id {2},
         .ply = 1,
         .result = 0,
         .white_elo = {},
         .black_elo = {}},
    };

    auto ins_res = fix.store.insert_batch(rows);
    REQUIRE(ins_res.has_value());

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto count = fix.store.row_count();
    REQUIRE(count.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(*count == 1);
}

TEST_CASE("position_store round-trip: insert then query columns directly", "[motif-db][position_store]")
{
    duck_fixture fix;
    REQUIRE(fix.store.initialize_schema().has_value());

    static constexpr std::uint64_t hash_rt = 0x1122334455667788;
    static constexpr std::uint32_t game_id = 7;
    static constexpr std::uint16_t ply_rt = 3;
    static constexpr std::int8_t result_rt = -1;
    static constexpr std::int16_t welo = 1500;
    static constexpr std::int16_t belo = 1600;

    motif::db::position_row const row {
        .zobrist_hash = motif::db::zobrist_hash {hash_rt},
        .game_id = motif::db::game_id {game_id},
        .ply = ply_rt,
        .result = result_rt,
        .white_elo = welo,
        .black_elo = belo,
    };

    REQUIRE(fix.store.insert_batch(std::span {&row, 1}).has_value());

    // result/white_elo/black_elo are per-game facts, normalized into
    // game_result (see position_store.cpp's create_game_result) rather than
    // stored inline on every position row.
    duckdb_result qres {};
    char const* query = "SELECT zobrist_hash, game_id, ply FROM position";
    REQUIRE(duckdb_query(fix.con, query, &qres) == DuckDBSuccess);
    REQUIRE(duckdb_row_count(&qres) == 1);

    CHECK(duckdb_value_uint64(&qres, 0, 0) == row.zobrist_hash.value);
    CHECK(duckdb_value_uint32(&qres, 1, 0) == row.game_id.value);
    CHECK(duckdb_value_uint16(&qres, 2, 0) == row.ply);
    duckdb_destroy_result(&qres);

    duckdb_result gres {};
    char const* game_result_query = "SELECT result, white_elo, black_elo FROM game_result WHERE game_id = 7";
    REQUIRE(duckdb_query(fix.con, game_result_query, &gres) == DuckDBSuccess);
    REQUIRE(duckdb_row_count(&gres) == 1);

    CHECK(static_cast<std::int8_t>(duckdb_value_int8(&gres, 0, 0)) == row.result);
    CHECK(duckdb_value_int16(&gres, 1, 0) == *row.white_elo);
    CHECK(duckdb_value_int16(&gres, 2, 0) == *row.black_elo);

    duckdb_destroy_result(&gres);
}

TEST_CASE("position_store::count_distinct_games_by_zobrist filters by game ids", "[motif-db][position_store]")
{
    duck_fixture fix;
    REQUIRE(fix.store.initialize_schema().has_value());

    auto const rows = make_opening_rows();
    REQUIRE(fix.store.insert_batch(rows).has_value());

    auto const count = fix.store.count_distinct_games_by_zobrist(motif::db::zobrist_hash {opening_root_hash},
                                                                 std::vector<motif::db::game_id> {
                                                                     motif::db::game_id {1U},
                                                                 });
    REQUIRE(count.has_value());
    CHECK(*count == 1);
}

TEST_CASE("position_store::query_opening_stats filtered computes elo_weighted_score", "[motif-db][position_store]")
{
    duck_fixture fix;
    REQUIRE(fix.store.initialize_schema().has_value());

    auto const rows = make_opening_rows();
    REQUIRE(fix.store.insert_batch(rows).has_value());

    auto const stats = fix.store.query_opening_stats(motif::db::zobrist_hash {opening_root_hash},
                                                     std::vector<motif::db::game_id> {
                                                         motif::db::game_id {1U},
                                                     });
    REQUIRE(stats.has_value());
    REQUIRE(stats->size() == 1);
    CHECK(stats->front().frequency == 1U);
    REQUIRE(stats->front().elo_weighted_score.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK_THAT(*stats->front().elo_weighted_score, Catch::Matchers::WithinAbs(1.0, score_tolerance));
}

TEST_CASE("position_store::rebuild_opening_stats_rollups stores direct and transposition counts", "[motif-db][position_store]")
{
    duck_fixture fix;
    REQUIRE(fix.store.initialize_schema().has_value());

    auto const rows = make_opening_rows();
    REQUIRE(fix.store.insert_batch(rows).has_value());
    REQUIRE(fix.store.rebuild_opening_stats_rollups().has_value());

    auto const stats = fix.store.query_opening_stats(motif::db::zobrist_hash {opening_root_hash});
    REQUIRE(stats.has_value());
    REQUIRE(stats->size() == 2);
    CHECK(stats->at(0).frequency == 1U);
    CHECK(stats->at(0).transposition_frequency == 1U);
    CHECK(stats->at(1).frequency == 1U);
    CHECK(stats->at(1).transposition_frequency == 1U);
}

TEST_CASE("position_store mutations invalidate opening rollups", "[motif-db][position_store]")
{
    duck_fixture fix;
    REQUIRE(fix.store.initialize_schema().has_value());
    REQUIRE(fix.store.insert_batch(make_opening_rows()).has_value());
    REQUIRE(fix.store.rebuild_opening_stats_rollups().has_value());
    REQUIRE(opening_rollups_exist(fix.con));

    REQUIRE(fix.store.delete_by_game_id(motif::db::game_id {999U}).has_value());
    CHECK_FALSE(opening_rollups_exist(fix.con));

    REQUIRE(fix.store.rebuild_opening_stats_rollups().has_value());
    REQUIRE(fix.store.update_elo_for_game(motif::db::game_id {999U}, 2500, std::nullopt).has_value());
    CHECK_FALSE(opening_rollups_exist(fix.con));
}

TEST_CASE("position_store::query_tree_slice filtered returns only requested games", "[motif-db][position_store]")
{
    duck_fixture fix;
    REQUIRE(fix.store.initialize_schema().has_value());

    auto const rows = make_opening_rows();
    REQUIRE(fix.store.insert_batch(rows).has_value());

    auto const slice = fix.store.query_tree_slice(motif::db::zobrist_hash {opening_root_hash},
                                                  2U,
                                                  std::vector<motif::db::game_id> {
                                                      motif::db::game_id {2U},
                                                  });
    REQUIRE(slice.has_value());
    REQUIRE(slice->size() == 2);
    CHECK(slice->at(0).game_id == motif::db::game_id {2U});
    CHECK(slice->at(0).depth == 1U);
    CHECK(slice->at(1).game_id == motif::db::game_id {2U});
    CHECK(slice->at(1).depth == 2U);
}

// Isolates the DuckDB appender's row-by-row API cost (begin_row/append_*/
// end_row per position row) from everything else in the import pipeline --
// no PGN parsing, no SQLite, no file I/O. A real game contributes ~87
// position rows (one per ply, median game length ~44 full moves per the
// 3.36M-game reference bundle), so this reproduces the actual row-count
// pattern stage2's inline-position path drives through insert_batch.
TEST_CASE("position_store::insert_batch throughput on synthetic 1M-game-shaped load", "[motif-db][position_store][performance]")
{
#ifndef NDEBUG
    SKIP("performance checks run only in release builds");
#endif

    duck_fixture fix;
    REQUIRE(fix.store.initialize_schema().has_value());

    constexpr std::size_t games = 100'000;
    constexpr std::size_t plies_per_game = 87;
    constexpr std::size_t flush_batch = 50'000;
    constexpr std::int16_t synthetic_elo = 2000;
    constexpr std::size_t total_rows = games * plies_per_game;

    std::vector<motif::db::position_row> pending;
    pending.reserve(flush_batch);

    auto const started = std::chrono::steady_clock::now();
    for (std::size_t row_index = 0; row_index < total_rows; ++row_index) {
        auto const ply = static_cast<std::uint16_t>(row_index % plies_per_game);
        pending.push_back(motif::db::position_row {
            .zobrist_hash = motif::db::zobrist_hash {row_index},
            .game_id = motif::db::game_id {static_cast<std::uint32_t>((row_index / plies_per_game) + 1)},
            .ply = ply,
            .encoded_move = static_cast<std::uint16_t>(ply + 1),
            .result = 1,
            .white_elo = synthetic_elo,
            .black_elo = synthetic_elo,
        });
        if (pending.size() >= flush_batch) {
            REQUIRE(fix.store.insert_batch(pending).has_value());
            pending.clear();
        }
    }
    if (!pending.empty()) {
        REQUIRE(fix.store.insert_batch(pending).has_value());
    }
    auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);

    auto const ns_per_row =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) / static_cast<double>(total_rows);
    auto const us_per_game = ns_per_row * static_cast<double>(plies_per_game) / 1000.0;

    fmt::print("\n=== position_store::insert_batch throughput ===\n"
               "  games:        {}\n"
               "  rows:         {}\n"
               "  elapsed:      {} ms\n"
               "  ns/row:       {:.1f}\n"
               "  us/game:      {:.2f} ({} rows/game)\n",
               games, total_rows, elapsed.count(), ns_per_row, us_per_game, plies_per_game);
}
