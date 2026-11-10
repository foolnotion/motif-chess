#include <cstdint>
#include <memory>
#include <string>
#include <utility>

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
        review_status TEXT NOT NULL DEFAULT 'new',
        white_elo     INTEGER,
        black_elo     INTEGER,
        content_revision INTEGER NOT NULL DEFAULT 1
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

// The durable game-ID allocator: exactly one row whose next_game_id is
// strictly greater than every game ID ever committed and never decreases.
// Deleted IDs are therefore never reissued (a bare INTEGER PRIMARY KEY would
// reuse the highest deleted rowid).
// language=sql
constexpr char const* bundle_meta_ddl = R"sql(
    CREATE TABLE IF NOT EXISTS bundle_meta (
        id           INTEGER PRIMARY KEY CHECK (id = 1),
        next_game_id INTEGER NOT NULL
    );
)sql";

// Seeds the allocator row above every existing game ID and, if the row
// already exists, only ever raises it (never lowers). On an empty game table
// this yields 1; a nonempty one arises only when upgrading a v5 database.
// language=sql
constexpr char const* bundle_meta_seed_sql = R"sql(
    INSERT OR IGNORE INTO bundle_meta(id, next_game_id)
        SELECT 1, COALESCE(MAX(id), 0) + 1 FROM game;
    UPDATE bundle_meta
        SET next_game_id = MAX(next_game_id, (SELECT COALESCE(MAX(id), 0) + 1 FROM game))
        WHERE id = 1;
)sql";

// The canonical mutation sequence: exactly one row whose mutation_seq counts
// index-relevant canonical transactions (not games, not statements) and never
// decreases. The application advances it inside the very transaction that
// changes canonical state, so rows and sequence share one fate. A published
// derived snapshot is usable only while its covered sequence equals this value.
// language=sql
constexpr char const* mutation_state_ddl = R"sql(
    CREATE TABLE IF NOT EXISTS mutation_state (
        id INTEGER PRIMARY KEY CHECK (id = 1),
        mutation_seq INTEGER NOT NULL
            CHECK (typeof(mutation_seq) = 'integer' AND mutation_seq >= 0)
    );
)sql";

// Seeds the sequence for a NEW database or a v6 -> v7 migration only. An
// ordinary open never runs this: a missing singleton is corruption.
// language=sql
constexpr char const* mutation_state_seed_sql = R"sql(
    INSERT OR IGNORE INTO mutation_state(id, mutation_seq) VALUES (1, 0);
)sql";

// The last published derived-index snapshot: a versioned JSON payload (artifact
// descriptors, index semantics, bundle identity) plus its FNV-1a-64 checksum.
// covered_mutation_seq is NULL for an unbuilt snapshot (no artifacts).
// language=sql
constexpr char const* published_snapshot_ddl = R"sql(
    CREATE TABLE IF NOT EXISTS published_snapshot (
        id INTEGER PRIMARY KEY CHECK (id = 1),
        publication_generation INTEGER NOT NULL
            CHECK (typeof(publication_generation) = 'integer' AND publication_generation >= 0),
        covered_mutation_seq INTEGER
            CHECK (covered_mutation_seq IS NULL
                   OR (typeof(covered_mutation_seq) = 'integer' AND covered_mutation_seq >= 0)),
        canonical_game_count INTEGER NOT NULL
            CHECK (typeof(canonical_game_count) = 'integer' AND canonical_game_count >= 0),
        payload_version INTEGER NOT NULL
            CHECK (typeof(payload_version) = 'integer' AND payload_version >= 1),
        payload_json TEXT NOT NULL CHECK (typeof(payload_json) = 'text'),
        payload_checksum TEXT NOT NULL
            CHECK (length(payload_checksum) = 16 AND payload_checksum NOT GLOB '*[^0-9a-f]*')
    );
)sql";

// Not INSERT OR IGNORE: that conflict clause would also swallow a CHECK
// violation and silently leave the bundle without its singleton row. The
// NOT EXISTS guard makes the statement idempotent while a malformed payload
// still fails the whole transaction.
// language=sql
constexpr char const* insert_bootstrap_snapshot_sql = R"sql(
    INSERT INTO published_snapshot(id, publication_generation, covered_mutation_seq,
                                   canonical_game_count, payload_version, payload_json, payload_checksum)
    SELECT 1, 0, NULL, 0, ?1, ?2, ?3
    WHERE NOT EXISTS (SELECT 1 FROM published_snapshot)
)sql";

