#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "motif/db/game_store.hpp"

#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>

#include "motif/db/error.hpp"
#include "motif/db/types.hpp"

// llvm-prefer-static-over-anonymous-namespace and misc-use-anonymous-namespace
// are mutually contradictory; anonymous namespace is preferred by the C++
// standard.

namespace
{

// Arbitrary encoded-move values used as stable test data.
constexpr std::uint16_t move_a = 0x1234U;
constexpr std::uint16_t move_b = 0x5678U;
constexpr std::uint16_t move_c = 0x9ABCU;
constexpr std::uint16_t move_d = 0x0001U;
constexpr std::uint16_t move_e = 0x0002U;

// Sentinel: a row id guaranteed not to exist in any fresh in-memory database.
constexpr std::uint32_t absent_id = 99999U;

// ── Fixture
// ───────────────────────────────────────────────────────────────────

struct sqlite3_deleter
{
    auto operator()(sqlite3* conn) const noexcept -> void { sqlite3_close(conn); }
};

using unique_sqlite3 = std::unique_ptr<sqlite3, sqlite3_deleter>;

struct db_fixture
{
    unique_sqlite3 db {make_db()};
    motif::db::game_store store {db.get()};

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

    // Count rows in any table — used to verify deduplication and cascade
    // behaviour.
    [[nodiscard]] auto count_rows(char const* table) const -> int
    {
        std::string sql = "SELECT COUNT(*) FROM ";
        sql += table;
        sqlite3_stmt* raw = nullptr;
        sqlite3_prepare_v2(db.get(), sql.c_str(), -1, &raw, nullptr);
        sqlite3_step(raw);
        int const cnt = sqlite3_column_int(raw, 0);
        sqlite3_finalize(raw);
        return cnt;
    }
};

// ── Test data builders
// ────────────────────────────────────────────────────────

[[nodiscard]] auto make_player(std::string name) -> motif::db::player
{  // NOLINT(llvm-prefer-static-over-anonymous-namespace)
    return motif::db::player {
        .name = std::move(name),
        .elo = std::nullopt,
        .title = std::nullopt,
        .country = std::nullopt,
    };
}

[[nodiscard]] auto make_game(  // NOLINT(llvm-prefer-static-over-anonymous-namespace)
    std::string white_name = "Kasparov",
    std::string black_name = "Karpov") -> motif::db::game
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
    };
}

[[nodiscard]] auto make_game_with_event(std::string event_name) -> motif::db::game
{  // NOLINT(llvm-prefer-static-over-anonymous-namespace)
    auto new_game = make_game();
    new_game.event_details = motif::db::event {
        .name = std::move(event_name),
        .site = "London",
        .date = std::nullopt,
    };
    return new_game;
}

}  // namespace

// ── AC #1/#2: Insert + get round-trip ────────────────────────────────────────

TEST_CASE("game_store: insert returns a valid id", "[motif_db][game_store]")
{
    db_fixture fix;
    auto const res = fix.store.insert(make_game());
    REQUIRE(res.has_value());
    CHECK(*res > 0U);
}

TEST_CASE("game_store: get reconstructs original metadata and moves", "[motif_db][game_store]")
{
    db_fixture fix;
    auto const src = make_game();

    auto const ins_res = fix.store.insert(src);
    REQUIRE(ins_res.has_value());

    auto const get_res = fix.store.get(*ins_res);
    REQUIRE(get_res.has_value());

    auto const& got = *get_res;
    CHECK(got.white.name == src.white.name);
    CHECK(got.black.name == src.black.name);
    CHECK(got.result == src.result);
    CHECK(got.moves == src.moves);
}

