#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <utility>
#include <vector>

#include "motif/db/position_store.hpp"

#include <duckdb.h>
#include <fmt/format.h>
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

// Column positions for query_elo_distribution SELECT
namespace elo_distribution_col
{
constexpr idx_t encoded_move = 0;
constexpr idx_t elo_bucket_floor = 1;
constexpr idx_t white_wins = 2;
constexpr idx_t draws = 3;
constexpr idx_t black_wins = 4;
constexpr idx_t game_count = 5;
}  // namespace elo_distribution_col

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
// present only in the filtered overload (query_opening_stats with game_ids)
constexpr idx_t elo_weighted_score = 11;
}  // namespace opening_stats_col

constexpr auto filtered_game_ids_table = "_filtered_game_ids";

auto run_query(duckdb_connection con, char const* sql) -> motif::db::result<void>
{
    duckdb_result res {};
    if (duckdb_query(con, sql, &res) == DuckDBError) {
        duckdb_destroy_result(&res);
        return tl::unexpected {motif::db::error_code::io_failure};
    }
    duckdb_destroy_result(&res);
    return {};
}

auto populate_filtered_game_ids_table(duckdb_connection con, std::vector<motif::db::game_id> const& game_ids) -> motif::db::result<void>
{
    // language=sql
    constexpr auto create_filtered_game_ids = R"sql(
        CREATE TEMP TABLE IF NOT EXISTS _filtered_game_ids (
            game_id UINTEGER NOT NULL PRIMARY KEY
        )
    )sql";
    // language=sql
    constexpr auto clear_filtered_game_ids = R"sql(
        DELETE FROM _filtered_game_ids
    )sql";

    if (auto create_res = run_query(con, create_filtered_game_ids); !create_res) {
        return create_res;
    }
    if (auto clear_res = run_query(con, clear_filtered_game_ids); !clear_res) {
        return clear_res;
    }

    duckdb_appender appender {};
    if (duckdb_appender_create(con, nullptr, filtered_game_ids_table, &appender) == DuckDBError) {
        return tl::unexpected {motif::db::error_code::io_failure};
    }

    for (auto const game_id : game_ids) {
        if (duckdb_appender_begin_row(appender) == DuckDBError) {
            duckdb_appender_destroy(&appender);
            return tl::unexpected {motif::db::error_code::io_failure};
        }
        if (duckdb_append_uint32(appender, game_id.value) == DuckDBError) {
            duckdb_appender_destroy(&appender);
            return tl::unexpected {motif::db::error_code::io_failure};
        }
        if (duckdb_appender_end_row(appender) == DuckDBError) {
            duckdb_appender_destroy(&appender);
            return tl::unexpected {motif::db::error_code::io_failure};
        }
    }

    auto const flush_res = duckdb_appender_flush(appender);
    duckdb_appender_destroy(&appender);
    if (flush_res == DuckDBError) {
        return tl::unexpected {motif::db::error_code::io_failure};
    }

    return {};
}

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

position_store::position_store(position_store&& other) noexcept
    : con_ {std::exchange(other.con_, nullptr)}
{
}

auto position_store::operator=(position_store&& other) noexcept -> position_store&
{
    if (this != &other) {
        con_ = std::exchange(other.con_, nullptr);
    }
    return *this;
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
        duckdb_append_uint64(appender, row.zobrist_hash.value);
        duckdb_append_uint32(appender, row.game_id.value);
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

auto position_store::query_by_zobrist(zobrist_hash const hash, std::size_t const limit, std::size_t const offset) const
    -> result<std::vector<position_match>>
{
    std::ostringstream sql;
    sql << "SELECT game_id, ply, result, white_elo, black_elo FROM position " "WHERE zobrist_hash = CAST(" << hash.value
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
            .game_id = motif::db::game_id {duckdb_value_uint32(&guard.res, by_zobrist_col::game_id, row)},
            .ply = duckdb_value_uint16(&guard.res, by_zobrist_col::ply, row),
            .result = duckdb_value_int8(&guard.res, by_zobrist_col::result, row),
            .white_elo = read_optional_int16(guard.res, by_zobrist_col::white_elo, row),
            .black_elo = read_optional_int16(guard.res, by_zobrist_col::black_elo, row),
        });
    }

    return matches;
}