// language=sql
constexpr char const* out_of_range_game_id_sql = R"sql(
    SELECT 1 FROM game WHERE id < 1 OR id > 4294967295 LIMIT 1
)sql";

// language=sql
constexpr char const* content_revision_exists_sql = R"sql(
    SELECT 1 FROM pragma_table_info('game') WHERE name = 'content_revision'
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

// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto table_exists(sqlite3* conn, char const* name) -> result<bool>
{
    auto stmt = prepare(conn, "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?1");
    if (!stmt) {
        return tl::unexpected {stmt.error()};
    }
    sqlite3_bind_text(stmt->get(), 1, name, -1, SQLITE_STATIC);
    auto const step_rc = sqlite3_step(stmt->get());
    if (step_rc == SQLITE_ROW) {
        return true;
    }
    if (step_rc == SQLITE_DONE) {
        return false;
    }
    return tl::unexpected {error_code::io_failure};
}

// Only called with compile-time table names.
// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto count_rows(sqlite3* conn, char const* table) -> result<std::int64_t>
{
    auto const sql = fmt::format("SELECT COUNT(*) FROM {}", table);
    auto stmt = prepare(conn, sql.c_str());
    if (!stmt) {
        return tl::unexpected {stmt.error()};
    }
    if (sqlite3_step(stmt->get()) != SQLITE_ROW) {
        return tl::unexpected {error_code::io_failure};
    }
    return static_cast<std::int64_t>(sqlite3_column_int64(stmt->get(), 0));
}

auto insert_bootstrap_snapshot(sqlite3* conn, snapshot_bootstrap const& bootstrap) -> result<void>
{
    auto stmt = prepare(conn, insert_bootstrap_snapshot_sql);
    if (!stmt) {
        return tl::unexpected {stmt.error()};
    }
    sqlite3_bind_int64(stmt->get(), 1, static_cast<sqlite3_int64>(bootstrap.payload_version));
    sqlite3_bind_text(stmt->get(), 2, bootstrap.payload_json.c_str(), static_cast<int>(bootstrap.payload_json.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(
        stmt->get(), 3, bootstrap.payload_checksum.c_str(), static_cast<int>(bootstrap.payload_checksum.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(stmt->get()) != SQLITE_DONE) {
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(conn)}};
    }
    return {};
}

// v5 -> v6 body (caller owns the transaction): content_revision column and the
// durable ID allocator. Refuses, leaving the database untouched, when a game
// ID is outside the uint32 range, since the allocator cannot seed past it.
auto apply_v6_step(sqlite3* conn) -> result<void>
{
    // Public game IDs are uint32 and 0 is invalid, so a v5 row outside
    // [1, 4294967295] can never be served by the allocator or the id-typed API.
    // Refuse before any ALTER, leaving the database untouched at v5. Checking
    // first also bounds MAX(id) + 1 in the seed statement below to 2^32, so
    // that arithmetic cannot overflow int64.
    {
        auto bad_id = prepare(conn, out_of_range_game_id_sql);
        if (!bad_id) {
            return tl::unexpected {bad_id.error()};
        }
        auto const bad_rc = sqlite3_step(bad_id->get());
        if (bad_rc == SQLITE_ROW) {
            return tl::unexpected {error {error_code::schema_mismatch, "game id outside 1..4294967295"}};
        }
        if (bad_rc != SQLITE_DONE) {
            return tl::unexpected {error {error_code::io_failure}};
        }
    }

    bool has_revision_column = false;
    {
        auto has_revision = prepare(conn, content_revision_exists_sql);
        if (!has_revision) {
            return tl::unexpected {has_revision.error()};
        }
        auto const revision_rc = sqlite3_step(has_revision->get());
        if (revision_rc != SQLITE_ROW && revision_rc != SQLITE_DONE) {
            return tl::unexpected {error {error_code::io_failure}};
        }
        has_revision_column = revision_rc == SQLITE_ROW;
    }
    if (!has_revision_column) {
        if (auto alter_res = exec(conn, "ALTER TABLE game ADD COLUMN content_revision INTEGER NOT NULL DEFAULT 1;"); !alter_res) {
            return tl::unexpected {alter_res.error()};
        }
    }

    if (auto meta_res = exec(conn, bundle_meta_ddl); !meta_res) {
        return tl::unexpected {meta_res.error()};
    }
    return exec(conn, bundle_meta_seed_sql);
}

// v6 -> v7 body (caller owns the transaction): the mutation sequence at 0 and,
// for a managed bundle, an unbuilt published_snapshot. No existing artifact is
// adopted; the manager rebuilds once. The sequence is seeded fresh because a
// v6 database carries no mutation history to resume.
auto apply_v7_step(sqlite3* conn, snapshot_bootstrap const* bootstrap) -> result<void>
{
    if (auto state_res = exec(conn, mutation_state_ddl); !state_res) {
        return tl::unexpected {state_res.error()};
    }
    if (auto seed_res = exec(conn, mutation_state_seed_sql); !seed_res) {
        return tl::unexpected {seed_res.error()};
    }
    if (auto snapshot_res = exec(conn, published_snapshot_ddl); !snapshot_res) {
        return tl::unexpected {snapshot_res.error()};
    }
    if (bootstrap != nullptr) {
        return insert_bootstrap_snapshot(conn, *bootstrap);
    }
    return {};
}
}  // namespace

