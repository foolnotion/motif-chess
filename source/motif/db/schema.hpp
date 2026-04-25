// NOLINTNEXTLINE(portability-avoid-pragma-once)
#pragma once

#include <cstdint>

#include "motif/db/error.hpp"

struct sqlite3;

namespace motif::db::schema
{

// Current schema version embedded in PRAGMA user_version.
inline constexpr std::uint32_t current_version = 1;

// Create all tables, indexes, and set WAL mode, foreign_keys ON, and
// PRAGMA user_version. Idempotent: uses CREATE TABLE IF NOT EXISTS throughout;
// safe to call on an already-initialized database.
auto initialize(sqlite3* conn) -> result<void>;

// Query PRAGMA user_version from the connection.
auto version(sqlite3* conn) -> result<std::uint32_t>;

}  // namespace motif::db::schema