auto position_store::query_tree_slice(zobrist_hash const root_hash, std::uint16_t const max_depth) const
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
         << max_depth << " WHERE p_root.zobrist_hash = CAST(" << root_hash.value << " AS UBIGINT)"
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
            .game_id = motif::db::game_id {duckdb_value_uint32(&guard.res, tree_slice_col::game_id, row)},
            .root_ply = duckdb_value_uint16(&guard.res, tree_slice_col::root_ply, row),
            .depth = duckdb_value_uint16(&guard.res, tree_slice_col::depth, row),
            .encoded_move = duckdb_value_uint16(&guard.res, tree_slice_col::encoded_move, row),
            .child_hash = motif::db::zobrist_hash {duckdb_value_uint64(&guard.res, tree_slice_col::child_hash, row)},
            .result = duckdb_value_int8(&guard.res, tree_slice_col::result, row),
            .white_elo = read_optional_int16(guard.res, tree_slice_col::white_elo, row),
            .black_elo = read_optional_int16(guard.res, tree_slice_col::black_elo, row),
        });
    }

    return rows;
}

auto position_store::query_opening_stats(zobrist_hash const hash) const -> result<std::vector<opening_stat_agg_row>>
{
    std::ostringstream sql;
    // deduped: one row per (game, move, child) that visited the root position.
    // child_agg: global stats for each child position across ALL games (not just
    // those that came through this parent), so transpositions are counted.
    // eco_sample uses game_ids from deduped (P→Q path) for board reconstruction.
    sql << "WITH deduped AS ("
           "SELECT "
           "p_root.game_id, "
           "p_cont.encoded_move, "
           "p_cont.zobrist_hash AS child_hash, "
           "MIN(p_root.ply) AS root_ply, "
           "p_root.result, "
           "p_root.white_elo, "
           "p_root.black_elo, "
           "CASE WHEN p_root.white_elo IS NOT NULL AND p_root.black_elo IS NOT NULL "
           "THEN CAST(p_root.result AS DOUBLE) * (p_root.white_elo + p_root.black_elo) / 2.0 "
           "ELSE NULL END AS weighted_contrib, "
           "CASE WHEN p_root.white_elo IS NOT NULL AND p_root.black_elo IS NOT NULL "
           "THEN (p_root.white_elo + p_root.black_elo) / 2.0 "
           "ELSE NULL END AS elo_weight "
           "FROM position p_root "
           "JOIN position p_cont "
           "ON  p_cont.game_id = p_root.game_id "
           "AND p_cont.ply = p_root.ply + 1 "
         << "WHERE p_root.zobrist_hash = CAST(" << hash.value << " AS UBIGINT) "
        << "GROUP BY p_root.game_id, p_cont.encoded_move, p_cont.zobrist_hash, "
           "p_root.result, p_root.white_elo, p_root.black_elo"
           "), "
           "child_hashes AS ("
           "SELECT DISTINCT child_hash FROM deduped"
           "), "
           "child_agg AS ("
           "SELECT "
           "uniq.zobrist_hash, "
           "COUNT(*) AS frequency, "
           "COUNT(CASE WHEN uniq.result > 0 THEN 1 END) AS white_wins, "
           "COUNT(CASE WHEN uniq.result = 0 THEN 1 END) AS draws, "
           "COUNT(CASE WHEN uniq.result < 0 THEN 1 END) AS black_wins, "
           "AVG(CAST(uniq.white_elo AS DOUBLE)) AS avg_white_elo, "
           "AVG(CAST(uniq.black_elo AS DOUBLE)) AS avg_black_elo "
           "FROM ("
           "SELECT DISTINCT p.zobrist_hash, p.game_id, p.result, p.white_elo, p.black_elo "
           "FROM position p "
           "JOIN child_hashes ON child_hashes.child_hash = p.zobrist_hash"
           ") AS uniq "
           "GROUP BY uniq.zobrist_hash"
           ") "
           "SELECT "
           "d.encoded_move, "
           "d.child_hash, "
           "MIN(d.root_ply) AS root_ply, "
           "MIN(ca.frequency) AS frequency, "
           "MIN(ca.white_wins) AS white_wins, "
           "MIN(ca.draws) AS draws, "
           "MIN(ca.black_wins) AS black_wins, "
           "MIN(ca.avg_white_elo) AS avg_white_elo, "
           "MIN(ca.avg_black_elo) AS avg_black_elo, "
           "MIN(d.game_id) AS eco_sample_min, "
           "MAX(d.game_id) AS eco_sample_max, "
           "SUM(d.weighted_contrib) / NULLIF(SUM(d.elo_weight), 0) AS elo_weighted_score "
           "FROM deduped d "
           "JOIN child_agg ca ON ca.zobrist_hash = d.child_hash "
           "GROUP BY d.encoded_move, d.child_hash";

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
            .cont_hash = motif::db::zobrist_hash {duckdb_value_uint64(&guard.res, opening_stats_col::cont_hash, row)},
            .root_ply = duckdb_value_uint16(&guard.res, opening_stats_col::root_ply, row),
            .frequency = duckdb_value_uint32(&guard.res, opening_stats_col::frequency, row),
            .white_wins = duckdb_value_uint32(&guard.res, opening_stats_col::white_wins, row),
            .draws = duckdb_value_uint32(&guard.res, opening_stats_col::draws, row),
            .black_wins = duckdb_value_uint32(&guard.res, opening_stats_col::black_wins, row),
            .avg_white_elo = read_optional_double(guard.res, opening_stats_col::avg_white_elo, row),
            .avg_black_elo = read_optional_double(guard.res, opening_stats_col::avg_black_elo, row),
            .eco_sample_min = motif::db::game_id {duckdb_value_uint32(&guard.res, opening_stats_col::eco_min, row)},
            .eco_sample_max = motif::db::game_id {duckdb_value_uint32(&guard.res, opening_stats_col::eco_max, row)},
            .elo_weighted_score = read_optional_double(guard.res, opening_stats_col::elo_weighted_score, row),
        });
    }

    return rows;
}