auto ensure_wal(sqlite3* conn) -> result<void>
{
    // Returns the mode actually in effect: "wal" (disk) or "memory" (:memory:
    // connections, where WAL does not apply). Anything else means the switch was
    // refused (another connection, an open transaction, an unsupported
    // filesystem) and must not be treated as success.
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
        return tl::unexpected {error {error_code::io_failure, "database could not be switched to WAL journal mode"}};
    }
    return {};
}

auto initialize(sqlite3* conn, snapshot_bootstrap const* const bootstrap) -> result<void>
{
    // WAL mode is required for on-disk databases.
    if (auto wal_res = ensure_wal(conn); !wal_res) {
        return tl::unexpected {wal_res.error()};
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
    // Finalize the pragma statement before the transaction: a statement left
    // mid-step (it returned its row but was never reset) can make COMMIT fail.
    fk_check->reset();

    // Create all tables and indexes in one transaction with the version stamp
    // and (for a managed bundle) the unbuilt snapshot row, so a crash can
    // never leave a v7 database that lacks its singleton rows.
    if (auto begin_res = exec(conn, "BEGIN IMMEDIATE;"); !begin_res) {
        return tl::unexpected {begin_res.error()};
    }
    auto const rollback = [conn](error err) -> result<void>
    {
        sqlite3_exec(conn, "ROLLBACK;", nullptr, nullptr, nullptr);
        return tl::unexpected {std::move(err)};
    };
    if (auto tables_res = create_tables(conn); !tables_res) {
        return rollback(tables_res.error());
    }
    if (bootstrap != nullptr) {
        if (auto snapshot_res = insert_bootstrap_snapshot(conn, *bootstrap); !snapshot_res) {
            return rollback(snapshot_res.error());
        }
    }
    auto const ver_sql = fmt::format("PRAGMA user_version = {};", current_version);
    if (auto ver_res = exec(conn, ver_sql.c_str()); !ver_res) {
        return rollback(ver_res.error());
    }
    if (auto commit_res = exec(conn, "COMMIT;"); !commit_res) {
        return rollback(commit_res.error());
    }
    return {};
}

auto create_tables(sqlite3* conn) -> result<void>
{
    // Decide "new database" before any DDL runs: a missing mutation singleton
    // on an EXISTING database is corruption (or a pre-v7 database that belongs
    // to migrate()), never an instruction to start counting again at 0 -- a
    // reseed could make an old snapshot's covered sequence match again after
    // canonical changes it never saw.
    auto const state_table = table_exists(conn, "mutation_state");
    if (!state_table) {
        return tl::unexpected {state_table.error()};
    }
    auto const game_table = table_exists(conn, "game");
    if (!game_table) {
        return tl::unexpected {game_table.error()};
    }
    if (*state_table) {
        auto const rows = count_rows(conn, "mutation_state");
        if (!rows) {
            return tl::unexpected {rows.error()};
        }
        if (*rows != 1) {
            return tl::unexpected {error {error_code::io_failure, "mutation_state singleton row is missing"}};
        }
    } else if (*game_table) {
        return tl::unexpected {error {error_code::schema_mismatch, "existing database has no mutation_state; migrate it instead"}};
    }

    if (auto ddl_res = exec(conn, ddl); !ddl_res) {
        return tl::unexpected {ddl_res.error()};
    }
    if (auto meta_res = exec(conn, bundle_meta_ddl); !meta_res) {
        return tl::unexpected {meta_res.error()};
    }
    if (auto index_res = create_identity_index(conn); !index_res) {
        return tl::unexpected {index_res.error()};
    }
    if (auto meta_res = exec(conn, bundle_meta_seed_sql); !meta_res) {
        return tl::unexpected {meta_res.error()};
    }
    if (auto state_res = exec(conn, mutation_state_ddl); !state_res) {
        return tl::unexpected {state_res.error()};
    }
    if (auto snapshot_res = exec(conn, published_snapshot_ddl); !snapshot_res) {
        return tl::unexpected {snapshot_res.error()};
    }
    if (*state_table) {
        return {};
    }
    return exec(conn, mutation_state_seed_sql);
}

auto migrate(sqlite3* conn, std::uint32_t const from_version, snapshot_bootstrap const* const bootstrap) -> result<void>
{
    if (from_version > current_version) {
        return tl::unexpected {error_code::schema_mismatch};
    }
    if (from_version == current_version) {
        return {};
    }

    // v2 -> v3 needed a real per-row hash, v3 -> v4 the collision-safe
    // move_hash/identity_collision pair, and v4 -> v5 a per-game elo backfill
    // that the old schema has no source data for; none of those is an
    // exec()-only migration. Refuse rather than silently bump user_version;
    // bundles below v5 must be rebuilt by reimporting.
    constexpr std::uint32_t oldest_migratable_version = 5;
    constexpr std::uint32_t v6_version = 6;
    if (from_version < oldest_migratable_version) {
        return tl::unexpected {error_code::schema_mismatch};
    }

    // v5 -> v6: one transaction, user_version last, so an interruption leaves
    // the database at v5 and the whole step reruns. Every step also tolerates
    // already-applied state, so a rerun after a crash (or against a database
    // some other tool half-upgraded) converges instead of failing on a
    // duplicate column or table.
    if (sqlite3_get_autocommit(conn) == 0) {
        // A caller-owned transaction would make "atomic" meaningless here.
        return tl::unexpected {error_code::invalid_argument};
    }
    if (auto begin_res = exec(conn, "BEGIN IMMEDIATE;"); !begin_res) {
        return tl::unexpected {begin_res.error()};
    }
    auto const rollback = [conn](error err) -> result<void>
    {
        sqlite3_exec(conn, "ROLLBACK;", nullptr, nullptr, nullptr);
        return tl::unexpected {std::move(err)};
    };

    // Another connection may have finished the migration between the caller's
    // version read and our lock.
    auto const locked_version = version(conn);
    if (!locked_version) {
        return rollback(locked_version.error());
    }
    if (*locked_version >= current_version) {
        sqlite3_exec(conn, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (*locked_version > current_version) {
            return tl::unexpected {error_code::schema_mismatch};
        }
        return {};
    }
    if (*locked_version < oldest_migratable_version) {
        return rollback(error {error_code::schema_mismatch});
    }

    if (*locked_version < v6_version) {
        if (auto step_res = apply_v6_step(conn); !step_res) {
            return rollback(step_res.error());
        }
    }
    if (auto step_res = apply_v7_step(conn, bootstrap); !step_res) {
        return rollback(step_res.error());
    }
    auto const ver_sql = fmt::format("PRAGMA user_version = {};", current_version);
    if (auto ver_res = exec(conn, ver_sql.c_str()); !ver_res) {
        return rollback(ver_res.error());
    }
    if (auto commit_res = exec(conn, "COMMIT;"); !commit_res) {
        return rollback(commit_res.error());
    }
    return {};
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
