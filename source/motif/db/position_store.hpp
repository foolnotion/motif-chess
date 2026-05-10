#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
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
    ~position_store() = default;
    position_store(position_store&& other) noexcept;
    auto operator=(position_store&& other) noexcept -> position_store&;

    position_store(position_store const&) = delete;
    auto operator=(position_store const&) -> position_store& = delete;

    auto initialize_schema() -> result<void>;
    auto sort_by_zobrist() -> result<void>;
    auto insert_batch(std::span<position_row const> rows) -> result<void>;
    auto row_count() const -> result<std::int64_t>;
    auto query_by_zobrist(zobrist_hash hash, std::size_t limit = 0, std::size_t offset = 0) const -> result<std::vector<position_match>>;
    auto query_tree_slice(zobrist_hash root_hash, std::uint16_t max_depth) const -> result<std::vector<tree_position_row>>;
    auto query_opening_stats(zobrist_hash hash) const -> result<std::vector<opening_stat_agg_row>>;
    auto query_opening_stats(zobrist_hash hash, std::vector<game_id> const& game_ids) const -> result<std::vector<opening_stat_agg_row>>;
    auto query_tree_slice(zobrist_hash root_hash, std::uint16_t max_depth, std::vector<game_id> const& game_ids) const
        -> result<std::vector<tree_position_row>>;
    auto sample_zobrist_hashes(std::size_t limit, std::uint64_t seed = 0) const -> result<std::vector<zobrist_hash>>;
    auto delete_by_game_id(game_id game_key) -> result<void>;
    auto update_elo_for_game(game_id game_key, std::optional<std::int16_t> new_white_elo, std::optional<std::int16_t> new_black_elo)
        -> result<void>;
    auto query_elo_distribution(zobrist_hash hash, int bucket_width) const -> result<std::vector<elo_distribution_row>>;
    auto query_elo_distribution(zobrist_hash hash, std::vector<game_id> const& game_ids, int bucket_width) const
        -> result<std::vector<elo_distribution_row>>;
    auto count_by_zobrist(zobrist_hash hash) const -> result<std::int64_t>;
    auto count_distinct_games_by_zobrist(zobrist_hash hash) const -> result<std::int64_t>;
    auto count_distinct_games_by_zobrist(zobrist_hash hash, std::vector<game_id> const& game_ids) const -> result<std::int64_t>;
    // Ordered by game_id for deterministic downstream pagination.
    auto distinct_game_ids_by_zobrist(zobrist_hash hash) const -> result<std::vector<game_id>>;

  private:
    duckdb_connection con_;
    mutable std::mutex filtered_game_ids_mutex_;
};

}  // namespace motif::db