auto position_store::sample_zobrist_hashes(std::size_t const limit, std::uint64_t const seed) const -> result<std::vector<zobrist_hash>>
{
    std::ostringstream sql;
    sql << "SELECT DISTINCT zobrist_hash FROM position USING SAMPLE reservoir(" << limit << " ROWS) REPEATABLE (" << seed << ")";

    result_guard guard {};
    if (duckdb_query(con_, sql.str().c_str(), &guard.res) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }

    auto const nrows = static_cast<std::size_t>(duckdb_row_count(&guard.res));
    std::vector<zobrist_hash> hashes;
    hashes.reserve(nrows);

    for (std::size_t i = 0; i < nrows; ++i) {
        hashes.push_back(motif::db::zobrist_hash {duckdb_value_uint64(&guard.res, 0, static_cast<idx_t>(i))});
    }

    return hashes;
}

auto position_store::delete_by_game_id(game_id const game_key) -> result<void>
{
    std::ostringstream sql;
    sql << "DELETE FROM position WHERE game_id = CAST(" << game_key.value << " AS UINTEGER)";

    result_guard guard {};
    if (duckdb_query(con_, sql.str().c_str(), &guard.res) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }
    return {};
}

auto position_store::update_elo_for_game(game_id const game_key,
                                         std::optional<std::int16_t> const new_white_elo,
                                         std::optional<std::int16_t> const new_black_elo) -> result<void>
{
    if (!new_white_elo && !new_black_elo) {
        return {};
    }

    std::ostringstream sql;
    sql << "UPDATE position SET ";
    bool first = true;
    if (new_white_elo) {
        sql << "white_elo = " << static_cast<int>(*new_white_elo);
        first = false;
    }
    if (new_black_elo) {
        if (!first) {
            sql << ", ";
        }
        sql << "black_elo = " << static_cast<int>(*new_black_elo);
    }
    sql << " WHERE game_id = CAST(" << game_key.value << " AS UINTEGER)";

    result_guard guard {};
    if (duckdb_query(con_, sql.str().c_str(), &guard.res) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }
    return {};
}

