#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>

#include "motif/db/error.hpp"
#include "motif/db/fault_injection.hpp"
#include "motif/db/game_store.hpp"
#include "motif/db/game_writer.hpp"
#include "motif/db/types.hpp"

// Mutation-sequence accounting (schema v7 mutation_state): the sequence counts
// index-relevant canonical TRANSACTIONS. See game_writer.hpp for the contract.

namespace
{

constexpr std::uint16_t move_a = 0x1234U;
constexpr std::uint16_t move_b = 0x5678U;
constexpr auto checked_batch_size = 25;
constexpr auto raw_batch_size = 10;
// Ratings used by the patch tests; the out-of-domain one lies above the signed 16-bit range.
constexpr auto white_rating = 2500;
constexpr auto black_rating = 2400;
constexpr auto raised_white_rating = 2501;
constexpr auto raised_black_rating = 2401;
constexpr auto out_of_domain_rating = 40000;
constexpr auto corrected_white_rating = 2450;
constexpr auto corrected_black_rating = -5;

struct sqlite3_deleter
{
    auto operator()(sqlite3* conn) const noexcept -> void { sqlite3_close(conn); }
};

using unique_sqlite3 = std::unique_ptr<sqlite3, sqlite3_deleter>;

struct sequence_fixture
{
    unique_sqlite3 db {make_db()};
    motif::db::game_store store {db.get()};
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members) -- the writer lives inside store; the fixture only names it.
    motif::db::game_writer& writer {store.writer()};

    sequence_fixture()
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

