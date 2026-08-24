#include <algorithm>
#include <array>
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
        encoded_move  USMALLINT NOT NULL
    )
)sql";

// result/white_elo/black_elo are per-game facts, not per-position ones. Kept
// out of `position` (one row per ply visited, ~44 rows/game on average) and
// normalized here (one row per game) instead -- storing them inline in
// `position` cost ~1.2 GiB of otherwise-redundant, incompressible bytes on a
// 3.36M-game bundle (that table is sorted by zobrist_hash, not by game_id,
// so the redundant per-game values never cluster and DuckDB's compression
// gets no benefit from the repetition).
// language=sql
constexpr auto create_game_result = R"sql(
    CREATE TABLE IF NOT EXISTS game_result (
        game_id    UINTEGER NOT NULL PRIMARY KEY,
        result     TINYINT  NOT NULL,
        white_elo  SMALLINT,
        black_elo  SMALLINT
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

// Rollup entries are only materialized for shallow root positions (see
// create_opening_stats_rollups below); deeper queries fall back to the
// dynamic path. Session 2 found queries beyond this depth have almost no
// transposition (COUNT(DISTINCT root_hash) ~= COUNT(*) past shallow depth),
// so materializing them buys little query-speed benefit for a large,
// storage-dominating share of rollup rows (94.2% of rows were root_ply > 20
// on the 3.36M-game bundle). Not yet validated against real opening-explorer
// usage patterns -- see docs/handoffs/2026-08-17-opening-explorer-storage.md.
constexpr auto opening_stats_max_root_ply = std::uint16_t {20};

// Column positions for SELECT p.game_id, p.ply, gr.result, gr.white_elo, gr.black_elo
// FROM position p JOIN game_result gr ON gr.game_id = p.game_id
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
// Final aggregate projection column in dynamic and materialized queries.
constexpr idx_t elo_weighted_score = 11;
constexpr idx_t transposition_frequency = 12;
}  // namespace opening_stats_col

constexpr auto filtered_game_ids_table = "_filtered_game_ids";

// Rebuilt from the dense position table. Incremental writes drop these tables
// so queries fall back to the exact dynamic path until the next rebuild.
constexpr auto drop_opening_stats_rollups = R"sql(
    DROP TABLE IF EXISTS opening_continuation;
    DROP TABLE IF EXISTS position_summary;
)sql";

// transposition_frequency (child_frequency below) is deliberately computed
// from the *whole* position table, unbounded by opening_stats_max_root_ply:
// it measures how common the child position is overall, independent of how
// deep the root of this particular edge is.
// language=sql
constexpr auto create_opening_stats_rollups_template = R"sql(
    CREATE TABLE opening_continuation AS
    WITH child_frequency AS (
        SELECT
            zobrist_hash,
            CAST(COUNT(*) AS UINTEGER) AS frequency
        FROM (
            SELECT DISTINCT zobrist_hash, game_id
            FROM position
        ) AS unique_games
        GROUP BY zobrist_hash
    ),
    edge_deduped AS (
        SELECT
            p_root.zobrist_hash AS root_hash,
            p_root.game_id,
            p_cont.encoded_move,
            p_cont.zobrist_hash AS child_hash,
            MIN(p_root.ply) AS root_ply,
            gr.result,
            gr.white_elo,
            gr.black_elo
        FROM position p_root
        JOIN position p_cont
            ON p_cont.game_id = p_root.game_id
           AND p_cont.ply = p_root.ply + 1
        JOIN game_result gr ON gr.game_id = p_root.game_id
        WHERE p_root.ply <= {0}
        GROUP BY p_root.zobrist_hash, p_root.game_id, p_cont.encoded_move,
                 p_cont.zobrist_hash, gr.result, gr.white_elo, gr.black_elo
    )
    SELECT
        d.root_hash,
        d.encoded_move,
        d.child_hash,
        CAST(MIN(d.root_ply) AS USMALLINT) AS root_ply,
        CAST(COUNT(*) AS UINTEGER) AS frequency,
        CAST(COUNT(CASE WHEN d.result > 0 THEN 1 END) AS UINTEGER) AS white_wins,
        CAST(COUNT(CASE WHEN d.result = 0 THEN 1 END) AS UINTEGER) AS draws,
        CAST(COUNT(CASE WHEN d.result < 0 THEN 1 END) AS UINTEGER) AS black_wins,
        AVG(CAST(d.white_elo AS DOUBLE)) AS avg_white_elo,
        AVG(CAST(d.black_elo AS DOUBLE)) AS avg_black_elo,
        CAST(MIN(d.game_id) AS UINTEGER) AS eco_sample_min,
        CAST(MAX(d.game_id) AS UINTEGER) AS eco_sample_max,
        SUM(CASE WHEN d.white_elo IS NOT NULL AND d.black_elo IS NOT NULL
                 THEN CAST(d.result AS DOUBLE) * (d.white_elo + d.black_elo) / 2.0
                 ELSE NULL END)
            / NULLIF(SUM(CASE WHEN d.white_elo IS NOT NULL AND d.black_elo IS NOT NULL
                              THEN (d.white_elo + d.black_elo) / 2.0
                              ELSE NULL END), 0) AS elo_weighted_score,
        s.frequency AS transposition_frequency
    FROM edge_deduped d
    JOIN child_frequency s ON s.zobrist_hash = d.child_hash
    GROUP BY d.root_hash, d.encoded_move, d.child_hash, s.frequency
    ORDER BY d.root_hash;
)sql";

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

