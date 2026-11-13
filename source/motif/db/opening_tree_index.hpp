#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "motif/db/error.hpp"
#include "motif/db/opening_tree_stats.hpp"
#include "motif/db/types.hpp"

namespace motif::db
{

class game_store;
struct replay_game_record;

using opening_tree_child_count_visitor = std::function<result<void>(zobrist_hash, std::uint32_t)>;
using opening_tree_child_count_stream = std::function<result<void>(opening_tree_child_count_visitor const&)>;

// On-disk format version of an opening-tree artifact this build WRITES;
// recorded in the published snapshot descriptor so an artifact this build
// cannot read is never trusted. Version 6 stores signed (zigzag) Elo sums,
// exact doubled weighted sums and explicit validity flags. Version 5
// checkpoints (unsigned sums, double weighted sums) are still READ and
// converted to the same raw rows; they are never rewritten eagerly.
inline constexpr std::uint32_t opening_tree_index_format_version = 6;
inline constexpr std::uint32_t opening_tree_index_legacy_format_version = 5;

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

// The hash of the canonical starting position: the only node aggregated past
// the horizon ("complete").
[[nodiscard]] auto opening_tree_start_hash() -> std::uint64_t;

// One distinct (root, move, child) edge a game uses, with the minimum root ply
// at which the game reaches it.
struct game_tree_edge
{
    std::uint64_t root_hash {};
    std::uint16_t encoded_move {};
    std::uint64_t child_hash {};
    std::uint16_t min_root_ply {};
};

// THE per-game traversal and dedup rule, shared by the full builder and every
// patch producer so there is exactly one implementation of it:
//
//   roots : the standard start, plus each hash reached at absolute ply <=
//           max_root_ply, deduplicated per game. Zero-move games and terminal
//           roots still contribute.
//   edges : for each move at root ply p, the edge when p <= max_root_ply OR the
//           root hash equals full_coverage_root_hash (so the complete start
//           node also captures late returns beyond the horizon), deduplicated
//           by the full (root, move, child) key with the minimum ply kept.
//   visited : (optional) every distinct hash the game reaches at any ply, which
//           is what child-global transposition counts are built from.
//
// The returned spans are valid until the next enumerate().
class tree_contribution_enumerator
{
  public:
    tree_contribution_enumerator(std::uint16_t max_root_ply, std::uint64_t full_coverage_root_hash, bool collect_visited);
    ~tree_contribution_enumerator();
    tree_contribution_enumerator(tree_contribution_enumerator const&) = delete;
    auto operator=(tree_contribution_enumerator const&) -> tree_contribution_enumerator& = delete;
    tree_contribution_enumerator(tree_contribution_enumerator&&) noexcept;
    auto operator=(tree_contribution_enumerator&&) noexcept -> tree_contribution_enumerator&;

    [[nodiscard]] auto enumerate(std::span<std::uint16_t const> moves) -> result<void>;
    [[nodiscard]] auto roots() const noexcept -> std::span<std::uint64_t const>;
    [[nodiscard]] auto edges() const noexcept -> std::span<game_tree_edge const>;
    [[nodiscard]] auto visited() const noexcept -> std::span<std::uint64_t const>;

  private:
    struct state;
    std::unique_ptr<state> state_;
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

// Sequential reader of a checkpoint's nodes in strictly ascending root-hash
// order. Each next() decodes and validates one node; memory use is one node.
class opening_tree_node_stream
{
  public:
    opening_tree_node_stream(opening_tree_node_stream&&) noexcept;
    auto operator=(opening_tree_node_stream&&) noexcept -> opening_tree_node_stream&;
    ~opening_tree_node_stream();
    opening_tree_node_stream(opening_tree_node_stream const&) = delete;
    auto operator=(opening_tree_node_stream const&) -> opening_tree_node_stream& = delete;

    // nullopt after the last node.
    [[nodiscard]] auto next() -> result<std::optional<raw_node>>;