    [[nodiscard]] auto seq() const -> std::uint64_t
    {
        auto const value = store.mutation_seq();
        REQUIRE(value.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        return *value;
    }

    void run(char const* sql) const { REQUIRE(sqlite3_exec(db.get(), sql, nullptr, nullptr, nullptr) == SQLITE_OK); }

    [[nodiscard]] auto scalar(char const* sql) const -> std::int64_t
    {
        sqlite3_stmt* raw = nullptr;
        REQUIRE(sqlite3_prepare_v2(db.get(), sql, -1, &raw, nullptr) == SQLITE_OK);
        REQUIRE(sqlite3_step(raw) == SQLITE_ROW);
        auto const value = sqlite3_column_int64(raw, 0);
        sqlite3_finalize(raw);
        return value;
    }
};

[[nodiscard]] auto make_game(std::string white = "Kasparov",
                             std::string black = "Karpov",
                             std::string result = "1-0",
                             std::optional<std::int32_t> white_elo = std::nullopt) -> motif::db::game
{
    return motif::db::game {
        .white = {.name = std::move(white), .elo = white_elo, .title = std::nullopt, .country = std::nullopt},
        .black = {.name = std::move(black), .elo = std::nullopt, .title = std::nullopt, .country = std::nullopt},
        .event_details = std::nullopt,
        .date = std::nullopt,
        .result = std::move(result),
        .eco = std::nullopt,
        .moves = {move_a, move_b},
        .extra_tags = {},
        .provenance = {},
    };
}

// Inserts a game and flips it to a user-added one so it may be patched.
[[nodiscard]] auto insert_manual(sequence_fixture& fix, std::string white = "Kasparov", std::string result = "1-0") -> motif::db::game_id
{
    auto const inserted = fix.store.insert(make_game(std::move(white), "Karpov", std::move(result)));
    REQUIRE(inserted.has_value());
    REQUIRE(fix.store.set_manual_provenance(*inserted, std::nullopt, "new").has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return *inserted;
}

}  // namespace

TEST_CASE("mutation_seq: a fresh schema starts at zero", "[motif_db][mutation_seq]")
{
    sequence_fixture const fix;
    CHECK(fix.seq() == 0U);
    CHECK_FALSE(fix.store.has_pending_canonical_change());
}

TEST_CASE("mutation_seq: a missing singleton is corruption, not a cue to reseed", "[motif_db][mutation_seq]")
{
    sequence_fixture fix;
    fix.run("DELETE FROM mutation_state");
    auto const value = fix.store.mutation_seq();
    REQUIRE_FALSE(value.has_value());
    CHECK(value.error() == motif::db::error_code::io_failure);
    // A standalone insert needs the sequence update to succeed: it fails and
    // rolls back instead of inventing a counter.
    auto const inserted = fix.store.insert(make_game());
    CHECK_FALSE(inserted.has_value());
    CHECK(fix.scalar("SELECT COUNT(*) FROM game") == 0);
}

TEST_CASE("mutation_seq: create_schema on an existing database does not reseed a vanished singleton", "[motif_db][mutation_seq]")
{
    sequence_fixture fix;
    REQUIRE(fix.store.insert(make_game()).has_value());
    fix.run("DELETE FROM mutation_state");
    REQUIRE_FALSE(fix.store.create_schema().has_value());
    CHECK(fix.scalar("SELECT COUNT(*) FROM mutation_state") == 0);
    CHECK_FALSE(fix.store.mutation_seq().has_value());
}

TEST_CASE("mutation_seq: standalone checked and raw inserts advance it once each", "[motif_db][mutation_seq]")
{
    sequence_fixture fix;
    REQUIRE(fix.store.insert(make_game("A", "B")).has_value());
    CHECK(fix.seq() == 1U);
    REQUIRE(fix.writer.drop_identity_index().has_value());
    REQUIRE(fix.writer.insert_raw(make_game("C", "D")).has_value());
    CHECK(fix.seq() == 2U);
}

TEST_CASE("mutation_seq: a duplicate insert does not advance it", "[motif_db][mutation_seq]")
{
    sequence_fixture fix;
    REQUIRE(fix.store.insert(make_game()).has_value());
    auto const before = fix.seq();
    auto const duplicate = fix.store.insert(make_game());
    REQUIRE_FALSE(duplicate.has_value());
    CHECK(duplicate.error() == motif::db::error_code::duplicate);
    CHECK(fix.seq() == before);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("mutation_seq: a batch of many inserts advances it exactly once, at commit", "[motif_db][mutation_seq]")
{
    sequence_fixture fix;
    REQUIRE(fix.writer.begin_transaction().has_value());
    CHECK_FALSE(fix.writer.has_pending_canonical_change());
    for (auto index = 0; index < checked_batch_size; ++index) {
        REQUIRE(fix.writer.insert(make_game("White " + std::to_string(index), "Black")).has_value());
    }
    // Rows exist on this connection but the sequence advance is deferred, so
    // the pending flag is what keeps position queries closed in this window.
    CHECK(fix.writer.has_pending_canonical_change());
    CHECK(fix.store.has_pending_canonical_change());
    CHECK(fix.seq() == 0U);
    REQUIRE(fix.writer.commit_transaction().has_value());
    CHECK_FALSE(fix.writer.has_pending_canonical_change());
    CHECK(fix.seq() == 1U);
    CHECK(fix.scalar("SELECT COUNT(*) FROM game") == 25);
}

TEST_CASE("mutation_seq: a raw batch advances once as well", "[motif_db][mutation_seq]")
{
    sequence_fixture fix;
    REQUIRE(fix.writer.drop_identity_index().has_value());
    REQUIRE(fix.writer.begin_transaction().has_value());
    for (auto index = 0; index < raw_batch_size; ++index) {
        REQUIRE(fix.writer.insert_raw(make_game("White " + std::to_string(index), "Black")).has_value());
    }
    REQUIRE(fix.writer.commit_transaction().has_value());
    CHECK(fix.seq() == 1U);
}

TEST_CASE("mutation_seq: rollback, an empty batch and a duplicate-only batch advance it zero times", "[motif_db][mutation_seq]")
{
    sequence_fixture fix;
    REQUIRE(fix.store.insert(make_game()).has_value());
    auto const baseline = fix.seq();

    REQUIRE(fix.writer.begin_transaction().has_value());
    REQUIRE(fix.writer.insert(make_game("X", "Y")).has_value());
    CHECK(fix.writer.has_pending_canonical_change());
    fix.writer.rollback_transaction();
    CHECK_FALSE(fix.writer.has_pending_canonical_change());
    CHECK(fix.seq() == baseline);
    CHECK(fix.scalar("SELECT COUNT(*) FROM game") == 1);

    REQUIRE(fix.writer.begin_transaction().has_value());
    REQUIRE(fix.writer.commit_transaction().has_value());
    CHECK(fix.seq() == baseline);

    REQUIRE(fix.writer.begin_transaction().has_value());
    auto const duplicate = fix.writer.insert(make_game());
    REQUIRE_FALSE(duplicate.has_value());
    CHECK(duplicate.error() == motif::db::error_code::duplicate);
    CHECK_FALSE(fix.writer.has_pending_canonical_change());
    REQUIRE(fix.writer.commit_transaction().has_value());
    CHECK(fix.seq() == baseline);
}

TEST_CASE("mutation_seq: a duplicate among accepted rows in a batch does not poison it", "[motif_db][mutation_seq]")
{
    sequence_fixture fix;
    REQUIRE(fix.writer.begin_transaction().has_value());
    REQUIRE(fix.writer.insert(make_game("A", "B")).has_value());
    CHECK_FALSE(fix.writer.insert(make_game("A", "B")).has_value());
    REQUIRE(fix.writer.insert(make_game("C", "D")).has_value());
    REQUIRE(fix.writer.commit_transaction().has_value());
    CHECK(fix.seq() == 1U);
    CHECK(fix.scalar("SELECT COUNT(*) FROM game") == 2);
}

TEST_CASE("mutation_seq: removal advances it once; a missing id does not", "[motif_db][mutation_seq]")
{
    sequence_fixture fix;
    auto const inserted = fix.store.insert(make_game());
    REQUIRE(inserted.has_value());
    auto const before = fix.seq();

    auto const missing = fix.store.remove(motif::db::game_id {999U});
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error() == motif::db::error_code::not_found);
    CHECK(fix.seq() == before);

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(fix.store.remove(*inserted).has_value());
    CHECK(fix.seq() == before + 1U);
}

TEST_CASE("mutation_seq: removal inside a writer batch joins the single advance", "[motif_db][mutation_seq]")
{
    sequence_fixture fix;
    auto const first = fix.store.insert(make_game("A", "B"));
    auto const second = fix.store.insert(make_game("C", "D"));
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    auto const before = fix.seq();

    REQUIRE(fix.writer.begin_transaction().has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(fix.store.remove(*first).has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(fix.store.remove(*second).has_value());
    REQUIRE(fix.writer.insert(make_game("E", "F")).has_value());
    REQUIRE(fix.writer.commit_transaction().has_value());
    CHECK(fix.seq() == before + 1U);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("mutation_seq: only result, white_elo and black_elo patches advance it", "[motif_db][mutation_seq]")
{
    // The classification: the derived indexes embed a game's id, moves, result
    // and per-game Elo. Everything else is read live from SQLite by every query.
    sequence_fixture fix;
    auto const game = insert_manual(fix);
    auto const baseline = fix.seq();

    auto const advances = [&](motif::db::game_patch const& patch) -> bool
    {
        auto const before = fix.seq();
        REQUIRE(fix.store.patch_metadata(game, patch).has_value());
        auto const after = fix.seq();
        REQUIRE((after == before || after == before + 1U));
        return after == before + 1U;
    };

    auto patch = motif::db::game_patch {};
    patch.white_name = "Renamed White";
    CHECK_FALSE(advances(patch));
    patch = {};
    patch.black_name = "Renamed Black";
    CHECK_FALSE(advances(patch));
    patch = {};
    patch.event = "Some Event";
    patch.site = "Some Site";
    CHECK_FALSE(advances(patch));
    patch = {};
    patch.date = "2024.05.06";
    CHECK_FALSE(advances(patch));
    patch = {};
    patch.eco = "B20";
    CHECK_FALSE(advances(patch));
    patch = {};
    patch.notes = "a note";
    CHECK_FALSE(advances(patch));
    patch = {};
    patch.source_label = "label";
    CHECK_FALSE(advances(patch));
    patch = {};
    patch.review_status = "reviewed";
    CHECK_FALSE(advances(patch));
    patch = {};
    CHECK_FALSE(advances(patch));  // an empty patch is a no-op
    CHECK(fix.seq() == baseline);

    patch = {};
    patch.result = "0-1";
    CHECK(advances(patch));
    patch = {};
    patch.white_elo = motif::db::rating_patch::set(white_rating);
    CHECK(advances(patch));
    patch = {};
    patch.black_elo = motif::db::rating_patch::set(black_rating);
    CHECK(advances(patch));
    CHECK(fix.seq() == baseline + 3U);

    // One transaction, one advance, however many indexed fields it touches.
    patch = {};
    patch.white_elo = motif::db::rating_patch::set(raised_white_rating);
    patch.black_elo = motif::db::rating_patch::set(raised_black_rating);
    patch.result = "1-0";
    patch.event = "Another Event";
    CHECK(advances(patch));
    CHECK(fix.seq() == baseline + 4U);
}

TEST_CASE("mutation_seq: rejected patches and manual provenance do not advance it", "[motif_db][mutation_seq]")
{
    sequence_fixture fix;
    auto const imported = fix.store.insert(make_game("Imported", "Player"));
    REQUIRE(imported.has_value());
    auto const before = fix.seq();

    // An imported game's result and ratings may be corrected; any other field
    // rejects the whole patch (including the correction it carries).
    auto patch = motif::db::game_patch {};
    patch.result = "0-1";
    patch.white_name = "Renamed";
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const not_editable = fix.store.patch_metadata(*imported, patch);
    REQUIRE_FALSE(not_editable.has_value());
    CHECK(not_editable.error() == motif::db::error_code::not_editable);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(fix.store.get(*imported)->result == "1-0");
    CHECK(fix.seq() == before);

    auto const not_found = fix.store.patch_metadata(motif::db::game_id {777U}, patch);
    REQUIRE_FALSE(not_found.has_value());
    CHECK(not_found.error() == motif::db::error_code::not_found);
    CHECK(fix.seq() == before);

    auto bad = motif::db::game_patch {};
    bad.result = "draw";
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(fix.store.patch_metadata(*imported, bad).error() == motif::db::error_code::invalid_argument);
    bad = {};
    bad.white_elo = motif::db::rating_patch::set(out_of_domain_rating);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(fix.store.patch_metadata(*imported, bad).error() == motif::db::error_code::invalid_argument);
    CHECK(fix.seq() == before);

    // Provenance is absent from the derived indexes.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(fix.store.set_manual_provenance(*imported, "label", "new").has_value());
    CHECK(fix.seq() == before);
}

TEST_CASE("mutation_seq: result and rating corrections on an imported game advance it once and nothing else is editable",
          "[motif_db][mutation_seq]")
{
    sequence_fixture fix;
    auto const imported = fix.store.insert(make_game("Imported", "Player"));
    REQUIRE(imported.has_value());
    auto const before = fix.seq();

    auto patch = motif::db::game_patch {};
    patch.result = "0-1";
    patch.white_elo = motif::db::rating_patch::set(corrected_white_rating);
    patch.black_elo = motif::db::rating_patch::set(corrected_black_rating);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(fix.store.patch_metadata(*imported, patch).has_value());
    CHECK(fix.seq() == before + 1U);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const corrected = fix.store.get(*imported);
    REQUIRE(corrected.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    CHECK(corrected->result == "0-1");
    CHECK(corrected->white.elo == 2450);
    CHECK(corrected->black.elo == -5);
    // Provenance is untouched: the game stays an imported one.
    CHECK(corrected->provenance.source_type == "imported");
    // NOLINTEND(bugprone-unchecked-optional-access)

    // Present -> absent is expressible, and restating a stored value is a no-op.
    patch = {};
    patch.white_elo = motif::db::rating_patch::clear();
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(fix.store.patch_metadata(*imported, patch).has_value());
    CHECK(fix.seq() == before + 2U);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK_FALSE(fix.store.get(*imported)->white.elo.has_value());
    patch = {};
    patch.white_elo = motif::db::rating_patch::clear();
    patch.result = "0-1";
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(fix.store.patch_metadata(*imported, patch).has_value());
    CHECK(fix.seq() == before + 2U);

    // An empty patch is a no-op, also for an imported game.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(fix.store.patch_metadata(*imported, motif::db::game_patch {}).has_value());
    CHECK(fix.seq() == before + 2U);
}

TEST_CASE("mutation_seq: a result patch that collides with another game's identity fails without advancing", "[motif_db][mutation_seq]")
{
    sequence_fixture fix;
    auto const first = insert_manual(fix, "Same", "1-0");
    auto const second = insert_manual(fix, "Same", "0-1");
    auto const before = fix.seq();

    // Same players, date, event and moves with the same result as `first`.
    auto patch = motif::db::game_patch {};
    patch.result = "1-0";
    auto const collided = fix.store.patch_metadata(second, patch);
    REQUIRE_FALSE(collided.has_value());
    CHECK(collided.error() == motif::db::error_code::duplicate);
    CHECK(fix.seq() == before);
    auto const unchanged = fix.store.get(second);
    REQUIRE(unchanged.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(unchanged->result == "0-1");
    static_cast<void>(first);
}

TEST_CASE("mutation_seq: dedup advances it only when it changes canonical rows", "[motif_db][mutation_seq]")
{
    sequence_fixture fix;
    REQUIRE(fix.writer.drop_identity_index().has_value());
    REQUIRE(fix.writer.insert_raw(make_game("A", "B")).has_value());
    REQUIRE(fix.writer.insert_raw(make_game("A", "B")).has_value());  // exact duplicate
    REQUIRE(fix.writer.insert_raw(make_game("C", "D")).has_value());
    auto const before = fix.seq();

    auto const summary = fix.writer.deduplicate();
    REQUIRE(summary.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(summary->removed == 1U);
    CHECK(fix.seq() == before + 1U);

    // Dropping and recreating the identity index alone is structural
    // maintenance, not a canonical mutation.
    REQUIRE(fix.writer.drop_identity_index().has_value());
    auto const clean = fix.writer.deduplicate();
    REQUIRE(clean.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(clean->removed == 0U);
    CHECK(fix.seq() == before + 1U);
}

TEST_CASE("mutation_seq: a dedup that only reassigns collision slots still advances it", "[motif_db][mutation_seq]")
{
    sequence_fixture fix;
    REQUIRE(fix.writer.drop_identity_index().has_value());
    // Two distinct games sharing every identity field but different moves: not
    // duplicates, but the second needs identity_collision = 1.
    auto first = make_game("A", "B");
    auto second = make_game("A", "B");
    second.moves = {move_b, move_a};
    REQUIRE(fix.writer.insert_raw(first).has_value());
    REQUIRE(fix.writer.insert_raw(second).has_value());
    // Raw inserts leave identity_collision = 0 on both, which the dedup reassigns
    // only when their move_hash values coincide. Force the collision shape.
    fix.run("UPDATE game SET move_hash = (SELECT move_hash FROM game ORDER BY id LIMIT 1)");
    auto const before = fix.seq();
    auto const summary = fix.writer.deduplicate();
    REQUIRE(summary.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(summary->removed == 0U);
    CHECK(fix.scalar("SELECT MAX(identity_collision) FROM game") == 1);
    CHECK(fix.seq() == before + 1U);
}

TEST_CASE("mutation_seq: a failed operation inside a batch poisons it so nothing partial can commit", "[motif_db][mutation_seq]")
{
    sequence_fixture fix;
    auto const first = insert_manual(fix, "Same", "1-0");
    auto const second = insert_manual(fix, "Same", "0-1");
    static_cast<void>(first);
    auto const before = fix.seq();
    auto const games_before = fix.scalar("SELECT COUNT(*) FROM game");

    // The trigger exists before the batch so the rollback keeps it.
    fix.run("CREATE TRIGGER reject_tag BEFORE INSERT ON game_tag " "BEGIN SELECT RAISE(ABORT, 'injected tag failure'); END;");
    REQUIRE(fix.writer.begin_transaction().has_value());
    REQUIRE(fix.writer.insert(make_game("New", "Game")).has_value());
    // The result UPDATE succeeds, then the notes tag insert is refused: a failure
    // after part of the patch was written. (A duplicate identity is refused by
    // the prepare step before anything is written, so it poisons nothing.)
    auto patch = motif::db::game_patch {};
    patch.result = "1/2-1/2";
    patch.notes = "a note";
    REQUIRE_FALSE(fix.store.patch_metadata(second, patch).has_value());

    auto const committed = fix.writer.commit_transaction();
    REQUIRE_FALSE(committed.has_value());
    // The batch was rolled back as a whole: no partial rows, no sequence move,
    // no lingering pending state, and the connection is usable again.
    CHECK(fix.seq() == before);
    CHECK(fix.scalar("SELECT COUNT(*) FROM game") == games_before);
    CHECK(fix.scalar("SELECT COUNT(*) FROM game WHERE result = '1/2-1/2'") == 0);
    CHECK_FALSE(fix.writer.has_pending_canonical_change());
    CHECK(sqlite3_get_autocommit(fix.db.get()) != 0);
    fix.run("DROP TRIGGER reject_tag");
    REQUIRE(fix.store.insert(make_game("After", "Failure")).has_value());
    CHECK(fix.seq() == before + 1U);
}

TEST_CASE("mutation_seq: a duplicate-identity patch inside a batch is refused before writing and leaves the batch healthy",
          "[motif_db][mutation_seq]")
{
    sequence_fixture fix;
    auto const first = insert_manual(fix, "Same", "1-0");
    auto const second = insert_manual(fix, "Same", "0-1");
    static_cast<void>(first);
    auto const before = fix.seq();

    REQUIRE(fix.writer.begin_transaction().has_value());
    auto patch = motif::db::game_patch {};
    patch.result = "1-0";  // collides with the first game's identity
    auto const refused = fix.store.patch_metadata(second, patch);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == motif::db::error_code::duplicate);
    REQUIRE(fix.writer.insert(make_game("New", "Game")).has_value());
    REQUIRE(fix.writer.commit_transaction().has_value());
    CHECK(fix.seq() == before + 1U);
    CHECK(fix.scalar("SELECT COUNT(*) FROM game WHERE result = '0-1'") == 1);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("mutation_seq: a failed COMMIT rolls the batch back and clears pending state", "[motif_db][mutation_seq]")
{
    sequence_fixture fix;
    REQUIRE(fix.store.insert(make_game("Base", "Game")).has_value());
    auto const before = fix.seq();

    REQUIRE(fix.writer.begin_transaction().has_value());
    REQUIRE(fix.writer.insert(make_game("A", "B")).has_value());
    CHECK(fix.writer.has_pending_canonical_change());
    {
        auto const failing = +[](motif::db::fault::point const where,
                                 motif::db::fault::artifact_kind,
                                 std::uint64_t) noexcept -> motif::db::fault::fault_action
        {
            return where == motif::db::fault::point::before_canonical_commit ? motif::db::fault::fault_action::fail_io
                                                                             : motif::db::fault::fault_action::proceed;
        };
        motif::db::fault::scoped_hook const hook {failing};
        auto const committed = fix.writer.commit_transaction();
        REQUIRE_FALSE(committed.has_value());
    }
    CHECK_FALSE(fix.writer.has_pending_canonical_change());
    CHECK(sqlite3_get_autocommit(fix.db.get()) != 0);
    CHECK(fix.seq() == before);
    CHECK(fix.scalar("SELECT COUNT(*) FROM game") == 1);
}

TEST_CASE("mutation_seq: an insert inside a caller-owned transaction advances immediately and honors its rollback",
          "[motif_db][mutation_seq]")
{
    sequence_fixture fix;
    auto const before = fix.seq();

    // A transaction the writer does not own: no commit hook will ever run, so
    // the advance must happen inside it, and nothing may be cached across its
    // rollback.
    fix.run("BEGIN");
    REQUIRE(fix.store.insert(make_game("A", "B")).has_value());
    CHECK(fix.seq() == before + 1U);
    CHECK_FALSE(fix.writer.has_pending_canonical_change());
    fix.run("ROLLBACK");
    CHECK(fix.seq() == before);  // read from the table: no cached value survives
    CHECK(fix.scalar("SELECT COUNT(*) FROM game") == 0);
    // The writer's own id caches are outside this guarantee (documented on
    // game_writer::insert): the player rows this transaction inserted are gone.
    fix.store.clear_insert_caches();

    fix.run("BEGIN");
    REQUIRE(fix.store.insert(make_game("A", "B")).has_value());
    fix.run("COMMIT");
    CHECK(fix.seq() == before + 1U);
}

TEST_CASE("mutation_seq: exhaustion rejects the operation instead of wrapping and rolls back its allocation", "[motif_db][mutation_seq]")
{
    sequence_fixture fix;
    fix.run("UPDATE mutation_state SET mutation_seq = 9223372036854775807");
    auto const allocator_before = fix.scalar("SELECT next_game_id FROM bundle_meta");

    auto const inserted = fix.store.insert(make_game());
    REQUIRE_FALSE(inserted.has_value());
    CHECK(fix.scalar("SELECT COUNT(*) FROM game") == 0);
    CHECK(fix.scalar("SELECT next_game_id FROM bundle_meta") == allocator_before);
    CHECK(fix.scalar("SELECT mutation_seq FROM mutation_state") == std::numeric_limits<std::int64_t>::max());

    // A batch fails at commit and rolls back, allocator included.
    REQUIRE(fix.writer.begin_transaction().has_value());
    REQUIRE(fix.writer.insert(make_game()).has_value());
    REQUIRE_FALSE(fix.writer.commit_transaction().has_value());
    CHECK(fix.scalar("SELECT COUNT(*) FROM game") == 0);
    CHECK(fix.scalar("SELECT next_game_id FROM bundle_meta") == allocator_before);
}

namespace
{

// Fails any attempt to advance the sequence: the operation's own writes have
// already happened by the time it fires, which is the partial-write window.
void reject_sequence_updates(sequence_fixture const& fix)
{
    fix.run("CREATE TRIGGER reject_seq BEFORE UPDATE ON mutation_state " "BEGIN SELECT RAISE(ABORT, 'injected sequence failure'); END;");
}

// Fails any game_tag insert, i.e. a later statement of a multi-statement
// operation after the game row itself was written.
void reject_tag_inserts(sequence_fixture const& fix)
{
    fix.run("CREATE TRIGGER reject_tag BEFORE INSERT ON game_tag " "BEGIN SELECT RAISE(ABORT, 'injected tag failure'); END;");
}

}  // namespace

TEST_CASE("mutation_seq: a failed insert in a caller-owned transaction leaves no partial row even if the caller commits",
          "[motif_db][mutation_seq][foreign_txn]")
{
    sequence_fixture fix;
    auto const before = fix.seq();
    reject_tag_inserts(fix);

    fix.run("BEGIN");
    auto tagged = make_game("Tagged", "Game");
    tagged.extra_tags = {{"Site", "somewhere"}};
    REQUIRE_FALSE(fix.store.insert(tagged).has_value());  // game row written, tag insert fails
    // The caller ignores the error and commits what it believes is a clean transaction.
    fix.run("COMMIT");
    CHECK(fix.scalar("SELECT COUNT(*) FROM game") == 0);
    CHECK(fix.scalar("SELECT COUNT(*) FROM game_tag") == 0);
    CHECK(fix.seq() == before);

    // The failure did not wedge the connection: once the fault is gone the
    // same insert succeeds in a new caller-owned transaction.
    fix.store.clear_insert_caches();
    fix.run("DROP TRIGGER reject_tag");
    fix.run("BEGIN");
    REQUIRE(fix.store.insert(tagged).has_value());
    fix.run("COMMIT");
    CHECK(fix.scalar("SELECT COUNT(*) FROM game") == 1);
    CHECK(fix.seq() == before + 1U);
}

TEST_CASE("mutation_seq: an insert whose sequence advance fails in a caller-owned transaction commits nothing",
          "[motif_db][mutation_seq][foreign_txn]")
{
    sequence_fixture fix;
    auto const before = fix.seq();
    auto const allocator_before = fix.scalar("SELECT next_game_id FROM bundle_meta");
    reject_sequence_updates(fix);

    fix.run("BEGIN");
    REQUIRE_FALSE(fix.store.insert(make_game("A", "B")).has_value());
    fix.run("COMMIT");
    CHECK(fix.scalar("SELECT COUNT(*) FROM game") == 0);
    CHECK(fix.scalar("SELECT next_game_id FROM bundle_meta") == allocator_before);
    CHECK(fix.seq() == before);

    // The raw path has the same guarantee.
    fix.store.clear_insert_caches();
    REQUIRE(fix.writer.drop_identity_index().has_value());
    fix.run("BEGIN");
    REQUIRE_FALSE(fix.writer.insert_raw(make_game("C", "D")).has_value());
    fix.run("COMMIT");
    CHECK(fix.scalar("SELECT COUNT(*) FROM game") == 0);
    CHECK(fix.seq() == before);
}

TEST_CASE("mutation_seq: a failed removal in a caller-owned transaction does not delete the game on the caller's commit",
          "[motif_db][mutation_seq][foreign_txn]")
{
    sequence_fixture fix;
    auto const inserted = fix.store.insert(make_game());
    REQUIRE(inserted.has_value());
    auto const before = fix.seq();
    reject_sequence_updates(fix);

    fix.run("BEGIN");
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE_FALSE(fix.store.remove(*inserted).has_value());
    fix.run("COMMIT");
    CHECK(fix.scalar("SELECT COUNT(*) FROM game") == 1);
    CHECK(fix.seq() == before);
}

TEST_CASE("mutation_seq: a failed multi-field patch in a caller-owned transaction leaves no partial indexed change",
          "[motif_db][mutation_seq][foreign_txn]")
{
    sequence_fixture fix;
    auto const game = insert_manual(fix, "Patched", "1-0");
    auto const before = fix.seq();
    reject_tag_inserts(fix);

    fix.run("BEGIN");
    auto patch = motif::db::game_patch {};
    patch.result = "0-1";  // indexed field, written first
    patch.notes = "a note";  // game_tag insert fails afterwards
    REQUIRE_FALSE(fix.store.patch_metadata(game, patch).has_value());
    fix.run("COMMIT");
    CHECK(fix.scalar("SELECT COUNT(*) FROM game WHERE result = '0-1'") == 0);
    CHECK(fix.scalar("SELECT COUNT(*) FROM game WHERE result = '1-0'") == 1);
    CHECK(fix.seq() == before);
}

TEST_CASE("mutation_seq: a patch whose sequence advance fails in a caller-owned transaction commits nothing",
          "[motif_db][mutation_seq][foreign_txn]")
{
    sequence_fixture fix;
    auto const game = insert_manual(fix, "Patched", "1-0");
    auto const before = fix.seq();
    reject_sequence_updates(fix);

    fix.run("BEGIN");
    auto patch = motif::db::game_patch {};
    patch.result = "0-1";
    REQUIRE_FALSE(fix.store.patch_metadata(game, patch).has_value());
    fix.run("COMMIT");
    CHECK(fix.scalar("SELECT COUNT(*) FROM game WHERE result = '0-1'") == 0);
    CHECK(fix.seq() == before);
}

TEST_CASE("mutation_seq: a failed dedup in a caller-owned transaction leaves neither deletions nor the recreated index",
          "[motif_db][mutation_seq][foreign_txn][dedup]")
{
    sequence_fixture fix;
    REQUIRE(fix.writer.drop_identity_index().has_value());
    REQUIRE(fix.writer.insert_raw(make_game("Dup", "Game")).has_value());
    REQUIRE(fix.writer.insert_raw(make_game("Dup", "Game")).has_value());
    auto const before = fix.seq();
    reject_sequence_updates(fix);  // fires after the delete and the index recreation

    fix.run("BEGIN");
    REQUIRE_FALSE(fix.writer.deduplicate().has_value());
    fix.run("COMMIT");
    CHECK(fix.scalar("SELECT COUNT(*) FROM game") == 2);
    CHECK(fix.scalar("SELECT COUNT(*) FROM sqlite_master WHERE name = 'game_identity_lookup'") == 0);
    CHECK(fix.seq() == before);
}

TEST_CASE("mutation_seq: a failed dedup poisons a writer batch and rolls back the dedup's own writes", "[motif_db][mutation_seq][dedup]")
{
    sequence_fixture fix;
    REQUIRE(fix.writer.drop_identity_index().has_value());
    // Two exact duplicates plus a distinct game sharing their identity fields:
    // dedup deletes the duplicate, then must move the distinct game's collision
    // slot -- the UPDATE the injected trigger refuses, after the DELETE applied.
    auto distinct = make_game("Dup", "Game");
    distinct.moves = {move_b, move_a};
    REQUIRE(fix.writer.insert_raw(make_game("Dup", "Game")).has_value());
    REQUIRE(fix.writer.insert_raw(make_game("Dup", "Game")).has_value());
    REQUIRE(fix.writer.insert_raw(distinct).has_value());
    // Model a move_hash collision so the distinct game lands in the same
    // identity partition and needs a collision slot.
    fix.run("UPDATE game SET move_hash = (SELECT move_hash FROM game WHERE id = 1)");
    auto const before = fix.seq();
    fix.run(
        "CREATE TRIGGER reject_collision_update BEFORE UPDATE ON game " "BEGIN SELECT RAISE(ABORT, 'injected collision failure'); END;");

    REQUIRE(fix.writer.begin_transaction().has_value());
    REQUIRE_FALSE(fix.writer.deduplicate().has_value());
    // Inside the still-open batch the dedup left nothing behind...
    CHECK(fix.scalar("SELECT COUNT(*) FROM game") == 3);
    CHECK(fix.scalar("SELECT COUNT(*) FROM sqlite_master WHERE name = 'game_identity_lookup'") == 0);
    // ...and the batch is poisoned: it can no longer commit anything.
    REQUIRE_FALSE(fix.writer.commit_transaction().has_value());
    CHECK(sqlite3_get_autocommit(fix.db.get()) != 0);
    CHECK(fix.scalar("SELECT COUNT(*) FROM game") == 3);
    CHECK(fix.seq() == before);
}
