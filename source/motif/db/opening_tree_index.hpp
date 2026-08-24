#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "motif/db/error.hpp"
#include "motif/db/types.hpp"

namespace motif::db
{

class game_store;
struct replay_game_record;

// Depth cutoff for the shallow opening index.
inline constexpr std::uint16_t opening_tree_index_default_max_root_ply {20};

struct opening_tree_index_build_options
{
    std::uint16_t max_root_ply {opening_tree_index_default_max_root_ply};
};

class opening_tree_index_builder
{
  public:
    explicit opening_tree_index_builder(std::filesystem::path path, opening_tree_index_build_options options = {});
    ~opening_tree_index_builder();
    opening_tree_index_builder(opening_tree_index_builder const&) = delete;
    auto operator=(opening_tree_index_builder const&) -> opening_tree_index_builder& = delete;
    opening_tree_index_builder(opening_tree_index_builder&&) noexcept;
    auto operator=(opening_tree_index_builder&&) noexcept -> opening_tree_index_builder&;

    [[nodiscard]] auto accumulate(replay_game_record const& game) -> result<void>;
    [[nodiscard]] auto finalize() -> result<void>;

  private:
    struct state;
    std::unique_ptr<state> state_;
};

// DuckDB-free replacement for the `opening_continuation` rollup: an
// immutable, sorted-by-hash index built in one streaming pass over a
// game_store's games, answering unfiltered query_opening_stats(hash)
// lookups. Not yet wired into any live import/query path.
class opening_tree_index
{
  public:
    using build_options = opening_tree_index_build_options;

    [[nodiscard]] static auto build(game_store& store, std::filesystem::path const& path, build_options const& opts = {}) -> result<void>;
    [[nodiscard]] static auto open(std::filesystem::path const& path) -> result<opening_tree_index>;

    // Distinct games that reached hash within the index depth. Returns zero
    // when the hash is absent, which is distinct from an indexed terminal
    // position whose continuation query is empty.
    [[nodiscard]] auto game_count(zobrist_hash hash) const -> result<std::uint32_t>;
    [[nodiscard]] auto query_opening_stats(zobrist_hash hash) const -> result<std::vector<opening_stat_agg_row>>;

  private:
    std::vector<std::uint64_t> node_hashes_;  // sorted ascending
    std::vector<std::uint32_t> node_game_counts_;  // parallel to node_hashes_
    std::vector<std::uint32_t> node_offsets_;  // node_hashes_.size() + 1 entries; index into continuations_
    std::vector<opening_stat_agg_row> continuations_;  // flattened, grouped by node in node_hashes_ order
};

}  // namespace motif::db
