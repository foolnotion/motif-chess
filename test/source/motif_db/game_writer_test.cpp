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

TEST_CASE("game_writer: insert failing after caching a new player id does not poison a later insert", "[motif_db][game_writer]")
{
    db_fixture fix;

    // extra_tags has the same key twice, so insert_game_tags's second
    // game_tag row violates the (game_id, tag_id) primary key -- insert()
    // fails and its txn_guard rolls back everything, including the new
    // "Fresh Player" row that find_or_insert_player already cached.
    auto failing = make_game("Fresh Player", "Karpov");
    failing.extra_tags = {{"dup", "1"}, {"dup", "2"}};
    auto const failing_res = fix.writer.insert(failing);
    REQUIRE_FALSE(failing_res.has_value());

    // A later, unrelated, valid insert reusing "Fresh Player" must not reuse
    // the now-dangling cached id -- it should re-resolve (re-insert) the
    // player and succeed rather than fail its game.white_id foreign key.
    auto const recovering = make_game("Fresh Player", "Someone Else");
    auto const recovering_res = fix.writer.insert(recovering);
    REQUIRE(recovering_res.has_value());

    auto const get_res = fix.store.get(*recovering_res);
    REQUIRE(get_res.has_value());
    CHECK(get_res->white.name == "Fresh Player");
}

TEST_CASE("game_writer: insert_raw failing after caching a new player id does not poison a later insert_raw", "[motif_db][game_writer]")
{
    db_fixture fix;

    // Standalone insert_raw (no outer transaction, matching how these tests
    // call it elsewhere) -- its own txn_guard is the real rollback boundary
    // here, unlike inside import_pipeline's batch ingest where it's nested
    // and a no-op.
    auto failing = make_game("Fresh Raw Player", "Karpov");
    failing.extra_tags = {{"dup", "1"}, {"dup", "2"}};
    auto const failing_res = fix.writer.insert_raw(failing);
    REQUIRE_FALSE(failing_res.has_value());

    auto const recovering = make_game("Fresh Raw Player", "Someone Else");
    auto const recovering_res = fix.writer.insert_raw(recovering);
    REQUIRE(recovering_res.has_value());

    auto const get_res = fix.store.get(*recovering_res);
    REQUIRE(get_res.has_value());
    CHECK(get_res->white.name == "Fresh Raw Player");
}

TEST_CASE("game_writer: same players/date/result but different moves are not treated as duplicates", "[motif_db][game_writer]")
{
    db_fixture fix;

    auto first = make_game();
    REQUIRE(fix.writer.insert(first).has_value());

    // Same identity fields as `first`, different moves -- a moves_hash bug
    // that ignored its input would collide and wrongly return `duplicate`.
    auto second = make_game();
    second.moves = {move_c, move_b, move_a};
    auto const second_res = fix.writer.insert(second);
    REQUIRE(second_res.has_value());

    auto const get_res = fix.store.get(*second_res);
    REQUIRE(get_res.has_value());
    CHECK(get_res->moves == second.moves);
}

TEST_CASE("game_writer: insert_raw does not reject duplicates", "[motif_db][game_writer]")
{
    db_fixture fix;

    auto const src = make_game();
    REQUIRE(fix.writer.identity_index_exists().has_value());
    CHECK(*fix.writer.identity_index_exists());
    REQUIRE(fix.writer.drop_identity_index().has_value());
    REQUIRE(fix.writer.identity_index_exists().has_value());
    CHECK_FALSE(*fix.writer.identity_index_exists());
    REQUIRE(fix.writer.insert_raw(src).has_value());

    auto const dup_res = fix.writer.insert_raw(src);
    REQUIRE(dup_res.has_value());
    CHECK(dup_res->value != 0);
}

TEST_CASE("game_writer: deduplicate removes exact duplicates from insert_raw", "[motif_db][game_writer]")
{
    db_fixture fix;

    auto const src = make_game();
    REQUIRE(fix.writer.drop_identity_index().has_value());
    auto const first_id = fix.writer.insert_raw(src);
    auto const dup_id = fix.writer.insert_raw(src);
    REQUIRE(first_id.has_value());
    REQUIRE(dup_id.has_value());

    auto const dedup_res = fix.writer.deduplicate();
    REQUIRE(dedup_res.has_value());
    CHECK(dedup_res->removed == 1);
    REQUIRE(fix.writer.identity_index_exists().has_value());
    CHECK(*fix.writer.identity_index_exists());

    // The surviving row is the lower id; the duplicate is gone.
    CHECK(fix.store.get(*first_id).has_value());
    CHECK_FALSE(fix.store.get(*dup_id).has_value());

    // Index is usable again, and a fresh duplicate is once more rejected by insert().
    auto const post_dedup_dup = fix.writer.insert(src);
    REQUIRE_FALSE(post_dedup_dup.has_value());
    CHECK(post_dedup_dup.error() == motif::db::error_code::duplicate);
}

TEST_CASE("game_writer: deduplicate reassigns identity_collision for distinct games sharing identity fields", "[motif_db][game_writer]")
{
    db_fixture fix;

    auto first = make_game();
    auto second = make_game();
    second.moves = {move_c, move_b, move_a};  // same identity fields, different moves/move_hash

    REQUIRE(fix.writer.drop_identity_index().has_value());
    auto const first_id = fix.writer.insert_raw(first);
    auto const second_id = fix.writer.insert_raw(second);
    REQUIRE(first_id.has_value());
    REQUIRE(second_id.has_value());

    auto const dedup_res = fix.writer.deduplicate();
    REQUIRE(dedup_res.has_value());
    CHECK(dedup_res->removed == 0);

    auto const first_get = fix.store.get(*first_id);
    auto const second_get = fix.store.get(*second_id);
    REQUIRE(first_get.has_value());
    REQUIRE(second_get.has_value());
    CHECK(first_get->moves == first.moves);
    CHECK(second_get->moves == second.moves);
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
