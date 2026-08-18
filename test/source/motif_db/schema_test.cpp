#include <filesystem>
#include <string>

#include "motif/db/schema.hpp"

#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>

#include "motif/db/error.hpp"

namespace
{

// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto open_memory() -> sqlite3*
{
    sqlite3* conn = nullptr;  // NOLINT(misc-const-correctness) — sqlite3 API
                              // requires mutable ptr
    sqlite3_open(":memory:", &conn);
    return conn;
}

// Opens an on-disk SQLite connection in a temp file.
struct disk_db
{
    std::filesystem::path path;
    sqlite3* conn {nullptr};

    explicit disk_db(std::string const& suffix)
    {
        path = std::filesystem::temp_directory_path() / ("motif_schema_test_" + suffix + ".db");
        sqlite3_open(path.c_str(), &conn);
    }

    ~disk_db()
    {
        if (conn != nullptr) {
            sqlite3_close(conn);
        }
        std::filesystem::remove(path);
    }

    disk_db(disk_db const&) = delete;
    auto operator=(disk_db const&) -> disk_db& = delete;
    disk_db(disk_db&&) = delete;
    auto operator=(disk_db&&) -> disk_db& = delete;
};

}  // namespace

TEST_CASE("schema::initialize on a fresh on-disk database succeeds", "[motif-db][schema]")
{
    disk_db ddb {"init"};
    REQUIRE(ddb.conn != nullptr);

    auto res = motif::db::schema::initialize(ddb.conn);
    REQUIRE(res.has_value());
}

TEST_CASE("schema::initialize is idempotent — second call also succeeds", "[motif-db][schema]")
{
    disk_db ddb {"idempotent"};
    REQUIRE(ddb.conn != nullptr);

    auto first = motif::db::schema::initialize(ddb.conn);
    REQUIRE(first.has_value());

    auto second = motif::db::schema::initialize(ddb.conn);
    REQUIRE(second.has_value());
}