// position_row still carries result/white_elo/black_elo per instance (every
// row of a game shares the same values, computed once by the import
// pipeline) -- only insert_batch's storage split cares that these belong in
// game_result, not position; callers are unaffected.
//
// Stages rows into a TEMP table via a duckdb_appender, then flushes them into
// game_result with one set-based INSERT ... ON CONFLICT DO NOTHING, instead
// of one prepared-statement round trip per game. The per-game upsert loop was
// the dominant cost of position_store::insert_batch: on the 100k-game bundle
// it took deferred SQLite-import-plus-rebuild from 8s to 192s. See
// docs/handoffs/2026-08-17-opening-explorer-storage.md, Session 8.
auto insert_game_results_for_batch(duckdb_connection con, std::span<motif::db::position_row const> rows) -> motif::db::result<void>
{
    // language=sql
    constexpr auto create_staging = R"sql(
        CREATE TEMP TABLE IF NOT EXISTS _game_result_staging (
            game_id    UINTEGER NOT NULL,
            result     TINYINT  NOT NULL,
            white_elo  SMALLINT,
            black_elo  SMALLINT
        )
    )sql";
    // language=sql
    constexpr auto clear_staging = R"sql(
        DELETE FROM _game_result_staging
    )sql";
    // language=sql
    constexpr auto flush_staging = R"sql(
        INSERT INTO game_result (game_id, result, white_elo, black_elo)
        SELECT game_id, result, white_elo, black_elo FROM _game_result_staging
        ON CONFLICT (game_id) DO NOTHING
    )sql";

    if (auto create_res = run_query(con, create_staging); !create_res) {
        return create_res;
    }
    if (auto clear_res = run_query(con, clear_staging); !clear_res) {
        return clear_res;
    }

    duckdb_appender appender {};
    if (duckdb_appender_create(con, nullptr, "_game_result_staging", &appender) == DuckDBError) {
        duckdb_appender_destroy(&appender);
        return tl::unexpected {motif::db::error_code::io_failure};
    }

    std::optional<motif::db::game_id> previous_game_id;
    for (auto const& row : rows) {
        if (previous_game_id.has_value() && *previous_game_id == row.game_id) {
            continue;
        }
        previous_game_id = row.game_id;

        duckdb_appender_begin_row(appender);
        duckdb_append_uint32(appender, row.game_id.value);
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
            return tl::unexpected {motif::db::error_code::io_failure};
        }
    }

    auto const flush_ret = duckdb_appender_flush(appender);
    duckdb_appender_destroy(&appender);
    if (flush_ret == DuckDBError) {
        return tl::unexpected {motif::db::error_code::io_failure};
    }

    return run_query(con, flush_staging);
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

