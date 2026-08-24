#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

#include "motif/db/error.hpp"
#include "motif/db/types.hpp"

namespace motif::db
{

class game_store;
struct replay_game_record;

using opening_tree_child_count_visitor = std::function<result<void>(zobrist_hash, std::uint32_t)>;
using opening_tree_child_count_stream = std::function<result<void>(opening_tree_child_count_visitor const&)>;

// Depth cutoff for the shallow opening index.
inline constexpr std::uint16_t opening_tree_index_default_max_root_ply {20};
inline constexpr std::size_t opening_tree_index_default_spill_threshold {1U << 20U};
inline constexpr std::size_t opening_tree_index_default_child_frequency_memory_limit_bytes {1U << 30U};

struct opening_tree_index_build_options
{
    std::uint16_t max_root_ply {opening_tree_index_default_max_root_ply};
    std::size_t spill_threshold {opening_tree_index_default_spill_threshold};
    std::size_t child_frequency_memory_limit_bytes {opening_tree_index_default_child_frequency_memory_limit_bytes};
};

struct opening_tree_index_build_metrics
{
    std::uint64_t game_count {};
    std::uint64_t root_record_count {};
    std::uint64_t edge_record_count {};
    std::uint64_t child_visit_count {};
    std::uint64_t root_spill_run_count {};
    std::uint64_t edge_spill_run_count {};
    std::uint64_t child_spill_run_count {};
    std::uint64_t child_frequency_lookup_count {};
    std::uint64_t child_frequency_loaded_count {};
    bool child_counts_external {};
    std::chrono::milliseconds replay_elapsed {};
    std::chrono::milliseconds root_merge_elapsed {};
    std::chrono::milliseconds edge_merge_elapsed {};
    std::chrono::milliseconds child_merge_elapsed {};
    std::chrono::milliseconds child_frequency_load_elapsed {};
    std::chrono::milliseconds index_write_elapsed {};
};

class opening_tree_index_builder
{
  public:
    explicit opening_tree_index_builder(std::filesystem::path path,
                                        opening_tree_index_build_options options = {},
                                        opening_tree_child_count_stream child_counts = {});
    ~opening_tree_index_builder();
    opening_tree_index_builder(opening_tree_index_builder const&) = delete;
    auto operator=(opening_tree_index_builder const&) -> opening_tree_index_builder& = delete;
    opening_tree_index_builder(opening_tree_index_builder&&) noexcept;
    auto operator=(opening_tree_index_builder&&) noexcept -> opening_tree_index_builder&;

    [[nodiscard]] auto accumulate(replay_game_record const& game) -> result<void>;
    [[nodiscard]] auto finalize() -> result<void>;
    [[nodiscard]] auto metrics() noexcept -> opening_tree_index_build_metrics&;

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

    [[nodiscard]] static auto build(game_store& store,
                                    std::filesystem::path const& path,
                                    build_options const& opts = {},
                                    opening_tree_index_build_metrics* metrics = nullptr) -> result<void>;
    [[nodiscard]] static auto build(game_store& store,
                                    std::filesystem::path const& path,
                                    build_options const& opts,
                                    opening_tree_index_build_metrics* metrics,
                                    opening_tree_child_count_stream child_counts) -> result<void>;
    [[nodiscard]] static auto open(std::filesystem::path const& path) -> result<opening_tree_index>;

    // Distinct games that reached hash within the index depth. Returns zero
    // when the hash is absent, which is distinct from an indexed terminal
    // position whose continuation query is empty.
    [[nodiscard]] auto game_count(zobrist_hash hash) const -> result<std::uint32_t>;
    [[nodiscard]] auto query_opening_stats(zobrist_hash hash) const -> result<std::vector<opening_stat_agg_row>>;
    [[nodiscard]] auto source_game_count() const noexcept -> std::uint64_t;
    [[nodiscard]] auto max_root_ply() const noexcept -> std::uint16_t;

    // True only for a node whose continuations were aggregated from every
    // occurrence of hash in every game, not just occurrences within
    // max_root_ply. Callers may treat such a node's query_opening_stats() as
    // authoritative without any max_ply cross-check against another index.
    // False for both an absent hash and a hash indexed only to max_root_ply.
    [[nodiscard]] auto is_complete(zobrist_hash hash) const -> bool;

  private:
    std::filesystem::path path_;
    std::vector<std::uint64_t> node_hashes_;  // sorted ascending
    std::vector<std::uint32_t> node_game_counts_;  // parallel to node_hashes_
    std::vector<std::uint64_t> node_data_offsets_;  // byte offset of each node's first continuation
    std::vector<std::uint32_t> node_continuation_counts_;
    std::vector<std::uint8_t> node_complete_;  // parallel to node_hashes_; 1 when fully aggregated past max_root_ply
    std::uint64_t source_game_count_ {};
    std::uint16_t max_root_ply_ {};
};

}  // namespace motif::db
