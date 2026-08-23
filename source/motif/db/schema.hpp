#pragma once

#include <cstdint>

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
inline constexpr std::uint32_t current_version = 5;

// Create all tables, indexes, and set WAL mode, foreign_keys ON, and
// PRAGMA user_version. Idempotent: uses CREATE TABLE IF NOT EXISTS throughout;
// safe to call on an already-initialized database.
auto initialize(sqlite3* conn) -> result<void>;

// Apply any pending migrations from the database's current user_version up to
// current_version. Returns schema_mismatch if the stored version is greater
// than current_version (downgrade is not supported).
auto migrate(sqlite3* conn, std::uint32_t from_version) -> result<void>;

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
