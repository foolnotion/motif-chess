#include <algorithm>
#include <cstdint>
#include <limits>
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
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members) -- the writer lives inside store; the fixture only names it.
    motif::db::game_writer& writer {store.writer()};

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

constexpr std::int64_t uint32_limit = 4294967295LL;

[[nodiscard]] auto scalar_int64(sqlite3* conn, char const* sql) -> std::int64_t
{
    sqlite3_stmt* raw = nullptr;
    REQUIRE(sqlite3_prepare_v2(conn, sql, -1, &raw, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(raw) == SQLITE_ROW);
    auto const value = sqlite3_column_int64(raw, 0);
    sqlite3_finalize(raw);
    return value;
}

[[nodiscard]] auto persisted_next_game_id(sqlite3* conn) -> std::int64_t
{
    return scalar_int64(conn, "SELECT next_game_id FROM bundle_meta WHERE id = 1");
}

void set_persisted_next_game_id(sqlite3* conn, std::int64_t value)
{
    auto const sql = "UPDATE bundle_meta SET next_game_id = " + std::to_string(value) + " WHERE id = 1";
    REQUIRE(sqlite3_exec(conn, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
}

[[nodiscard]] auto game_count(sqlite3* conn) -> std::int64_t
{
    return scalar_int64(conn, "SELECT COUNT(*) FROM game");
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
    auto const first_res = fix.writer.insert_raw(src);
    REQUIRE(first_res.has_value());

    auto const dup_res = fix.writer.insert_raw(src);
    REQUIRE(dup_res.has_value());
    CHECK(dup_res->value != 0);
    // The duplicate is a distinct row with its own id, and the allocator
    // advanced past both.
    CHECK(dup_res->value != first_res->value);
    CHECK(game_count(fix.db.get()) == 2);
    CHECK(scalar_int64(fix.db.get(), "SELECT COUNT(DISTINCT id) FROM game") == 2);
    CHECK(persisted_next_game_id(fix.db.get()) == static_cast<std::int64_t>(std::max(first_res->value, dup_res->value)) + 1);
    CHECK(persisted_next_game_id(fix.db.get()) == 3);
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

TEST_CASE("game_writer: a fresh schema starts the allocator at 1 and gives games content_revision 1", "[motif_db][game_writer][allocator]")
{
    db_fixture fix;
    CHECK(persisted_next_game_id(fix.db.get()) == 1);
    CHECK(scalar_int64(fix.db.get(), "SELECT COUNT(*) FROM bundle_meta") == 1);

    auto const first = fix.writer.insert(make_game("Alpha", "Beta"));
    REQUIRE(first.has_value());
    CHECK(first->value == 1U);
    CHECK(persisted_next_game_id(fix.db.get()) == 2);
    CHECK(scalar_int64(fix.db.get(), "SELECT COUNT(*) FROM game WHERE content_revision = 1") == 1);
}

TEST_CASE("game_writer: deleting the highest game id never lets insert reuse it", "[motif_db][game_writer][allocator]")
{
    db_fixture fix;

    auto const first = fix.writer.insert(make_game("Alpha", "Beta"));
    auto const second = fix.writer.insert(make_game("Gamma", "Delta"));
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(fix.store.remove(*second).has_value());

    auto const third = fix.writer.insert(make_game("Epsilon", "Zeta"));
    REQUIRE(third.has_value());
    CHECK(third->value > second->value);
    CHECK(persisted_next_game_id(fix.db.get()) == static_cast<std::int64_t>(third->value) + 1);
}

TEST_CASE("game_writer: raw batch ids are not reused after deleting the highest game", "[motif_db][game_writer][allocator]")
{
    db_fixture fix;

    REQUIRE(fix.writer.drop_identity_index().has_value());
    REQUIRE(fix.writer.begin_transaction().has_value());
    auto const first = fix.writer.insert_raw(make_game("Alpha", "Beta"));
    auto const second = fix.writer.insert_raw(make_game("Gamma", "Delta"));
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(fix.writer.commit_transaction().has_value());
    REQUIRE(fix.store.remove(*second).has_value());

    REQUIRE(fix.writer.begin_transaction().has_value());
    auto const third = fix.writer.insert_raw(make_game("Epsilon", "Zeta"));
    REQUIRE(third.has_value());
    REQUIRE(fix.writer.commit_transaction().has_value());
    CHECK(third->value > second->value);
}

TEST_CASE("game_writer: a batch persists the counter once, at commit, with its rows", "[motif_db][game_writer][allocator]")
{
    db_fixture fix;

    REQUIRE(fix.writer.begin_transaction().has_value());
    auto const first = fix.writer.insert(make_game("Alpha", "Beta"));
    auto const second = fix.writer.insert(make_game("Gamma", "Delta"));
    auto const third = fix.writer.insert(make_game("Epsilon", "Zeta"));
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(third.has_value());
    CHECK(second->value == first->value + 1U);
    CHECK(third->value == second->value + 1U);

    // No per-row UPDATE: mid-batch the table still holds the pre-batch value.
    CHECK(persisted_next_game_id(fix.db.get()) == 1);

    REQUIRE(fix.writer.commit_transaction().has_value());
    CHECK(persisted_next_game_id(fix.db.get()) == static_cast<std::int64_t>(third->value) + 1);
}

TEST_CASE("game_writer: rolling back a batch never disturbs committed ids; aborted ids may be reissued",
          "[motif_db][game_writer][allocator]")
{
    db_fixture fix;

    auto const kept = fix.writer.insert(make_game("Alpha", "Beta"));
    REQUIRE(kept.has_value());
    auto const counter_before = persisted_next_game_id(fix.db.get());
    CHECK(counter_before == static_cast<std::int64_t>(kept->value) + 1);

    REQUIRE(fix.writer.begin_transaction().has_value());
    auto const aborted_first = fix.writer.insert(make_game("Gamma", "Delta"));
    auto const aborted_second = fix.writer.insert(make_game("Epsilon", "Zeta"));
    REQUIRE(aborted_first.has_value());
    REQUIRE(aborted_second.has_value());
    fix.writer.rollback_transaction();

    // The table counter is exactly what it was: the batch persisted nothing.
    CHECK(persisted_next_game_id(fix.db.get()) == counter_before);
    CHECK(game_count(fix.db.get()) == 1);

    // The contract is only about committed ids: they are never reused and the
    // counter always exceeds them. The aborted ids were never durable, so the
    // allocator may (and does) hand them out again.
    auto const after_writer = fix.writer.insert(make_game("Eta", "Theta"));
    REQUIRE(after_writer.has_value());
    CHECK(after_writer->value > kept->value);
    auto const after_store = fix.store.insert(make_game("Iota", "Kappa"));
    REQUIRE(after_store.has_value());
    CHECK(after_store->value > after_writer->value);
    CHECK(persisted_next_game_id(fix.db.get()) == static_cast<std::int64_t>(after_store->value) + 1);
    CHECK(persisted_next_game_id(fix.db.get()) == scalar_int64(fix.db.get(), "SELECT MAX(id) FROM game") + 1);
    CHECK(game_count(fix.db.get()) == 3);
    CHECK(scalar_int64(fix.db.get(), "SELECT COUNT(DISTINCT id) FROM game") == 3);
}

TEST_CASE("game_writer: a duplicate insert does not consume an id", "[motif_db][game_writer][allocator]")
{
    db_fixture fix;

    auto const src = make_game();
    REQUIRE(fix.writer.insert(src).has_value());
    auto const dup = fix.writer.insert(src);
    REQUIRE_FALSE(dup.has_value());
    CHECK(dup.error() == motif::db::error_code::duplicate);
    CHECK(persisted_next_game_id(fix.db.get()) == 2);
}

TEST_CASE("game_writer: game_store and its writer share one allocator, including inside an open batch",
          "[motif_db][game_writer][allocator]")
{
    db_fixture fix;

    // store.insert() and store.writer() are the same object, so a standalone
    // insert through either cannot repeat an id.
    auto const via_store = fix.store.insert(make_game("Alpha", "Beta"));
    auto const via_writer = fix.writer.insert(make_game("Gamma", "Delta"));
    auto const via_store_again = fix.store.insert(make_game("Epsilon", "Zeta"));
    REQUIRE(via_store.has_value());
    REQUIRE(via_writer.has_value());
    REQUIRE(via_store_again.has_value());
    CHECK(via_writer->value > via_store->value);
    CHECK(via_store_again->value > via_writer->value);
    CHECK(&fix.store.writer() == &fix.writer);

    // An insert through the store while the writer's batch is open joins the
    // batch's allocator: no repeated id, and the persisted counter does not
    // move until the batch commits.
    auto const counter_before = persisted_next_game_id(fix.db.get());
    REQUIRE(fix.writer.begin_transaction().has_value());
    auto const in_batch = fix.writer.insert(make_game("Eta", "Theta"));
    auto const via_store_mid_batch = fix.store.insert(make_game("Iota", "Kappa"));
    auto const in_batch_again = fix.writer.insert(make_game("Lambda", "Mu"));
    REQUIRE(in_batch.has_value());
    REQUIRE(via_store_mid_batch.has_value());
    REQUIRE(in_batch_again.has_value());
    CHECK(via_store_mid_batch->value == in_batch->value + 1U);
    CHECK(in_batch_again->value == via_store_mid_batch->value + 1U);
    CHECK(persisted_next_game_id(fix.db.get()) == counter_before);

    REQUIRE(fix.writer.commit_transaction().has_value());
    CHECK(persisted_next_game_id(fix.db.get()) == static_cast<std::int64_t>(in_batch_again->value) + 1);
    CHECK(game_count(fix.db.get()) == 6);
    CHECK(scalar_int64(fix.db.get(), "SELECT COUNT(DISTINCT id) FROM game") == 6);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("game_writer: standalone inserts cache the counter and issue one counter UPDATE each", "[motif_db][game_writer][allocator]")
{
    db_fixture fix;

    struct statement_log
    {
        int counter_selects {0};
        int counter_updates {0};
    } log;

    auto const trace = [](unsigned /*mask*/, void* ctx, void* /*stmt*/, void* sql_text) -> int
    {
        auto& seen = *static_cast<statement_log*>(ctx);
        std::string const sql {static_cast<char const*>(sql_text)};
        if (sql.contains("SELECT next_game_id")) {
            ++seen.counter_selects;
        }
        if (sql.contains("UPDATE bundle_meta")) {
            ++seen.counter_updates;
        }
        return 0;
    };
    REQUIRE(sqlite3_trace_v2(fix.db.get(), SQLITE_TRACE_STMT, trace, &log) == SQLITE_OK);

    constexpr int inserts = 5;
    std::int64_t last_id = 0;
    for (int i = 0; i < inserts; ++i) {
        auto const res = fix.writer.insert(make_game("White " + std::to_string(i), "Black"));
        REQUIRE(res.has_value());
        CHECK(std::cmp_greater(res->value, last_id));
        last_id = static_cast<std::int64_t>(res->value);
    }
    sqlite3_trace_v2(fix.db.get(), 0, nullptr, nullptr);

    // One counter load for the first insert, then the cache serves the rest;
    // each insert writes the counter exactly once, inside its own transaction.
    CHECK(log.counter_selects == 1);
    CHECK(log.counter_updates == inserts);
    CHECK(persisted_next_game_id(fix.db.get()) == last_id + 1);
}

TEST_CASE("game_writer: the cached counter survives deleting rows and notices an external advance", "[motif_db][game_writer][allocator]")
{
    db_fixture fix;

    auto const first = fix.writer.insert(make_game("Alpha", "Beta"));
    auto const second = fix.writer.insert(make_game("Gamma", "Delta"));
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(fix.store.remove(*second).has_value());
    REQUIRE(fix.store.remove(*first).has_value());
    CHECK(game_count(fix.db.get()) == 0);

    // Deleting every row leaves the cached counter valid: no id is reused.
    auto const third = fix.writer.insert(make_game("Epsilon", "Zeta"));
    REQUIRE(third.has_value());
    CHECK(third->value > second->value);

    // Another party raises bundle_meta behind the writer's back. The writer
    // cannot see that before writing; its next counter UPDATE finds the table
    // already ahead of its cache, drops the cache, and the insert after that
    // reloads and allocates from the table's value.
    constexpr auto external_next_id = 500;
    set_persisted_next_game_id(fix.db.get(), external_next_id);
    auto const fourth = fix.writer.insert(make_game("Eta", "Theta"));
    REQUIRE(fourth.has_value());
    auto const fifth = fix.writer.insert(make_game("Iota", "Kappa"));
    REQUIRE(fifth.has_value());
    CHECK(fifth->value == 500U);
    CHECK(persisted_next_game_id(fix.db.get()) == 501);
}

TEST_CASE("game_writer: the last uint32 id is allocated, then allocation fails without wrapping", "[motif_db][game_writer][allocator]")
{
    db_fixture fix;
    set_persisted_next_game_id(fix.db.get(), uint32_limit);

    auto const last = fix.writer.insert(make_game("Alpha", "Beta"));
    REQUIRE(last.has_value());
    CHECK(last->value == std::numeric_limits<std::uint32_t>::max());
    CHECK(persisted_next_game_id(fix.db.get()) == uint32_limit + 1);

    auto const overflow = fix.writer.insert(make_game("Gamma", "Delta"));
    REQUIRE_FALSE(overflow.has_value());
    CHECK(overflow.error() == motif::db::error_code::io_failure);
    CHECK(overflow.error().message == "game id space exhausted");
    CHECK(game_count(fix.db.get()) == 1);
    CHECK(persisted_next_game_id(fix.db.get()) == uint32_limit + 1);

    // Same through the raw path inside a batch, and through game_store.
    REQUIRE(fix.writer.drop_identity_index().has_value());
    REQUIRE(fix.writer.begin_transaction().has_value());
    auto const raw_overflow = fix.writer.insert_raw(make_game("Epsilon", "Zeta"));
    REQUIRE_FALSE(raw_overflow.has_value());
    fix.writer.rollback_transaction();
    auto const store_overflow = fix.store.insert(make_game("Eta", "Theta"));
    REQUIRE_FALSE(store_overflow.has_value());
    CHECK(game_count(fix.db.get()) == 1);
}