auto position_store::count_by_zobrist(zobrist_hash const hash) const -> result<std::int64_t>
{
    std::ostringstream sql;
    sql << "SELECT COUNT(*) FROM position WHERE zobrist_hash = CAST(" << hash.value << " AS UBIGINT)";

    result_guard guard {};
    if (duckdb_query(con_, sql.str().c_str(), &guard.res) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }
    return duckdb_value_int64(&guard.res, 0, 0);
}

auto position_store::count_distinct_games_by_zobrist(zobrist_hash const hash) const -> result<std::int64_t>
{
    std::ostringstream sql;
    sql << "SELECT COUNT(DISTINCT game_id) FROM position WHERE zobrist_hash = CAST(" << hash.value << " AS UBIGINT)";

    result_guard guard {};
    if (duckdb_query(con_, sql.str().c_str(), &guard.res) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }
    return duckdb_value_int64(&guard.res, 0, 0);
}

auto position_store::distinct_game_ids_by_zobrist(zobrist_hash const hash) const -> result<std::vector<game_id>>
{
    std::ostringstream sql;
    sql << "SELECT DISTINCT game_id FROM position WHERE zobrist_hash = CAST(" << hash.value << " AS UBIGINT) ORDER BY game_id";

    result_guard guard {};
    if (duckdb_query(con_, sql.str().c_str(), &guard.res) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }

    auto const nrows = static_cast<std::size_t>(duckdb_row_count(&guard.res));
    auto game_ids = std::vector<game_id> {};
    game_ids.reserve(nrows);
    for (std::size_t index = 0; index < nrows; ++index) {
        game_ids.push_back(game_id {duckdb_value_uint32(&guard.res, 0, static_cast<idx_t>(index))});
    }

    return game_ids;
}

auto position_store::count_distinct_games_by_zobrist(zobrist_hash const hash, std::vector<game_id> const& game_ids) const
    -> result<std::int64_t>
{
    if (game_ids.empty()) {
        return std::int64_t {0};
    }

    auto const lock = std::scoped_lock {filtered_game_ids_mutex_};
    if (auto populate_res = populate_filtered_game_ids_table(con_, game_ids); !populate_res) {
        return tl::unexpected {populate_res.error()};
    }

    auto const sql = fmt::format(
        R"sql(
            SELECT COUNT(DISTINCT p.game_id)
            FROM position p
            JOIN _filtered_game_ids fgi ON fgi.game_id = p.game_id
            WHERE p.zobrist_hash = CAST({} AS UBIGINT)
        )sql",
        hash.value);

    result_guard guard {};
    if (duckdb_query(con_, sql.c_str(), &guard.res) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }
    return duckdb_value_int64(&guard.res, 0, 0);
}