TEST_CASE("schema::version returns k_version after initialize", "[motif-db][schema]")
{
    disk_db ddb {"version"};
    REQUIRE(ddb.conn != nullptr);

    auto init = motif::db::schema::initialize(ddb.conn);
    REQUIRE(init.has_value());

    auto ver = motif::db::schema::version(ddb.conn);
    REQUIRE(ver.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(*ver == motif::db::schema::current_version);
}

TEST_CASE("schema::version on fresh connection returns 0 (not yet initialized)", "[motif-db][schema]")
{
    disk_db ddb {"zero"};
    REQUIRE(ddb.conn != nullptr);

    auto ver = motif::db::schema::version(ddb.conn);
    REQUIRE(ver.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(*ver == 0U);
}

TEST_CASE("schema::migrate refuses a v2 database rather than silently bumping the version", "[motif-db][schema]")
{
    disk_db ddb {"migrate-v2-refused"};
    REQUIRE(ddb.conn != nullptr);

    // v2's moves_hash-free game/ux_game_identity shape, frozen here rather
    // than reused from schema.cpp's current DDL, since the whole point is
    // to simulate a database stuck at the old version.
    // language=sql
    constexpr char const* v2_ddl = R"sql(
        CREATE TABLE player (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, elo INTEGER, title TEXT, country TEXT);
        CREATE TABLE event (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, site TEXT, date TEXT);
        CREATE TABLE tag (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE);
        CREATE TABLE game (
            id INTEGER PRIMARY KEY, white_id INTEGER NOT NULL REFERENCES player(id),
            black_id INTEGER NOT NULL REFERENCES player(id), event_id INTEGER REFERENCES event(id),
            date TEXT, result TEXT NOT NULL, eco TEXT, moves BLOB NOT NULL,
            source_type TEXT NOT NULL DEFAULT 'imported', source_label TEXT, review_status TEXT NOT NULL DEFAULT 'new'
        );
        CREATE UNIQUE INDEX ux_game_identity ON game(white_id, black_id, COALESCE(event_id, -1), COALESCE(date, ''), result, moves);
        CREATE TABLE game_tag (game_id INTEGER NOT NULL REFERENCES game(id) ON DELETE CASCADE, tag_id INTEGER NOT NULL REFERENCES tag(id), value TEXT NOT NULL, PRIMARY KEY (game_id, tag_id));
        CREATE TABLE schema_migrations (name TEXT NOT NULL PRIMARY KEY, applied_at TEXT NOT NULL);
        PRAGMA user_version = 2;
    )sql";
    REQUIRE(sqlite3_exec(ddb.conn, v2_ddl, nullptr, nullptr, nullptr) == SQLITE_OK);

    auto migrate_res = motif::db::schema::migrate(ddb.conn, 2U);
    REQUIRE_FALSE(migrate_res.has_value());
    CHECK(migrate_res.error() == motif::db::error_code::schema_mismatch);

    // Refused, so user_version must be left untouched -- not silently bumped
    // to current_version with rows that don't have a usable moves_hash.
    auto ver = motif::db::schema::version(ddb.conn);
    REQUIRE(ver.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(*ver == 2U);
}

TEST_CASE("schema::migrate refuses a v1 database without mutating it first", "[motif-db][schema]")
{
    disk_db ddb {"migrate-v1-no-partial-mutation"};
    REQUIRE(ddb.conn != nullptr);

    // v1's shape: no source_type/source_label/review_status columns (added
    // in v1->v2) and no moves_hash (added in v2->v3). Regression coverage
    // for a bug where migrate() ran the v1->v2 ALTER TABLE steps before
    // reaching the v3 refusal, leaving a v1 database half-migrated (new
    // columns present, user_version still 1) even though the whole
    // migration was ultimately refused.
    // language=sql
    constexpr char const* v1_ddl = R"sql(
        CREATE TABLE player (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, elo INTEGER, title TEXT, country TEXT);
        CREATE TABLE event (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, site TEXT, date TEXT);
        CREATE TABLE tag (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE);
        CREATE TABLE game (
            id INTEGER PRIMARY KEY, white_id INTEGER NOT NULL REFERENCES player(id),
            black_id INTEGER NOT NULL REFERENCES player(id), event_id INTEGER REFERENCES event(id),
            date TEXT, result TEXT NOT NULL, eco TEXT, moves BLOB NOT NULL
        );
        CREATE UNIQUE INDEX ux_game_identity ON game(white_id, black_id, COALESCE(event_id, -1), COALESCE(date, ''), result, moves);
        CREATE TABLE game_tag (game_id INTEGER NOT NULL REFERENCES game(id) ON DELETE CASCADE, tag_id INTEGER NOT NULL REFERENCES tag(id), value TEXT NOT NULL, PRIMARY KEY (game_id, tag_id));
        CREATE TABLE schema_migrations (name TEXT NOT NULL PRIMARY KEY, applied_at TEXT NOT NULL);
        PRAGMA user_version = 1;
    )sql";
    REQUIRE(sqlite3_exec(ddb.conn, v1_ddl, nullptr, nullptr, nullptr) == SQLITE_OK);

    auto migrate_res = motif::db::schema::migrate(ddb.conn, 1U);
    REQUIRE_FALSE(migrate_res.has_value());
    CHECK(migrate_res.error() == motif::db::error_code::schema_mismatch);

    auto ver = motif::db::schema::version(ddb.conn);
    REQUIRE(ver.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(*ver == 1U);

    // The v1->v2 columns must not have been added by the refused attempt.
    auto* stmt = static_cast<sqlite3_stmt*>(nullptr);
    REQUIRE(sqlite3_prepare_v2(ddb.conn, "SELECT source_type FROM game LIMIT 0", -1, &stmt, nullptr) != SQLITE_OK);
    sqlite3_finalize(stmt);

    // A second attempt must fail the same way, not degrade into a generic
    // io_failure from a duplicate-column ALTER TABLE re-run.
    auto second_migrate_res = motif::db::schema::migrate(ddb.conn, 1U);
    REQUIRE_FALSE(second_migrate_res.has_value());
    CHECK(second_migrate_res.error() == motif::db::error_code::schema_mismatch);
}

TEST_CASE("schema::initialize on :memory: succeeds (WAL falls back to memory mode)", "[motif-db][schema]")
{
    sqlite3* conn = open_memory();
    REQUIRE(conn != nullptr);

    auto res = motif::db::schema::initialize(conn);
    // WAL is not applicable to :memory: but initialize must not fail.
    REQUIRE(res.has_value());

    sqlite3_close(conn);
}
