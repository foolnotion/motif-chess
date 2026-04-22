// NOLINTNEXTLINE(portability-avoid-pragma-once)
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <duckdb.h>

#include "motif/db/error.hpp"
#include "motif/db/types.hpp"

namespace motif::db {

class position_store {
public:
    explicit position_store(duckdb_connection con) noexcept;

    auto initialize_schema() -> result<void>;
    auto create_zobrist_index() -> result<void>;
    auto drop_zobrist_index() -> result<void>;
    auto sort_by_zobrist() -> result<void>;
    auto insert_batch(std::span<position_row const> rows) -> result<void>;
    auto row_count() -> result<std::int64_t>;
    auto query_by_zobrist(std::uint64_t zobrist_hash)
        -> result<std::vector<position_match>>;
    auto query_opening_moves(std::uint64_t zobrist_hash)
        -> result<std::vector<opening_move_stat>>;
    auto sample_zobrist_hashes(std::size_t limit)
        -> result<std::vector<std::uint64_t>>;

private:
    duckdb_connection con_;
};

} // namespace motif::db