auto has_opening_stats_rollups(duckdb_connection con) -> motif::db::result<bool>
{
    // language=sql
    constexpr auto sql = R"sql(
        SELECT COUNT(*) > 0
        FROM information_schema.tables
        WHERE table_schema = 'main'
          AND table_name = 'opening_continuation'
    )sql";

    result_guard guard {};
    if (duckdb_query(con, sql, &guard.res) == DuckDBError) {
        return tl::unexpected {motif::db::error_code::io_failure};
    }
    return duckdb_value_boolean(&guard.res, 0, 0);
}

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

auto position_store::lock_connection() const -> std::unique_lock<std::recursive_mutex>
{
    return std::unique_lock {connection_mutex_};
}

auto position_store::initialize_schema() -> result<void>
{
    auto const lock = std::scoped_lock {connection_mutex_};
    result_guard guard {};
    if (duckdb_query(con_, create_position, &guard.res) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }
    result_guard game_result_guard {};
    if (duckdb_query(con_, create_game_result, &game_result_guard.res) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }
    return {};
}

auto position_store::sort_by_zobrist() -> result<void>
{
    auto const lock = std::scoped_lock {connection_mutex_};
    result_guard guard {};
    if (duckdb_query(con_, sort_position_by_zobrist, &guard.res) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }
    return {};
}

auto position_store::rebuild_opening_stats_rollups() -> result<void>
{
    auto const lock = std::scoped_lock {connection_mutex_};
    if (auto drop_res = run_query(con_, drop_opening_stats_rollups); !drop_res) {
        return drop_res;
    }
    auto const sql = fmt::format(create_opening_stats_rollups_template, opening_stats_max_root_ply);
    return run_query(con_, sql.c_str());
}

auto position_store::checkpoint() -> result<void>
{
    // DuckDB refuses CHECKPOINT inside an active transaction, so this is a
    // separate call rather than folded into rebuild_opening_stats_rollups()
    // -- database_manager::sort_positions() and rebuild_position_store()
    // both call that from inside an explicit BEGIN/COMMIT, and running
    // CHECKPOINT there rolled the whole transaction back (reproduced:
    // "import_pipeline: run imports games..." started failing outright).
    // Only import_pipeline.cpp's un-transacted call to
    // rebuild_opening_stats_rollups() (the "no sorting" inline path) needs
    // this: without it, CREATE TABLE opening_continuation there was not
    // reliably visible to a connection that reopens the file fresh
    // (reproduced with database_manager::close() + a new duckdb_open() on
    // the same path; confirmed fixed by this CHECKPOINT, 3/3 repeat runs).
    auto const lock = std::scoped_lock {connection_mutex_};
    return run_query(con_, "CHECKPOINT");
}

