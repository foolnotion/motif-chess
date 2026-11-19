#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "motif/db/error.hpp"
#include "motif/db/manifest.hpp"
#include "motif/db/opening_tree_index.hpp"
#include "motif/db/opening_tree_stats.hpp"
#include "motif/db/types.hpp"

namespace motif::db
{

// ── Tree patch run, format v1 ────────────────────────────────────────────────
//
// An immutable file `opening_tree.patch.<random128hex>` holding signed tree
// deltas for the half-open sequence interval (from, to]. All fixed-width
// integers are little-endian.
//
//   header:  "MOTIFTP1"(8) | u32 format_version(=1) | u32 header_length |
//            u32 index_semantics | 16B bundle identity | u64 canonical_start_hash |
//            u16 max_root_ply | u16 completeness_policy(=1) | u32 operation_flags |
//            u64 from_exclusive | u64 to_inclusive | zigzag source_game_delta |
//            u64 node_count
//            (header_length counts every byte up to and including node_count)
//   node*  : u64 root_hash | zigzag game_count_delta | u32 edge_count | edge*
//            (strictly ascending root_hash; an empty edge list is valid)
//   edge   : u16 encoded_move | u64 child_hash | 10 x zigzag stats in the order
//            frequency, white_wins, draws, black_wins, white_sum, white_count,
//            black_sum, black_count, numerator2, weight2 | uleb validity_mask |
//            [if mask & candidates: u16 min_root_ply | u32 sample_min | u32 sample_max]
//            (strictly ascending (move, child) within a node)
//   EOF    : exactly after the last record; the artifact descriptor's FNV-1a-64
//            covers header and body.
//
// validity_mask: invalidate_min_ply(1) / invalidate_samples(2) make the edge's minimum
// root ply / sample game-id bounds unknown after a retraction; candidates(4) carries a
// contribution that is min/max-combined with the base while the base is still exact;
// exact_extrema(8) carries the EXACT extrema of the games that remain once a retraction
// has been applied (always together with candidates): they replace the base's values, which
// stay exact instead of being invalidated. Only a retraction (negative frequency delta) may
// carry it, and never together with an invalidate bit.
//
// operation_flags: metadata(1) = result/rating deltas only; membership(2) = the
// run may add or remove games (source/node/frequency deltas, validity-mask
// bits). Unknown bits are rejected. A run without the membership bit must have
// zero source/node/frequency deltas and zero masks.
inline constexpr std::uint32_t tree_patch_operation_metadata = 1;
inline constexpr std::uint32_t tree_patch_operation_membership = 2;
inline constexpr std::uint32_t tree_patch_operation_mask = 3;

// validity_mask bits.
inline constexpr std::uint64_t tree_patch_mask_invalidate_min_ply = 1;
inline constexpr std::uint64_t tree_patch_mask_invalidate_samples = 2;
inline constexpr std::uint64_t tree_patch_mask_candidates = 4;
inline constexpr std::uint64_t tree_patch_mask_exact_extrema = 8;
inline constexpr std::uint64_t tree_patch_mask_all = 15;

// A single run file larger than this is refused before it is read.
inline constexpr std::uint64_t tree_patch_max_encoded_bytes = std::uint64_t {64} << 20U;

// What a header must repeat so a run moved between bundles or retargeted by a
// descriptor edit is detected.
struct tree_patch_identity
{
    identity_bytes bundle_id {};
    std::uint32_t index_semantics {index_semantics_version};
    std::uint64_t canonical_start_hash {};
    std::uint16_t max_root_ply {};

    auto operator==(tree_patch_identity const&) const -> bool = default;
};

struct tree_patch_edge
{
    std::uint16_t encoded_move {};
    std::uint64_t child_hash {};
    raw_edge_stats delta {};
    std::uint64_t validity_mask {};
    // Meaningful only when validity_mask has the candidates bit.
    std::uint16_t candidate_min_root_ply {};
    std::uint32_t candidate_sample_min {};
    std::uint32_t candidate_sample_max {};

    [[nodiscard]] auto key() const noexcept -> tree_edge_key
    {
        return tree_edge_key {.encoded_move = encoded_move, .child_hash = child_hash};
    }

    auto operator==(tree_patch_edge const&) const -> bool = default;
};

struct tree_patch_node
{
    std::uint64_t root_hash {};
    std::int64_t game_count_delta {};
    // Span into tree_patch_run::edges.
    std::uint32_t edge_begin {};
    std::uint32_t edge_count {};

    auto operator==(tree_patch_node const&) const -> bool = default;
};

struct tree_patch_run
{
    tree_patch_identity identity;
    std::uint32_t operations {tree_patch_operation_metadata};
    std::uint64_t from_exclusive_seq {};
    std::uint64_t to_inclusive_seq {};
    std::int64_t source_game_delta {};
    std::vector<tree_patch_node> nodes;  // strictly ascending root_hash
    std::vector<tree_patch_edge> edges;  // flat; each node owns a contiguous ascending span
    // Size of the encoded file this run came from or encodes to (0 until set).
    std::uint64_t encoded_bytes {};

