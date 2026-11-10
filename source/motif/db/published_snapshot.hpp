#pragma once

#include <cstdint>

#include "motif/db/error.hpp"
#include "motif/db/manifest.hpp"

struct sqlite3;

namespace motif::db
{

// SQL codec for the two v7 singleton tables. Every reader treats a missing or
// malformed singleton as corruption (io_failure), never as a cue to reseed:
// seeding belongs to schema creation and migration only.

// mutation_state.mutation_seq. The row must exist exactly once and hold a
// non-negative integer.
auto read_mutation_seq(sqlite3* conn) -> result<std::uint64_t>;

// The published_snapshot row, checksum-verified and structurally validated
// (see parse_manifest). io_failure when the row is absent, malformed, or its
// payload fails the checksum; schema_mismatch when its payload version is not
// supported.
auto read_published_snapshot(sqlite3* conn) -> result<db_manifest>;

// Replaces the singleton row with next. The caller owns the transaction and
// must have begun it (BEGIN IMMEDIATE). Requires, inside that transaction,
// that mutation_seq still equals expected_mutation_seq and that the stored
// publication_generation still equals next.publication_generation - 1, so a
// concurrent or repeated publication can never silently overwrite another's.
// Fails (nothing written) otherwise.
auto replace_published_snapshot(sqlite3* conn, db_manifest const& next, std::uint64_t expected_mutation_seq) -> result<void>;

}  // namespace motif::db