auto position_store::query_opening_stats(zobrist_hash const hash, std::vector<game_id> const& game_ids) const
    -> result<std::vector<opening_stat_agg_row>>
{
    if (game_ids.empty()) {
        return std::vector<opening_stat_agg_row> {};
    }

    auto const lock = std::scoped_lock {filtered_game_ids_mutex_};
    if (auto populate_res = populate_filtered_game_ids_table(con_, game_ids); !populate_res) {
        return tl::unexpected {populate_res.error()};
    }

    // language=sql
    auto const sql = fmt::format(R"sql(
        WITH deduped AS (
            SELECT
                p_root.game_id,
                p_cont.encoded_move,
                p_cont.zobrist_hash AS child_hash,
                MIN(p_root.ply) AS root_ply,
                p_root.result,
                p_root.white_elo,
                p_root.black_elo,
                CASE WHEN p_root.white_elo IS NOT NULL AND p_root.black_elo IS NOT NULL
                     THEN CAST(p_root.result AS DOUBLE) * (p_root.white_elo + p_root.black_elo) / 2.0
                     ELSE NULL END AS weighted_contrib,
                CASE WHEN p_root.white_elo IS NOT NULL AND p_root.black_elo IS NOT NULL
                     THEN (p_root.white_elo + p_root.black_elo) / 2.0
                     ELSE NULL END AS elo_weight
            FROM position p_root
            JOIN _filtered_game_ids fgi_root ON fgi_root.game_id = p_root.game_id
            JOIN position p_cont
            ON  p_cont.game_id = p_root.game_id
            AND p_cont.ply = p_root.ply + 1
            WHERE p_root.zobrist_hash = CAST({0} AS UBIGINT)
            GROUP BY p_root.game_id, p_cont.encoded_move, p_cont.zobrist_hash,
                     p_root.result, p_root.white_elo, p_root.black_elo
        ),
        child_hashes AS (
            SELECT DISTINCT child_hash FROM deduped
        ),
        child_agg AS (
            SELECT
                uniq.zobrist_hash,
                COUNT(*) AS frequency,
                COUNT(CASE WHEN uniq.result > 0 THEN 1 END) AS white_wins,
                COUNT(CASE WHEN uniq.result = 0 THEN 1 END) AS draws,
                COUNT(CASE WHEN uniq.result < 0 THEN 1 END) AS black_wins,
                AVG(CAST(uniq.white_elo AS DOUBLE)) AS avg_white_elo,
                AVG(CAST(uniq.black_elo AS DOUBLE)) AS avg_black_elo
            FROM (
                SELECT DISTINCT p.zobrist_hash, p.game_id, p.result, p.white_elo, p.black_elo
                FROM position p
                JOIN _filtered_game_ids fgi ON fgi.game_id = p.game_id
                JOIN child_hashes ON child_hashes.child_hash = p.zobrist_hash
            ) AS uniq
            GROUP BY uniq.zobrist_hash
        )
        SELECT
            d.encoded_move,
            d.child_hash,
            MIN(d.root_ply) AS root_ply,
            MIN(ca.frequency) AS frequency,
            MIN(ca.white_wins) AS white_wins,
            MIN(ca.draws) AS draws,
            MIN(ca.black_wins) AS black_wins,
            MIN(ca.avg_white_elo) AS avg_white_elo,
            MIN(ca.avg_black_elo) AS avg_black_elo,
            MIN(d.game_id) AS eco_sample_min,
            MAX(d.game_id) AS eco_sample_max,
            SUM(d.weighted_contrib) / NULLIF(SUM(d.elo_weight), 0) AS elo_weighted_score
        FROM deduped d
        JOIN child_agg ca ON ca.zobrist_hash = d.child_hash
        GROUP BY d.encoded_move, d.child_hash
    )sql",
                                 hash.value);

    result_guard guard {};
    if (duckdb_query(con_, sql.c_str(), &guard.res) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }

    auto const nrows = static_cast<std::size_t>(duckdb_row_count(&guard.res));
    std::vector<opening_stat_agg_row> rows;
    rows.reserve(nrows);

    for (std::size_t i = 0; i < nrows; ++i) {
        auto const row = static_cast<idx_t>(i);
        rows.push_back(opening_stat_agg_row {
            .cont_encoded_move = duckdb_value_uint16(&guard.res, opening_stats_col::cont_encoded_move, row),
            .cont_hash = motif::db::zobrist_hash {duckdb_value_uint64(&guard.res, opening_stats_col::cont_hash, row)},
            .root_ply = duckdb_value_uint16(&guard.res, opening_stats_col::root_ply, row),
            .frequency = duckdb_value_uint32(&guard.res, opening_stats_col::frequency, row),
            .white_wins = duckdb_value_uint32(&guard.res, opening_stats_col::white_wins, row),
            .draws = duckdb_value_uint32(&guard.res, opening_stats_col::draws, row),
            .black_wins = duckdb_value_uint32(&guard.res, opening_stats_col::black_wins, row),
            .avg_white_elo = read_optional_double(guard.res, opening_stats_col::avg_white_elo, row),
            .avg_black_elo = read_optional_double(guard.res, opening_stats_col::avg_black_elo, row),
            .eco_sample_min = motif::db::game_id {duckdb_value_uint32(&guard.res, opening_stats_col::eco_min, row)},
            .eco_sample_max = motif::db::game_id {duckdb_value_uint32(&guard.res, opening_stats_col::eco_max, row)},
            .elo_weighted_score = read_optional_double(guard.res, opening_stats_col::elo_weighted_score, row),
        });
    }

    return rows;
}

