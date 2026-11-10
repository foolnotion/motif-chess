#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "motif/db/error.hpp"
#include "motif/db/manifest.hpp"
#include "motif/db/opening_tree_index.hpp"
#include "motif/db/opening_tree_patch.hpp"
#include "motif/db/opening_tree_stats.hpp"
#include "motif/db/patch_chain.hpp"
#include "motif/db/types.hpp"

namespace motif::db
{

using tree_patch_run_ptr = std::shared_ptr<tree_patch_run const>;

// Resolves a child hash to its current global distinct-game count; used only
// when the snapshot's child-frequency mode is "postings".
using tree_child_frequency_resolver = std::function<result<std::uint32_t>(zobrist_hash)>;

// The merged tree reader: an immutable checkpoint plus an ordered chain of
// immutable patch runs, answering lookups as the checkpoint merged with every
// run, node by node.
//
// Lookup cost is one checkpoint directory search and one search per run; only
// the matching sorted edge spans take part in a k-way merge, and no patch file
// is read at query time (runs are decoded and validated once at open).
//
// Any structural or merged-row violation found at open fails the whole
// composite: a corrupt run never degrades to "just that node" or "just that
// run". The merged invariants (non-negative counts, W+D+B == frequency, rating
// population bounds, frequency <= node count, canonical start node present with
// count == source games, exact extrema within the horizon, ...) are validated
// for every node any run touches; query-time merges re-check them defensively.
class composite_opening_tree
{
  public:
    // `runs` must be in chronological order and contiguous from one another.
    // max_decoded_bytes bounds the runs' resident representation.
    [[nodiscard]] static auto open(std::filesystem::path const& checkpoint_path,
                                   std::vector<tree_patch_run_ptr> runs,
                                   tree_child_frequency_mode mode,
                                   std::uint64_t max_decoded_bytes = patch_chain_limits {}.hard_decoded_bytes)
        -> result<composite_opening_tree>;

    // The composite that results from appending `run` to this chain: it shares
    // this one's immutable checkpoint and every existing run (no checkpoint
    // reopen, no directory copy) and validates the merged invariants of only the
    // nodes the new run touches, plus the canonical start node. This is the
    // per-edit candidate state; it fails (leaving this composite untouched) on
    // a discontiguous or foreign run, a decoded-memory overflow, or any merged
    // row violation.
    [[nodiscard]] auto with_appended_run(tree_patch_run_ptr run,
                                         std::uint64_t max_decoded_bytes = patch_chain_limits {}.hard_decoded_bytes) const
        -> result<composite_opening_tree>;

    [[nodiscard]] auto game_count(zobrist_hash hash) const -> result<std::uint32_t>;
    [[nodiscard]] auto is_complete(zobrist_hash hash) const -> bool;
    // Merged raw sufficient statistics; nullopt when the node does not exist.
    [[nodiscard]] auto query_raw_node(zobrist_hash hash) const -> result<std::optional<raw_node>>;
    // The one raw -> presentation conversion over the merged node. In
    // "postings" mode the resolver supplies every child count; in "checkpoint"
    // mode the cached counts are used and the resolver is ignored. An edge whose
    // extrema are not exact (a retraction invalidated them) cannot be presented
    // and fails the call; callers fall back to exact postings.
    [[nodiscard]] auto query_opening_stats(zobrist_hash hash, tree_child_frequency_resolver const& resolver = {}) const
        -> result<std::vector<opening_stat_agg_row>>;
    // Every merged node in ascending root order, one node at a time.
    [[nodiscard]] auto for_each_node(std::function<result<void>(raw_node const&)> const& visitor) const -> result<void>;

    // Merged source game count (checkpoint count plus every run's delta).
    [[nodiscard]] auto source_game_count() const noexcept -> std::uint64_t { return source_game_count_; }

    [[nodiscard]] auto max_root_ply() const noexcept -> std::uint16_t { return checkpoint_->max_root_ply(); }

    [[nodiscard]] auto child_frequency_mode() const noexcept -> tree_child_frequency_mode { return mode_; }

    [[nodiscard]] auto checkpoint() const noexcept -> opening_tree_index const& { return *checkpoint_; }

    [[nodiscard]] auto runs() const noexcept -> std::span<tree_patch_run_ptr const> { return runs_; }

    [[nodiscard]] auto usage() const noexcept -> patch_chain_usage;

  private:
    // Merged-invariant validation of every node touched by `runs_to_check` plus
    // the canonical start node.
    [[nodiscard]] auto validate_touched_nodes(std::span<tree_patch_run_ptr const> runs_to_check) const -> result<void>;
    composite_opening_tree() = default;

    std::shared_ptr<opening_tree_index const> checkpoint_;
    std::vector<tree_patch_run_ptr> runs_;
    tree_child_frequency_mode mode_ {tree_child_frequency_mode::checkpoint};
    std::uint64_t source_game_count_ {};
};

struct tree_fold_result
{
    std::uint64_t node_count {};
    std::uint64_t edge_count {};
    std::uint64_t source_game_count {};
    // Retained from the input: a fold never certifies cached child counts it did
    // not verify.
    tree_child_frequency_mode child_frequency_mode {tree_child_frequency_mode::checkpoint};
};

// Synchronous streaming compaction: folds `checkpoint` plus `runs` (include the
// pending run when folding on behalf of an edit) into a new v6 checkpoint at
// output_path by a sorted merge, holding one merged node in memory at a time.
// It never builds from the store and never replays a game. Extrema, sample
// bounds and cached child counts are carried through unchanged; validity flags
// stay sticky. Zero-frequency edges and zero-count nodes disappear.
//
// The caller validates the result (opening_tree_index::open) before publishing.
// output_path is replaced by the finished checkpoint (the manager hands in its
// own reserved, empty staging file); on failure nothing but that file remains.
[[nodiscard]] auto fold_tree_chain(opening_tree_index const& checkpoint,
                                   std::span<tree_patch_run_ptr const> runs,
                                   tree_child_frequency_mode mode,
                                   std::filesystem::path const& output_path) -> result<tree_fold_result>;

}  // namespace motif::db
