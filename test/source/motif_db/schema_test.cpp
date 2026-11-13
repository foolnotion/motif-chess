#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "motif/db/schema.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <sqlite3.h>

#include "motif/db/error.hpp"
#include "motif/db/game_store.hpp"
#include "motif/db/game_writer.hpp"
#include "motif/db/types.hpp"

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

    // v1's shape: no source_type/source_label/review_status, no moves_hash.
    // Regression coverage for migrate() once running v1->v2's ALTER TABLE
    // before the v3 refusal, leaving a half-migrated v1 database.
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

TEST_CASE("schema::migrate refuses a v3 database rather than silently bumping the version", "[motif-db][schema]")
{
    disk_db ddb {"migrate-v3-refused"};
    REQUIRE(ddb.conn != nullptr);

    // v3's moves_hash-keyed shape, frozen here rather than reused from
    // schema.cpp's current DDL, since the whole point is to simulate a
    // database stuck at the old version.
    // language=sql
    constexpr char const* v3_ddl = R"sql(
        CREATE TABLE player (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, elo INTEGER, title TEXT, country TEXT);
        CREATE TABLE event (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, site TEXT, date TEXT);
        CREATE TABLE tag (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE);
        CREATE TABLE game (
            id INTEGER PRIMARY KEY, white_id INTEGER NOT NULL REFERENCES player(id),
            black_id INTEGER NOT NULL REFERENCES player(id), event_id INTEGER REFERENCES event(id),
            date TEXT, result TEXT NOT NULL, eco TEXT, moves BLOB NOT NULL, moves_hash INTEGER NOT NULL,
            source_type TEXT NOT NULL DEFAULT 'imported', source_label TEXT, review_status TEXT NOT NULL DEFAULT 'new'
        );
        CREATE UNIQUE INDEX ux_game_identity ON game(white_id, black_id, COALESCE(event_id, -1), COALESCE(date, ''), result, moves_hash);
        CREATE TABLE game_tag (game_id INTEGER NOT NULL REFERENCES game(id) ON DELETE CASCADE, tag_id INTEGER NOT NULL REFERENCES tag(id), value TEXT NOT NULL, PRIMARY KEY (game_id, tag_id));
        CREATE TABLE schema_migrations (name TEXT NOT NULL PRIMARY KEY, applied_at TEXT NOT NULL);
        PRAGMA user_version = 3;
    )sql";
    REQUIRE(sqlite3_exec(ddb.conn, v3_ddl, nullptr, nullptr, nullptr) == SQLITE_OK);

    auto migrate_res = motif::db::schema::migrate(ddb.conn, 3U);
    REQUIRE_FALSE(migrate_res.has_value());
    CHECK(migrate_res.error() == motif::db::error_code::schema_mismatch);

    // Refused, so user_version must be left untouched -- not silently bumped
    // to current_version with rows that don't have a usable move_hash.
    auto ver = motif::db::schema::version(ddb.conn);
    REQUIRE(ver.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(*ver == 3U);
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

namespace
{

// v5's shape, frozen here rather than reused from schema.cpp's current DDL,
// since the whole point is to simulate a database stuck at the old version.
// language=sql
constexpr char const* v5_ddl = R"sql(
    CREATE TABLE player (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, elo INTEGER, title TEXT, country TEXT);
    CREATE TABLE event (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, site TEXT, date TEXT);
    CREATE TABLE tag (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE);
    CREATE TABLE game (
        id INTEGER PRIMARY KEY,
        white_id INTEGER NOT NULL REFERENCES player(id),
        black_id INTEGER NOT NULL REFERENCES player(id),
        event_id INTEGER REFERENCES event(id),
        date TEXT,
        result TEXT NOT NULL,
        eco TEXT,
        moves BLOB NOT NULL,
        move_hash BLOB NOT NULL,
        identity_collision INTEGER NOT NULL DEFAULT 0,
        source_type TEXT NOT NULL DEFAULT 'imported',
        source_label TEXT,
        review_status TEXT NOT NULL DEFAULT 'new',
        white_elo INTEGER,
        black_elo INTEGER
    );
    CREATE UNIQUE INDEX game_identity_lookup ON game(
        white_id, black_id, COALESCE(event_id, -1), COALESCE(date, ''), result, move_hash, identity_collision);
    CREATE TABLE game_tag (
        game_id INTEGER NOT NULL REFERENCES game(id) ON DELETE CASCADE,
        tag_id INTEGER NOT NULL REFERENCES tag(id),
        value TEXT NOT NULL,
        PRIMARY KEY (game_id, tag_id));
    CREATE TABLE schema_migrations (name TEXT NOT NULL PRIMARY KEY, applied_at TEXT NOT NULL);
    PRAGMA user_version = 5;
)sql";

// Ids 1, 2 and 5: the gap at 3-4 stands for deleted games. A v5 database
// cannot remember a deleted *highest* id, so the migrated allocator can only
// start above the highest surviving one.
// language=sql
constexpr char const* v5_rows = R"sql(
    INSERT INTO player(id, name) VALUES (1, 'Alpha'), (2, 'Beta');
    INSERT INTO tag(id, name) VALUES (1, 'Round');
    INSERT INTO game(id, white_id, black_id, result, moves, move_hash, white_elo, black_elo) VALUES
        (1, 1, 2, '1-0',     x'0102', x'01', 1500, 1600),
        (2, 2, 1, '0-1',     x'0304', x'02', NULL, NULL),
        (5, 1, 2, '1/2-1/2', x'',     x'03', 1700, NULL);
    INSERT INTO game_tag(game_id, tag_id, value) VALUES (5, 1, '7');
)sql";

