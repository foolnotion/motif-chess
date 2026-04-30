// NOLINTNEXTLINE(portability-avoid-pragma-once)
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <duckdb.h>

#include "motif/db/error.hpp"
#include "motif/db/types.hpp"

namespace motif::db
{

class position_store
{
  public:
    explicit position_store(duckdb_connection con) noexcept;

    auto initialize_schema() -> result<void>;
    auto sort_by_zobrist() -> result<void>;
    auto insert_batch(std::span<position_row const> rows) -> result<void>;
    auto row_count() const -> result<std::int64_t>;
    auto query_by_zobrist(std::uint64_t zobrist_hash, std::size_t limit = 0, std::size_t offset = 0) const
        -> result<std::vector<position_match>>;
    auto query_opening_moves(std::uint64_t zobrist_hash) const -> result<std::vector<opening_move_stat>>;
    auto query_tree_slice(std::uint64_t root_hash, std::uint16_t max_depth) const -> result<std::vector<tree_position_row>>;
    auto sample_zobrist_hashes(std::size_t limit, std::uint64_t seed = 0) const -> result<std::vector<std::uint64_t>>;
    auto delete_by_game_id(std::uint32_t game_id) -> result<void>;
    auto count_by_zobrist(std::uint64_t zobrist_hash) const -> result<std::int64_t>;

  private:
    duckdb_connection con_;
};

}  // namespace motif::db
