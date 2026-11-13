#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#include "motif/db/opening_tree_composite.hpp"

#include <tl/expected.hpp>

#include "motif/db/error.hpp"
#include "motif/db/manifest.hpp"
#include "motif/db/opening_tree_index.hpp"
#include "motif/db/opening_tree_patch.hpp"
#include "motif/db/opening_tree_stats.hpp"
#include "motif/db/patch_chain.hpp"
#include "motif/db/types.hpp"

namespace motif::db
{

namespace
{

constexpr auto u32_max = static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max());

auto corrupt(char const* what) -> tl::unexpected<error>
{
    return tl::unexpected {error {error_code::io_failure, what}};
}

auto start_hash() -> std::uint64_t
{
    static auto const hash = opening_tree_start_hash();
    return hash;
}

struct merge_context
{
    std::uint64_t start_hash {};
    std::uint16_t max_root_ply {};
    tree_child_frequency_mode mode {tree_child_frequency_mode::checkpoint};
};

// One run's contribution to one node.
struct patch_view
{
    std::int64_t game_count_delta {};
    std::span<tree_patch_edge const> edges;
};

// The validity-mask bits of one patch edge, decoded once.
struct patch_flags
{
    bool invalidate_min {};
    bool invalidate_samples {};
    bool candidates {};
};

auto flags_of(tree_patch_edge const& patch) noexcept -> patch_flags
{
    return {.invalidate_min = (patch.validity_mask & tree_patch_mask_invalidate_min_ply) != 0U,
            .invalidate_samples = (patch.validity_mask & tree_patch_mask_invalidate_samples) != 0U,
            .candidates = (patch.validity_mask & tree_patch_mask_candidates) != 0U};
}

// A fresh edge has no live base, so its extrema come only from candidates.
void seed_extrema(raw_edge& edge, tree_patch_edge const& patch, patch_flags const flags)
{
    edge.min_root_ply_valid = false;
    edge.sample_bounds_valid = false;
    edge.min_root_ply = 0;
    edge.sample_min = 0;
    edge.sample_max = 0;
    if (flags.candidates && !flags.invalidate_min) {
        edge.min_root_ply = patch.candidate_min_root_ply;
        edge.min_root_ply_valid = true;
    }
    if (flags.candidates && !flags.invalidate_samples) {
        edge.sample_min = patch.candidate_sample_min;
        edge.sample_max = patch.candidate_sample_max;
        edge.sample_bounds_valid = true;
    }
}

// Invalid flags are sticky; candidates combine only while the base field is
// still exact.
void combine_extrema(raw_edge& edge, tree_patch_edge const& patch, patch_flags const flags)
{
    if (flags.invalidate_min) {
        edge.min_root_ply_valid = false;
    }
    if (flags.invalidate_samples) {
        edge.sample_bounds_valid = false;
    }
    if (flags.candidates && edge.min_root_ply_valid) {
        edge.min_root_ply = std::min(edge.min_root_ply, patch.candidate_min_root_ply);
    }
    if (flags.candidates && edge.sample_bounds_valid) {
        edge.sample_min = std::min(edge.sample_min, patch.candidate_sample_min);
        edge.sample_max = std::max(edge.sample_max, patch.candidate_sample_max);
    }
    if (!edge.min_root_ply_valid) {
        edge.min_root_ply = 0;
    }
    if (!edge.sample_bounds_valid) {
        edge.sample_min = 0;
        edge.sample_max = 0;
    }
}

// Applies one patch edge to the edge accumulated so far. `fresh` is true while
// the edge has no live base (absent, or retracted to zero), in which case
// extrema come only from candidates.
auto apply_patch_edge(raw_edge& edge, bool& fresh, tree_patch_edge const& patch) -> result<void>
{
    auto const sum = add_stats(edge.stats, patch.delta);
    if (!sum) {
        return corrupt("merged edge statistic overflow");
    }
    edge.stats = *sum;
    if (fresh) {
        seed_extrema(edge, patch, flags_of(patch));
    } else {
        combine_extrema(edge, patch, flags_of(patch));
    }
    fresh = false;
    if (edge.stats.frequency == 0) {
        // A fully retracted edge must leave no numerical residue (double
        // retraction or a metadata patch on an absent edge would).
        if (!is_zero(edge.stats)) {
            return corrupt("zero-frequency edge retains statistics");
        }
        fresh = true;
    }
    return {};
}

// The node's game count after every run's delta; checked against the u32 range.
auto merged_game_count(std::optional<raw_node> const& base, std::span<patch_view const> const patches) -> result<std::int64_t>
{
    auto count = static_cast<std::int64_t>(base ? base->game_count : 0U);
    for (auto const& patch : patches) {
        auto const next = checked_add_i64(count, patch.game_count_delta);
        if (!next) {
            return corrupt("merged node count overflow");
        }
        count = *next;
    }
    if (count < 0 || count > u32_max) {
        return corrupt("merged node count out of range");
    }
    return count;
}

// The smallest edge key any cursor (base or run) still points at.
auto smallest_pending_key(std::span<raw_edge const> const base_edges,
                          std::size_t const base_pos,
                          std::span<patch_view const> const patches,
                          std::span<std::size_t const> const positions) -> std::optional<tree_edge_key>
{
    auto key = std::optional<tree_edge_key> {};
    auto const consider = [&key](tree_edge_key const& candidate) -> void
    {
        if (!key || candidate < *key) {
            key = candidate;
        }
    };
    if (base_pos < base_edges.size()) {
        consider(base_edges[base_pos].key);
    }
    for (std::size_t index = 0; index < patches.size(); ++index) {
        if (positions[index] < patches[index].edges.size()) {
            consider(patches[index].edges[positions[index]].key());
        }
    }
    return key;
}

// Applies, in chronological order (run 0 first), every run's edge at `key`.
auto apply_patches_at(tree_edge_key const& key,
                      std::span<patch_view const> const patches,
                      std::span<std::size_t> const positions,
                      raw_edge& edge,
                      bool& fresh) -> result<void>
{
    for (std::size_t index = 0; index < patches.size(); ++index) {
        auto const run_edges = patches[index].edges;
        if (positions[index] < run_edges.size() && run_edges[positions[index]].key() == key) {
            if (auto applied = apply_patch_edge(edge, fresh, run_edges[positions[index]]); !applied) {
                return applied;
            }
            ++positions[index];
        }
    }
    return {};
}

// Merges the base edges with every run's edges in key order; retracted edges
// disappear.
auto merge_edges(std::span<raw_edge const> const base_edges, std::span<patch_view const> const patches) -> result<std::vector<raw_edge>>
{
    auto base_pos = std::size_t {0};
    auto positions = std::vector<std::size_t>(patches.size(), 0U);
    auto merged = std::vector<raw_edge> {};
    merged.reserve(base_edges.size());

    while (true) {
        auto const key = smallest_pending_key(base_edges, base_pos, patches, positions);
        if (!key) {
            return merged;
        }
        auto edge = raw_edge {.key = *key,
                              .stats = {},
                              .min_root_ply = 0,
                              .min_root_ply_valid = false,
                              .sample_min = 0,
                              .sample_max = 0,
                              .sample_bounds_valid = false,
                              .child_frequency = std::nullopt};
        auto fresh = true;
        if (base_pos < base_edges.size() && base_edges[base_pos].key == *key) {
            edge = base_edges[base_pos];
            fresh = false;
            ++base_pos;
        }
        if (auto applied = apply_patches_at(*key, patches, positions, edge, fresh); !applied) {
            return tl::unexpected {applied.error()};
        }
        if (edge.stats.frequency != 0) {
            merged.push_back(edge);  // a retracted edge disappears
        }
    }
}

// Validates every edge of a merged node against the merged invariants.
auto validate_merged_node(raw_node const& node, merge_context const& context) -> result<void>
{
    auto const edge_context =
        merged_edge_context {.node_game_count = node.game_count, .max_root_ply = context.max_root_ply, .node_complete = node.complete};
    for (auto const& edge : node.edges) {
        // In postings mode the cached child count is stale by construction (membership
        // changed since the checkpoint) and is resolved from postings at presentation,
        // so it must not be held to the direct-frequency bound here.
        auto checked = edge;
        if (context.mode == tree_child_frequency_mode::postings) {
            checked.child_frequency.reset();
        }
        if (auto valid = validate_merged_edge(checked, edge_context); !valid) {
            return valid;
        }
        if (context.mode == tree_child_frequency_mode::checkpoint && !edge.child_frequency) {
            return corrupt("edge has no cached child frequency but the snapshot trusts the checkpoint cache");
        }
    }
    return {};
}

// Merges one node: the base (if any) with every run's span, edge by edge in
// key order. Returns nullopt when the node ceases to exist.
auto merge_node(std::uint64_t const root,
                std::optional<raw_node> base,
                std::span<patch_view const> const patches,
                merge_context const& context) -> result<std::optional<raw_node>>
{
    auto const count = merged_game_count(base, patches);
    if (!count) {
        return tl::unexpected {count.error()};
    }
    auto const base_edges = base ? std::span<raw_edge const> {base->edges} : std::span<raw_edge const> {};
    auto merged = merge_edges(base_edges, patches);
    if (!merged) {
        return tl::unexpected {merged.error()};
    }

    if (*count == 0) {
        if (!merged->empty()) {
            return corrupt("a node with no games retains live edges");
        }
        return std::optional<raw_node> {};
    }
    auto node = raw_node {.root_hash = root,
                          .game_count = static_cast<std::uint32_t>(*count),
                          .complete = root == context.start_hash,
                          .edges = std::move(*merged)};
    if (auto valid = validate_merged_node(node, context); !valid) {
        return tl::unexpected {valid.error()};
    }
    return std::optional<raw_node> {std::move(node)};
}

// Checks that the runs belong together with this checkpoint and returns the
// merged source game count.
auto validate_run_set(opening_tree_index const& checkpoint,
                      std::span<tree_patch_run_ptr const> const runs,
                      tree_child_frequency_mode const mode,
                      std::uint64_t const max_decoded_bytes) -> result<std::uint64_t>
{
    if (checkpoint.source_game_count() > static_cast<std::uint64_t>(u32_max)) {
        return corrupt("checkpoint source game count out of range");
    }
    auto source = static_cast<std::int64_t>(checkpoint.source_game_count());
    auto decoded = std::uint64_t {0};
    for (std::size_t index = 0; index < runs.size(); ++index) {
        auto const& run = runs[index];
        if (!run) {
            return corrupt("null patch run");
        }
        if (run->identity.max_root_ply != checkpoint.max_root_ply() || run->identity.canonical_start_hash != start_hash()
            || run->identity.index_semantics != index_semantics_version || (index > 0 && run->identity != runs[0]->identity))
        {
            return corrupt("patch run does not belong to this checkpoint");
        }
        if (index > 0 && run->from_exclusive_seq != runs[index - 1]->to_inclusive_seq) {
            return corrupt("patch runs are not contiguous");
        }
        if ((run->operations & tree_patch_operation_membership) != 0U && mode != tree_child_frequency_mode::postings) {
            return corrupt("membership patches require the postings child-frequency mode");
        }
        decoded += run->decoded_bytes();
        if (decoded > max_decoded_bytes) {
            return corrupt("patch runs exceed the resident memory cap");
        }
        auto const next = checked_add_i64(source, run->source_game_delta);
        if (!next || *next < 0 || *next > u32_max) {
            return corrupt("merged source game count out of range");
        }
        source = *next;
    }
    return static_cast<std::uint64_t>(source);
}

// Merges the checkpoint's node stream with every run's sorted node list, one
// merged node at a time.
class merged_node_stream
{
  public:
    static auto create(opening_tree_index const& checkpoint, std::span<tree_patch_run_ptr const> runs, merge_context context)
        -> result<merged_node_stream>
    {
        auto base = checkpoint.stream_nodes();
        if (!base) {
            return tl::unexpected {base.error()};
        }
        auto stream = merged_node_stream {std::move(*base), std::vector<tree_patch_run_ptr> {runs.begin(), runs.end()}, context};
        auto first = stream.base_.next();
        if (!first) {
            return tl::unexpected {first.error()};
        }
        stream.base_head_ = std::move(*first);
        return stream;
    }