auto position_store::query_elo_distribution(zobrist_hash const hash, int const bucket_width) const
    -> result<std::vector<elo_distribution_row>>
{
    if (bucket_width <= 0) {
        return tl::unexpected {error_code::invalid_argument};
    }
    // language=sql
    auto const sql = fmt::format(
        R"sql(
        WITH deduped AS (
            SELECT
                p_root.game_id,
                p_cont.encoded_move,
                p_root.result,
                CAST(floor(CAST(p_root.white_elo + p_root.black_elo AS DOUBLE) / 2.0 / {1}) * {1} AS INTEGER) AS elo_bucket_floor
            FROM position p_root
            JOIN position p_cont
                ON p_cont.game_id = p_root.game_id
               AND p_cont.ply = p_root.ply + 1
            WHERE p_root.zobrist_hash = CAST({0} AS UBIGINT)
              AND p_root.white_elo IS NOT NULL
              AND p_root.black_elo IS NOT NULL
            GROUP BY p_root.game_id, p_cont.encoded_move, p_root.result, p_root.white_elo, p_root.black_elo
        ),
        move_buckets AS (
            SELECT
                encoded_move,
                elo_bucket_floor,
                COUNT(CASE WHEN result > 0 THEN 1 END) AS white_wins,
                COUNT(CASE WHEN result = 0 THEN 1 END) AS draws,
                COUNT(CASE WHEN result < 0 THEN 1 END) AS black_wins,
                COUNT(*) AS game_count
            FROM deduped
            GROUP BY encoded_move, elo_bucket_floor
        ),
        per_move_range AS (
            SELECT encoded_move,
                   MIN(elo_bucket_floor) AS min_bucket,
                   MAX(elo_bucket_floor) AS max_bucket
            FROM move_buckets
            GROUP BY encoded_move
        ),
        global_range AS (
            SELECT MIN(min_bucket) AS gmin, MAX(max_bucket) AS gmax
            FROM per_move_range
        ),
        all_buckets AS (
            SELECT CAST(unnest(generate_series(gmin::BIGINT, gmax::BIGINT, {1}::BIGINT)) AS INTEGER) AS bucket
            FROM global_range
            WHERE gmin IS NOT NULL
        ),
        all_moves AS (
            SELECT DISTINCT encoded_move FROM move_buckets
        ),
        bucket_spine AS (
            SELECT m.encoded_move, ab.bucket AS elo_bucket_floor
            FROM all_moves m
            CROSS JOIN all_buckets ab
            JOIN per_move_range r ON r.encoded_move = m.encoded_move
            WHERE ab.bucket >= r.min_bucket
              AND ab.bucket <= r.max_bucket
        )
        SELECT
            CAST(bs.encoded_move AS USMALLINT) AS encoded_move,
            bs.elo_bucket_floor,
            CAST(COALESCE(mb.white_wins, 0) AS UINTEGER) AS white_wins,
            CAST(COALESCE(mb.draws, 0) AS UINTEGER) AS draws,
            CAST(COALESCE(mb.black_wins, 0) AS UINTEGER) AS black_wins,
            CAST(COALESCE(mb.game_count, 0) AS UINTEGER) AS game_count
        FROM bucket_spine bs
        LEFT JOIN move_buckets mb
            ON mb.encoded_move = bs.encoded_move
           AND mb.elo_bucket_floor = bs.elo_bucket_floor
        ORDER BY bs.encoded_move, bs.elo_bucket_floor
        )sql",
        hash.value,
        bucket_width);

    result_guard guard {};
    if (duckdb_query(con_, sql.c_str(), &guard.res) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }

    auto const nrows = static_cast<std::size_t>(duckdb_row_count(&guard.res));
    std::vector<elo_distribution_row> rows;
    rows.reserve(nrows);

    for (std::size_t i = 0; i < nrows; ++i) {
        auto const row = static_cast<idx_t>(i);
        rows.push_back(elo_distribution_row {
            .encoded_move = duckdb_value_uint16(&guard.res, elo_distribution_col::encoded_move, row),
            .elo_bucket_floor = duckdb_value_int32(&guard.res, elo_distribution_col::elo_bucket_floor, row),
            .white_wins = duckdb_value_uint32(&guard.res, elo_distribution_col::white_wins, row),
            .draws = duckdb_value_uint32(&guard.res, elo_distribution_col::draws, row),
            .black_wins = duckdb_value_uint32(&guard.res, elo_distribution_col::black_wins, row),
            .game_count = duckdb_value_uint32(&guard.res, elo_distribution_col::game_count, row),
        });
    }

    return rows;
}

