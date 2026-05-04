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
constexpr auto create_position = R"sql(
    CREATE TABLE IF NOT EXISTS position (
        zobrist_hash  UBIGINT   NOT NULL,
        game_id       UINTEGER  NOT NULL,
        ply           USMALLINT NOT NULL,
        encoded_move  USMALLINT NOT NULL,
        result        TINYINT   NOT NULL,
        white_elo     SMALLINT,
        black_elo     SMALLINT
    )
)sql";

// language=sql
constexpr auto sort_position_by_zobrist = R"sql(
    DROP TABLE IF EXISTS position_sorted;

    CREATE TABLE position_sorted AS
    SELECT *
    FROM position
    ORDER BY zobrist_hash;

    DROP TABLE position;
    ALTER TABLE position_sorted RENAME TO position;
)sql";

// Column positions for SELECT game_id, ply, result, white_elo, black_elo FROM position
namespace by_zobrist_col
{
constexpr idx_t game_id = 0;
constexpr idx_t ply = 1;
constexpr idx_t result = 2;
constexpr idx_t white_elo = 3;
constexpr idx_t black_elo = 4;
}  // namespace by_zobrist_col

// Column positions for query_tree_slice SELECT
namespace tree_slice_col
{
constexpr idx_t game_id = 0;
constexpr idx_t root_ply = 1;
constexpr idx_t depth = 2;
constexpr idx_t child_hash = 3;
constexpr idx_t encoded_move = 4;
constexpr idx_t result = 5;
constexpr idx_t white_elo = 6;
constexpr idx_t black_elo = 7;
}  // namespace tree_slice_col

// Column positions for query_opening_stats SELECT
namespace opening_stats_col
{
constexpr idx_t cont_encoded_move = 0;
constexpr idx_t cont_hash = 1;
constexpr idx_t root_ply = 2;
constexpr idx_t frequency = 3;
constexpr idx_t white_wins = 4;
constexpr idx_t draws = 5;
constexpr idx_t black_wins = 6;
constexpr idx_t avg_white_elo = 7;
constexpr idx_t avg_black_elo = 8;
constexpr idx_t eco_min = 9;
constexpr idx_t eco_max = 10;
constexpr idx_t total_games = 11;
}  // namespace opening_stats_col

struct result_guard
{
    duckdb_result res {};
    result_guard() = default;

    ~result_guard() noexcept { duckdb_destroy_result(&res); }

    result_guard(result_guard const&) = delete;
    auto operator=(result_guard const&) -> result_guard& = delete;
    result_guard(result_guard&&) = delete;
    auto operator=(result_guard&&) -> result_guard& = delete;
};

[[nodiscard]] auto read_optional_int16(duckdb_result& res, idx_t col, idx_t row) -> std::optional<std::int16_t>
{
    if (duckdb_value_is_null(&res, col, row)) {
        return std::nullopt;
    }
    return static_cast<std::int16_t>(duckdb_value_int16(&res, col, row));
}

[[nodiscard]] auto read_optional_double(duckdb_result& res, idx_t col, idx_t row) -> std::optional<double>
{
    if (duckdb_value_is_null(&res, col, row)) {
        return std::nullopt;
    }
    return duckdb_value_double(&res, col, row);
}

}  // namespace

