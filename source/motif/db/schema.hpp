#pragma once

#include <cstdint>

#include "motif/db/error.hpp"

struct sqlite3;

namespace motif::db::schema
{

// Current schema version embedded in PRAGMA user_version.
// v1 → v2: added source_type, source_label, review_status columns to game.
// v2 → v3: added moves_hash; ux_game_identity now keys on it instead of the
// raw moves blob, so a move sequence is no longer stored twice on disk (once
// in the row, once in the index). Not migratable in place -- see
// schema::migrate -- bundles at v2 or earlier must be rebuilt by reimporting.
inline constexpr std::uint32_t current_version = 3;

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

}  // namespace motif::db::schema
