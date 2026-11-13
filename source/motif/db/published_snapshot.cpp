#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include "motif/db/published_snapshot.hpp"

#include <sqlite3.h>
#include <tl/expected.hpp>

#include "motif/db/error.hpp"
#include "motif/db/manifest.hpp"
#include "motif/db/sqlite_util.hpp"

namespace motif::db
{

namespace
{

using detail::unique_stmt;

auto prepare(sqlite3* conn, char const* sql) -> result<unique_stmt>
{
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(conn, sql, -1, &raw, nullptr) != SQLITE_OK || raw == nullptr) {
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(conn)}};
    }
    return unique_stmt {raw};
}

auto malformed(char const* what) -> tl::unexpected<error>
{
    return tl::unexpected {error {error_code::io_failure, what}};
}

// Reads a column that must be a non-negative SQLite integer.
auto non_negative_integer(sqlite3_stmt* stmt, int const column) -> std::optional<std::uint64_t>
{
    if (sqlite3_column_type(stmt, column) != SQLITE_INTEGER) {
        return std::nullopt;
    }
    auto const value = sqlite3_column_int64(stmt, column);
    if (value < 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(value);
}

auto text_column(sqlite3_stmt* stmt, int const column) -> std::optional<std::string>
{
    if (sqlite3_column_type(stmt, column) != SQLITE_TEXT) {
        return std::nullopt;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto const* text = reinterpret_cast<char const*>(sqlite3_column_text(stmt, column));
    auto const bytes = sqlite3_column_bytes(stmt, column);
    if (text == nullptr || bytes < 0) {
        return std::nullopt;
    }
    return std::string {text, static_cast<std::size_t>(bytes)};
}

// Positions past the small literals clang-tidy accepts: the payload_checksum
// column of the snapshot SELECT, and bind slots 5-7 of the snapshot UPDATE.
constexpr int payload_checksum_column = 5;
constexpr int payload_json_param = 5;
constexpr int payload_checksum_param = 6;
constexpr int expected_generation_param = 7;

}  // namespace

auto read_mutation_seq(sqlite3* conn) -> result<std::uint64_t>
{
    auto stmt = prepare(conn, "SELECT mutation_seq FROM mutation_state WHERE id = 1");
    if (!stmt) {
        return tl::unexpected {stmt.error()};
    }
    if (sqlite3_step(stmt->get()) != SQLITE_ROW) {
        return malformed("mutation_state singleton is missing");
    }
    auto const seq = non_negative_integer(stmt->get(), 0);
    if (!seq) {
        return malformed("mutation_state.mutation_seq is corrupt");
    }
    return *seq;
}

auto read_published_snapshot(sqlite3* conn) -> result<db_manifest>
{
    auto stmt = prepare(conn,
                        "SELECT publication_generation, covered_mutation_seq, canonical_game_count, payload_version, "
                        "payload_json, payload_checksum FROM published_snapshot WHERE id = 1");
    if (!stmt) {
        return tl::unexpected {stmt.error()};
    }
    auto const step_rc = sqlite3_step(stmt->get());
    if (step_rc == SQLITE_DONE) {
        return malformed("published_snapshot singleton is missing");
    }
    if (step_rc != SQLITE_ROW) {
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(conn)}};
    }
    auto const generation = non_negative_integer(stmt->get(), 0);
    auto const games = non_negative_integer(stmt->get(), 2);
    auto const payload_version = non_negative_integer(stmt->get(), 3);
    auto covered = std::optional<std::uint64_t> {};
    if (sqlite3_column_type(stmt->get(), 1) != SQLITE_NULL) {
        covered = non_negative_integer(stmt->get(), 1);
        if (!covered) {
            return malformed("published_snapshot.covered_mutation_seq is corrupt");
        }
    }
    auto payload_json = text_column(stmt->get(), 4);
    auto payload_checksum = text_column(stmt->get(), payload_checksum_column);
    if (!generation || !games || !payload_version || !payload_json || !payload_checksum
        || *payload_version > std::numeric_limits<std::uint32_t>::max())
    {
        return malformed("published_snapshot row is malformed");
    }
    return parse_manifest(
        *generation, covered, *games, static_cast<std::uint32_t>(*payload_version), std::move(*payload_json), std::move(*payload_checksum));
}

auto replace_published_snapshot(sqlite3* conn, db_manifest const& next, std::uint64_t const expected_mutation_seq) -> result<void>
{
    constexpr auto int64_max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (next.publication_generation == 0 || next.publication_generation > int64_max || next.canonical_game_count > int64_max
        || (next.covered_mutation_seq && *next.covered_mutation_seq > int64_max))
    {
        return tl::unexpected {error {error_code::invalid_argument, "snapshot counter out of range"}};
    }
    auto const current_seq = read_mutation_seq(conn);
    if (!current_seq) {
        return tl::unexpected {current_seq.error()};
    }
    if (*current_seq != expected_mutation_seq) {
        return tl::unexpected {error {error_code::io_failure, "canonical state changed during publication"}};
    }

    auto stmt = prepare(conn,
                        "UPDATE published_snapshot SET publication_generation = ?1, covered_mutation_seq = ?2, "
                        "canonical_game_count = ?3, payload_version = ?4, payload_json = ?5, payload_checksum = ?6 "
                        "WHERE id = 1 AND publication_generation = ?7");
    if (!stmt) {
        return tl::unexpected {stmt.error()};
    }
    sqlite3_bind_int64(stmt->get(), 1, static_cast<sqlite3_int64>(next.publication_generation));
    if (next.covered_mutation_seq) {
        sqlite3_bind_int64(stmt->get(), 2, static_cast<sqlite3_int64>(*next.covered_mutation_seq));
    } else {
        sqlite3_bind_null(stmt->get(), 2);
    }
    sqlite3_bind_int64(stmt->get(), 3, static_cast<sqlite3_int64>(next.canonical_game_count));
    sqlite3_bind_int64(stmt->get(), 4, static_cast<sqlite3_int64>(next.payload_version));
    sqlite3_bind_text(
        stmt->get(), payload_json_param, next.payload_json.c_str(), static_cast<int>(next.payload_json.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt->get(),
                      payload_checksum_param,
                      next.payload_checksum.c_str(),
                      static_cast<int>(next.payload_checksum.size()),
                      SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt->get(), expected_generation_param, static_cast<sqlite3_int64>(next.publication_generation - 1U));
    if (sqlite3_step(stmt->get()) != SQLITE_DONE) {
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(conn)}};
    }
    if (sqlite3_changes(conn) != 1) {
        return tl::unexpected {error {error_code::io_failure, "published_snapshot generation changed during publication"}};
    }
    return {};
}

}  // namespace motif::db
