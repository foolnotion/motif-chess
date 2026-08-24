#include <cstdint>
#include <memory>
#include <string>

#include "motif/db/schema.hpp"

#include <fmt/format.h>
#include <sqlite3.h>
#include <tl/expected.hpp>

#include "motif/db/error.hpp"
#include "motif/db/sqlite_util.hpp"

namespace motif::db::schema
{

namespace
{

using motif::db::detail::unique_stmt;

// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto exec(sqlite3* conn, char const* sql) -> result<void>
{
    int const ret = sqlite3_exec(conn, sql, nullptr, nullptr, nullptr);
    if (ret != SQLITE_OK) {
        return tl::unexpected {error_code::io_failure};
    }
    return {};
}

// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto prepare(sqlite3* conn, char const* sql) -> result<unique_stmt>
{
    sqlite3_stmt* raw = nullptr;
    int const ret = sqlite3_prepare_v2(conn, sql, -1, &raw, nullptr);
    if (ret != SQLITE_OK || raw == nullptr) {
        return tl::unexpected {error_code::io_failure};
    }
    return unique_stmt {raw};
}

// language=sql
constexpr char const* ddl = R"sql(
    CREATE TABLE IF NOT EXISTS player (
        id      INTEGER PRIMARY KEY,
        name    TEXT    NOT NULL UNIQUE,
        elo     INTEGER,
        title   TEXT,
        country TEXT
    );

    CREATE TABLE IF NOT EXISTS event (
        id   INTEGER PRIMARY KEY,
        name TEXT NOT NULL UNIQUE,
        site TEXT,
        date TEXT
    );

    CREATE TABLE IF NOT EXISTS tag (
        id   INTEGER PRIMARY KEY,
        name TEXT NOT NULL UNIQUE
    );

    CREATE TABLE IF NOT EXISTS game (
        id            INTEGER PRIMARY KEY,
        white_id      INTEGER NOT NULL REFERENCES player(id),
        black_id      INTEGER NOT NULL REFERENCES player(id),
        event_id      INTEGER REFERENCES event(id),
        date          TEXT,
        result        TEXT NOT NULL,
        eco           TEXT,
        moves         BLOB NOT NULL,
        move_hash     BLOB NOT NULL,
        identity_collision INTEGER NOT NULL DEFAULT 0,
        source_type   TEXT NOT NULL DEFAULT 'imported',
        source_label  TEXT,
        review_status TEXT NOT NULL DEFAULT 'new'
    );

    CREATE TABLE IF NOT EXISTS game_tag (
        game_id INTEGER NOT NULL REFERENCES game(id) ON DELETE CASCADE,
        tag_id  INTEGER NOT NULL REFERENCES tag(id),
        value   TEXT    NOT NULL,
        PRIMARY KEY (game_id, tag_id)
    );

    CREATE TABLE IF NOT EXISTS schema_migrations (
        name       TEXT NOT NULL PRIMARY KEY,
        applied_at TEXT NOT NULL
    );
)sql";

// Keyed on move_hash rather than the raw moves blob: an identity index on
// moves would store every move sequence a second time in the index B-tree,
// doubling that column's disk cost for no benefit over a hash.
// identity_collision distinguishes distinct games that share every other
// indexed field plus a move_hash collision, so the index stays unique
// without a hash collision ever causing a false duplicate rejection.
// language=sql
constexpr char const* identity_index_ddl = R"sql(
    CREATE UNIQUE INDEX IF NOT EXISTS game_identity_lookup ON game(
        white_id,
        black_id,
        COALESCE(event_id, -1),
        COALESCE(date, ''),
        result,
        move_hash,
        identity_collision
    );
)sql";

// language=sql
constexpr auto identity_index_exists_sql = R"sql(
    SELECT 1 FROM sqlite_master
    WHERE type = 'index' AND name = 'game_identity_lookup'
)sql";

}  // namespace