auto position_store::query_elo_distribution(zobrist_hash const hash, std::vector<game_id> const& game_ids, int const bucket_width) const
    -> result<std::vector<elo_distribution_row>>
{
    if (bucket_width <= 0) {
        return tl::unexpected {error_code::invalid_argument};
    }
    if (game_ids.empty()) {
        return std::vector<elo_distribution_row> {};
    }

    auto const lock = std::scoped_lock {filtered_game_ids_mutex_};
    if (auto populate_res = populate_filtered_game_ids_table(con_, game_ids); !populate_res) {
        return tl::unexpected {populate_res.error()};
    }

    // language=sql
    auto const sql = fmt::format(
        R"sql(
        WITH deduped AS (
            SELECT
                p_root.game_id,
                p_cont.encoded_move,
                p_root.result,
                CAST(floor(CAST(p_root.white_elo + p_root.black_elo AS DOUBLE) / 2.0 / {1}) * {1} AS INTEGER) AS elo_bucket_floor
            FROM position p_root
            JOIN _filtered_game_ids fgi ON fgi.game_id = p_root.game_id
            JOIN position p_cont
                ON p_cont.game_id = p_root.game_id
               AND p_cont.ply = p_root.ply + 1
            WHERE p_root.zobrist_hash = CAST({0} AS UBIGINT)
              AND p_root.white_elo IS NOT NULL
              AND p_root.black_elo IS NOT NULL
            GROUP BY p_root.game_id, p_cont.encoded_move, p_root.result, p_root.white_elo, p_root.black_elo
        ),
        move_buckets AS (
            SELECT
                encoded_move,
                elo_bucket_floor,
                COUNT(CASE WHEN result > 0 THEN 1 END) AS white_wins,
                COUNT(CASE WHEN result = 0 THEN 1 END) AS draws,
                COUNT(CASE WHEN result < 0 THEN 1 END) AS black_wins,
                COUNT(*) AS game_count
            FROM deduped
            GROUP BY encoded_move, elo_bucket_floor
        ),
        per_move_range AS (
            SELECT encoded_move,
                   MIN(elo_bucket_floor) AS min_bucket,
                   MAX(elo_bucket_floor) AS max_bucket
            FROM move_buckets
            GROUP BY encoded_move
        ),
        global_range AS (
            SELECT MIN(min_bucket) AS gmin, MAX(max_bucket) AS gmax
            FROM per_move_range
        ),
        all_buckets AS (
            SELECT CAST(unnest(generate_series(gmin::BIGINT, gmax::BIGINT, {1}::BIGINT)) AS INTEGER) AS bucket
            FROM global_range
            WHERE gmin IS NOT NULL
        ),
        all_moves AS (
            SELECT DISTINCT encoded_move FROM move_buckets
        ),
        bucket_spine AS (
            SELECT m.encoded_move, ab.bucket AS elo_bucket_floor
            FROM all_moves m
            CROSS JOIN all_buckets ab
            JOIN per_move_range r ON r.encoded_move = m.encoded_move
            WHERE ab.bucket >= r.min_bucket
              AND ab.bucket <= r.max_bucket
        )
        SELECT
            CAST(bs.encoded_move AS USMALLINT) AS encoded_move,
            bs.elo_bucket_floor,
            CAST(COALESCE(mb.white_wins, 0) AS UINTEGER) AS white_wins,
            CAST(COALESCE(mb.draws, 0) AS UINTEGER) AS draws,
            CAST(COALESCE(mb.black_wins, 0) AS UINTEGER) AS black_wins,
            CAST(COALESCE(mb.game_count, 0) AS UINTEGER) AS game_count
        FROM bucket_spine bs
        LEFT JOIN move_buckets mb
            ON mb.encoded_move = bs.encoded_move
           AND mb.elo_bucket_floor = bs.elo_bucket_floor
        ORDER BY bs.encoded_move, bs.elo_bucket_floor
        )sql",
        hash.value,
        bucket_width);

    result_guard guard {};
    if (duckdb_query(con_, sql.c_str(), &guard.res) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }

    auto const nrows = static_cast<std::size_t>(duckdb_row_count(&guard.res));
    std::vector<elo_distribution_row> rows;
    rows.reserve(nrows);

    for (std::size_t i = 0; i < nrows; ++i) {
        auto const row = static_cast<idx_t>(i);
        rows.push_back(elo_distribution_row {
            .encoded_move = duckdb_value_uint16(&guard.res, elo_distribution_col::encoded_move, row),
            .elo_bucket_floor = duckdb_value_int32(&guard.res, elo_distribution_col::elo_bucket_floor, row),
            .white_wins = duckdb_value_uint32(&guard.res, elo_distribution_col::white_wins, row),
            .draws = duckdb_value_uint32(&guard.res, elo_distribution_col::draws, row),
            .black_wins = duckdb_value_uint32(&guard.res, elo_distribution_col::black_wins, row),
            .game_count = duckdb_value_uint32(&guard.res, elo_distribution_col::game_count, row),
        });
    }

    return rows;
}