    auto next() -> result<std::optional<raw_node>>
    {
        while (true) {
            auto const root = smallest_pending_root();
            if (!root) {
                return std::optional<raw_node> {};
            }
            auto base = take_base_node(*root);
            if (!base) {
                return tl::unexpected {base.error()};
            }
            auto views = collect_patch_views(*root);
            if (views.empty()) {
                return std::move(*base);  // untouched checkpoint node
            }
            auto merged = merge_node(*root, std::move(*base), views, context_);
            if (!merged) {
                return tl::unexpected {merged.error()};
            }
            if (*merged) {
                return merged;
            }
        }
    }

  private:
    merged_node_stream(opening_tree_node_stream base, std::vector<tree_patch_run_ptr> runs, merge_context context)
        : base_ {std::move(base)}
        , runs_ {std::move(runs)}
        , positions_(runs_.size(), 0U)
        , context_ {context}
    {
    }

    // The smallest root hash any cursor (checkpoint or run) still points at.
    [[nodiscard]] auto smallest_pending_root() const -> std::optional<std::uint64_t>
    {
        auto root = std::optional<std::uint64_t> {};
        if (base_head_) {
            root = base_head_->root_hash;
        }
        for (std::size_t index = 0; index < runs_.size(); ++index) {
            if (positions_[index] < runs_[index]->nodes.size()) {
                auto const candidate = runs_[index]->nodes[positions_[index]].root_hash;
                root = root ? std::min(*root, candidate) : candidate;
            }
        }
        return root;
    }