namespace motif::db
{

position_store::position_store(duckdb_connection con) noexcept
    : con_ {con}
{
}

auto position_store::initialize_schema() -> result<void>
{
    result_guard guard {};
    if (duckdb_query(con_, create_position, &guard.res) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }
    return {};
}

auto position_store::sort_by_zobrist() -> result<void>
{
    result_guard guard {};
    if (duckdb_query(con_, sort_position_by_zobrist, &guard.res) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }
    return {};
}

auto position_store::insert_batch(std::span<position_row const> rows) -> result<void>
{
    duckdb_appender appender {};
    if (duckdb_appender_create(con_, nullptr, "position", &appender) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }

    for (auto const& row : rows) {
        duckdb_appender_begin_row(appender);
        duckdb_append_uint64(appender, row.zobrist_hash);
        duckdb_append_uint32(appender, row.game_id);
        duckdb_append_uint16(appender, row.ply);
        duckdb_append_uint16(appender, row.encoded_move);
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
    result_guard guard {};
    if (duckdb_query(con_, "SELECT count(*) FROM position", &guard.res) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }
    return duckdb_value_int64(&guard.res, 0, 0);
}

auto position_store::query_by_zobrist(std::uint64_t const zobrist_hash, std::size_t const limit, std::size_t const offset) const
    -> result<std::vector<position_match>>
{
    std::ostringstream sql;
    sql << "SELECT game_id, ply, result, white_elo, black_elo FROM position " "WHERE zobrist_hash = CAST(" << zobrist_hash
        << " AS UBIGINT) ORDER BY game_id, ply";
    if (limit > 0) {
        sql << " LIMIT " << limit;
    }
    if (offset > 0) {
        sql << " OFFSET " << offset;
    }

    result_guard guard {};
    if (duckdb_query(con_, sql.str().c_str(), &guard.res) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }

    auto const nrows = static_cast<std::size_t>(duckdb_row_count(&guard.res));
    std::vector<position_match> matches;
    matches.reserve(nrows);

    for (std::size_t i = 0; i < nrows; ++i) {
        auto const row = static_cast<idx_t>(i);
        matches.push_back(position_match {
            .game_id = duckdb_value_uint32(&guard.res, by_zobrist_col::game_id, row),
            .ply = duckdb_value_uint16(&guard.res, by_zobrist_col::ply, row),
            .result = duckdb_value_int8(&guard.res, by_zobrist_col::result, row),
            .white_elo = read_optional_int16(guard.res, by_zobrist_col::white_elo, row),
            .black_elo = read_optional_int16(guard.res, by_zobrist_col::black_elo, row),
        });
    }

    return matches;
}

auto position_store::query_tree_slice(std::uint64_t const root_hash, std::uint16_t const max_depth) const
    -> result<std::vector<tree_position_row>>
{
    std::ostringstream sql;
    sql << "SELECT "
           "p_root.game_id, "
           "p_root.ply AS root_ply, "
           "CAST(p_cont.ply - p_root.ply AS USMALLINT) AS depth, "
           "p_cont.zobrist_hash AS child_hash, "
           "p_cont.encoded_move, "
           "p_cont.result, "
           "p_cont.white_elo, "
           "p_cont.black_elo "
           "FROM position p_root "
           "JOIN position p_cont "
           "ON  p_root.game_id = p_cont.game_id "
           "AND p_cont.ply > p_root.ply "
           "AND p_cont.ply <= p_root.ply + "
        << max_depth << " WHERE p_root.zobrist_hash = CAST(" << root_hash << " AS UBIGINT)"
        << " ORDER BY p_root.game_id, p_cont.ply";

    result_guard guard {};
    if (duckdb_query(con_, sql.str().c_str(), &guard.res) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }

    auto const nrows = static_cast<std::size_t>(duckdb_row_count(&guard.res));
    std::vector<tree_position_row> rows;
    rows.reserve(nrows);

    for (std::size_t i = 0; i < nrows; ++i) {
        auto const row = static_cast<idx_t>(i);
        rows.push_back(tree_position_row {
            .game_id = duckdb_value_uint32(&guard.res, tree_slice_col::game_id, row),
            .root_ply = duckdb_value_uint16(&guard.res, tree_slice_col::root_ply, row),
            .depth = duckdb_value_uint16(&guard.res, tree_slice_col::depth, row),
            .encoded_move = duckdb_value_uint16(&guard.res, tree_slice_col::encoded_move, row),
            .child_hash = duckdb_value_uint64(&guard.res, tree_slice_col::child_hash, row),
            .result = duckdb_value_int8(&guard.res, tree_slice_col::result, row),
            .white_elo = read_optional_int16(guard.res, tree_slice_col::white_elo, row),
            .black_elo = read_optional_int16(guard.res, tree_slice_col::black_elo, row),
        });
    }

    return rows;
}

auto position_store::query_opening_stats(std::uint64_t const zobrist_hash, std::optional<std::uint64_t> const parent_hash) const
    -> result<std::vector<opening_stat_agg_row>>
{
    // The deduped CTE aggregates per (game_id, continuation) so that AVG and
    // COUNT(DISTINCT) work over the same unique-game population. Without this, a
    // game that revisits the root via repetition inflates AVG without COUNT.
    //
    // When parent_hash is provided the CTE is further constrained: only games
    // where parent_hash appears at ply N and zobrist_hash at ply N+1 are included.
    // This eliminates transposition inflation — a position reachable via multiple
    // move orders is counted only through the specific path the client navigated.
    std::ostringstream sql;
    sql << "WITH deduped AS ("
           "SELECT "
           "p_root.game_id, "
           "p_cont.encoded_move, "
           "p_cont.zobrist_hash AS child_hash, "
           "MIN(p_root.ply) AS root_ply, "
           "p_root.result, "
           "p_root.white_elo, "
           "p_root.black_elo "
           "FROM position ";

    if (parent_hash.has_value()) {
        sql << "p_parent "
               "JOIN position p_root "
               "ON  p_root.game_id = p_parent.game_id "
               "AND p_root.ply = p_parent.ply + 1 "
            << "JOIN position p_cont "
               "ON  p_cont.game_id = p_root.game_id "
               "AND p_cont.ply = p_root.ply + 1 "
            << "WHERE p_parent.zobrist_hash = CAST(" << *parent_hash << " AS UBIGINT) "
            << "AND p_root.zobrist_hash = CAST(" << zobrist_hash << " AS UBIGINT) ";
    } else {
        sql << "p_root "
               "JOIN position p_cont "
               "ON  p_cont.game_id = p_root.game_id "
               "AND p_cont.ply = p_root.ply + 1 "
            << "WHERE p_root.zobrist_hash = CAST(" << zobrist_hash << " AS UBIGINT) ";
    }

    sql << "GROUP BY p_root.game_id, p_cont.encoded_move, p_cont.zobrist_hash, "
           "p_root.result, p_root.white_elo, p_root.black_elo"
           ") "
           "SELECT "
           "encoded_move, "
           "child_hash, "
           "MIN(root_ply) AS root_ply, "
           "COUNT(DISTINCT game_id) AS frequency, "
           "COUNT(DISTINCT CASE WHEN result > 0 THEN game_id END) AS white_wins, "
           "COUNT(DISTINCT CASE WHEN result = 0 THEN game_id END) AS draws, "
           "COUNT(DISTINCT CASE WHEN result < 0 THEN game_id END) AS black_wins, "
           "AVG(CAST(white_elo AS DOUBLE)) AS avg_white_elo, "
           "AVG(CAST(black_elo AS DOUBLE)) AS avg_black_elo, "
           "MIN(game_id) AS eco_sample_min, "
           "MAX(game_id) AS eco_sample_max, "
           "(SELECT COUNT(DISTINCT game_id) FROM deduped) AS total_games "
           "FROM deduped "
           "GROUP BY encoded_move, child_hash";

    result_guard guard {};
    if (duckdb_query(con_, sql.str().c_str(), &guard.res) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }

    auto const nrows = static_cast<std::size_t>(duckdb_row_count(&guard.res));
    std::vector<opening_stat_agg_row> rows;
    rows.reserve(nrows);

    for (std::size_t i = 0; i < nrows; ++i) {
        auto const row = static_cast<idx_t>(i);
        rows.push_back(opening_stat_agg_row {
            .cont_encoded_move = duckdb_value_uint16(&guard.res, opening_stats_col::cont_encoded_move, row),
            .cont_hash = duckdb_value_uint64(&guard.res, opening_stats_col::cont_hash, row),
            .root_ply = duckdb_value_uint16(&guard.res, opening_stats_col::root_ply, row),
            .frequency = duckdb_value_uint32(&guard.res, opening_stats_col::frequency, row),
            .white_wins = duckdb_value_uint32(&guard.res, opening_stats_col::white_wins, row),
            .draws = duckdb_value_uint32(&guard.res, opening_stats_col::draws, row),
            .black_wins = duckdb_value_uint32(&guard.res, opening_stats_col::black_wins, row),
            .avg_white_elo = read_optional_double(guard.res, opening_stats_col::avg_white_elo, row),
            .avg_black_elo = read_optional_double(guard.res, opening_stats_col::avg_black_elo, row),
            .eco_sample_min = duckdb_value_uint32(&guard.res, opening_stats_col::eco_min, row),
            .eco_sample_max = duckdb_value_uint32(&guard.res, opening_stats_col::eco_max, row),
            .total_games = duckdb_value_uint32(&guard.res, opening_stats_col::total_games, row),
        });
    }

    return rows;
}

auto position_store::sample_zobrist_hashes(std::size_t const limit, std::uint64_t const seed) const -> result<std::vector<std::uint64_t>>
{
    std::ostringstream sql;
    sql << "SELECT DISTINCT zobrist_hash FROM position USING SAMPLE reservoir(" << limit << " ROWS) REPEATABLE (" << seed << ")";

    result_guard guard {};
    if (duckdb_query(con_, sql.str().c_str(), &guard.res) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }

    auto const nrows = static_cast<std::size_t>(duckdb_row_count(&guard.res));
    std::vector<std::uint64_t> hashes;
    hashes.reserve(nrows);

    for (std::size_t i = 0; i < nrows; ++i) {
        hashes.push_back(duckdb_value_uint64(&guard.res, 0, static_cast<idx_t>(i)));
    }

    return hashes;
}

auto position_store::delete_by_game_id(std::uint32_t const game_id) -> result<void>
{
    std::ostringstream sql;
    sql << "DELETE FROM position WHERE game_id = CAST(" << game_id << " AS UINTEGER)";

    result_guard guard {};
    if (duckdb_query(con_, sql.str().c_str(), &guard.res) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }
    return {};
}

auto position_store::count_by_zobrist(std::uint64_t const zobrist_hash) const -> result<std::int64_t>
{
    std::ostringstream sql;
    sql << "SELECT COUNT(*) FROM position WHERE zobrist_hash = CAST(" << zobrist_hash << " AS UBIGINT)";

    result_guard guard {};
    if (duckdb_query(con_, sql.str().c_str(), &guard.res) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }
    return duckdb_value_int64(&guard.res, 0, 0);
}

auto position_store::count_distinct_games_by_zobrist(std::uint64_t const zobrist_hash) const -> result<std::int64_t>
{
    std::ostringstream sql;
    sql << "SELECT COUNT(DISTINCT game_id) FROM position WHERE zobrist_hash = CAST(" << zobrist_hash << " AS UBIGINT)";

    result_guard guard {};
    if (duckdb_query(con_, sql.str().c_str(), &guard.res) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }
    return duckdb_value_int64(&guard.res, 0, 0);
}

}  // namespace motif::db
