#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "motif/db/game_writer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>

#include "motif/db/error.hpp"
#include "motif/db/game_store.hpp"
#include "motif/db/types.hpp"

namespace
{

constexpr std::uint16_t move_a = 0x1234U;
constexpr std::uint16_t move_b = 0x5678U;
constexpr std::uint16_t move_c = 0x9ABCU;

struct sqlite3_deleter
{
    auto operator()(sqlite3* conn) const noexcept -> void { sqlite3_close(conn); }
};

using unique_sqlite3 = std::unique_ptr<sqlite3, sqlite3_deleter>;

struct db_fixture
{
    unique_sqlite3 db {make_db()};
    motif::db::game_store store {db.get()};
    motif::db::game_writer writer {db.get()};

    db_fixture()
    {
        auto const schema_res = store.create_schema();
        REQUIRE(schema_res.has_value());
    }

    [[nodiscard]] static auto make_db() -> unique_sqlite3
    {
        sqlite3* raw = nullptr;
        sqlite3_open(":memory:", &raw);
        return unique_sqlite3 {raw};
    }
};

[[nodiscard]] auto make_player(std::string name) -> motif::db::player
{
    return motif::db::player {
        .name = std::move(name),
        .elo = std::nullopt,
        .title = std::nullopt,
        .country = std::nullopt,
    };
}

[[nodiscard]] auto make_game(std::string white_name = "Kasparov", std::string black_name = "Karpov") -> motif::db::game
{
    return motif::db::game {
        .white = make_player(std::move(white_name)),
        .black = make_player(std::move(black_name)),
        .event_details = std::nullopt,
        .date = std::nullopt,
        .result = "1-0",
        .eco = std::nullopt,
        .moves = {move_a, move_b, move_c},
        .extra_tags = {},
        .provenance = {},
    };
}

}  // namespace

TEST_CASE("game_writer: insert round-trips through game_store", "[motif_db][game_writer]")
{
    db_fixture fix;

    auto const src = make_game();
    auto const ins_res = fix.writer.insert(src);
    REQUIRE(ins_res.has_value());

    auto const get_res = fix.store.get(*ins_res);
    REQUIRE(get_res.has_value());
    CHECK(get_res->white.name == src.white.name);
    CHECK(get_res->black.name == src.black.name);
    CHECK(get_res->moves == src.moves);
}

TEST_CASE("game_writer: duplicate insert returns duplicate", "[motif_db][game_writer]")
{
    db_fixture fix;

    auto const src = make_game();
    REQUIRE(fix.writer.insert(src).has_value());

    auto const dup_res = fix.writer.insert(src);
    REQUIRE_FALSE(dup_res.has_value());
    CHECK(dup_res.error() == motif::db::error_code::duplicate);
}

TEST_CASE("game_writer: batched transaction commits multiple inserts", "[motif_db][game_writer]")
{
    db_fixture fix;

    REQUIRE(fix.writer.begin_transaction().has_value());
    auto const first_id = fix.writer.insert(make_game("Alpha", "Beta"));
    auto const second_id = fix.writer.insert(make_game("Gamma", "Delta"));
    REQUIRE(first_id.has_value());
    REQUIRE(second_id.has_value());
    REQUIRE(fix.writer.commit_transaction().has_value());

    auto const first = fix.store.get(*first_id);
    auto const second = fix.store.get(*second_id);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->white.name == "Alpha");
    CHECK(second->white.name == "Gamma");
}