    // Removes and returns the checkpoint node at `root` (nullopt when the
    // checkpoint has none there) and advances the checkpoint stream.
    auto take_base_node(std::uint64_t const root) -> result<std::optional<raw_node>>
    {
        if (!base_head_ || base_head_->root_hash != root) {
            return std::optional<raw_node> {};
        }
        auto taken = std::move(base_head_);
        auto following = base_.next();
        if (!following) {
            return tl::unexpected {following.error()};
        }
        base_head_ = std::move(*following);
        return taken;
    }

    // Every run's contribution to `root`, advancing those runs past it.
    auto collect_patch_views(std::uint64_t const root) -> std::vector<patch_view>
    {
        auto views = std::vector<patch_view> {};
        for (std::size_t index = 0; index < runs_.size(); ++index) {
            if (positions_[index] < runs_[index]->nodes.size() && runs_[index]->nodes[positions_[index]].root_hash == root) {
                auto const& node = runs_[index]->nodes[positions_[index]];
                views.push_back(patch_view {.game_count_delta = node.game_count_delta, .edges = runs_[index]->node_edges(node)});
                ++positions_[index];
            }
        }
        return views;
    }

    opening_tree_node_stream base_;
    std::optional<raw_node> base_head_;
    std::vector<tree_patch_run_ptr> runs_;
    std::vector<std::size_t> positions_;
    merge_context context_;
};

}  // namespace

