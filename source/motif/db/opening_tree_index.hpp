#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "motif/db/error.hpp"
#include "motif/db/types.hpp"

namespace motif::db
{

class game_store;

// Matches position_store.cpp's opening_stats_max_root_ply, the depth cutoff
// the existing DuckDB rollup uses.
inline constexpr std::uint16_t opening_tree_index_default_max_root_ply {20};

struct opening_tree_index_build_options
{
    std::uint16_t max_root_ply {opening_tree_index_default_max_root_ply};
};

// First-slice replacement for the `opening_continuation` DuckDB rollup: an
// immutable, sorted-by-hash index built in one in-memory pass over a
// game_store's games, answering unfiltered query_opening_stats(hash) lookups
// without DuckDB. See docs/handoffs (redesign advisory) for why -- the SQL
// rollup duplicates the fact table it's built from, and DuckDB's columnar
// compression gets nothing from a table sorted by a random hash.
//
// Not yet bounded-memory (build() holds every node/edge in RAM) and not yet
// wired into any live import/query path -- see the plan this shipped under
// for the rest of the migration.
class opening_tree_index
{
  public:
    using build_options = opening_tree_index_build_options;

    [[nodiscard]] static auto build(game_store& store, std::filesystem::path const& path, build_options const& opts = {}) -> result<void>;
    [[nodiscard]] static auto open(std::filesystem::path const& path) -> result<opening_tree_index>;

    [[nodiscard]] auto query_opening_stats(zobrist_hash hash) const -> result<std::vector<opening_stat_agg_row>>;

  private:
    std::vector<std::uint64_t> node_hashes_;  // sorted ascending
    std::vector<std::uint32_t> node_offsets_;  // node_hashes_.size() + 1 entries; index into continuations_
    std::vector<opening_stat_agg_row> continuations_;  // flattened, grouped by node in node_hashes_ order
};

}  // namespace motif::db
