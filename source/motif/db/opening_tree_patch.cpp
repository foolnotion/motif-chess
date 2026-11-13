#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "motif/db/opening_tree_patch.hpp"

#include <tl/expected.hpp>

#include "motif/db/error.hpp"
#include "motif/db/manifest.hpp"
#include "motif/db/opening_tree_index.hpp"
#include "motif/db/opening_tree_stats.hpp"
#include "motif/db/types.hpp"

namespace motif::db
{

namespace
{

constexpr auto patch_magic = std::string_view {"MOTIFTP1"};
constexpr auto completeness_policy_code = std::uint16_t {1};
constexpr auto min_node_record_bytes = std::size_t {8 + 1 + 4};
constexpr auto min_edge_record_bytes = std::size_t {2 + 8 + 10 + 1};
// magic (8) + format version (4) + header length (4); the header length counts these too.
constexpr auto fixed_prefix_bytes = std::size_t {16};
// Capacity estimates for the encode buffer; only a hint, never a limit.
constexpr auto typical_node_record_bytes = std::size_t {16};
constexpr auto typical_edge_record_bytes = std::size_t {32};
constexpr auto int64_max_u = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

auto corrupt(char const* what) -> tl::unexpected<error>
{
    return tl::unexpected {error {error_code::io_failure, what}};
}

}  // namespace

auto tree_patch_run::find_node(std::uint64_t const root_hash) const noexcept -> tree_patch_node const*
{
    auto const found = std::ranges::lower_bound(nodes, root_hash, {}, &tree_patch_node::root_hash);
    if (found == nodes.end() || found->root_hash != root_hash) {
        return nullptr;
    }
    return &*found;
}

auto tree_patch_run::node_edges(tree_patch_node const& node) const noexcept -> std::span<tree_patch_edge const>
{
    return std::span<tree_patch_edge const> {edges}.subspan(node.edge_begin, node.edge_count);
}

auto tree_patch_run::decoded_bytes() const noexcept -> std::uint64_t
{
    return sizeof(tree_patch_run) + (nodes.capacity() * sizeof(tree_patch_node)) + (edges.capacity() * sizeof(tree_patch_edge));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one flat list of independent structural invariants.
auto validate_tree_patch_run(tree_patch_run const& run) -> result<void>
{
    if (run.operations == 0U || (run.operations & ~tree_patch_operation_mask) != 0U) {
        return corrupt("invalid patch operation flags");
    }
    if (run.from_exclusive_seq >= run.to_inclusive_seq || run.to_inclusive_seq > int64_max_u) {
        return corrupt("invalid patch sequence interval");
    }
    auto const membership = (run.operations & tree_patch_operation_membership) != 0U;
    if (!membership && run.source_game_delta != 0) {
        return corrupt("metadata patch changes the source game count");
    }
    auto edge_cursor = std::uint64_t {0};
    auto previous_root = std::optional<std::uint64_t> {};
    for (auto const& node : run.nodes) {
        if ((previous_root && *previous_root >= node.root_hash) || node.edge_begin != edge_cursor
            || static_cast<std::uint64_t>(node.edge_begin) + node.edge_count > run.edges.size())
        {
            return corrupt("patch nodes are not strictly ascending or their edge spans are not contiguous");
        }
        if (!membership && node.game_count_delta != 0) {
            return corrupt("metadata patch changes a node game count");
        }
        previous_root = node.root_hash;
        edge_cursor += node.edge_count;
        auto previous_key = std::optional<tree_edge_key> {};
        for (auto const& edge : run.node_edges(node)) {
            if (previous_key && !(*previous_key < edge.key())) {
                return corrupt("patch edges are not strictly ascending");
            }
            previous_key = edge.key();
            if ((edge.validity_mask & ~tree_patch_mask_all) != 0U) {
                return corrupt("unknown patch validity bits");
            }
            auto const has_candidates = (edge.validity_mask & tree_patch_mask_candidates) != 0U;
            if (has_candidates ? (edge.candidate_sample_min == 0U || edge.candidate_sample_min > edge.candidate_sample_max)
                               : (edge.candidate_min_root_ply != 0U || edge.candidate_sample_min != 0U || edge.candidate_sample_max != 0U))
            {
                return corrupt("invalid patch extrema candidates");
            }
            auto const& delta = edge.delta;
            auto const tallies = checked_add_i64(delta.white_wins, delta.draws);
            auto const total = tallies ? checked_add_i64(*tallies, delta.black_wins) : std::nullopt;
            if (!total || *total != delta.frequency) {
                return corrupt("patch result deltas disagree with the frequency delta");
            }
            if (!membership && (delta.frequency != 0 || edge.validity_mask != 0U)) {
                return corrupt("metadata patch changes membership state");
            }
        }
    }
    if (edge_cursor != run.edges.size()) {
        return corrupt("patch edge spans do not cover every edge");
    }
    return {};
}

auto encode_tree_patch_run(tree_patch_run const& run) -> result<std::string>
{
    if (auto valid = validate_tree_patch_run(run); !valid) {
        return tl::unexpected {valid.error()};
    }
    auto tail = std::string {};
    append_fixed_le(tail, run.identity.index_semantics);
    for (auto const byte : run.identity.bundle_id) {
        tail.push_back(static_cast<char>(byte));
    }
    append_fixed_le(tail, run.identity.canonical_start_hash);
    append_fixed_le(tail, run.identity.max_root_ply);
    append_fixed_le(tail, completeness_policy_code);
    append_fixed_le(tail, run.operations);
    append_fixed_le(tail, run.from_exclusive_seq);
    append_fixed_le(tail, run.to_inclusive_seq);
    append_zigzag(tail, run.source_game_delta);
    append_fixed_le(tail, static_cast<std::uint64_t>(run.nodes.size()));

    auto out = std::string {};
    out.reserve(fixed_prefix_bytes + tail.size() + (run.nodes.size() * typical_node_record_bytes)
                + (run.edges.size() * typical_edge_record_bytes));
    out.append(patch_magic);
    append_fixed_le(out, tree_patch_format_version);
    append_fixed_le(out, static_cast<std::uint32_t>(fixed_prefix_bytes + tail.size()));
    out.append(tail);

    for (auto const& node : run.nodes) {
        append_fixed_le(out, node.root_hash);
        append_zigzag(out, node.game_count_delta);
        append_fixed_le(out, node.edge_count);
        for (auto const& edge : run.node_edges(node)) {
            append_fixed_le(out, edge.encoded_move);
            append_fixed_le(out, edge.child_hash);
            for (auto const field : raw_edge_stat_fields) {
                append_zigzag(out, edge.delta.*field);
            }
            append_uleb128(out, edge.validity_mask);
            if ((edge.validity_mask & tree_patch_mask_candidates) != 0U) {
                append_fixed_le(out, edge.candidate_min_root_ply);
                append_fixed_le(out, edge.candidate_sample_min);
                append_fixed_le(out, edge.candidate_sample_max);
            }
        }
    }
    if (out.size() > tree_patch_max_encoded_bytes) {
        return corrupt("patch run exceeds the encoded size limit");
    }
    return out;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one strict, sequential parse of an untrusted file.
auto decode_tree_patch_run(std::string_view const bytes, tree_patch_expectation const& expected) -> result<tree_patch_run>
{
    if (bytes.size() > tree_patch_max_encoded_bytes) {
        return corrupt("patch run exceeds the encoded size limit");
    }
    auto reader = byte_reader {bytes};
    auto const magic_bytes = reader.read_bytes(patch_magic.size());
    if (!magic_bytes || *magic_bytes != patch_magic) {
        return corrupt("bad patch magic");
    }
    auto run = tree_patch_run {};
    auto const version = reader.read_u32();
    auto const header_length = reader.read_u32();
    auto const semantics = reader.read_u32();
    auto const bundle = reader.read_bytes(run.identity.bundle_id.size());
    auto const start_hash = reader.read_u64();
    auto const max_root_ply = reader.read_u16();
    auto const policy = reader.read_u16();
    auto const operations = reader.read_u32();
    auto const from = reader.read_u64();
    auto const to_seq = reader.read_u64();
    auto const source_delta = reader.read_zigzag();
    auto const node_count = reader.read_u64();
    if (!version || !header_length || !semantics || !bundle || !start_hash || !max_root_ply || !policy || !operations || !from || !to_seq
        || !source_delta || !node_count)
    {
        return corrupt("truncated patch header");
    }
    if (*version != tree_patch_format_version || *header_length != reader.position() || *policy != completeness_policy_code) {
        return corrupt("unsupported patch header");
    }
    std::ranges::transform(
        *bundle, run.identity.bundle_id.begin(), [](char const byte) -> std::uint8_t { return static_cast<std::uint8_t>(byte); });
    run.identity.index_semantics = *semantics;
    run.identity.canonical_start_hash = *start_hash;
    run.identity.max_root_ply = *max_root_ply;
    if (run.identity != expected.identity) {
        return corrupt("patch header identity disagrees with its bundle");
    }
    run.operations = *operations;
    run.from_exclusive_seq = *from;
    run.to_inclusive_seq = *to_seq;
    run.source_game_delta = *source_delta;
    if (run.from_exclusive_seq != expected.from_exclusive_seq || run.to_inclusive_seq != expected.to_inclusive_seq) {
        return corrupt("patch header interval disagrees with its descriptor");
    }
    if (*node_count > reader.remaining() / min_node_record_bytes) {
        return corrupt("patch node count exceeds the file");
    }
    run.nodes.reserve(static_cast<std::size_t>(*node_count));
    for (std::uint64_t node_index = 0; node_index < *node_count; ++node_index) {
        auto const root = reader.read_u64();
        auto const game_delta = reader.read_zigzag();
        auto const edge_count = reader.read_u32();
        if (!root || !game_delta || !edge_count || *edge_count > reader.remaining() / min_edge_record_bytes) {
            return corrupt("truncated patch node");
        }
        if (run.edges.size() + *edge_count > std::numeric_limits<std::uint32_t>::max()) {
            return corrupt("patch edge count overflow");
        }
        run.nodes.push_back(tree_patch_node {.root_hash = *root,
                                             .game_count_delta = *game_delta,
                                             .edge_begin = static_cast<std::uint32_t>(run.edges.size()),
                                             .edge_count = *edge_count});
        for (std::uint32_t edge_index = 0; edge_index < *edge_count; ++edge_index) {
            auto edge = tree_patch_edge {};
            auto const move = reader.read_u16();
            auto const child = reader.read_u64();
            if (!move || !child) {
                return corrupt("truncated patch edge");
            }
            edge.encoded_move = *move;
            edge.child_hash = *child;
            for (auto const field : raw_edge_stat_fields) {
                auto const value = reader.read_zigzag();
                if (!value) {
                    return corrupt("truncated or malformed patch statistic");
                }
                edge.delta.*field = *value;
            }
            auto const mask = reader.read_uleb128();
            if (!mask || (*mask & ~tree_patch_mask_all) != 0U) {
                return corrupt("malformed patch validity mask");
            }
            edge.validity_mask = *mask;
            if ((*mask & tree_patch_mask_candidates) != 0U) {
                auto const min_ply = reader.read_u16();
                auto const sample_min = reader.read_u32();
                auto const sample_max = reader.read_u32();
                if (!min_ply || !sample_min || !sample_max) {
                    return corrupt("truncated patch candidates");
                }
                edge.candidate_min_root_ply = *min_ply;
                edge.candidate_sample_min = *sample_min;
                edge.candidate_sample_max = *sample_max;
            }
            run.edges.push_back(edge);
        }
    }
    if (!reader.at_end()) {
        return corrupt("trailing bytes after the patch records");
    }
    run.nodes.shrink_to_fit();
    run.edges.shrink_to_fit();
    run.encoded_bytes = bytes.size();
    if (auto valid = validate_tree_patch_run(run); !valid) {
        return tl::unexpected {valid.error()};
    }
    return run;
}

auto write_tree_patch_run(std::filesystem::path const& path, tree_patch_run const& run) -> result<void>
{
    auto bytes = encode_tree_patch_run(run);
    if (!bytes) {
        return tl::unexpected {bytes.error()};
    }
    std::ofstream out {path, std::ios::binary | std::ios::trunc};
    out.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
    out.close();
    if (!out) {
        return corrupt("patch run write failed");
    }
    return {};
}

auto load_tree_patch_run(std::filesystem::path const& path, artifact_descriptor const& descriptor, tree_patch_expectation const& expected)
    -> result<tree_patch_run>
{
    std::error_code size_error;
    auto const size = std::filesystem::file_size(path, size_error);
    if (size_error || size != descriptor.size_bytes || size > tree_patch_max_encoded_bytes) {
        return corrupt("patch run size disagrees with its descriptor");
    }
    std::ifstream input {path, std::ios::binary};
    if (!input) {
        return corrupt("cannot open patch run");
    }
    auto bytes = std::string {std::istreambuf_iterator<char> {input}, std::istreambuf_iterator<char> {}};
    if (bytes.size() != size || fnv1a64(bytes) != descriptor.checksum) {
        return corrupt("patch run checksum disagrees with its descriptor");
    }
    return decode_tree_patch_run(bytes, expected);
}

// ── Patch producer ───────────────────────────────────────────────────────────

struct tree_patch_builder::state
{
    struct node_accumulator
    {
        std::int64_t game_count_delta {};
        std::map<tree_edge_key, tree_patch_edge> edges;
    };

    tree_patch_identity identity;
    tree_contribution_enumerator enumerator;
    std::uint32_t operations {0};
    std::int64_t source_delta {0};
    std::map<std::uint64_t, node_accumulator> nodes;

    explicit state(tree_patch_identity run_identity)
        : identity {run_identity}
        , enumerator {run_identity.max_root_ply, run_identity.canonical_start_hash, /*collect_visited=*/false}
    {
    }
};

tree_patch_builder::tree_patch_builder(tree_patch_identity identity)
    : state_ {std::make_unique<state>(identity)}
{
}

tree_patch_builder::~tree_patch_builder() = default;
tree_patch_builder::tree_patch_builder(tree_patch_builder&&) noexcept = default;
auto tree_patch_builder::operator=(tree_patch_builder&&) noexcept -> tree_patch_builder& = default;

namespace
{

struct extrema_update
{
    std::uint64_t mask {};
    std::uint16_t min_root_ply {};
    std::uint32_t sample {};
};

// Folds one edge contribution into the node's coalesced edge: statistics add
// with overflow checks, validity bits OR, and candidate extrema combine by
// min/max.
template<typename NodeAccumulator>
auto accumulate_edge(NodeAccumulator& node, game_tree_edge const& edge, raw_edge_stats const& delta, extrema_update const& extrema)
    -> result<void>
{
    auto const key = tree_edge_key {.encoded_move = edge.encoded_move, .child_hash = edge.child_hash};
    auto const entry = node.edges.try_emplace(key, tree_patch_edge {.encoded_move = key.encoded_move, .child_hash = key.child_hash}).first;
    auto& target = entry->second;
    auto const sum = add_stats(target.delta, delta);
    if (!sum) {
        return corrupt("patch statistic overflow");
    }
    target.delta = *sum;
    if ((extrema.mask & tree_patch_mask_candidates) != 0U) {
        if ((target.validity_mask & tree_patch_mask_candidates) != 0U) {
            target.candidate_min_root_ply = std::min(target.candidate_min_root_ply, extrema.min_root_ply);
            target.candidate_sample_min = std::min(target.candidate_sample_min, extrema.sample);
            target.candidate_sample_max = std::max(target.candidate_sample_max, extrema.sample);
        } else {
            target.candidate_min_root_ply = extrema.min_root_ply;
            target.candidate_sample_min = extrema.sample;
            target.candidate_sample_max = extrema.sample;
        }
    }
    target.validity_mask |= extrema.mask;
    return {};
}

auto bump(std::int64_t& counter, std::int64_t const amount) -> result<void>
{
    auto const next = checked_add_i64(counter, amount);
    if (!next) {
        return corrupt("patch count overflow");
    }
    counter = *next;
    return {};
}

}  // namespace

auto tree_patch_builder::add_metadata_edit(tree_game_state const& before, tree_game_state const& after) -> result<void>
{
    auto& self = *state_;
    if (!std::ranges::equal(before.moves, after.moves)) {
        return tl::unexpected {error {error_code::invalid_argument, "a metadata edit cannot change the moves"}};
    }
    auto const before_stats = make_game_stats(before.result, before.white_elo, before.black_elo);
    auto const after_stats = make_game_stats(after.result, after.white_elo, after.black_elo);
    if (!before_stats || !after_stats) {
        return tl::unexpected {before_stats ? after_stats.error() : before_stats.error()};
    }
    auto const delta = subtract_stats(*after_stats, *before_stats);
    if (!delta) {
        return corrupt("patch statistic overflow");
    }
    self.operations |= tree_patch_operation_metadata;
    if (is_zero(*delta)) {
        return {};
    }
    if (auto enumerated = self.enumerator.enumerate(after.moves); !enumerated) {
        return enumerated;
    }
    for (auto const& edge : self.enumerator.edges()) {
        if (auto added = accumulate_edge(self.nodes[edge.root_hash], edge, *delta, {}); !added) {
            return added;
        }
    }
    return {};
}

auto tree_patch_builder::add_game_insertion(tree_game_state const& game) -> result<void>
{
    auto& self = *state_;
    if (game.id.value == 0U) {
        return tl::unexpected {error {error_code::invalid_argument, "a game needs a nonzero id"}};
    }
    auto const stats = make_game_stats(game.result, game.white_elo, game.black_elo);
    if (!stats) {
        return tl::unexpected {stats.error()};
    }
    if (auto enumerated = self.enumerator.enumerate(game.moves); !enumerated) {
        return enumerated;
    }
    if (auto bumped = bump(self.source_delta, 1); !bumped) {
        return bumped;
    }
    self.operations |= tree_patch_operation_membership;
    for (auto const root : self.enumerator.roots()) {
        if (auto bumped = bump(self.nodes[root].game_count_delta, 1); !bumped) {
            return bumped;
        }
    }
    for (auto const& edge : self.enumerator.edges()) {
        auto const extrema =
            extrema_update {.mask = tree_patch_mask_candidates, .min_root_ply = edge.min_root_ply, .sample = game.id.value};
        if (auto added = accumulate_edge(self.nodes[edge.root_hash], edge, *stats, extrema); !added) {
            return added;
        }
    }
    return {};
}

auto tree_patch_builder::add_game_removal(tree_game_state const& game) -> result<void>
{
    auto& self = *state_;
    auto const stats = make_game_stats(game.result, game.white_elo, game.black_elo);
    if (!stats) {
        return tl::unexpected {stats.error()};
    }
    auto const negated = subtract_stats(raw_edge_stats {}, *stats);
    if (!negated) {
        return corrupt("patch statistic overflow");
    }
    if (auto enumerated = self.enumerator.enumerate(game.moves); !enumerated) {
        return enumerated;
    }
    if (auto bumped = bump(self.source_delta, -1); !bumped) {
        return bumped;
    }
    self.operations |= tree_patch_operation_membership;
    for (auto const root : self.enumerator.roots()) {
        if (auto bumped = bump(self.nodes[root].game_count_delta, -1); !bumped) {
            return bumped;
        }
    }
    for (auto const& edge : self.enumerator.edges()) {
        auto const extrema = extrema_update {.mask = tree_patch_mask_invalidate_min_ply | tree_patch_mask_invalidate_samples};
        if (auto added = accumulate_edge(self.nodes[edge.root_hash], edge, *negated, extrema); !added) {
            return added;
        }
    }
    return {};
}

auto tree_patch_builder::build(std::uint64_t const from_exclusive_seq, std::uint64_t const to_inclusive_seq) -> result<tree_patch_run>
{
    auto& self = *state_;
    auto run = tree_patch_run {.identity = self.identity,
                               .operations = self.operations != 0U ? self.operations : tree_patch_operation_metadata,
                               .from_exclusive_seq = from_exclusive_seq,
                               .to_inclusive_seq = to_inclusive_seq,
                               .source_game_delta = self.source_delta,
                               .nodes = {},
                               .edges = {},
                               .encoded_bytes = 0};
    for (auto const& [root, node] : self.nodes) {
        auto const begin = run.edges.size();
        for (auto const& [key, edge] : node.edges) {
            if (!is_zero(edge.delta) || edge.validity_mask != 0U) {
                run.edges.push_back(edge);
            }
        }
        auto const count = run.edges.size() - begin;
        if (node.game_count_delta == 0 && count == 0U) {
            continue;
        }
        run.nodes.push_back(tree_patch_node {.root_hash = root,
                                             .game_count_delta = node.game_count_delta,
                                             .edge_begin = static_cast<std::uint32_t>(begin),
                                             .edge_count = static_cast<std::uint32_t>(count)});
    }
    auto encoded = encode_tree_patch_run(run);
    if (!encoded) {
        return tl::unexpected {encoded.error()};
    }
    run.encoded_bytes = encoded->size();
    run.nodes.shrink_to_fit();
    run.edges.shrink_to_fit();
    return run;
}

}  // namespace motif::db