auto composite_opening_tree::open(std::filesystem::path const& checkpoint_path,
                                  std::vector<tree_patch_run_ptr> runs,
                                  tree_child_frequency_mode const mode,
                                  std::uint64_t const max_decoded_bytes) -> result<composite_opening_tree>
{
    auto opened = opening_tree_index::open(checkpoint_path);
    if (!opened) {
        return tl::unexpected {opened.error()};
    }
    auto tree = composite_opening_tree {};
    tree.checkpoint_ = std::make_shared<opening_tree_index const>(std::move(*opened));
    tree.runs_ = std::move(runs);
    tree.mode_ = mode;
    auto const source = validate_run_set(*tree.checkpoint_, tree.runs_, mode, max_decoded_bytes);
    if (!source) {
        return tl::unexpected {source.error()};
    }
    tree.source_game_count_ = *source;

    if (auto valid = tree.validate_touched_nodes(tree.runs_); !valid) {
        return tl::unexpected {valid.error()};
    }
    return tree;
}

auto composite_opening_tree::validate_touched_nodes(std::span<tree_patch_run_ptr const> const runs_to_check) const -> result<void>
{
    // Validate every node any checked run touches against the merged invariants.
    auto touched = std::vector<std::uint64_t> {};
    for (auto const& run : runs_to_check) {
        for (auto const& node : run->nodes) {
            touched.push_back(node.root_hash);
        }
    }
    std::ranges::sort(touched);
    auto const unique_end = std::ranges::unique(touched);
    touched.erase(unique_end.begin(), unique_end.end());
    for (auto const root : touched) {
        auto merged = query_raw_node(zobrist_hash {root});
        if (!merged) {
            return tl::unexpected {merged.error()};
        }
        auto const& merged_node = *merged;
        if (merged_node && merged_node->game_count > source_game_count_) {
            return corrupt("merged node count exceeds the source game count");
        }
    }
    // The canonical start node exists for any non-empty corpus and counts every
    // game; an empty corpus has no positive-count root.
    auto const start = query_raw_node(zobrist_hash {start_hash()});
    if (!start) {
        return tl::unexpected {start.error()};
    }
    auto const& start_node = *start;
    if (source_game_count_ == 0U ? start_node.has_value() : (!start_node.has_value() || start_node->game_count != source_game_count_)) {
        return corrupt("the canonical start node disagrees with the source game count");
    }
    return {};
}