auto position_store::query_tree_slice(zobrist_hash const root_hash,
                                      std::uint16_t const max_depth,
                                      std::vector<game_id> const& game_ids) const -> result<std::vector<tree_position_row>>
{
    if (game_ids.empty()) {
        return std::vector<tree_position_row> {};
    }

    auto const lock = std::scoped_lock {filtered_game_ids_mutex_};
    if (auto populate_res = populate_filtered_game_ids_table(con_, game_ids); !populate_res) {
        return tl::unexpected {populate_res.error()};
    }

    auto const sql = fmt::format(
        R"sql(
            SELECT
                p_root.game_id,
                p_root.ply AS root_ply,
                CAST(p_cont.ply - p_root.ply AS USMALLINT) AS depth,
                p_cont.zobrist_hash AS child_hash,
                p_cont.encoded_move,
                p_cont.result,
                p_cont.white_elo,
                p_cont.black_elo
            FROM position p_root
            JOIN _filtered_game_ids fgi ON fgi.game_id = p_root.game_id
            JOIN position p_cont
            ON  p_root.game_id = p_cont.game_id
            AND p_cont.ply > p_root.ply
            AND p_cont.ply <= p_root.ply + {}
            WHERE p_root.zobrist_hash = CAST({} AS UBIGINT)
            ORDER BY p_root.game_id, p_cont.ply
        )sql",
        max_depth,
        root_hash.value);

    result_guard guard {};
    if (duckdb_query(con_, sql.c_str(), &guard.res) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }

    auto const nrows = static_cast<std::size_t>(duckdb_row_count(&guard.res));
    std::vector<tree_position_row> rows;
    rows.reserve(nrows);

    for (std::size_t i = 0; i < nrows; ++i) {
        auto const row = static_cast<idx_t>(i);
        rows.push_back(tree_position_row {
            .game_id = motif::db::game_id {duckdb_value_uint32(&guard.res, tree_slice_col::game_id, row)},
            .root_ply = duckdb_value_uint16(&guard.res, tree_slice_col::root_ply, row),
            .depth = duckdb_value_uint16(&guard.res, tree_slice_col::depth, row),
            .encoded_move = duckdb_value_uint16(&guard.res, tree_slice_col::encoded_move, row),
            .child_hash = motif::db::zobrist_hash {duckdb_value_uint64(&guard.res, tree_slice_col::child_hash, row)},
            .result = duckdb_value_int8(&guard.res, tree_slice_col::result, row),
            .white_elo = read_optional_int16(guard.res, tree_slice_col::white_elo, row),
            .black_elo = read_optional_int16(guard.res, tree_slice_col::black_elo, row),
        });
    }

    return rows;
}

}  // namespace motif::db
