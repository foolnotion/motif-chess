#include "motif/db/position_store.hpp"

#include <cstdint>
#include <span>

#include "motif/db/types.hpp"

#include <duckdb.h>
#include <tl/expected.hpp>

#include "motif/db/error.hpp"

namespace {

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
constexpr char const* create_position_idx = R"sql(
    CREATE INDEX IF NOT EXISTS idx_position_zobrist_hash
    ON position(zobrist_hash)
)sql";

} // namespace

namespace motif::db {

position_store::position_store(duckdb_connection con) noexcept
    : con_{con}
{
}

auto position_store::initialize_schema() -> result<void>
{
    duckdb_result res{};
    auto const create_ret = duckdb_query(con_, create_position, &res);
    duckdb_destroy_result(&res);
    if (create_ret == DuckDBError) {
        return tl::unexpected{error_code::io_failure};
    }

    duckdb_result idx_res{};
    auto const idx_ret = duckdb_query(con_, create_position_idx, &idx_res);
    duckdb_destroy_result(&idx_res);
    if (idx_ret == DuckDBError) {
        return tl::unexpected{error_code::io_failure};
    }

    return {};
}

auto position_store::insert_batch(std::span<position_row const> rows) -> result<void>
{
    duckdb_appender appender{};
    if (duckdb_appender_create(con_, nullptr, "position", &appender) == DuckDBError) {
        return tl::unexpected{error_code::io_failure};
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
            return tl::unexpected{error_code::io_failure};
        }
    }

    auto const flush_ret = duckdb_appender_flush(appender);
    duckdb_appender_destroy(&appender);
    if (flush_ret == DuckDBError) {
        return tl::unexpected{error_code::io_failure};
    }

    return {};
}

auto position_store::row_count() -> result<std::int64_t>
{
    duckdb_result res{};
    if (duckdb_query(con_, "SELECT count(*) FROM position", &res) == DuckDBError) {
        duckdb_destroy_result(&res);
        return tl::unexpected{error_code::io_failure};
    }
    auto const count = duckdb_value_int64(&res, 0, 0);
    duckdb_destroy_result(&res);
    return count;
}

} // namespace motif::db