void run_sql(sqlite3* conn, char const* sql)
{
    char* err = nullptr;
    auto const status = sqlite3_exec(conn, sql, nullptr, nullptr, &err);
    INFO((err != nullptr ? err : "no error"));
    sqlite3_free(err);
    REQUIRE(status == SQLITE_OK);
}

[[nodiscard]] auto scalar_text(sqlite3* conn, char const* sql) -> std::string
{
    sqlite3_stmt* raw = nullptr;
    REQUIRE(sqlite3_prepare_v2(conn, sql, -1, &raw, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(raw) == SQLITE_ROW);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto const* text = reinterpret_cast<char const*>(sqlite3_column_text(raw, 0));
    std::string value {text != nullptr ? text : ""};
    sqlite3_finalize(raw);
    return value;
}

[[nodiscard]] auto scalar_int(sqlite3* conn, char const* sql) -> std::int64_t
{
    return std::stoll(scalar_text(conn, sql));
}

// Every row of game as one string, for before/after comparison. Excludes
// content_revision, which the migration adds.
[[nodiscard]] auto game_snapshot(sqlite3* conn) -> std::string
{
    return scalar_text(conn,
                       "SELECT group_concat(id || ':' || white_id || ':' || black_id || ':' || result || ':' || hex(moves) || ':' "
                       "|| hex(move_hash) || ':' || COALESCE(white_elo, 'n') || ':' || COALESCE(black_elo, 'n'), '|') "
                       "FROM (SELECT * FROM game ORDER BY id)");
}

// name|type|notnull|default|pk for each column, sorted by name.
[[nodiscard]] auto table_columns(sqlite3* conn, std::string const& table) -> std::vector<std::string>
{
    constexpr auto pragma_column_count = 5;
    std::vector<std::string> columns;
    sqlite3_stmt* raw = nullptr;
    auto const sql = "SELECT name, type, \"notnull\", COALESCE(dflt_value, ''), pk FROM pragma_table_info('" + table + "')";
    REQUIRE(sqlite3_prepare_v2(conn, sql.c_str(), -1, &raw, nullptr) == SQLITE_OK);
    while (sqlite3_step(raw) == SQLITE_ROW) {
        std::string column;
        for (int col = 0; col < pragma_column_count; ++col) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            column += reinterpret_cast<char const*>(sqlite3_column_text(raw, col));
            column += '|';
        }
        columns.push_back(std::move(column));
    }
    sqlite3_finalize(raw);
    std::ranges::sort(columns);
    return columns;
}

[[nodiscard]] auto make_v5_game() -> motif::db::game
{
    return motif::db::game {
        .white = {.name = "Alpha", .elo = std::nullopt, .title = std::nullopt, .country = std::nullopt},
        .black = {.name = "Beta", .elo = std::nullopt, .title = std::nullopt, .country = std::nullopt},
        .event_details = std::nullopt,
        .date = std::nullopt,
        .result = "1-0",
        .eco = std::nullopt,
        .moves = {1U, 2U, 3U},
        .extra_tags = {},
        .provenance = {},
    };
}

}  // namespace