  private:
    friend class opening_tree_index;
    struct state;
    explicit opening_tree_node_stream(std::unique_ptr<state> stream_state) noexcept;
    std::unique_ptr<state> state_;
};

// Streams raw nodes into a new v6 checkpoint file (written to path + ".tmp" and
// renamed on finish). Nodes must arrive in strictly ascending root order with
// strictly ascending edge keys. The node count is patched into the header at
// finish(), so a fold needs no second pass.
class opening_tree_checkpoint_writer
{
  public:
    [[nodiscard]] static auto create(std::filesystem::path path, std::uint16_t max_root_ply, std::uint64_t source_game_count)
        -> result<opening_tree_checkpoint_writer>;
    ~opening_tree_checkpoint_writer();
    opening_tree_checkpoint_writer(opening_tree_checkpoint_writer&&) noexcept;
    auto operator=(opening_tree_checkpoint_writer&&) noexcept -> opening_tree_checkpoint_writer&;
    opening_tree_checkpoint_writer(opening_tree_checkpoint_writer const&) = delete;
    auto operator=(opening_tree_checkpoint_writer const&) -> opening_tree_checkpoint_writer& = delete;

    [[nodiscard]] auto write_node(raw_node const& node) -> result<void>;
    [[nodiscard]] auto finish() -> result<void>;
    [[nodiscard]] auto node_count() const noexcept -> std::uint64_t;

  private:
    struct state;
    explicit opening_tree_checkpoint_writer(std::unique_ptr<state> writer_state) noexcept;
    std::unique_ptr<state> state_;
};

// DuckDB-free replacement for the `opening_continuation` rollup: an
// immutable, sorted-by-hash index built in one streaming pass over a
// game_store's games, answering unfiltered query_opening_stats(hash)
// lookups. This is the checkpoint reader; the composite reader layers patch
// runs over it.
class opening_tree_index
{
  public:
    using build_options = opening_tree_index_build_options;

    [[nodiscard]] static auto build(game_store const& store,
                                    std::filesystem::path const& path,
                                    build_options const& opts = {},
                                    opening_tree_index_build_metrics* metrics = nullptr) -> result<void>;
    [[nodiscard]] static auto build(game_store const& store,
                                    std::filesystem::path const& path,
                                    build_options const& opts,
                                    opening_tree_index_build_metrics* metrics,
                                    opening_tree_child_count_stream child_counts) -> result<void>;
    // Reads a v5 or v6 checkpoint, validating every record.
    [[nodiscard]] static auto open(std::filesystem::path const& path) -> result<opening_tree_index>;

    // Distinct games that reached hash within the index depth. Returns zero
    // when the hash is absent, which is distinct from an indexed terminal
    // position whose continuation query is empty.
    [[nodiscard]] auto game_count(zobrist_hash hash) const -> result<std::uint32_t>;
    [[nodiscard]] auto query_opening_stats(zobrist_hash hash) const -> result<std::vector<opening_stat_agg_row>>;
    // The node's raw sufficient statistics, nullopt when the hash is absent.
    // A BASELINE result: it is not current bundle statistics once patch runs
    // exist (use the composite reader for that).
    [[nodiscard]] auto query_raw_node(zobrist_hash hash) const -> result<std::optional<raw_node>>;
    [[nodiscard]] auto stream_nodes() const -> result<opening_tree_node_stream>;
    [[nodiscard]] auto source_game_count() const noexcept -> std::uint64_t;
    [[nodiscard]] auto max_root_ply() const noexcept -> std::uint16_t;
    [[nodiscard]] auto checkpoint_format_version() const noexcept -> std::uint32_t;
    [[nodiscard]] auto node_count() const noexcept -> std::uint64_t;

    [[nodiscard]] auto path() const noexcept -> std::filesystem::path const& { return path_; }

    // True only for a node whose continuations were aggregated from every
    // occurrence of hash in every game, not just occurrences within
    // max_root_ply. Callers may treat such a node's query_opening_stats() as
    // authoritative without any max_ply cross-check against another index.
    // False for both an absent hash and a hash indexed only to max_root_ply.
    [[nodiscard]] auto is_complete(zobrist_hash hash) const -> bool;

  private:
    [[nodiscard]] auto node_index(std::uint64_t hash) const noexcept -> std::optional<std::size_t>;

    std::filesystem::path path_;
    std::vector<std::uint64_t> node_hashes_;  // sorted ascending
    std::vector<std::uint32_t> node_game_counts_;  // parallel to node_hashes_
    std::vector<std::uint64_t> node_data_offsets_;  // byte offset of each node's first continuation
    std::vector<std::uint32_t> node_continuation_counts_;
    std::vector<std::uint8_t> node_complete_;  // parallel to node_hashes_; 1 when fully aggregated past max_root_ply
    std::uint64_t source_game_count_ {};
    std::uint16_t max_root_ply_ {};
    std::uint32_t format_version_ {opening_tree_index_format_version};
};

}  // namespace motif::db