TEST_CASE("game_store: get with event reconstructs event details", "[motif_db][game_store]")
{
    db_fixture fix;
    auto src = make_game();
    src.date = "2024.01.15";
    src.eco = "B12";
    src.event_details = motif::db::event {
        .name = "World Championship",
        .site = "Dubai",
        .date = "2024",
    };

    auto const ins_res = fix.store.insert(src);
    REQUIRE(ins_res.has_value());

    auto const get_res = fix.store.get(*ins_res);
    REQUIRE(get_res.has_value());

    auto const& got = *get_res;
    CHECK(got.date == src.date);
    CHECK(got.eco == src.eco);

    REQUIRE(got.event_details.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by REQUIRE
    // above
    auto const& got_event = got.event_details.value();
    CHECK(got_event.name == "World Championship");
    CHECK(got_event.site == "Dubai");
    CHECK(got_event.date == "2024");
}

TEST_CASE("game_store: get with extra tags round-trips tag key-value pairs", "[motif_db][game_store]")
{
    db_fixture fix;
    auto src = make_game();
    src.extra_tags = {{"Source", "TWIC"}, {"Annotator", "Bronstein"}};

    auto const ins_res = fix.store.insert(src);
    REQUIRE(ins_res.has_value());

    auto const get_res = fix.store.get(*ins_res);
    REQUIRE(get_res.has_value());
    CHECK(get_res->extra_tags == src.extra_tags);
}

TEST_CASE("game_store: get_opening_context returns moves elo eco and opening tag", "[motif_db][game_store]")
{
    db_fixture fix;
    auto src = make_game();
    src.white.elo = 2500;
    src.black.elo = 2400;
    src.eco = "C20";
    src.extra_tags = {{"Opening", "King's Pawn Game"}, {"Source", "TWIC"}};

    auto const ins_res = fix.store.insert(src);
    REQUIRE(ins_res.has_value());

    auto const context_res = fix.store.get_opening_context(*ins_res);
    REQUIRE(context_res.has_value());
    CHECK(context_res->white_elo == src.white.elo);
    CHECK(context_res->black_elo == src.black.elo);
    CHECK(context_res->eco == src.eco);
    CHECK(context_res->opening_name == std::optional<std::string> {"King's Pawn Game"});
    CHECK(context_res->moves == src.moves);
}

TEST_CASE("game_store: get_game_contexts returns all requested contexts", "[motif_db][game_store]")
{
    db_fixture fix;

    auto first = make_game();
    first.eco = "C20";
    first.extra_tags = {{"Opening", "King's Pawn Game"}};

    auto second = make_game("Alpha", "Beta");
    second.eco = "B01";
    second.extra_tags = {{"Opening", "Scandinavian Defense"}};

    auto const first_id = fix.store.insert(first);
    auto const second_id = fix.store.insert(second);
    REQUIRE(first_id.has_value());
    REQUIRE(second_id.has_value());

    auto contexts = fix.store.get_game_contexts({*first_id, *second_id});
    REQUIRE(contexts.has_value());
    REQUIRE(contexts->size() == 2);

    CHECK(contexts->at(*first_id).eco == first.eco);
    CHECK(contexts->at(*first_id).opening_name == std::optional<std::string> {"King's Pawn Game"});
    CHECK(contexts->at(*first_id).moves == first.moves);
    CHECK(contexts->at(*second_id).eco == second.eco);
    CHECK(contexts->at(*second_id).opening_name == std::optional<std::string> {"Scandinavian Defense"});
    CHECK(contexts->at(*second_id).moves == second.moves);
}

TEST_CASE("game_store: create_schema fails when foreign keys cannot be enabled", "[motif_db][game_store]")
{
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(":memory:", &raw) == SQLITE_OK);
    unique_sqlite3 const db_conn {raw};

    REQUIRE(sqlite3_exec(db_conn.get(), "BEGIN;", nullptr, nullptr, nullptr) == SQLITE_OK);

    motif::db::game_store store {db_conn.get()};
    auto const schema_res = store.create_schema();
    REQUIRE_FALSE(schema_res.has_value());
    CHECK(schema_res.error() == motif::db::error_code::io_failure);

    REQUIRE(sqlite3_exec(db_conn.get(), "ROLLBACK;", nullptr, nullptr, nullptr) == SQLITE_OK);
}

TEST_CASE("game_store: get on unknown id returns not_found", "[motif_db][game_store]")
{
    db_fixture fix;
    auto const res = fix.store.get(absent_id);
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error() == motif::db::error_code::not_found);
}

// ── AC #1: Player deduplication ──────────────────────────────────────────────

TEST_CASE("game_store: same player name is reused across games", "[motif_db][game_store]")
{
    db_fixture fix;

    // "Magnus" appears as white in both games; should produce a single player
    // row.
    auto game1 = make_game("Magnus", "Nakamura");
    auto game2 = make_game("Magnus", "So");
    game2.moves = {move_d, move_e};  // distinct game

    REQUIRE(fix.store.insert(game1).has_value());
    REQUIRE(fix.store.insert(game2).has_value());

    constexpr int expected_player_rows = 3;  // Magnus, Nakamura, So
    CHECK(fix.count_rows("player") == expected_player_rows);
}