TEST_CASE("schema::migrate upgrades a v5 database in place", "[motif-db][schema][migration]")
{
    disk_db ddb {"migrate-v5"};
    REQUIRE(ddb.conn != nullptr);
    run_sql(ddb.conn, v5_ddl);
    run_sql(ddb.conn, v5_rows);
    auto const before = game_snapshot(ddb.conn);

    auto const migrate_res = motif::db::schema::migrate(ddb.conn, 5U);
    REQUIRE(migrate_res.has_value());

    auto const ver = motif::db::schema::version(ddb.conn);
    REQUIRE(ver.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(*ver == motif::db::schema::current_version);
    CHECK(scalar_int(ddb.conn, "SELECT COUNT(*) FROM game") == 3);
    CHECK(scalar_int(ddb.conn, "SELECT COUNT(*) FROM game WHERE content_revision = 1") == 3);
    CHECK(scalar_int(ddb.conn, "SELECT COUNT(*) FROM bundle_meta") == 1);
    CHECK(scalar_int(ddb.conn, "SELECT next_game_id FROM bundle_meta WHERE id = 1") == 6);
    CHECK(game_snapshot(ddb.conn) == before);
    CHECK(scalar_int(ddb.conn, "SELECT COUNT(*) FROM game_tag") == 1);

    // The migrated database allocates from the persisted counter.
    motif::db::game_store store {ddb.conn};
    auto& writer = store.writer();
    auto const inserted = writer.insert(make_v5_game());
    REQUIRE(inserted.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(inserted->value == 6U);
    CHECK(scalar_int(ddb.conn, "SELECT next_game_id FROM bundle_meta WHERE id = 1") == 7);
}

TEST_CASE("schema::migrate run twice is a no-op", "[motif-db][schema][migration]")
{
    disk_db ddb {"migrate-twice"};
    REQUIRE(ddb.conn != nullptr);
    run_sql(ddb.conn, v5_ddl);
    run_sql(ddb.conn, v5_rows);

    REQUIRE(motif::db::schema::migrate(ddb.conn, 5U).has_value());
    auto const after_first = game_snapshot(ddb.conn);

    // Once with the up-to-date version, and once with a stale caller-supplied
    // v5 (a second process may have finished the upgrade first).
    REQUIRE(motif::db::schema::migrate(ddb.conn, 6U).has_value());
    REQUIRE(motif::db::schema::migrate(ddb.conn, 5U).has_value());

    auto const ver = motif::db::schema::version(ddb.conn);
    REQUIRE(ver.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(*ver == motif::db::schema::current_version);
    CHECK(game_snapshot(ddb.conn) == after_first);
    CHECK(scalar_int(ddb.conn, "SELECT COUNT(*) FROM bundle_meta") == 1);
    CHECK(scalar_int(ddb.conn, "SELECT next_game_id FROM bundle_meta WHERE id = 1") == 6);
    CHECK(scalar_int(ddb.conn, "SELECT COUNT(*) FROM game WHERE content_revision = 1") == 3);
}

TEST_CASE("schema::migrate finishes a v5 database whose column and table already exist", "[motif-db][schema][migration]")
{
    disk_db ddb {"migrate-resume"};
    REQUIRE(ddb.conn != nullptr);
    run_sql(ddb.conn, v5_ddl);
    run_sql(ddb.conn, v5_rows);
    // As if an earlier run's steps had been applied without reaching the
    // user_version bump, with an allocator row that is too low.
    run_sql(ddb.conn, "ALTER TABLE game ADD COLUMN content_revision INTEGER NOT NULL DEFAULT 1;");
    run_sql(ddb.conn,
            "CREATE TABLE bundle_meta (id INTEGER PRIMARY KEY CHECK (id = 1), next_game_id INTEGER NOT NULL);"
            "INSERT INTO bundle_meta(id, next_game_id) VALUES (1, 2);");

    REQUIRE(motif::db::schema::migrate(ddb.conn, 5U).has_value());

    auto const ver = motif::db::schema::version(ddb.conn);
    REQUIRE(ver.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(*ver == motif::db::schema::current_version);
    // The allocator is raised above the highest existing id, never lowered.
    CHECK(scalar_int(ddb.conn, "SELECT next_game_id FROM bundle_meta WHERE id = 1") == 6);
}

TEST_CASE("schema::migrate rolls back completely when a step fails", "[motif-db][schema][migration]")
{
    disk_db ddb {"migrate-atomic"};
    REQUIRE(ddb.conn != nullptr);
    run_sql(ddb.conn, v5_ddl);
    run_sql(ddb.conn, v5_rows);
    // An incompatible bundle_meta makes the allocator seeding fail after the
    // ALTER TABLE already succeeded inside the transaction.
    run_sql(ddb.conn, "CREATE TABLE bundle_meta (unrelated INTEGER);");

    auto const migrate_res = motif::db::schema::migrate(ddb.conn, 5U);
    REQUIRE_FALSE(migrate_res.has_value());
    CHECK(migrate_res.error() == motif::db::error_code::io_failure);

    auto const ver = motif::db::schema::version(ddb.conn);
    REQUIRE(ver.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(*ver == 5U);
    CHECK(scalar_int(ddb.conn, "SELECT COUNT(*) FROM pragma_table_info('game') WHERE name = 'content_revision'") == 0);
    CHECK(sqlite3_get_autocommit(ddb.conn) != 0);
}

TEST_CASE("schema::migrate refuses a v4 database without mutating it", "[motif-db][schema][migration]")
{
    disk_db ddb {"migrate-v4-refused"};
    REQUIRE(ddb.conn != nullptr);

    // v4: v5 without the per-game elo columns.
    // language=sql
    constexpr char const* v4_ddl = R"sql(
        CREATE TABLE player (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, elo INTEGER, title TEXT, country TEXT);
        CREATE TABLE event (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, site TEXT, date TEXT);
        CREATE TABLE tag (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE);
        CREATE TABLE game (
            id INTEGER PRIMARY KEY, white_id INTEGER NOT NULL REFERENCES player(id),
            black_id INTEGER NOT NULL REFERENCES player(id), event_id INTEGER REFERENCES event(id),
            date TEXT, result TEXT NOT NULL, eco TEXT, moves BLOB NOT NULL, move_hash BLOB NOT NULL,
            identity_collision INTEGER NOT NULL DEFAULT 0,
            source_type TEXT NOT NULL DEFAULT 'imported', source_label TEXT, review_status TEXT NOT NULL DEFAULT 'new'
        );
        CREATE TABLE game_tag (game_id INTEGER NOT NULL REFERENCES game(id) ON DELETE CASCADE, tag_id INTEGER NOT NULL REFERENCES tag(id), value TEXT NOT NULL, PRIMARY KEY (game_id, tag_id));
        CREATE TABLE schema_migrations (name TEXT NOT NULL PRIMARY KEY, applied_at TEXT NOT NULL);
        PRAGMA user_version = 4;
    )sql";
    run_sql(ddb.conn, v4_ddl);

    auto const migrate_res = motif::db::schema::migrate(ddb.conn, 4U);
    REQUIRE_FALSE(migrate_res.has_value());
    CHECK(migrate_res.error() == motif::db::error_code::schema_mismatch);

    auto const ver = motif::db::schema::version(ddb.conn);
    REQUIRE(ver.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(*ver == 4U);
    CHECK(scalar_int(ddb.conn, "SELECT COUNT(*) FROM sqlite_master WHERE name = 'bundle_meta'") == 0);
    CHECK(scalar_int(ddb.conn, "SELECT COUNT(*) FROM pragma_table_info('game') WHERE name = 'content_revision'") == 0);
}

TEST_CASE("schema::migrate refuses to run inside a caller's transaction", "[motif-db][schema][migration]")
{
    disk_db ddb {"migrate-in-txn"};
    REQUIRE(ddb.conn != nullptr);
    run_sql(ddb.conn, v5_ddl);

    run_sql(ddb.conn, "BEGIN;");
    auto const migrate_res = motif::db::schema::migrate(ddb.conn, 5U);
    REQUIRE_FALSE(migrate_res.has_value());
    CHECK(migrate_res.error() == motif::db::error_code::invalid_argument);
    run_sql(ddb.conn, "ROLLBACK;");

    auto const ver = motif::db::schema::version(ddb.conn);
    REQUIRE(ver.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(*ver == 5U);
}

TEST_CASE("schema::migrate refuses a v5 database holding a game id outside 1..4294967295 and leaves it untouched",
          "[motif-db][schema][migration]")
{
    // Each case is a v5 database that is otherwise valid plus one offending row.
    auto const bad_row = GENERATE(table<char const*, char const*>({
        {"zero", "0"},
        {"negative", "-3"},
        {"just above uint32 max", "4294967296"},
        {"int64 max", "9223372036854775807"},
    }));
    INFO(std::get<0>(bad_row));

    disk_db ddb {"migrate-bad-id"};
    REQUIRE(ddb.conn != nullptr);
    run_sql(ddb.conn, v5_ddl);
    run_sql(ddb.conn, v5_rows);
    auto const insert_bad = std::string {"INSERT INTO game(id, white_id, black_id, result, moves, move_hash) VALUES ("}
        + std::get<1>(bad_row) + ", 1, 2, '1-0', x'0506', x'09');";
    run_sql(ddb.conn, insert_bad.c_str());
    auto const before = game_snapshot(ddb.conn);

    auto const migrate_res = motif::db::schema::migrate(ddb.conn, 5U);
    REQUIRE_FALSE(migrate_res.has_value());
    CHECK(migrate_res.error() == motif::db::error_code::schema_mismatch);

    // Database untouched: still v5, no ALTER, no allocator table, no open
    // transaction, every row as it was.
    auto const ver = motif::db::schema::version(ddb.conn);
    REQUIRE(ver.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(*ver == 5U);
    CHECK(scalar_int(ddb.conn, "SELECT COUNT(*) FROM pragma_table_info('game') WHERE name = 'content_revision'") == 0);
    CHECK(scalar_int(ddb.conn, "SELECT COUNT(*) FROM sqlite_master WHERE name = 'bundle_meta'") == 0);
    CHECK(sqlite3_get_autocommit(ddb.conn) != 0);
    CHECK(game_snapshot(ddb.conn) == before);
}

TEST_CASE("schema::migrate accepts the boundary ids 1 and 4294967295 and seeds past the maximum", "[motif-db][schema][migration]")
{
    disk_db ddb {"migrate-boundary-ids"};
    REQUIRE(ddb.conn != nullptr);
    run_sql(ddb.conn, v5_ddl);
    run_sql(ddb.conn,
            "INSERT INTO player(id, name) VALUES (1, 'Alpha'), (2, 'Beta');"
            "INSERT INTO game(id, white_id, black_id, result, moves, move_hash) VALUES"
            " (1, 1, 2, '1-0', x'0102', x'01'), (4294967295, 2, 1, '0-1', x'0304', x'02');");

    REQUIRE(motif::db::schema::migrate(ddb.conn, 5U).has_value());
    CHECK(scalar_int(ddb.conn, "SELECT next_game_id FROM bundle_meta WHERE id = 1") == 4294967296LL);
}

TEST_CASE("schema::initialize and a migrated v5 database agree on the tables the PR touches", "[motif-db][schema][migration]")
{
    disk_db fresh {"equiv-fresh"};
    disk_db migrated {"equiv-migrated"};
    REQUIRE(fresh.conn != nullptr);
    REQUIRE(migrated.conn != nullptr);

    REQUIRE(motif::db::schema::initialize(fresh.conn).has_value());
    run_sql(migrated.conn, v5_ddl);
    run_sql(migrated.conn, v5_rows);
    REQUIRE(motif::db::schema::migrate(migrated.conn, 5U).has_value());

    CHECK(table_columns(fresh.conn, "game") == table_columns(migrated.conn, "game"));
    CHECK(table_columns(fresh.conn, "bundle_meta") == table_columns(migrated.conn, "bundle_meta"));
    CHECK(table_columns(fresh.conn, "game_tag") == table_columns(migrated.conn, "game_tag"));
    CHECK_FALSE(table_columns(fresh.conn, "bundle_meta").empty());

    CHECK(scalar_int(fresh.conn, "SELECT next_game_id FROM bundle_meta WHERE id = 1") == 1);
    CHECK(scalar_int(fresh.conn, "SELECT COUNT(*) FROM bundle_meta") == 1);
}

TEST_CASE("schema::create_tables is idempotent and never lowers the allocator", "[motif-db][schema]")
{
    sqlite3* conn = open_memory();
    REQUIRE(conn != nullptr);

    REQUIRE(motif::db::schema::create_tables(conn).has_value());
    run_sql(conn, "UPDATE bundle_meta SET next_game_id = 50 WHERE id = 1;");
    REQUIRE(motif::db::schema::create_tables(conn).has_value());
    CHECK(scalar_int(conn, "SELECT next_game_id FROM bundle_meta WHERE id = 1") == 50);
    CHECK(scalar_int(conn, "SELECT COUNT(*) FROM bundle_meta") == 1);
    // The single-row CHECK rejects a second allocator row.
    CHECK(sqlite3_exec(conn, "INSERT INTO bundle_meta(id, next_game_id) VALUES (2, 1)", nullptr, nullptr, nullptr) != SQLITE_OK);

    sqlite3_close(conn);
}

TEST_CASE("schema::create_tables on an existing database never reseeds a missing mutation_state singleton",
          "[motif-db][schema][mutation_seq]")
{
    sqlite3* conn = open_memory();
    REQUIRE(conn != nullptr);

    REQUIRE(motif::db::schema::create_tables(conn).has_value());
    run_sql(conn, "UPDATE mutation_state SET mutation_seq = 7;");
    // Idempotent while the singleton is intact: no reseed, value preserved.
    REQUIRE(motif::db::schema::create_tables(conn).has_value());
    CHECK(scalar_int(conn, "SELECT mutation_seq FROM mutation_state") == 7);

    // A vanished singleton is corruption. Reseeding at 0 would let a snapshot
    // that covered an earlier sequence look current again.
    run_sql(conn, "DELETE FROM mutation_state;");
    auto const reseeded = motif::db::schema::create_tables(conn);
    REQUIRE_FALSE(reseeded.has_value());
    CHECK(reseeded.error() == motif::db::error_code::io_failure);
    CHECK(scalar_int(conn, "SELECT COUNT(*) FROM mutation_state") == 0);

    sqlite3_close(conn);
}

TEST_CASE("schema::create_tables refuses an existing pre-v7 database instead of seeding it", "[motif-db][schema][mutation_seq]")
{
    sqlite3* conn = open_memory();
    REQUIRE(conn != nullptr);
    REQUIRE(motif::db::schema::create_tables(conn).has_value());
    run_sql(conn, "DROP TABLE mutation_state; DROP TABLE published_snapshot;");

    auto const refused = motif::db::schema::create_tables(conn);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == motif::db::error_code::schema_mismatch);
    CHECK(scalar_int(conn, "SELECT COUNT(*) FROM sqlite_master WHERE name = 'mutation_state'") == 0);

    sqlite3_close(conn);
}

TEST_CASE("schema::migrate rejects a version newer than current_version", "[motif-db][schema][migration]")
{
    disk_db ddb {"migrate-newer"};
    REQUIRE(ddb.conn != nullptr);
    auto const migrate_res = motif::db::schema::migrate(ddb.conn, motif::db::schema::current_version + 1U);
    REQUIRE_FALSE(migrate_res.has_value());
    CHECK(migrate_res.error() == motif::db::error_code::schema_mismatch);
}

namespace
{

// A well-formed unbuilt snapshot as the manager would hand to the schema layer.
// The schema module never interprets the payload, only stores it.
[[nodiscard]] auto make_bootstrap() -> motif::db::schema::snapshot_bootstrap
{
    return {.payload_version = 1, .payload_json = "{\"bundle\":{}}", .payload_checksum = "0123456789abcdef"};
}

// Turns an initialized v7 database into the v6 shape (no mutation_state, no
// published_snapshot) so the v6 -> v7 step can be exercised in isolation.
void downgrade_to_v6(sqlite3* conn)
{
    run_sql(conn, "DROP TABLE mutation_state; DROP TABLE published_snapshot; PRAGMA user_version = 6;");
}

}  // namespace

TEST_CASE("schema::initialize creates the mutation sequence at 0 and the unbuilt snapshot atomically", "[motif-db][schema]")
{
    disk_db ddb {"init-v7"};
    REQUIRE(ddb.conn != nullptr);
    auto const bootstrap = make_bootstrap();
    REQUIRE(motif::db::schema::initialize(ddb.conn, &bootstrap).has_value());

    CHECK(motif::db::schema::current_version == 7U);
    CHECK(scalar_int(ddb.conn, "SELECT COUNT(*) FROM mutation_state") == 1);
    CHECK(scalar_int(ddb.conn, "SELECT mutation_seq FROM mutation_state WHERE id = 1") == 0);
    CHECK(scalar_int(ddb.conn, "SELECT COUNT(*) FROM published_snapshot") == 1);
    CHECK(scalar_int(ddb.conn, "SELECT publication_generation FROM published_snapshot") == 0);
    CHECK(scalar_int(ddb.conn, "SELECT covered_mutation_seq IS NULL FROM published_snapshot") == 1);
    CHECK(scalar_int(ddb.conn, "SELECT canonical_game_count FROM published_snapshot") == 0);
    CHECK(scalar_text(ddb.conn, "SELECT payload_json FROM published_snapshot") == bootstrap.payload_json);
    CHECK(sqlite3_get_autocommit(ddb.conn) != 0);

    // A second call neither reseeds the sequence nor replaces the snapshot.
    run_sql(ddb.conn, "UPDATE mutation_state SET mutation_seq = 9; UPDATE published_snapshot SET publication_generation = 4;");
    REQUIRE(motif::db::schema::initialize(ddb.conn, &bootstrap).has_value());
    CHECK(scalar_int(ddb.conn, "SELECT mutation_seq FROM mutation_state") == 9);
    CHECK(scalar_int(ddb.conn, "SELECT publication_generation FROM published_snapshot") == 4);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("schema: the singleton tables reject a second row and malformed values", "[motif-db][schema]")
{
    sqlite3* conn = open_memory();
    REQUIRE(conn != nullptr);
    auto const bootstrap = make_bootstrap();
    REQUIRE(motif::db::schema::initialize(conn, &bootstrap).has_value());

    auto const rejected = [conn](char const* sql) -> bool { return sqlite3_exec(conn, sql, nullptr, nullptr, nullptr) != SQLITE_OK; };
    CHECK(rejected("INSERT INTO mutation_state(id, mutation_seq) VALUES (2, 0)"));
    CHECK(rejected("UPDATE mutation_state SET mutation_seq = -1"));
    CHECK(rejected("UPDATE mutation_state SET mutation_seq = 'x'"));
    CHECK(rejected("INSERT INTO published_snapshot VALUES (2, 0, NULL, 0, 1, 'x', '0123456789abcdef')"));
    CHECK(rejected("UPDATE published_snapshot SET payload_checksum = 'NOT-HEX-AT-ALL!!'"));
    CHECK(rejected("UPDATE published_snapshot SET payload_checksum = 'abc'"));
    CHECK(rejected("UPDATE published_snapshot SET covered_mutation_seq = -1"));
    CHECK(rejected("UPDATE published_snapshot SET payload_version = 0"));
    CHECK_FALSE(rejected("UPDATE published_snapshot SET covered_mutation_seq = 3"));

    sqlite3_close(conn);
}

TEST_CASE("schema::migrate upgrades v6 to v7 seeding the sequence and an unbuilt snapshot", "[motif-db][schema][migration]")
{
    disk_db ddb {"migrate-v6"};
    REQUIRE(ddb.conn != nullptr);
    REQUIRE(motif::db::schema::initialize(ddb.conn).has_value());
    run_sql(ddb.conn,
            "INSERT INTO player(id, name) VALUES (1, 'Alpha'), (2, 'Beta');"
            "INSERT INTO game(id, white_id, black_id, result, moves, move_hash) VALUES (7, 1, 2, '1-0', x'0102', x'01');"
            "UPDATE bundle_meta SET next_game_id = 8;");
    downgrade_to_v6(ddb.conn);
    auto const before = game_snapshot(ddb.conn);

    auto const bootstrap = make_bootstrap();
    REQUIRE(motif::db::schema::migrate(ddb.conn, 6U, &bootstrap).has_value());

    auto const ver = motif::db::schema::version(ddb.conn);
    REQUIRE(ver.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(*ver == motif::db::schema::current_version);
    // Historical v6 state is not a mutation count: the sequence starts fresh.
    CHECK(scalar_int(ddb.conn, "SELECT mutation_seq FROM mutation_state") == 0);
    CHECK(scalar_int(ddb.conn, "SELECT COUNT(*) FROM published_snapshot") == 1);
    CHECK(scalar_int(ddb.conn, "SELECT covered_mutation_seq IS NULL FROM published_snapshot") == 1);
    CHECK(scalar_int(ddb.conn, "SELECT publication_generation FROM published_snapshot") == 0);
    // Allocator and content revisions are untouched.
    CHECK(scalar_int(ddb.conn, "SELECT next_game_id FROM bundle_meta") == 8);
    CHECK(scalar_int(ddb.conn, "SELECT COUNT(*) FROM game WHERE content_revision = 1") == 1);
    CHECK(game_snapshot(ddb.conn) == before);

    // Migrating again, with a stale caller version, neither reseeds nor re-adopts.
    run_sql(ddb.conn, "UPDATE mutation_state SET mutation_seq = 5; UPDATE published_snapshot SET publication_generation = 2;");
    REQUIRE(motif::db::schema::migrate(ddb.conn, 6U, &bootstrap).has_value());
    CHECK(scalar_int(ddb.conn, "SELECT mutation_seq FROM mutation_state") == 5);
    CHECK(scalar_int(ddb.conn, "SELECT publication_generation FROM published_snapshot") == 2);
}

TEST_CASE("schema::migrate v6 to v7 without a bootstrap seeds only the sequence", "[motif-db][schema][migration]")
{
    disk_db ddb {"migrate-v6-nobundle"};
    REQUIRE(ddb.conn != nullptr);
    REQUIRE(motif::db::schema::initialize(ddb.conn).has_value());
    downgrade_to_v6(ddb.conn);

    REQUIRE(motif::db::schema::migrate(ddb.conn, 6U).has_value());
    CHECK(scalar_int(ddb.conn, "SELECT COUNT(*) FROM mutation_state") == 1);
    CHECK(scalar_int(ddb.conn, "SELECT COUNT(*) FROM published_snapshot") == 0);
}

TEST_CASE("schema::migrate v6 to v7 rolls back completely when the snapshot row is refused", "[motif-db][schema][migration]")
{
    disk_db ddb {"migrate-v6-rollback"};
    REQUIRE(ddb.conn != nullptr);
    REQUIRE(motif::db::schema::initialize(ddb.conn).has_value());
    downgrade_to_v6(ddb.conn);

    // A checksum that violates the table's CHECK fails the insert after the
    // mutation_state table was already created inside the transaction.
    auto bad = make_bootstrap();
    bad.payload_checksum = "not-a-checksum";
    auto const migrate_res = motif::db::schema::migrate(ddb.conn, 6U, &bad);
    REQUIRE_FALSE(migrate_res.has_value());

    auto const ver = motif::db::schema::version(ddb.conn);
    REQUIRE(ver.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(*ver == 6U);
    CHECK(scalar_int(ddb.conn, "SELECT COUNT(*) FROM sqlite_master WHERE name IN ('mutation_state', 'published_snapshot')") == 0);
    CHECK(sqlite3_get_autocommit(ddb.conn) != 0);

    // The same database then migrates cleanly.
    auto const good = make_bootstrap();
    REQUIRE(motif::db::schema::migrate(ddb.conn, 6U, &good).has_value());
}

TEST_CASE("schema::migrate upgrades v5 straight to v7 in one transaction", "[motif-db][schema][migration]")
{
    disk_db ddb {"migrate-v5-to-v7"};
    REQUIRE(ddb.conn != nullptr);
    run_sql(ddb.conn, v5_ddl);
    run_sql(ddb.conn, v5_rows);
    auto const bootstrap = make_bootstrap();
    REQUIRE(motif::db::schema::migrate(ddb.conn, 5U, &bootstrap).has_value());
    CHECK(scalar_int(ddb.conn, "SELECT next_game_id FROM bundle_meta") == 6);
    CHECK(scalar_int(ddb.conn, "SELECT mutation_seq FROM mutation_state") == 0);
    CHECK(scalar_int(ddb.conn, "SELECT COUNT(*) FROM published_snapshot") == 1);
}
