#pragma once

#include <cstdint>
#include <string>

#include "motif/db/error.hpp"

struct sqlite3;

namespace motif::db::schema
{

// Current schema version embedded in PRAGMA user_version.
// v1 → v2: added source_type, source_label, review_status columns to game.
// v2 → v3: added moves_hash (see schema.cpp's migrate() for why v2-or-earlier
// bundles aren't migrated in place).
// v3 → v4: replaced moves_hash with a collision-safe move_hash + identity_collision
// pair (see schema.cpp's migrate() for why v3-or-earlier bundles aren't migrated
// in place).
// v4 → v5: added white_elo/black_elo columns to game, holding the rating each
// side actually had in that specific game. Previously elo lived only on the
// shared player row keyed by name, so patching one game's elo (or importing a
// second game for a name already in the DB) silently left every other game
// for that name unaffected/unupdated -- elo is per-game data, not a player
// attribute. Bundles below v5 have no per-game elo to backfill and must be
// rebuilt by reimporting (see migrate() below).
// v5 → v6: added the single-row bundle_meta table, whose next_game_id is a
// durable monotonic game-ID allocator (a bare INTEGER PRIMARY KEY reuses the
// highest deleted ID), and game.content_revision (1 for every game; it
// identifies replacement content for a stable ID). Migrated in place by
// migrate(); older binaries reject v6 because their current_version is lower.
// v6 → v7: added mutation_state (the single-row canonical mutation sequence)
// and published_snapshot (the single-row record of the last published derived-
// index snapshot: versioned JSON payload plus checksum). SQLite, not
// manifest.json, is now authoritative for which derived artifacts exist;
// manifest.json is a rewritable cache of that row. Migrated in place by
// migrate(); older binaries reject v7 because their current_version is lower.
inline constexpr std::uint32_t current_version = 7;

// An unbuilt published_snapshot row: generation 0, null coverage, no
// artifacts. database_manager supplies the encoded payload (bundle identity
// and index semantics only) so the schema module never interprets it.
struct snapshot_bootstrap
{
    std::uint32_t payload_version {};
    std::string payload_json;
    std::string payload_checksum;
};

// Create all tables, indexes, the allocator row, and the mutation sequence
// row (seeded at 0), and set WAL mode, foreign_keys ON, and PRAGMA
// user_version. When bootstrap is non-null the unbuilt published_snapshot row
// is inserted in the same transaction as user_version, so a v7 database
// created for a bundle never exists without its snapshot row. Idempotent:
// uses CREATE ... IF NOT EXISTS / INSERT OR IGNORE throughout; safe to call
// on an already-initialized database.
auto initialize(sqlite3* conn, snapshot_bootstrap const* bootstrap = nullptr) -> result<void>;

// Puts the connection's database in WAL journal mode (a no-op when it already
// is, and for in-memory databases, which have no WAL). Fails, changing nothing
// the caller relies on, when SQLite refuses the switch -- e.g. another
// connection is open or a transaction is active. Needs exclusive access to
// perform the transition, which database_manager has through the bundle lock.
// database_manager::open() calls it before migrating or accepting any disk
// bundle: snapshot publication and the FULL-synchronous guarantees assume WAL.
auto ensure_wal(sqlite3* conn) -> result<void>;

// The shared schema definition: create every table and index (game_identity_lookup
// included), seed the bundle_meta allocator row above any existing game ID and
// the mutation_state row at 0. Touches no pragma and not user_version, so
// callers that own those decisions (initialize(), game_store::create_schema())
// layer them on top. Idempotent. Only for a NEW database: an ordinary open of an
// existing v7 database must not call it, because a missing singleton there is
// corruption, not something to reseed.
auto create_tables(sqlite3* conn) -> result<void>;

// Bring a database at from_version up to current_version. v5 -> v6 -> v7 are
// supported (one atomic transaction, safe to rerun); anything older returns
// schema_mismatch without touching the database, as does a version greater
// than current_version (downgrade is not supported). It also returns
// schema_mismatch, with the database left untouched at v5, when any existing
// game.id is outside [1, 4294967295] (the range of the uint32 game_id), since
// such a row cannot be represented or seeded past. Fails with invalid_argument
// if the connection already has an open transaction.
//
// The v7 step seeds mutation_seq = 0 and, when bootstrap is non-null, inserts
// an unbuilt published_snapshot row. Existing derived artifacts are never
// adopted: the manager performs one full rebuild on the next open.
//
// Connection requirements: migrate() neither sets nor verifies
// PRAGMA foreign_keys and does not touch journal_mode. It is meant to be called
// on a connection prepared by database_manager::open(), which enables and
// verifies foreign_keys before migrating and keeps it enabled afterwards. A
// caller passing its own connection must enable foreign_keys first.
auto migrate(sqlite3* conn, std::uint32_t from_version, snapshot_bootstrap const* bootstrap = nullptr) -> result<void>;

// Query PRAGMA user_version from the connection.
auto version(sqlite3* conn) -> result<std::uint32_t>;

// Create the game_identity_lookup unique index (see schema.cpp for its
// column list and why it's keyed on move_hash rather than the raw moves
// blob). Idempotent. Used both by initialize() and by the raw-ingest import
// path, which drops the index before bulk insert and recreates it here after
// deduplication so per-row insert cost doesn't include index maintenance.
auto create_identity_index(sqlite3* conn) -> result<void>;

// Drop game_identity_lookup if present. Only safe to call around a bulk
// raw-ingest window that ends with create_identity_index() -- the index is
// what makes game_writer::insert()'s duplicate check possible, so leaving it
// dropped breaks correctness for any other insert path sharing the connection.
auto drop_identity_index(sqlite3* conn) -> result<void>;

// Whether game_identity_lookup exists. A missing index marks an interrupted
// raw import and requires deduplication before normal writes can resume.
auto identity_index_exists(sqlite3* conn) -> result<bool>;

}  // namespace motif::db::schema