// ── AC #1: Event deduplication ───────────────────────────────────────────────

TEST_CASE("game_store: same event name is reused across games", "[motif_db][game_store]")
{
    db_fixture fix;

    auto game1 = make_game_with_event("Tata Steel 2024");
    auto game2 = make_game("Anand", "Giri");  // different players → distinct game
    game2.event_details = motif::db::event {
        .name = "Tata Steel 2024",
        .site = std::nullopt,
        .date = std::nullopt,
    };

    REQUIRE(fix.store.insert(game1).has_value());
    REQUIRE(fix.store.insert(game2).has_value());

    CHECK(fix.count_rows("event") == 1);
}

// ── AC #1: Duplicate game rejection ──────────────────────────────────────────

TEST_CASE("game_store: inserting identical game returns duplicate", "[motif_db][game_store]")
{
    db_fixture fix;
    auto const src = make_game();

    REQUIRE(fix.store.insert(src).has_value());

    auto const dup_res = fix.store.insert(src);
    REQUIRE_FALSE(dup_res.has_value());
    CHECK(dup_res.error() == motif::db::error_code::duplicate);
}

TEST_CASE("game_store: different moves make two games distinct", "[motif_db][game_store]")
{
    db_fixture fix;

    auto game1 = make_game();
    auto game2 = make_game();
    game2.moves = {move_d, move_e};  // different move sequence → not a duplicate

    REQUIRE(fix.store.insert(game1).has_value());
    REQUIRE(fix.store.insert(game2).has_value());

    constexpr int expected_game_rows = 2;
    CHECK(fix.count_rows("game") == expected_game_rows);
}

// ── AC #3: Remove semantics
// ───────────────────────────────────────────────────

TEST_CASE("game_store: remove deletes game and game_tag rows", "[motif_db][game_store]")
{
    db_fixture fix;

    auto src = make_game();
    src.extra_tags = {{"Round", "1"}, {"Board", "1"}};

    auto const ins_res = fix.store.insert(src);
    REQUIRE(ins_res.has_value());

    CHECK(fix.count_rows("game") == 1);
    constexpr int expected_tags = 2;
    CHECK(fix.count_rows("game_tag") == expected_tags);

    auto const rem_res = fix.store.remove(*ins_res);
    REQUIRE(rem_res.has_value());

    CHECK(fix.count_rows("game") == 0);
    CHECK(fix.count_rows("game_tag") == 0);
}

TEST_CASE("game_store: remove preserves player and event rows", "[motif_db][game_store]")
{
    db_fixture fix;
    auto src = make_game_with_event("Candidates 2024");

    auto const ins_res = fix.store.insert(src);
    REQUIRE(ins_res.has_value());

    constexpr int expected_players = 2;
    CHECK(fix.count_rows("player") == expected_players);
    CHECK(fix.count_rows("event") == 1);

    REQUIRE(fix.store.remove(*ins_res).has_value());

    CHECK(fix.count_rows("game") == 0);
    CHECK(fix.count_rows("player") == expected_players);  // preserved
    CHECK(fix.count_rows("event") == 1);  // preserved
}

TEST_CASE("game_store: remove on unknown id returns not_found", "[motif_db][game_store]")
{
    db_fixture fix;
    auto const res = fix.store.remove(absent_id);
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error() == motif::db::error_code::not_found);
}

TEST_CASE("game_store: get after remove returns not_found", "[motif_db][game_store]")
{
    db_fixture fix;
    auto const ins_res = fix.store.insert(make_game());
    REQUIRE(ins_res.has_value());

    REQUIRE(fix.store.remove(*ins_res).has_value());

    auto const get_res = fix.store.get(*ins_res);
    REQUIRE_FALSE(get_res.has_value());
    CHECK(get_res.error() == motif::db::error_code::not_found);
}

// ── AC #4: Edge cases
// ─────────────────────────────────────────────────────────

TEST_CASE("game_store: game with no moves round-trips correctly", "[motif_db][game_store]")
{
    db_fixture fix;
    auto src = make_game();
    src.moves = {};

    auto const ins_res = fix.store.insert(src);
    REQUIRE(ins_res.has_value());

    auto const get_res = fix.store.get(*ins_res);
    REQUIRE(get_res.has_value());
    CHECK(get_res->moves.empty());
}