auto composite_opening_tree::with_appended_run(tree_patch_run_ptr run, std::uint64_t const max_decoded_bytes) const
    -> result<composite_opening_tree>
{
    if (!run) {
        return corrupt("null patch run");
    }
    auto tree = composite_opening_tree {};
    tree.checkpoint_ = checkpoint_;
    tree.runs_ = runs_;
    tree.runs_.push_back(run);
    tree.mode_ = mode_;
    auto const source = validate_run_set(*tree.checkpoint_, tree.runs_, mode_, max_decoded_bytes);
    if (!source) {
        return tl::unexpected {source.error()};
    }
    tree.source_game_count_ = *source;
    auto const only_new = std::array<tree_patch_run_ptr, 1> {std::move(run)};
    if (auto valid = tree.validate_touched_nodes(only_new); !valid) {
        return tl::unexpected {valid.error()};
    }
    return tree;
}

auto composite_opening_tree::usage() const noexcept -> patch_chain_usage
{
    auto usage = patch_chain_usage {.runs = runs_.size(), .encoded_bytes = 0, .decoded_bytes = 0};
    for (auto const& run : runs_) {
        usage.encoded_bytes += run->encoded_bytes;
        usage.decoded_bytes += run->decoded_bytes();
    }
    return usage;
}

auto composite_opening_tree::query_raw_node(zobrist_hash const hash) const -> result<std::optional<raw_node>>
{
    auto base = checkpoint_->query_raw_node(hash);
    if (!base) {
        return tl::unexpected {base.error()};
    }
    auto views = std::vector<patch_view> {};
    for (auto const& run : runs_) {
        if (auto const* node = run->find_node(hash.value); node != nullptr) {
            views.push_back(patch_view {.game_count_delta = node->game_count_delta, .edges = run->node_edges(*node)});
        }
    }
    if (views.empty()) {
        return base;
    }
    return merge_node(hash.value,
                      std::move(*base),
                      views,
                      merge_context {.start_hash = start_hash(), .max_root_ply = checkpoint_->max_root_ply(), .mode = mode_});
}

auto composite_opening_tree::game_count(zobrist_hash const hash) const -> result<std::uint32_t>
{
    auto base = checkpoint_->game_count(hash);
    if (!base) {
        return tl::unexpected {base.error()};
    }
    auto count = static_cast<std::int64_t>(*base);
    for (auto const& run : runs_) {
        if (auto const* node = run->find_node(hash.value); node != nullptr) {
            auto const next = checked_add_i64(count, node->game_count_delta);
            if (!next) {
                return corrupt("merged node count overflow");
            }
            count = *next;
        }
    }
    if (count < 0 || count > u32_max) {
        return corrupt("merged node count out of range");
    }
    return static_cast<std::uint32_t>(count);
}