auto position_store::insert_batch(std::span<position_row const> rows) -> result<void>
{
    auto const lock = std::scoped_lock {connection_mutex_};
    if (rows.empty()) {
        return {};
    }
    if (auto drop_res = run_query(con_, drop_opening_stats_rollups); !drop_res) {
        return drop_res;
    }
    duckdb_appender appender {};
    if (duckdb_appender_create(con_, nullptr, "position", &appender) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }

    // Vectorized append: writes whole column vectors directly through their
    // data pointers instead of one begin_row/append_*/end_row API-call
    // sequence per row. Measured 225.9 ns/row (~19.65 us/game at the
    // corpus's ~87 rows/game median) for the row-by-row appender in
    // isolation -- this is DuckDB's documented high-throughput path for
    // exactly that case. `position` has no nullable columns, so no validity
    // mask handling is needed.
    std::array<duckdb_logical_type, 4> types {
        duckdb_create_logical_type(DUCKDB_TYPE_UBIGINT),
        duckdb_create_logical_type(DUCKDB_TYPE_UINTEGER),
        duckdb_create_logical_type(DUCKDB_TYPE_USMALLINT),
        duckdb_create_logical_type(DUCKDB_TYPE_USMALLINT),
    };
    duckdb_data_chunk chunk = duckdb_create_data_chunk(types.data(), types.size());
    for (auto& type : types) {
        duckdb_destroy_logical_type(&type);
    }

    auto const vector_size = static_cast<std::size_t>(duckdb_vector_size());
    std::size_t offset = 0;
    while (offset < rows.size()) {
        auto const chunk_rows = std::min(vector_size, rows.size() - offset);

        duckdb_data_chunk_reset(chunk);
        auto const hash_data =
            std::span {static_cast<std::uint64_t*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(chunk, 0))), vector_size};
        auto const game_id_data =
            std::span {static_cast<std::uint32_t*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(chunk, 1))), vector_size};
        auto const ply_data =
            std::span {static_cast<std::uint16_t*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(chunk, 2))), vector_size};
        auto const move_data =
            std::span {static_cast<std::uint16_t*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(chunk, 3))), vector_size};

        for (std::size_t i = 0; i < chunk_rows; ++i) {
            auto const& row = rows[offset + i];
            hash_data[i] = row.zobrist_hash.value;
            game_id_data[i] = row.game_id.value;
            ply_data[i] = row.ply;
            move_data[i] = row.encoded_move;
        }
        duckdb_data_chunk_set_size(chunk, chunk_rows);

        if (duckdb_append_data_chunk(appender, chunk) == DuckDBError) {
            duckdb_destroy_data_chunk(&chunk);
            duckdb_appender_destroy(&appender);
            return tl::unexpected {error_code::io_failure};
        }
        offset += chunk_rows;
    }
    duckdb_destroy_data_chunk(&chunk);

    auto const flush_ret = duckdb_appender_flush(appender);
    duckdb_appender_destroy(&appender);
    if (flush_ret == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }

    return insert_game_results_for_batch(con_, rows);
}

auto position_store::row_count() const -> result<std::int64_t>
{
    auto const lock = std::scoped_lock {connection_mutex_};
    result_guard guard {};
    if (duckdb_query(con_, "SELECT count(*) FROM position", &guard.res) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }
    return duckdb_value_int64(&guard.res, 0, 0);
}