    [[nodiscard]] auto find_node(std::uint64_t root_hash) const noexcept -> tree_patch_node const*;
    [[nodiscard]] auto node_edges(tree_patch_node const& node) const noexcept -> std::span<tree_patch_edge const>;
    // Bytes this decoded representation actually occupies (capacity-based),
    // which is what the resident-memory cap counts.
    [[nodiscard]] auto decoded_bytes() const noexcept -> std::uint64_t;

    auto operator==(tree_patch_run const&) const -> bool = default;
};

// Structural and semantic validation shared by the encoder and the decoder.
[[nodiscard]] auto validate_tree_patch_run(tree_patch_run const& run) -> result<void>;

[[nodiscard]] auto encode_tree_patch_run(tree_patch_run const& run) -> result<std::string>;

struct tree_patch_expectation
{
    tree_patch_identity identity;
    std::uint64_t from_exclusive_seq {};
    std::uint64_t to_inclusive_seq {};
};

// Strict decode: every declared count is checked against the bytes available
// before any allocation, and any header/identity/interval disagreement, bad
// flag, order violation, trailing byte or truncation is io_failure.
[[nodiscard]] auto decode_tree_patch_run(std::string_view bytes, tree_patch_expectation const& expected) -> result<tree_patch_run>;

[[nodiscard]] auto write_tree_patch_run(std::filesystem::path const& path, tree_patch_run const& run) -> result<void>;

// Reads path once, verifies its size and FNV-1a-64 against the descriptor, then
// decodes it.
[[nodiscard]] auto load_tree_patch_run(std::filesystem::path const& path,
                                       artifact_descriptor const& descriptor,
                                       tree_patch_expectation const& expected) -> result<tree_patch_run>;

// ── Patch producer ───────────────────────────────────────────────────────────

// The exact extrema of one edge over a set of games.
struct tree_exact_extrema
{
    std::uint16_t min_root_ply {};
    std::uint32_t sample_min {};
    std::uint32_t sample_max {};
};

struct tree_game_state
{
    game_id id {};
    std::span<std::uint16_t const> moves;
    std::int8_t result {};
    std::optional<std::int32_t> white_elo;
    std::optional<std::int32_t> black_elo;
};

// Resolves the extrema of an edge once the game being removed is gone; see
// tree_patch_builder::add_game_removal.
using tree_removal_extrema_resolver = std::function<result<std::optional<tree_exact_extrema>>(game_tree_edge const&)>;

// Accumulates per-game contributions into one coalesced run using the shared
// tree_contribution_enumerator (so traversal and dedup are never reimplemented)
// and checked arithmetic. Edges whose net delta is zero and carry no validity
// information, and nodes with no net change, are dropped.
class tree_patch_builder
{
  public:
    explicit tree_patch_builder(tree_patch_identity identity);
    ~tree_patch_builder();
    tree_patch_builder(tree_patch_builder const&) = delete;
    auto operator=(tree_patch_builder const&) -> tree_patch_builder& = delete;
    tree_patch_builder(tree_patch_builder&&) noexcept;
    auto operator=(tree_patch_builder&&) noexcept -> tree_patch_builder&;

    // One game's result/rating edit: C(after) - C(before) on every distinct
    // covered edge; node and source counts do not change. The two states must
    // describe the same moves.
    [[nodiscard]] auto add_metadata_edit(tree_game_state const& before, tree_game_state const& after) -> result<void>;
    // Whole-game membership: +C with extrema candidates (insert) or -C (remove),
    // and +/-1 on every covered root.
    //
    // A removal without `resolver` invalidates the extrema the game could have
    // witnessed on every edge it uses. With one, the resolver is asked for each
    // edge, in the enumerator's order: it returns the exact extrema of the games
    // that remain (the edge then carries them, replacing the base's), or nullopt
    // when the game was no witness or the edge disappears with it (no extrema
    // information is recorded). Only one such removal fits a run, and no other
    // membership operation may share it: the resolver describes the corpus with
    // that single game removed.
    [[nodiscard]] auto add_game_insertion(tree_game_state const& game) -> result<void>;
    [[nodiscard]] auto add_game_removal(tree_game_state const& game, tree_removal_extrema_resolver const& resolver = {}) -> result<void>;

    // Finishes the run for (from, to]. An empty run is valid: it certifies an
    // epoch whose numerical delta is empty.
    [[nodiscard]] auto build(std::uint64_t from_exclusive_seq, std::uint64_t to_inclusive_seq) -> result<tree_patch_run>;

  private:
    struct state;
    std::unique_ptr<state> state_;
};

}  // namespace motif::db
