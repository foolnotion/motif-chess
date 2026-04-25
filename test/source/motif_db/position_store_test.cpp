#include <cstdint>
#include <span>
#include <vector>

#include "motif/db/position_store.hpp"

#include <catch2/catch_test_macros.hpp>
#include <duckdb.h>

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

}  // namespace

TEST_CASE("position_store::initialize_schema creates table with zero rows",
          "[motif-db][position_store]")
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

TEST_CASE("position_store::initialize_schema is idempotent",
          "[motif-db][position_store]")
{
    duck_fixture fix;
    REQUIRE(fix.store.initialize_schema().has_value());
    REQUIRE(fix.store.initialize_schema().has_value());
}

TEST_CASE("position_store::insert_batch increases row_count",
          "[motif-db][position_store]")
{
    duck_fixture fix;
    REQUIRE(fix.store.initialize_schema().has_value());

    static constexpr std::uint64_t hash_a = 0xDEADBEEF'CAFEBABE;
    static constexpr std::uint64_t hash_b = 0x0123456789ABCDEF;
    static constexpr std::int16_t elo_white = 2800;
    static constexpr std::int16_t elo_black = 2750;

    std::vector<motif::db::position_row> rows {
        {.zobrist_hash = hash_a,
         .game_id = 1,
         .ply = 1,
         .result = 1,
         .white_elo = {elo_white},
         .black_elo = {elo_black}},
        {.zobrist_hash = hash_b,
         .game_id = 1,
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

TEST_CASE("position_store::insert_batch accepts null elo columns",
          "[motif-db][position_store]")
{
    duck_fixture fix;
    REQUIRE(fix.store.initialize_schema().has_value());

    static constexpr std::uint64_t hash_null_elo = 0xABCDABCDABCDABCD;

    std::vector<motif::db::position_row> rows {
        {.zobrist_hash = hash_null_elo,
         .game_id = 2,
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

TEST_CASE("position_store round-trip: insert then query columns directly",
          "[motif-db][position_store]")
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
        .zobrist_hash = hash_rt,
        .game_id = game_id,
        .ply = ply_rt,
        .result = result_rt,
        .white_elo = welo,
        .black_elo = belo,
    };

    REQUIRE(fix.store.insert_batch(std::span {&row, 1}).has_value());

    duckdb_result qres {};
    char const* query = "SELECT zobrist_hash, game_id, ply, result,"
                        " white_elo, black_elo FROM position";
    REQUIRE(duckdb_query(fix.con, query, &qres) == DuckDBSuccess);
    REQUIRE(duckdb_row_count(&qres) == 1);

    CHECK(duckdb_value_uint64(&qres, 0, 0) == row.zobrist_hash);
    CHECK(duckdb_value_uint32(&qres, 1, 0) == row.game_id);
    CHECK(duckdb_value_uint16(&qres, 2, 0) == row.ply);
    CHECK(static_cast<std::int8_t>(duckdb_value_int8(&qres, 3, 0))
          == row.result);
    CHECK(duckdb_value_int16(&qres, 4, 0) == *row.white_elo);
    CHECK(duckdb_value_int16(&qres, 5, 0) == *row.black_elo);

    duckdb_destroy_result(&qres);
}
