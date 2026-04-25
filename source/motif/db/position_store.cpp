#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <sstream>
#include <vector>

#include "motif/db/position_store.hpp"

#include <duckdb.h>
#include <tl/expected.hpp>

#include "motif/db/error.hpp"
#include "motif/db/types.hpp"

namespace
{

// language=sql
constexpr char const* create_position = R"sql(
    CREATE TABLE IF NOT EXISTS position (
        zobrist_hash  UBIGINT   NOT NULL,
        game_id       UINTEGER  NOT NULL,
        ply           USMALLINT NOT NULL,
        result        TINYINT   NOT NULL,
        white_elo     SMALLINT,
        black_elo     SMALLINT
    )
)sql";

// language=sql
constexpr char const* sort_position_by_zobrist = R"sql(
    DROP TABLE IF EXISTS position_sorted;

    CREATE TABLE position_sorted AS
    SELECT *
    FROM position
    ORDER BY zobrist_hash;

    DROP TABLE position;
    ALTER TABLE position_sorted RENAME TO position;
)sql";

}  // namespace

namespace motif::db
{

position_store::position_store(duckdb_connection con) noexcept
    : con_ {con}
{
}

auto position_store::initialize_schema() -> result<void>
{
    duckdb_result res {};
    auto const create_ret = duckdb_query(con_, create_position, &res);
    duckdb_destroy_result(&res);
    if (create_ret == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }

    return {};
}

auto position_store::sort_by_zobrist() -> result<void>
{
    duckdb_result res {};
    auto const ret = duckdb_query(con_, sort_position_by_zobrist, &res);
    duckdb_destroy_result(&res);
    if (ret == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }

    return {};
}

auto position_store::insert_batch(std::span<position_row const> rows)
    -> result<void>
{
    duckdb_appender appender {};
    if (duckdb_appender_create(con_, nullptr, "position", &appender)
        == DuckDBError)
    {
        return tl::unexpected {error_code::io_failure};
    }

    for (auto const& row : rows) {
        duckdb_appender_begin_row(appender);
        duckdb_append_uint64(appender, row.zobrist_hash);
        duckdb_append_uint32(appender, row.game_id);
        duckdb_append_uint16(appender, row.ply);
        duckdb_append_int8(appender, row.result);
        if (row.white_elo.has_value()) {
            duckdb_append_int16(appender, *row.white_elo);
        } else {
            duckdb_append_null(appender);
        }
        if (row.black_elo.has_value()) {
            duckdb_append_int16(appender, *row.black_elo);
        } else {
            duckdb_append_null(appender);
        }
        if (duckdb_appender_end_row(appender) == DuckDBError) {
            duckdb_appender_destroy(&appender);
            return tl::unexpected {error_code::io_failure};
        }
    }

    auto const flush_ret = duckdb_appender_flush(appender);
    duckdb_appender_destroy(&appender);
    if (flush_ret == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }

    return {};
}

auto position_store::row_count() const -> result<std::int64_t>
{
    duckdb_result res {};
    if (duckdb_query(con_, "SELECT count(*) FROM position", &res)
        == DuckDBError)
    {
        duckdb_destroy_result(&res);
        return tl::unexpected {error_code::io_failure};
    }
    auto const count = duckdb_value_int64(&res, 0, 0);
    duckdb_destroy_result(&res);
    return count;
}

auto position_store::query_by_zobrist(std::uint64_t const zobrist_hash,
                                      std::size_t const limit,
                                      std::size_t const offset) const
    -> result<std::vector<position_match>>
{
    duckdb_result res {};
    std::ostringstream sql;
    sql << "SELECT game_id, ply, result, white_elo, black_elo FROM position "
           "WHERE zobrist_hash = CAST("
        << zobrist_hash << " AS UBIGINT) "
           "ORDER BY game_id, ply";
    if (limit > 0) {
        sql << " LIMIT " << limit;
    }
    if (offset > 0) {
        sql << " OFFSET " << offset;
    }
    if (duckdb_query(con_, sql.str().c_str(), &res) == DuckDBError) {
        duckdb_destroy_result(&res);
        return tl::unexpected {error_code::io_failure};
    }

    auto const row_count = static_cast<std::size_t>(duckdb_row_count(&res));
    std::vector<position_match> matches;
    matches.reserve(row_count);

    for (std::size_t i = 0; i < row_count; ++i) {
        auto const row_idx = static_cast<idx_t>(i);
        auto white_elo = std::optional<std::int16_t> {};
        if (!duckdb_value_is_null(&res, 3, row_idx)) {
            white_elo =
                static_cast<std::int16_t>(duckdb_value_int16(&res, 3, row_idx));
        }
        auto black_elo = std::optional<std::int16_t> {};
        if (!duckdb_value_is_null(&res, 4, row_idx)) {
            black_elo =
                static_cast<std::int16_t>(duckdb_value_int16(&res, 4, row_idx));
        }
        matches.push_back(position_match {
            .game_id = duckdb_value_uint32(&res, 0, row_idx),
            .ply = duckdb_value_uint16(&res, 1, row_idx),
            .result = duckdb_value_int8(&res, 2, row_idx),
            .white_elo = white_elo,
            .black_elo = black_elo,
        });
    }

    duckdb_destroy_result(&res);
    return matches;
}

auto position_store::query_opening_moves(std::uint64_t const zobrist_hash) const
    -> result<std::vector<opening_move_stat>>
{
    duckdb_result res {};
    std::ostringstream sql;
    sql << "SELECT game_id, ply, result, white_elo, black_elo FROM position "
           "WHERE zobrist_hash = CAST("
        << zobrist_hash << " AS UBIGINT)";
    if (duckdb_query(con_, sql.str().c_str(), &res) == DuckDBError) {
        duckdb_destroy_result(&res);
        return tl::unexpected {error_code::io_failure};
    }

    auto const row_count = static_cast<std::size_t>(duckdb_row_count(&res));
    std::vector<opening_move_stat> stats;
    stats.reserve(row_count);

    for (std::size_t i = 0; i < row_count; ++i) {
        auto const row_idx = static_cast<idx_t>(i);
        auto white_elo = std::optional<std::int16_t> {};
        if (!duckdb_value_is_null(&res, 3, row_idx)) {
            white_elo =
                static_cast<std::int16_t>(duckdb_value_int16(&res, 3, row_idx));
        }
        auto black_elo = std::optional<std::int16_t> {};
        if (!duckdb_value_is_null(&res, 4, row_idx)) {
            black_elo =
                static_cast<std::int16_t>(duckdb_value_int16(&res, 4, row_idx));
        }
        stats.push_back(opening_move_stat {
            .game_id = duckdb_value_uint32(&res, 0, row_idx),
            .ply = duckdb_value_uint16(&res, 1, row_idx),
            .result = duckdb_value_int8(&res, 2, row_idx),
            .white_elo = white_elo,
            .black_elo = black_elo,
        });
    }

    duckdb_destroy_result(&res);
    return stats;
}

auto position_store::query_tree_slice(std::uint64_t const root_hash,
                                      std::uint16_t const max_depth) const
    -> result<std::vector<tree_position_row>>
{
    constexpr auto white_elo_col = 5;
    constexpr auto black_elo_col = 6;

    duckdb_result res {};
    std::ostringstream sql;
    sql << "SELECT "
           "p_root.game_id, "
           "p_root.ply AS root_ply, "
           "CAST(p_cont.ply - p_root.ply AS USMALLINT) AS depth, "
           "p_cont.zobrist_hash AS child_hash, "
           "p_cont.result, "
           "p_cont.white_elo, "
           "p_cont.black_elo "
           "FROM position p_root "
           "JOIN position p_cont "
           "ON  p_root.game_id = p_cont.game_id "
           "AND p_cont.ply > p_root.ply "
           "AND p_cont.ply <= p_root.ply + "
        << max_depth
        << " WHERE p_root.zobrist_hash = CAST(" << root_hash
        << " AS UBIGINT)"
        << " ORDER BY p_root.game_id, p_cont.ply";
    if (duckdb_query(con_, sql.str().c_str(), &res) == DuckDBError) {
        duckdb_destroy_result(&res);
        return tl::unexpected {error_code::io_failure};
    }

    auto const row_count = static_cast<std::size_t>(duckdb_row_count(&res));
    std::vector<tree_position_row> rows;
    rows.reserve(row_count);

    for (std::size_t i = 0; i < row_count; ++i) {
        auto const row_idx = static_cast<idx_t>(i);
        auto white_elo = std::optional<std::int16_t> {};
        if (!duckdb_value_is_null(&res, white_elo_col, row_idx)) {
            white_elo = static_cast<std::int16_t>(
                duckdb_value_int16(&res, white_elo_col, row_idx));
        }
        auto black_elo = std::optional<std::int16_t> {};
        if (!duckdb_value_is_null(&res, black_elo_col, row_idx)) {
            black_elo = static_cast<std::int16_t>(
                duckdb_value_int16(&res, black_elo_col, row_idx));
        }
        rows.push_back(tree_position_row {
            .game_id = duckdb_value_uint32(&res, 0, row_idx),
            .root_ply = duckdb_value_uint16(&res, 1, row_idx),
            .depth = duckdb_value_uint16(&res, 2, row_idx),
            .child_hash = duckdb_value_uint64(&res, 3, row_idx),
            .result = duckdb_value_int8(&res, 4, row_idx),
            .white_elo = white_elo,
            .black_elo = black_elo,
        });
    }

    duckdb_destroy_result(&res);
    return rows;
}

auto position_store::sample_zobrist_hashes(std::size_t const limit,
                                           std::uint64_t const seed) const
    -> result<std::vector<std::uint64_t>>
{
    duckdb_result res {};
    std::ostringstream sql;
    sql << "SELECT DISTINCT zobrist_hash FROM position USING SAMPLE reservoir("
        << limit << " ROWS) REPEATABLE (" << seed << ")";
    if (duckdb_query(con_, sql.str().c_str(), &res) == DuckDBError) {
        duckdb_destroy_result(&res);
        return tl::unexpected {error_code::io_failure};
    }

    auto const row_count = static_cast<std::size_t>(duckdb_row_count(&res));
    std::vector<std::uint64_t> hashes;
    hashes.reserve(row_count);

    for (std::size_t i = 0; i < row_count; ++i) {
        hashes.push_back(duckdb_value_uint64(&res, 0, static_cast<idx_t>(i)));
    }

    duckdb_destroy_result(&res);
    return hashes;
}

}  // namespace motif::db