auto composite_opening_tree::is_complete(zobrist_hash const hash) const -> bool
{
    if (hash.value != start_hash()) {
        return false;
    }
    auto const count = game_count(hash);
    return count && *count != 0U;
}

auto composite_opening_tree::query_opening_stats(zobrist_hash const hash, tree_child_frequency_resolver const& resolver) const
    -> result<std::vector<opening_stat_agg_row>>
{
    auto node = query_raw_node(hash);
    if (!node) {
        return tl::unexpected {node.error()};
    }
    auto rows = std::vector<opening_stat_agg_row> {};
    auto const& live = *node;
    if (!live) {
        return rows;
    }
    rows.reserve(live->edges.size());
    for (auto const& edge : live->edges) {
        auto child_frequency = std::optional<std::uint32_t> {};
        if (mode_ == tree_child_frequency_mode::postings) {
            if (!resolver) {
                return corrupt("postings child-frequency mode needs a resolver");
            }
            auto resolved = resolver(zobrist_hash {edge.key.child_hash});
            if (!resolved) {
                return tl::unexpected {resolved.error()};
            }
            child_frequency = *resolved;
        } else {
            child_frequency = edge.child_frequency;
        }
        if (!child_frequency) {
            return corrupt("edge has no child frequency");
        }
        auto row = to_opening_stat_row(edge, *child_frequency);
        if (!row) {
            return tl::unexpected {row.error()};
        }
        rows.push_back(*row);
    }
    return rows;
}

auto composite_opening_tree::for_each_node(std::function<result<void>(raw_node const&)> const& visitor) const -> result<void>
{
    auto stream = merged_node_stream::create(
        *checkpoint_, runs_, merge_context {.start_hash = start_hash(), .max_root_ply = checkpoint_->max_root_ply(), .mode = mode_});
    if (!stream) {
        return tl::unexpected {stream.error()};
    }
    while (true) {
        auto node = stream->next();
        if (!node) {
            return tl::unexpected {node.error()};
        }
        auto const& current = *node;
        if (!current) {
            return {};
        }
        if (auto visited = visitor(*current); !visited) {
            return visited;
        }
    }
}

auto fold_tree_chain(opening_tree_index const& checkpoint,
                     std::span<tree_patch_run_ptr const> const runs,
                     tree_child_frequency_mode const mode,
                     std::filesystem::path const& output_path) -> result<tree_fold_result>
{
    auto const source = validate_run_set(checkpoint, runs, mode, std::numeric_limits<std::uint64_t>::max());
    if (!source) {
        return tl::unexpected {source.error()};
    }
    auto stream = merged_node_stream::create(
        checkpoint, runs, merge_context {.start_hash = start_hash(), .max_root_ply = checkpoint.max_root_ply(), .mode = mode});
    if (!stream) {
        return tl::unexpected {stream.error()};
    }
    auto writer = opening_tree_checkpoint_writer::create(output_path, checkpoint.max_root_ply(), *source);
    if (!writer) {
        return tl::unexpected {writer.error()};
    }
    auto result_info = tree_fold_result {.node_count = 0, .edge_count = 0, .source_game_count = *source, .child_frequency_mode = mode};
    while (true) {
        auto node = stream->next();
        if (!node) {
            return tl::unexpected {node.error()};
        }
        auto& current = *node;
        if (!current) {
            break;
        }
        if (mode == tree_child_frequency_mode::postings) {
            // Caches are not trusted once membership has changed: drop them rather than
            // certify stale values (every count is resolved from postings).
            for (auto& edge : current->edges) {
                edge.child_frequency.reset();
            }
        }
        if (auto written = writer->write_node(*current); !written) {
            return tl::unexpected {written.error()};
        }
        ++result_info.node_count;
        result_info.edge_count += current->edges.size();
    }
    if (auto finished = writer->finish(); !finished) {
        return tl::unexpected {finished.error()};
    }
    return result_info;
}

}  // namespace motif::db