auto initialize(sqlite3* conn) -> result<void>
{
    // WAL mode — required for on-disk databases; returns current mode as text.
    // Acceptable results: "wal" (disk) or "memory" (:memory: connections).
    auto wal_stmt = prepare(conn, "PRAGMA journal_mode = WAL;");
    if (!wal_stmt) {
        return tl::unexpected {wal_stmt.error()};
    }
    if (sqlite3_step(wal_stmt->get()) != SQLITE_ROW) {
        return tl::unexpected {error_code::io_failure};
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto const* mode_raw = reinterpret_cast<char const*>(sqlite3_column_text(wal_stmt->get(), 0));
    if (mode_raw == nullptr) {
        return tl::unexpected {error_code::io_failure};
    }
    std::string const journal_mode {mode_raw};
    if (journal_mode != "wal" && journal_mode != "memory") {
        return tl::unexpected {error_code::io_failure};
    }

    // Foreign key enforcement.
    auto fk_res = exec(conn, "PRAGMA foreign_keys = ON;");
    if (!fk_res) {
        return tl::unexpected {fk_res.error()};
    }
    // Verify enforcement actually took effect (SQLite can silently ignore it).
    auto fk_check = prepare(conn, "PRAGMA foreign_keys;");
    if (!fk_check) {
        return tl::unexpected {fk_check.error()};
    }
    if (sqlite3_step(fk_check->get()) != SQLITE_ROW) {
        return tl::unexpected {error_code::io_failure};
    }
    if (sqlite3_column_int(fk_check->get(), 0) != 1) {
        return tl::unexpected {error_code::io_failure};
    }

    // Create all tables and indexes.
    auto ddl_res = exec(conn, ddl);
    if (!ddl_res) {
        return tl::unexpected {ddl_res.error()};
    }
    if (auto index_res = create_identity_index(conn); !index_res) {
        return tl::unexpected {index_res.error()};
    }

    // Set schema version.
    auto const ver_sql = fmt::format("PRAGMA user_version = {};", current_version);
    return exec(conn, ver_sql.c_str());
}

auto migrate(sqlite3* /*conn*/, std::uint32_t const from_version) -> result<void>
{
    if (from_version > current_version) {
        return tl::unexpected {error_code::schema_mismatch};
    }
    if (from_version == current_version) {
        return {};
    }

    // Every version bump so far has needed either a real per-row hash
    // (v2 -> v3) or the new collision-safe move_hash/identity_collision pair
    // (v3 -> v4), neither of which an exec()-only migration can produce.
    // Refuse rather than silently bump user_version; bundles below
    // current_version must be rebuilt by reimporting.
    return tl::unexpected {error_code::schema_mismatch};
}

auto version(sqlite3* conn) -> result<std::uint32_t>
{
    auto stmt = prepare(conn, "PRAGMA user_version;");
    if (!stmt) {
        return tl::unexpected {stmt.error()};
    }
    if (sqlite3_step(stmt->get()) != SQLITE_ROW) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const val = sqlite3_column_int64(stmt->get(), 0);
    if (val < 0) {
        return tl::unexpected {error_code::io_failure};
    }
    return static_cast<std::uint32_t>(val);
}

auto create_identity_index(sqlite3* conn) -> result<void>
{
    return exec(conn, identity_index_ddl);
}

auto drop_identity_index(sqlite3* conn) -> result<void>
{
    return exec(conn, "DROP INDEX IF EXISTS game_identity_lookup;");
}

auto identity_index_exists(sqlite3* conn) -> result<bool>
{
    auto stmt = prepare(conn, identity_index_exists_sql);
    if (!stmt) {
        return tl::unexpected {stmt.error()};
    }

    auto const step_rc = sqlite3_step(stmt->get());
    if (step_rc == SQLITE_ROW) {
        return true;
    }
    if (step_rc == SQLITE_DONE) {
        return false;
    }
    return tl::unexpected {error_code::io_failure};
}

}  // namespace motif::db::schema