auto position_store::query_by_zobrist(zobrist_hash const hash, std::size_t const limit, std::size_t const offset) const
    -> result<std::vector<position_match>>
{
    auto const lock = std::scoped_lock {connection_mutex_};
    auto sql = fmt::format(
        "SELECT p.game_id, p.ply, gr.result, gr.white_elo, gr.black_elo "
        "FROM position p JOIN game_result gr ON gr.game_id = p.game_id "
        "WHERE p.zobrist_hash = CAST({} AS UBIGINT) ORDER BY p.game_id, p.ply",
        hash.value);
    if (limit > 0) {
        sql += fmt::format(" LIMIT {}", limit);
    }
    if (offset > 0) {
        sql += fmt::format(" OFFSET {}", offset);
    }

    result_guard guard {};
    if (duckdb_query(con_, sql.c_str(), &guard.res) == DuckDBError) {
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

auto position_store::query_min_ply_by_game(zobrist_hash const hash, std::size_t const limit) const -> result<std::vector<position_match>>
{
    auto const lock = std::scoped_lock {connection_mutex_};
    auto sql = fmt::format(
        "SELECT game_id, MIN(ply) AS ply "
        "FROM position WHERE zobrist_hash = CAST({} AS UBIGINT) "
        "GROUP BY game_id ORDER BY game_id",
        hash.value);
    if (limit > 0) {
        sql += fmt::format(" LIMIT {}", limit);
    }

    result_guard guard {};
    if (duckdb_query(con_, sql.c_str(), &guard.res) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }

    auto const nrows = static_cast<std::size_t>(duckdb_row_count(&guard.res));
    std::vector<position_match> matches;
    matches.reserve(nrows);
    constexpr idx_t game_id_col = 0;
    constexpr idx_t ply_col = 1;
    for (std::size_t i = 0; i < nrows; ++i) {
        auto const row = static_cast<idx_t>(i);
        matches.push_back(position_match {
            .game_id = motif::db::game_id {duckdb_value_uint32(&guard.res, game_id_col, row)},
            .ply = duckdb_value_uint16(&guard.res, ply_col, row),
            .result = 0,
            .white_elo = std::nullopt,
            .black_elo = std::nullopt,
        });
    }

    return matches;
}

auto position_store::query_tree_slice(zobrist_hash const root_hash, std::uint16_t const max_depth) const
    -> result<std::vector<tree_position_row>>
{
    auto const lock = std::scoped_lock {connection_mutex_};
    auto const sql = fmt::format(
        "SELECT "
        "p_root.game_id, "
        "p_root.ply AS root_ply, "
        "CAST(p_cont.ply - p_root.ply AS USMALLINT) AS depth, "
        "p_cont.zobrist_hash AS child_hash, "
        "p_cont.encoded_move, "
        "gr.result, "
        "gr.white_elo, "
        "gr.black_elo "
        "FROM position p_root "
        "JOIN position p_cont "
        "ON  p_root.game_id = p_cont.game_id "
        "AND p_cont.ply > p_root.ply "
        "AND p_cont.ply <= p_root.ply + {} "
        "JOIN game_result gr ON gr.game_id = p_root.game_id "
        "WHERE p_root.zobrist_hash = CAST({} AS UBIGINT) "
        "ORDER BY p_root.game_id, p_cont.ply",
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

auto position_store::query_opening_stats(zobrist_hash const hash) const -> result<std::vector<opening_stat_agg_row>>
{
    auto const lock = std::scoped_lock {connection_mutex_};
    auto const rollups_res = has_opening_stats_rollups(con_);
    if (!rollups_res) {
        return tl::unexpected {rollups_res.error()};
    }
    if (*rollups_res) {
        // opening_continuation only materializes roots with
        // ply <= opening_stats_max_root_ply. An empty result here is
        // ambiguous -- genuinely no continuations, or root deeper than the
        // cap -- and in both cases the dynamic path below gives the correct
        // ground truth, so fall through rather than returning early.
        // language=sql
        auto const sql = fmt::format(
            R"sql(
                SELECT
                    encoded_move,
                    child_hash,
                    root_ply,
                    frequency,
                    white_wins,
                    draws,
                    black_wins,
                    avg_white_elo,
                    avg_black_elo,
                    eco_sample_min,
                    eco_sample_max,
                    elo_weighted_score,
                    transposition_frequency
                FROM opening_continuation
                WHERE root_hash = CAST({} AS UBIGINT)
            )sql",
            hash.value);

        result_guard guard {};
        if (duckdb_query(con_, sql.c_str(), &guard.res) == DuckDBError) {
            return tl::unexpected {error_code::io_failure};
        }

        auto const nrows = static_cast<std::size_t>(duckdb_row_count(&guard.res));
        auto rows = std::vector<opening_stat_agg_row> {};
        rows.reserve(nrows);
        for (std::size_t i = 0; i < nrows; ++i) {
            auto const row = static_cast<idx_t>(i);
            rows.push_back(opening_stat_agg_row {
                .cont_encoded_move = duckdb_value_uint16(&guard.res, opening_stats_col::cont_encoded_move, row),
                .cont_hash = zobrist_hash {duckdb_value_uint64(&guard.res, opening_stats_col::cont_hash, row)},
                .root_ply = duckdb_value_uint16(&guard.res, opening_stats_col::root_ply, row),
                .frequency = duckdb_value_uint32(&guard.res, opening_stats_col::frequency, row),
                .transposition_frequency = duckdb_value_uint32(&guard.res, opening_stats_col::transposition_frequency, row),
                .white_wins = duckdb_value_uint32(&guard.res, opening_stats_col::white_wins, row),
                .draws = duckdb_value_uint32(&guard.res, opening_stats_col::draws, row),
                .black_wins = duckdb_value_uint32(&guard.res, opening_stats_col::black_wins, row),
                .avg_white_elo = read_optional_double(guard.res, opening_stats_col::avg_white_elo, row),
                .avg_black_elo = read_optional_double(guard.res, opening_stats_col::avg_black_elo, row),
                .eco_sample_min = game_id {duckdb_value_uint32(&guard.res, opening_stats_col::eco_min, row)},
                .eco_sample_max = game_id {duckdb_value_uint32(&guard.res, opening_stats_col::eco_max, row)},
                .elo_weighted_score = read_optional_double(guard.res, opening_stats_col::elo_weighted_score, row),
            });
        }
        if (!rows.empty()) {
            return rows;
        }
    }

    // deduped: one row per (game, move, child) that visited the root position.
    // child_agg: global distinct-game count for each child position, retained as
    // transposition_frequency while the primary statistics come from deduped.
    // eco_sample uses game_ids from deduped (P→Q path) for board reconstruction.
    // result/white_elo/black_elo are per-game, joined in from game_result
    // (see create_game_result) rather than read inline from position.
    // language=sql
    auto const sql = fmt::format(
        R"sql(
            WITH deduped AS (
                SELECT
                    p_root.game_id,
                    p_cont.encoded_move,
                    p_cont.zobrist_hash AS child_hash,
                    MIN(p_root.ply) AS root_ply,
                    gr.result,
                    gr.white_elo,
                    gr.black_elo,
                    CASE WHEN gr.white_elo IS NOT NULL AND gr.black_elo IS NOT NULL
                         THEN CAST(gr.result AS DOUBLE) * (gr.white_elo + gr.black_elo) / 2.0
                         ELSE NULL END AS weighted_contrib,
                    CASE WHEN gr.white_elo IS NOT NULL AND gr.black_elo IS NOT NULL
                         THEN (gr.white_elo + gr.black_elo) / 2.0
                         ELSE NULL END AS elo_weight
                FROM position p_root
                JOIN position p_cont
                    ON p_cont.game_id = p_root.game_id
                   AND p_cont.ply = p_root.ply + 1
                JOIN game_result gr ON gr.game_id = p_root.game_id
                WHERE p_root.zobrist_hash = CAST({0} AS UBIGINT)
                GROUP BY p_root.game_id, p_cont.encoded_move, p_cont.zobrist_hash,
                         gr.result, gr.white_elo, gr.black_elo
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
                    SELECT DISTINCT p.zobrist_hash, p.game_id, gr.result, gr.white_elo, gr.black_elo
                    FROM position p
                    JOIN game_result gr ON gr.game_id = p.game_id
                    JOIN child_hashes ON child_hashes.child_hash = p.zobrist_hash
                ) AS uniq
                GROUP BY uniq.zobrist_hash
            )
            SELECT
                d.encoded_move,
                d.child_hash,
                MIN(d.root_ply) AS root_ply,
                COUNT(*) AS frequency,
                COUNT(CASE WHEN d.result > 0 THEN 1 END) AS white_wins,
                COUNT(CASE WHEN d.result = 0 THEN 1 END) AS draws,
                COUNT(CASE WHEN d.result < 0 THEN 1 END) AS black_wins,
                AVG(CAST(d.white_elo AS DOUBLE)) AS avg_white_elo,
                AVG(CAST(d.black_elo AS DOUBLE)) AS avg_black_elo,
                MIN(d.game_id) AS eco_sample_min,
                MAX(d.game_id) AS eco_sample_max,
                SUM(d.weighted_contrib) / NULLIF(SUM(d.elo_weight), 0) AS elo_weighted_score,
                MIN(ca.frequency) AS transposition_frequency
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
            .transposition_frequency = duckdb_value_uint32(&guard.res, opening_stats_col::transposition_frequency, row),
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
    auto const lock = std::scoped_lock {connection_mutex_};
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
    auto const lock = std::scoped_lock {connection_mutex_};
    if (auto drop_res = run_query(con_, drop_opening_stats_rollups); !drop_res) {
        return drop_res;
    }
    auto const sql = fmt::format("DELETE FROM position WHERE game_id = CAST({} AS UINTEGER)", game_key.value);
    result_guard guard {};
    if (duckdb_query(con_, sql.c_str(), &guard.res) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }

    auto const game_result_sql = fmt::format("DELETE FROM game_result WHERE game_id = CAST({} AS UINTEGER)", game_key.value);
    result_guard game_result_guard {};
    if (duckdb_query(con_, game_result_sql.c_str(), &game_result_guard.res) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }
    return {};
}

auto position_store::update_elo_for_game(game_id const game_key,
                                         std::optional<std::int16_t> const new_white_elo,
                                         std::optional<std::int16_t> const new_black_elo) -> result<void>
{
    auto const lock = std::scoped_lock {connection_mutex_};
    if (!new_white_elo && !new_black_elo) {
        return {};
    }

    if (auto drop_res = run_query(con_, drop_opening_stats_rollups); !drop_res) {
        return drop_res;
    }

    // game_result now holds one row per game (not one per ply), so this is a
    // single-row update instead of an update across every ply of the game.
    std::string sql = "UPDATE game_result SET ";
    bool first = true;
    if (new_white_elo) {
        sql += fmt::format("white_elo = {}", static_cast<int>(*new_white_elo));
        first = false;
    }
    if (new_black_elo) {
        if (!first) {
            sql += ", ";
        }
        sql += fmt::format("black_elo = {}", static_cast<int>(*new_black_elo));
    }
    sql += fmt::format(" WHERE game_id = CAST({} AS UINTEGER)", game_key.value);

    result_guard guard {};
    if (duckdb_query(con_, sql.c_str(), &guard.res) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }
    return {};
}

auto position_store::update_result_for_game(game_id const game_key, std::int8_t const new_result) -> result<void>
{
    auto const lock = std::scoped_lock {connection_mutex_};
    if (auto drop_res = run_query(con_, drop_opening_stats_rollups); !drop_res) {
        return drop_res;
    }

    auto const sql = fmt::format(
        "UPDATE game_result SET result = {} WHERE game_id = CAST({} AS UINTEGER)", static_cast<int>(new_result), game_key.value);

    result_guard guard {};
    if (duckdb_query(con_, sql.c_str(), &guard.res) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }
    return {};
}

auto position_store::count_by_zobrist(zobrist_hash const hash) const -> result<std::int64_t>
{
    auto const lock = std::scoped_lock {connection_mutex_};
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
    auto const lock = std::scoped_lock {connection_mutex_};
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
    auto const lock = std::scoped_lock {connection_mutex_};
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

    auto const lock = std::scoped_lock {connection_mutex_};
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

    auto const lock = std::scoped_lock {connection_mutex_};
    if (auto populate_res = populate_filtered_game_ids_table(con_, game_ids); !populate_res) {
        return tl::unexpected {populate_res.error()};
    }

    // result/white_elo/black_elo are per-game, joined in from game_result
    // rather than read inline from position (see create_game_result).
    // language=sql
    auto const sql = fmt::format(R"sql(
        WITH deduped AS (
            SELECT
                p_root.game_id,
                p_cont.encoded_move,
                p_cont.zobrist_hash AS child_hash,
                MIN(p_root.ply) AS root_ply,
                gr.result,
                gr.white_elo,
                gr.black_elo,
                CASE WHEN gr.white_elo IS NOT NULL AND gr.black_elo IS NOT NULL
                     THEN CAST(gr.result AS DOUBLE) * (gr.white_elo + gr.black_elo) / 2.0
                     ELSE NULL END AS weighted_contrib,
                CASE WHEN gr.white_elo IS NOT NULL AND gr.black_elo IS NOT NULL
                     THEN (gr.white_elo + gr.black_elo) / 2.0
                     ELSE NULL END AS elo_weight
            FROM position p_root
            JOIN _filtered_game_ids fgi_root ON fgi_root.game_id = p_root.game_id
            JOIN position p_cont
            ON  p_cont.game_id = p_root.game_id
            AND p_cont.ply = p_root.ply + 1
            JOIN game_result gr ON gr.game_id = p_root.game_id
            WHERE p_root.zobrist_hash = CAST({0} AS UBIGINT)
            GROUP BY p_root.game_id, p_cont.encoded_move, p_cont.zobrist_hash,
                     gr.result, gr.white_elo, gr.black_elo
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
                SELECT DISTINCT p.zobrist_hash, p.game_id, gr.result, gr.white_elo, gr.black_elo
                FROM position p
                JOIN _filtered_game_ids fgi ON fgi.game_id = p.game_id
                JOIN game_result gr ON gr.game_id = p.game_id
                JOIN child_hashes ON child_hashes.child_hash = p.zobrist_hash
            ) AS uniq
            GROUP BY uniq.zobrist_hash
        )
        SELECT
            d.encoded_move,
            d.child_hash,
            MIN(d.root_ply) AS root_ply,
            COUNT(*) AS frequency,
            COUNT(CASE WHEN d.result > 0 THEN 1 END) AS white_wins,
            COUNT(CASE WHEN d.result = 0 THEN 1 END) AS draws,
            COUNT(CASE WHEN d.result < 0 THEN 1 END) AS black_wins,
            AVG(CAST(d.white_elo AS DOUBLE)) AS avg_white_elo,
            AVG(CAST(d.black_elo AS DOUBLE)) AS avg_black_elo,
            MIN(d.game_id) AS eco_sample_min,
            MAX(d.game_id) AS eco_sample_max,
            SUM(d.weighted_contrib) / NULLIF(SUM(d.elo_weight), 0) AS elo_weighted_score,
            MIN(ca.frequency) AS transposition_frequency
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
            .transposition_frequency = duckdb_value_uint32(&guard.res, opening_stats_col::transposition_frequency, row),
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
    auto const lock = std::scoped_lock {connection_mutex_};
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
                gr.result,
                CAST(floor(CAST(gr.white_elo + gr.black_elo AS DOUBLE) / 2.0 / {1}) * {1} AS INTEGER) AS elo_bucket_floor
            FROM position p_root
            JOIN position p_cont
                ON p_cont.game_id = p_root.game_id
               AND p_cont.ply = p_root.ply + 1
            JOIN game_result gr ON gr.game_id = p_root.game_id
            WHERE p_root.zobrist_hash = CAST({0} AS UBIGINT)
              AND gr.white_elo IS NOT NULL
              AND gr.black_elo IS NOT NULL
            GROUP BY p_root.game_id, p_cont.encoded_move, gr.result, gr.white_elo, gr.black_elo
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

    auto const lock = std::scoped_lock {connection_mutex_};
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
                gr.result,
                CAST(floor(CAST(gr.white_elo + gr.black_elo AS DOUBLE) / 2.0 / {1}) * {1} AS INTEGER) AS elo_bucket_floor
            FROM position p_root
            JOIN _filtered_game_ids fgi ON fgi.game_id = p_root.game_id
            JOIN position p_cont
                ON p_cont.game_id = p_root.game_id
               AND p_cont.ply = p_root.ply + 1
            JOIN game_result gr ON gr.game_id = p_root.game_id
            WHERE p_root.zobrist_hash = CAST({0} AS UBIGINT)
              AND gr.white_elo IS NOT NULL
              AND gr.black_elo IS NOT NULL
            GROUP BY p_root.game_id, p_cont.encoded_move, gr.result, gr.white_elo, gr.black_elo
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

    auto const lock = std::scoped_lock {connection_mutex_};
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
                gr.result,
                gr.white_elo,
                gr.black_elo
            FROM position p_root
            JOIN _filtered_game_ids fgi ON fgi.game_id = p_root.game_id
            JOIN position p_cont
            ON  p_root.game_id = p_cont.game_id
            AND p_cont.ply > p_root.ply
            AND p_cont.ply <= p_root.ply + {}
            JOIN game_result gr ON gr.game_id = p_root.game_id
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
