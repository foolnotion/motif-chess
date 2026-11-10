// PR 6a: tree patch-run format v1, the merged (checkpoint + runs) reader, the
// streaming fold, chain admission, and the rebuild-equivalence properties.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "motif/db/opening_tree_patch.hpp"

#include <catch2/catch_test_macros.hpp>

#include "composite_test_support.hpp"
#include "motif/db/manifest.hpp"
#include "motif/db/opening_tree_composite.hpp"
#include "motif/db/opening_tree_index.hpp"
#include "motif/db/opening_tree_stats.hpp"
#include "motif/db/patch_chain.hpp"

namespace
{

using namespace motif::db;  // NOLINT(google-build-using-namespace) -- the file exercises most of motif::db; qualifying every name buries
                            // the assertions.
using motif_test::collect_nodes;
using motif_test::patch_identity;
using motif_test::temp_dir;
using motif_test::tree_state_of;
using motif_test::write_tree_checkpoint;

// magic (8) + format version (4) + header length (4).
constexpr auto header_prefix_bytes = std::size_t {16};
// sample_run() covers sequences (7, 8]; its candidate edges carry these extrema.
constexpr auto sample_from_seq = std::uint64_t {7};
constexpr auto sample_to_seq = std::uint64_t {8};
constexpr auto candidate_ply = std::uint16_t {5};
constexpr auto candidate_sample_low = std::uint32_t {3};
constexpr auto candidate_sample_high = std::uint32_t {9};
// One ply either side of the default horizon the checkpoints are built with.
constexpr auto default_horizon = opening_tree_index_default_max_root_ply;
constexpr auto foreign_horizon = static_cast<std::uint16_t>(default_horizon - 1U);
constexpr auto beyond_horizon = static_cast<std::uint16_t>(default_horizon + 1U);

// ── An independent writer of the documented byte layout ─────────────────────
//
// Deliberately written from the format description alone and WITHOUT any of the
// production validation, so malformed runs can be forged and the production
// encoder's exact bytes can be pinned.

auto reference_encode(tree_patch_run const& run, std::optional<std::uint32_t> const node_count_override = std::nullopt) -> std::string
{
    auto tail = std::string {};
    append_fixed_le(tail, run.identity.index_semantics);
    for (auto const byte : run.identity.bundle_id) {
        tail.push_back(static_cast<char>(byte));
    }
    append_fixed_le(tail, run.identity.canonical_start_hash);
    append_fixed_le(tail, run.identity.max_root_ply);
    append_fixed_le(tail, std::uint16_t {1});
    append_fixed_le(tail, run.operations);
    append_fixed_le(tail, run.from_exclusive_seq);
    append_fixed_le(tail, run.to_inclusive_seq);
    append_zigzag(tail, run.source_game_delta);
    append_fixed_le(tail,
                    node_count_override ? static_cast<std::uint64_t>(*node_count_override) : static_cast<std::uint64_t>(run.nodes.size()));
    auto out = std::string {"MOTIFTP1"};
    append_fixed_le(out, std::uint32_t {1});
    append_fixed_le(out, static_cast<std::uint32_t>(header_prefix_bytes + tail.size()));
    out += tail;
    for (auto const& node : run.nodes) {
        append_fixed_le(out, node.root_hash);
        append_zigzag(out, node.game_count_delta);
        append_fixed_le(out, node.edge_count);
        for (auto index = std::uint32_t {0}; index < node.edge_count; ++index) {
            auto const& edge = run.edges[static_cast<std::size_t>(node.edge_begin) + index];
            append_fixed_le(out, edge.encoded_move);
            append_fixed_le(out, edge.child_hash);
            for (auto const value : {edge.delta.frequency,
                                     edge.delta.white_wins,
                                     edge.delta.draws,
                                     edge.delta.black_wins,
                                     edge.delta.white_sum,
                                     edge.delta.white_count,
                                     edge.delta.black_sum,
                                     edge.delta.black_count,
                                     edge.delta.numerator2,
                                     edge.delta.weight2})
            {
                append_zigzag(out, value);
            }
            append_uleb128(out, edge.validity_mask);
            if ((edge.validity_mask & 4U) != 0U) {
                append_fixed_le(out, edge.candidate_min_root_ply);
                append_fixed_le(out, edge.candidate_sample_min);
                append_fixed_le(out, edge.candidate_sample_max);
            }
        }
    }
    return out;
}

auto expectation_of(tree_patch_run const& run) -> tree_patch_expectation
{
    return tree_patch_expectation {
        .identity = run.identity, .from_exclusive_seq = run.from_exclusive_seq, .to_inclusive_seq = run.to_inclusive_seq};
}

auto edge_of(std::uint16_t const move, std::uint64_t const child, raw_edge_stats const& delta, std::uint64_t const mask = 0)
    -> tree_patch_edge
{
    auto edge = tree_patch_edge {.encoded_move = move, .child_hash = child, .delta = delta, .validity_mask = mask};
    if ((mask & tree_patch_mask_candidates) != 0U) {
        edge.candidate_min_root_ply = candidate_ply;
        edge.candidate_sample_min = candidate_sample_low;
        edge.candidate_sample_max = candidate_sample_high;
    }
    return edge;
}

// A run with membership deltas, an empty node, invalidation flags, candidates
// and an absent-base-style edge.
auto sample_run() -> tree_patch_run
{
    auto run = tree_patch_run {.identity = patch_identity(),
                               .operations = tree_patch_operation_metadata | tree_patch_operation_membership,
                               .from_exclusive_seq = sample_from_seq,
                               .to_inclusive_seq = sample_to_seq,
                               .source_game_delta = 2,
                               .nodes = {},
                               .edges = {},
                               .encoded_bytes = 0};
    auto const insertion = raw_edge_stats {.frequency = 2,
                                           .white_wins = 1,
                                           .draws = 0,
                                           .black_wins = 1,
                                           .white_sum = 4'600,
                                           .white_count = 2,
                                           .black_sum = -9,
                                           .black_count = 1,
                                           .numerator2 = 0,
                                           .weight2 = 100};
    auto const retraction = raw_edge_stats {.frequency = -1,
                                            .white_wins = 0,
                                            .draws = -1,
                                            .black_wins = 0,
                                            .white_sum = 0,
                                            .white_count = 0,
                                            .black_sum = 0,
                                            .black_count = 0,
                                            .numerator2 = 0,
                                            .weight2 = 0};
    auto const metadata_only = raw_edge_stats {.frequency = 0,
                                               .white_wins = -1,
                                               .draws = 1,
                                               .black_wins = 0,
                                               .white_sum = -2'300,
                                               .white_count = -1,
                                               .black_sum = 0,
                                               .black_count = 0,
                                               .numerator2 = -4'800,
                                               .weight2 = -4'800};
    // NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers) -- child and root hashes are arbitrary distinct labels
    // for the fixture's edges and nodes.
    run.edges = {edge_of(1, 0xA1, insertion, tree_patch_mask_candidates),
                 edge_of(2, 0xA2, retraction, tree_patch_mask_invalidate_min_ply | tree_patch_mask_invalidate_samples),
                 edge_of(3, 0xB1, metadata_only),
                 edge_of(4, 0xC1, insertion, tree_patch_mask_candidates | tree_patch_mask_invalidate_samples)};
    run.nodes = {tree_patch_node {.root_hash = 0x10, .game_count_delta = 2, .edge_begin = 0, .edge_count = 2},
                 tree_patch_node {.root_hash = 0x20, .game_count_delta = 0, .edge_begin = 2, .edge_count = 1},
                 tree_patch_node {.root_hash = 0x30, .game_count_delta = -1, .edge_begin = 3, .edge_count = 0},
                 tree_patch_node {.root_hash = 0x40, .game_count_delta = 1, .edge_begin = 3, .edge_count = 1}};
    // NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
    return run;
}

}  // namespace

TEST_CASE("tree patch run supports a zero-ply horizon", "[motif-db][pr6a][patch]")
{
    auto run = sample_run();
    run.identity.max_root_ply = 0;
    run.nodes.clear();
    run.edges.clear();
    auto const encoded = encode_tree_patch_run(run);
    REQUIRE(encoded.has_value());
    auto const decoded = decode_tree_patch_run(*encoded, expectation_of(run));
    REQUIRE(decoded.has_value());
    CHECK(decoded->identity.max_root_ply == 0);
}

TEST_CASE("a tree patch run encodes to exactly the documented layout and round-trips", "[motif-db][pr6a][patch]")
{
    auto const run = sample_run();
    REQUIRE(validate_tree_patch_run(run).has_value());
    auto const encoded = encode_tree_patch_run(run);
    REQUIRE(encoded.has_value());
    CHECK(*encoded == reference_encode(run));
    CHECK(encoded->substr(0, 8) == "MOTIFTP1");

    auto const decoded = decode_tree_patch_run(*encoded, expectation_of(run));
    REQUIRE(decoded.has_value());
    CHECK(decoded->identity == run.identity);
    CHECK(decoded->operations == run.operations);
    CHECK(decoded->source_game_delta == 2);
    CHECK(decoded->nodes == run.nodes);
    CHECK(decoded->edges == run.edges);
    CHECK(decoded->encoded_bytes == encoded->size());
    // Membership deltas, an empty node, invalidation flags and candidates survived.
    REQUIRE(decoded->find_node(0x30) != nullptr);
    CHECK(decoded->node_edges(*decoded->find_node(0x30)).empty());
    CHECK(decoded->find_node(0x30)->game_count_delta == -1);
    CHECK(decoded->find_node(0x99) == nullptr);
    CHECK(decoded->edges[1].validity_mask == (tree_patch_mask_invalidate_min_ply | tree_patch_mask_invalidate_samples));
    CHECK(decoded->edges[0].candidate_sample_max == 9U);
    CHECK(decoded->decoded_bytes() >= decoded->nodes.size() * sizeof(tree_patch_node));

    // An empty certificate run is valid.
    auto empty = tree_patch_run {.identity = patch_identity(),
                                 .operations = tree_patch_operation_metadata,
                                 .from_exclusive_seq = 3,
                                 .to_inclusive_seq = 4,
                                 .nodes = {},
                                 .edges = {}};
    auto const empty_bytes = encode_tree_patch_run(empty);
    REQUIRE(empty_bytes.has_value());
    CHECK(decode_tree_patch_run(*empty_bytes, expectation_of(empty)).has_value());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("every truncation and every header/identity disagreement of a patch run is rejected", "[motif-db][pr6a][patch]")
{
    auto const run = sample_run();
    auto const bytes = *encode_tree_patch_run(run);
    auto const expected = expectation_of(run);

    for (std::size_t length = 0; length < bytes.size(); ++length) {
        CHECK_FALSE(decode_tree_patch_run(std::string_view {bytes}.substr(0, length), expected).has_value());
    }
    CHECK_FALSE(decode_tree_patch_run(bytes + '\x00', expected).has_value());  // trailing byte

    auto const header_length = static_cast<std::size_t>(static_cast<std::uint8_t>(bytes[12]));
    REQUIRE(header_length < 200U);
    // Offsets within the fixed header: version 8, header_length 12, semantics 16,
    // bundle 20, start hash 36, horizon 44, policy 46, operations 48, from 52, to 60.
    auto const flipped = [&](std::size_t const offset, char const value) -> std::string
    {
        auto copy = bytes;
        copy[offset] = value;
        return copy;
    };
    CHECK_FALSE(decode_tree_patch_run(flipped(0, 'X'), expected).has_value());  // magic
    CHECK_FALSE(decode_tree_patch_run(flipped(8, '\x02'), expected).has_value());  // version
    CHECK_FALSE(decode_tree_patch_run(flipped(12, static_cast<char>(header_length + 1)), expected).has_value());  // header length
    CHECK_FALSE(decode_tree_patch_run(flipped(16, '\x02'), expected).has_value());  // index semantics
    CHECK_FALSE(decode_tree_patch_run(flipped(20, '\x7f'), expected).has_value());  // bundle identity
    CHECK_FALSE(decode_tree_patch_run(flipped(36, '\x7f'), expected).has_value());  // start hash
    CHECK_FALSE(decode_tree_patch_run(flipped(44, '\x15'), expected).has_value());  // horizon 21
    CHECK_FALSE(decode_tree_patch_run(flipped(46, '\x02'), expected).has_value());  // completeness policy
    CHECK_FALSE(decode_tree_patch_run(flipped(48, '\x00'), expected).has_value());  // no operation flags
    CHECK_FALSE(decode_tree_patch_run(flipped(48, '\x07'), expected).has_value());  // unknown operation bit
    CHECK_FALSE(decode_tree_patch_run(flipped(52, '\x09'), expected).has_value());  // from changed
    CHECK_FALSE(decode_tree_patch_run(flipped(60, '\x0a'), expected).has_value());  // to changed

    // The same bytes under a different expectation (moved to another bundle, or
    // described by a different interval) are refused.
    auto other = expected;
    other.identity.bundle_id[0] ^= 0x01U;
    CHECK_FALSE(decode_tree_patch_run(bytes, other).has_value());
    other = expected;
    other.to_inclusive_seq = sample_to_seq + 1;
    CHECK_FALSE(decode_tree_patch_run(bytes, other).has_value());
    other = expected;
    other.identity.max_root_ply = foreign_horizon;
    CHECK_FALSE(decode_tree_patch_run(bytes, other).has_value());

    // A hostile node count is checked against the bytes present, not allocated.
    CHECK_FALSE(decode_tree_patch_run(reference_encode(run, 0x7fffffffU), expected).has_value());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("structural violations in a patch run body are rejected by the decoder and the encoder", "[motif-db][pr6a][patch]")
{
    auto const good = sample_run();
    auto const rejected = [&](auto const& mutate) -> bool
    {
        auto run = good;
        mutate(run);
        // The encoder refuses what the decoder would refuse...
        auto const encoder_rejects = !encode_tree_patch_run(run).has_value();
        // ...and the decoder rejects the same malformed bytes forged independently.
        auto const decoder_rejects = !decode_tree_patch_run(reference_encode(run), expectation_of(run)).has_value();
        return encoder_rejects && decoder_rejects;
    };
    CHECK_FALSE(rejected([](tree_patch_run& /*run*/) -> void {}));
    CHECK(rejected(
        [](tree_patch_run& target) -> void
        {
            std::swap(target.nodes[0], target.nodes[1]);
            target.nodes[0].edge_begin = 0;
            target.nodes[1].edge_begin = 2;
        }));  // unsorted roots
    CHECK(rejected([](tree_patch_run& target) -> void { target.nodes[1].root_hash = target.nodes[0].root_hash; }));  // duplicate root
    CHECK(rejected([](tree_patch_run& target) -> void { std::swap(target.edges[0], target.edges[1]); }));  // unsorted edge keys
    CHECK(rejected(
        [](tree_patch_run& target) -> void
        {
            target.edges[1].encoded_move = target.edges[0].encoded_move;
            target.edges[1].child_hash = target.edges[0].child_hash;
        }));  // duplicate key
    CHECK(rejected([](tree_patch_run& target) -> void { target.edges[0].delta.frequency = 5; }));  // W+D+B != frequency
    CHECK(rejected([](tree_patch_run& target) -> void { target.edges[0].validity_mask = 8; }));  // unknown mask bit
    CHECK(rejected([](tree_patch_run& target) -> void { target.edges[0].candidate_sample_min = 0; }));  // sample bound 0
    CHECK(rejected([](tree_patch_run& target) -> void { target.edges[0].candidate_sample_min = 10; }));  // min above max
    // (not decoder-visible: those bytes are simply a different, valid run)
    {
        auto run = good;
        run.edges[2].candidate_min_root_ply = 4;  // candidate payload without the candidates bit
        CHECK_FALSE(encode_tree_patch_run(run).has_value());
    }
    CHECK(rejected([](tree_patch_run& target) -> void { target.from_exclusive_seq = target.to_inclusive_seq; }));  // empty interval
    CHECK(rejected([](tree_patch_run& target) -> void { target.from_exclusive_seq = 9; }));  // reversed
    CHECK(rejected([](tree_patch_run& target) -> void
                   { target.to_inclusive_seq = std::numeric_limits<std::uint64_t>::max(); }));  // beyond int64
    {
        auto run = good;
        run.nodes[0].edge_count = 3;  // spans overlap the next node
        CHECK_FALSE(encode_tree_patch_run(run).has_value());
    }
    CHECK(rejected([](tree_patch_run& target) -> void { target.operations = 0; }));
    CHECK(rejected([](tree_patch_run& target) -> void { target.operations = 4; }));

    // Without the membership bit a run may not touch source/node/frequency/validity state.
    auto const metadata_only_rejected = [&](auto const& mutate) -> bool
    {
        auto run = good;
        run.operations = tree_patch_operation_metadata;
        run.source_game_delta = 0;
        for (auto& node : run.nodes) {
            node.game_count_delta = 0;
        }
        for (auto& edge : run.edges) {
            edge.delta = {};
            edge.validity_mask = 0;
            edge.candidate_min_root_ply = 0;
            edge.candidate_sample_min = 0;
            edge.candidate_sample_max = 0;
        }
        run.edges[0].delta.white_wins = -1;
        run.edges[0].delta.draws = 1;
        REQUIRE(encode_tree_patch_run(run).has_value());  // the baseline is acceptable
        mutate(run);
        return !encode_tree_patch_run(run).has_value() && !decode_tree_patch_run(reference_encode(run), expectation_of(run)).has_value();
    };
    CHECK(metadata_only_rejected([](tree_patch_run& target) -> void { target.source_game_delta = 1; }));
    CHECK(metadata_only_rejected([](tree_patch_run& target) -> void { target.nodes[0].game_count_delta = 1; }));
    CHECK(metadata_only_rejected(
        [](tree_patch_run& target) -> void
        {
            target.edges[0].delta.frequency = 1;
            target.edges[0].delta.draws = 2;
        }));
    CHECK(
        metadata_only_rejected([](tree_patch_run& target) -> void { target.edges[0].validity_mask = tree_patch_mask_invalidate_min_ply; }));
}

TEST_CASE("a non-minimal varint inside a patch statistic is rejected", "[motif-db][pr6a][patch]")
{
    auto run = sample_run();
    auto bytes = reference_encode(run);
    // The first node's first edge: find its frequency varint (zigzag(2) = 4) after
    // the header, node header and the edge's move/child, and widen it.
    auto const header_length = static_cast<std::size_t>(static_cast<std::uint8_t>(bytes[12]));
    auto const frequency_offset = header_length + 8 + 1 + 4 + 2 + 8;
    REQUIRE(static_cast<std::uint8_t>(bytes[frequency_offset]) == 4U);
    bytes.replace(frequency_offset, 1, std::string {'\x84', '\x00'});
    CHECK_FALSE(decode_tree_patch_run(bytes, expectation_of(run)).has_value());
}

TEST_CASE("loading a patch run verifies its descriptor size and checksum", "[motif-db][pr6a][patch]")
{
    temp_dir const dir {"patch-load"};
    auto const run = sample_run();
    auto const path = dir.path / "opening_tree.patch.0123456789abcdef0123456789abcdef";
    REQUIRE(write_tree_patch_run(path, run).has_value());
    auto const bytes = *encode_tree_patch_run(run);
    auto descriptor = artifact_descriptor {.id = "0123456789abcdef0123456789abcdef",
                                           .filename = path.filename().string(),
                                           .format = std::string {tree_patch_format_name},
                                           .format_version = tree_patch_format_version,
                                           .size_bytes = bytes.size(),
                                           .checksum = fnv1a64(bytes)};
    CHECK(load_tree_patch_run(path, descriptor, expectation_of(run)).has_value());

    auto wrong_size = descriptor;
    wrong_size.size_bytes += 1;
    CHECK_FALSE(load_tree_patch_run(path, wrong_size, expectation_of(run)).has_value());
    auto wrong_sum = descriptor;
    wrong_sum.checksum ^= 1U;
    CHECK_FALSE(load_tree_patch_run(path, wrong_sum, expectation_of(run)).has_value());
    CHECK_FALSE(load_tree_patch_run(dir.path / "missing", descriptor, expectation_of(run)).has_value());

    // A flipped byte keeps the size but fails the checksum.
    auto damaged = bytes;
    damaged[damaged.size() - 3] = static_cast<char>(static_cast<unsigned char>(damaged[damaged.size() - 3]) ^ 0x01U);
    {
        std::ofstream out {path, std::ios::binary | std::ios::trunc};
        out.write(damaged.data(), static_cast<std::streamsize>(damaged.size()));
    }
    CHECK_FALSE(load_tree_patch_run(path, descriptor, expectation_of(run)).has_value());
}

// ── The merged reader and its invariants ─────────────────────────────────────

namespace
{

struct small_tree
{
    temp_dir dir {"merge"};
    std::vector<motif_test::model_game> corpus;
    std::filesystem::path checkpoint;

    explicit small_tree(std::vector<motif_test::model_game> games)
        : corpus {std::move(games)}
        , checkpoint {dir.path / "base.idx"}
    {
        write_tree_checkpoint(checkpoint, corpus);
    }
};

auto open_composite(small_tree const& tree,
                    std::vector<tree_patch_run> runs,
                    tree_child_frequency_mode const mode = tree_child_frequency_mode::checkpoint,
                    std::uint64_t const max_decoded = patch_chain_limits {}.hard_decoded_bytes) -> result<composite_opening_tree>
{
    auto shared = std::vector<tree_patch_run_ptr> {};
    for (auto& run : runs) {
        shared.push_back(std::make_shared<tree_patch_run const>(std::move(run)));
    }
    return composite_opening_tree::open(tree.checkpoint, std::move(shared), mode, max_decoded);
}

auto metadata_run(std::vector<motif_test::model_game> const& corpus,
                  std::size_t const index,
                  motif_test::model_game const& after,
                  std::uint64_t const from) -> tree_patch_run
{
    auto builder = tree_patch_builder {patch_identity()};
    auto const document = static_cast<std::uint32_t>(index + 1U);
    REQUIRE(builder.add_metadata_edit(tree_state_of(corpus[index], document), tree_state_of(after, document)).has_value());
    auto run = builder.build(from, from + 1U);
    REQUIRE(run.has_value());
    return std::move(*run);
}

auto rebuilt_nodes(std::filesystem::path const& scratch, std::vector<motif_test::model_game> const& corpus) -> std::vector<raw_node>
{
    write_tree_checkpoint(scratch, corpus);
    auto opened = opening_tree_index::open(scratch);
    REQUIRE(opened.has_value());
    return collect_nodes(*opened);
}

}  // namespace

TEST_CASE("a metadata run merged over the checkpoint equals a rebuild of the edited corpus", "[motif-db][pr6a][merge]")
{
    constexpr auto corpus_seed = 31U;
    constexpr auto corpus_size = 12U;
    constexpr auto negative_black_rating = -7;
    small_tree const tree {motif_test::make_corpus(corpus_seed, corpus_size)};
    auto edited = tree.corpus;
    edited[2].result = "0-1";  // the late-return game: 1-0 -> 0-1 and ratings change
    edited[2].white_elo = std::nullopt;
    edited[2].black_elo = negative_black_rating;
    auto const run = metadata_run(tree.corpus, 2, edited[2], 0);
    CHECK(run.operations == tree_patch_operation_metadata);
    CHECK(run.source_game_delta == 0);
    CHECK(std::ranges::all_of(run.nodes, [](tree_patch_node const& node) -> bool { return node.game_count_delta == 0; }));

    auto composite = open_composite(tree, {run});
    REQUIRE(composite.has_value());
    CHECK(composite->source_game_count() == tree.corpus.size());
    auto const fresh = rebuilt_nodes(tree.dir.path / "fresh.idx", edited);
    CHECK(collect_nodes(*composite) == fresh);

    // Point lookups agree with the streaming merge, including absent roots.
    for (auto const& node : fresh) {
        auto const direct = composite->query_raw_node(zobrist_hash {node.root_hash});
        REQUIRE(direct.has_value());
        REQUIRE(direct->has_value());
        CHECK(motif_test::require_value(*direct) == node);
        CHECK(composite->game_count(zobrist_hash {node.root_hash}).value_or(0U) == node.game_count);
    }
    CHECK_FALSE(composite->query_raw_node(zobrist_hash {0xDEADBEEF}).value().has_value());
    CHECK(composite->game_count(zobrist_hash {0xDEADBEEF}).value_or(1U) == 0U);
    CHECK(composite->is_complete(zobrist_hash {opening_tree_start_hash()}));
    CHECK_FALSE(composite->is_complete(zobrist_hash {0xDEADBEEF}));

    // Presentation is the one raw -> row conversion, so it matches a fresh checkpoint's.
    auto fresh_index = opening_tree_index::open(tree.dir.path / "fresh.idx");
    REQUIRE(fresh_index.has_value());
    auto const expected_rows = fresh_index->query_opening_stats(zobrist_hash {opening_tree_start_hash()});
    auto const merged_rows = composite->query_opening_stats(zobrist_hash {opening_tree_start_hash()});
    REQUIRE(expected_rows.has_value());
    REQUIRE(merged_rows.has_value());
    REQUIRE(merged_rows->size() == expected_rows->size());
    for (std::size_t index = 0; index < merged_rows->size(); ++index) {
        auto const& left = (*merged_rows)[index];
        auto const& right = (*expected_rows)[index];
        CHECK(left.cont_encoded_move == right.cont_encoded_move);
        CHECK(left.frequency == right.frequency);
        CHECK(left.white_wins == right.white_wins);
        CHECK(left.draws == right.draws);
        CHECK(left.black_wins == right.black_wins);
        CHECK(left.avg_white_elo == right.avg_white_elo);
        CHECK(left.avg_black_elo == right.avg_black_elo);
        CHECK(left.elo_weighted_score == right.elo_weighted_score);
        CHECK(left.transposition_frequency == right.transposition_frequency);
    }
}

TEST_CASE("an empty certificate run merges as a no-op", "[motif-db][pr6a][merge]")
{
    constexpr auto corpus_seed = 32U;
    constexpr auto corpus_size = 8U;
    constexpr auto draw_from_seq = std::uint64_t {4};
    small_tree tree {motif_test::make_corpus(corpus_seed, corpus_size)};
    // draw <-> '*' share the encoded result and an unchanged rating set: no
    // numerical delta, but the epoch is still certified by an (empty) run.
    auto draw = tree.corpus[1];
    draw.result = "1/2-1/2";
    auto unknown = draw;
    unknown.result = "*";
    auto builder = tree_patch_builder {patch_identity()};
    REQUIRE(builder.add_metadata_edit(tree_state_of(draw, 2), tree_state_of(unknown, 2)).has_value());
    auto run = builder.build(draw_from_seq, draw_from_seq + 1U);
    REQUIRE(run.has_value());
    CHECK(run->nodes.empty());
    CHECK(run->edges.empty());
    auto composite = open_composite(tree, {*run});
    REQUIRE(composite.has_value());
    CHECK(collect_nodes(*composite) == rebuilt_nodes(tree.dir.path / "same.idx", tree.corpus));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("membership deltas create absent-base nodes, retract edges to zero and invalidate extrema", "[motif-db][pr6a][merge]")
{
    constexpr auto corpus_seed = 33U;
    constexpr auto corpus_size = 10U;
    constexpr auto novel_white_rating = -40;
    constexpr auto novel_black_rating = 2200;
    small_tree tree {motif_test::make_corpus(corpus_seed, corpus_size)};
    auto const base_count = tree.corpus.size();

    // Insert a game with a novel opening (absent-base nodes and edges), and remove
    // the zero-move game and a game with a unique line.
    auto novel = motif_test::model_game {.moves = motif_test::uci_moves({"b1c3", "b8c6", "c3b5", "a7a6", "b5c3", "c6b8", "c3b1"}),
                                         .result = "1-0",
                                         .white_elo = novel_white_rating,
                                         .black_elo = novel_black_rating};
    auto builder = tree_patch_builder {patch_identity()};
    auto const new_id = static_cast<std::uint32_t>(base_count + 1U);
    REQUIRE(builder.add_game_insertion(tree_state_of(novel, new_id)).has_value());
    REQUIRE(builder.add_game_removal(tree_state_of(tree.corpus[0], 1)).has_value());  // zero moves
    REQUIRE(builder.add_game_removal(tree_state_of(tree.corpus[1], 2)).has_value());  // fool's mate: terminal root
    auto run = builder.build(0, 1);
    REQUIRE(run.has_value());
    CHECK(run->operations == tree_patch_operation_membership);
    CHECK(run->source_game_delta == -1);  // +1 -2

    // Membership needs the postings child-frequency mode.
    CHECK_FALSE(open_composite(tree, {*run}, tree_child_frequency_mode::checkpoint).has_value());
    auto composite = open_composite(tree, {*run}, tree_child_frequency_mode::postings);
    INFO((composite.has_value() ? std::string {} : composite.error().message));
    REQUIRE(composite.has_value());
    CHECK(composite->source_game_count() == base_count - 1U);

    // Expected corpus: games 3..N plus the novel one (ids differ from a rebuild, so
    // compare everything but sample bounds, which depend on ids).
    auto remaining = std::vector<motif_test::model_game> {tree.corpus.begin() + 2, tree.corpus.end()};
    remaining.push_back(novel);
    auto const fresh = rebuilt_nodes(tree.dir.path / "fresh.idx", remaining);
    auto const merged = collect_nodes(*composite);
    REQUIRE(merged.size() == fresh.size());
    auto invalid_edges = std::size_t {0};
    auto valid_edges = std::size_t {0};
    for (std::size_t index = 0; index < merged.size(); ++index) {
        CHECK(merged[index].root_hash == fresh[index].root_hash);
        CHECK(merged[index].game_count == fresh[index].game_count);
        CHECK(merged[index].complete == fresh[index].complete);
        REQUIRE(merged[index].edges.size() == fresh[index].edges.size());
        for (std::size_t edge_index = 0; edge_index < merged[index].edges.size(); ++edge_index) {
            auto const& got = merged[index].edges[edge_index];
            auto const& want = fresh[index].edges[edge_index];
            CHECK(got.key == want.key);
            CHECK(got.stats == want.stats);
            if (got.min_root_ply_valid) {
                CHECK(got.min_root_ply == want.min_root_ply);
                ++valid_edges;
            } else {
                ++invalid_edges;
            }
        }
    }
    CHECK(invalid_edges > 0U);  // retractions invalidated some extrema
    CHECK(valid_edges > 0U);  // inserted-only edges carry exact candidates

    // The start node counts every remaining game and is complete.
    auto const start = composite->query_raw_node(zobrist_hash {opening_tree_start_hash()});
    REQUIRE(start.has_value());
    REQUIRE(start->has_value());
    auto const& start_node = motif_test::require_value(*start);
    CHECK(start_node.game_count == base_count - 1U);
    CHECK(start_node.complete);

    // Postings mode resolves child counts through the caller; invalid extrema cannot
    // be presented (the manager then answers from exact postings).
    constexpr auto oversized_child_frequency = std::uint32_t {1000};
    auto resolver = tree_child_frequency_resolver {[oversized_child_frequency](zobrist_hash) -> result<std::uint32_t>
                                                   { return oversized_child_frequency; }};
    auto const rows = composite->query_opening_stats(zobrist_hash {opening_tree_start_hash()}, resolver);
    // Some start edge lost a game, so at least one edge is not presentable.
    bool any_invalid = false;
    for (auto const& edge : start_node.edges) {
        any_invalid = any_invalid || !edge.min_root_ply_valid || !edge.sample_bounds_valid;
    }
    CHECK(rows.has_value() == !any_invalid);
    CHECK_FALSE(composite->query_opening_stats(zobrist_hash {opening_tree_start_hash()}).has_value());  // resolver required
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("any corrupt or inconsistent run fails the whole composite at open", "[motif-db][pr6a][merge]")
{
    constexpr auto corpus_seed = 34U;
    constexpr auto corpus_size = 10U;
    constexpr auto raised_rating = 2100;
    small_tree tree {motif_test::make_corpus(corpus_seed, corpus_size)};
    auto edited = tree.corpus[4];
    edited.result = edited.result == "1-0" ? "0-1" : "1-0";
    edited.white_elo = raised_rating;
    auto const good_run = metadata_run(tree.corpus, 4, edited, 0);
    REQUIRE(open_composite(tree, {good_run}).has_value());

    auto const mode_postings = tree_child_frequency_mode::postings;
    auto const rejects = [&](std::vector<tree_patch_run> runs,
                             tree_child_frequency_mode const mode = tree_child_frequency_mode::postings) -> bool
    { return !open_composite(tree, std::move(runs), mode).has_value(); };

    // A game that is not in the corpus: a metadata patch on edges that do not
    // exist leaves numerical residue on zero-frequency edges; removing it drives
    // counts negative.
    constexpr auto stranger_white_rating = 10;
    constexpr auto stranger_black_rating = 20;
    auto stranger = motif_test::model_game {.moves = motif_test::uci_moves({"a2a3", "a7a6", "a3a4"}),
                                            .result = "1-0",
                                            .white_elo = stranger_white_rating,
                                            .black_elo = stranger_black_rating};
    auto stranger_after = stranger;
    stranger_after.result = "0-1";
    CHECK(rejects({metadata_run({stranger}, 0, stranger_after, 0)}, tree_child_frequency_mode::checkpoint));
    {
        auto builder = tree_patch_builder {patch_identity()};
        REQUIRE(builder.add_game_removal(tree_state_of(stranger, 99)).has_value());
        auto run = builder.build(0, 1);
        REQUIRE(run.has_value());
        CHECK(rejects({*run}));
    }

    // Double retraction of a real game across two runs.
    {
        auto first = tree_patch_builder {patch_identity()};
        REQUIRE(first.add_game_removal(tree_state_of(tree.corpus[5], 6)).has_value());
        auto second = tree_patch_builder {patch_identity()};
        REQUIRE(second.add_game_removal(tree_state_of(tree.corpus[5], 6)).has_value());
        auto const first_run = first.build(0, 1);
        auto const second_run = second.build(1, 2);
        REQUIRE(first_run.has_value());
        REQUIRE(second_run.has_value());
        CHECK(open_composite(tree, {*first_run}, mode_postings).has_value());  // one retraction is fine
        CHECK(rejects({*first_run, *second_run}));
    }

    // Hand-forged deltas against the start node's first edge (membership form).
    auto base_nodes = [&]() -> std::vector<raw_node>
    {
        auto opened = opening_tree_index::open(tree.checkpoint);
        REQUIRE(opened.has_value());
        return collect_nodes(*opened);
    }();
    auto const start_index = static_cast<std::size_t>(std::ranges::find_if(base_nodes, [](raw_node const& n) -> bool { return n.complete; })
                                                      - base_nodes.begin());
    REQUIRE(start_index < base_nodes.size());
    auto const& start = base_nodes[start_index];
    REQUIRE_FALSE(start.edges.empty());
    auto const forged = [&](auto const& tweak) -> tree_patch_run
    {
        auto run = tree_patch_run {.identity = patch_identity(),
                                   .operations = tree_patch_operation_membership,
                                   .from_exclusive_seq = 0,
                                   .to_inclusive_seq = 1,
                                   .nodes = {},
                                   .edges = {}};
        run.nodes.push_back(tree_patch_node {.root_hash = start.root_hash, .game_count_delta = 0, .edge_begin = 0, .edge_count = 1});
        run.edges.push_back(edge_of(start.edges[0].key.encoded_move, start.edges[0].key.child_hash, raw_edge_stats {}));
        tweak(run);
        return run;
    };
    // Control: a neutral forged run (zero delta, a validity-mask bit) is accepted.
    CHECK_FALSE(
        rejects({forged([](tree_patch_run& target) -> void { target.edges[0].validity_mask = tree_patch_mask_invalidate_samples; })}));
    // frequency above the node's game count (W/D/B stay consistent).
    CHECK(rejects({forged(
        [](tree_patch_run& target) -> void
        {
            target.edges[0].delta.frequency = 1000;
            target.edges[0].delta.draws = 1000;
        })}));
    // negative final tallies.
    CHECK(rejects({forged(
        [&](tree_patch_run& target) -> void
        {
            target.edges[0].delta.white_wins = -(start.edges[0].stats.white_wins + 1);
            target.edges[0].delta.draws = start.edges[0].stats.white_wins + 1;
        })}));
    // rating sum beyond its population bound.
    CHECK(rejects({forged([](tree_patch_run& target) -> void { target.edges[0].delta.white_sum = 1'000'000'000; })}));
    // rating population above the frequency.
    CHECK(rejects({forged([](tree_patch_run& target) -> void { target.edges[0].delta.black_count = 1'000; })}));
    // weighted sums beyond their bound.
    CHECK(rejects({forged([](tree_patch_run& target) -> void { target.edges[0].delta.weight2 = 1'000'000'000; })}));
    // a node with no games that keeps live edges.
    CHECK(rejects({forged(
        [&](tree_patch_run& target) -> void
        {
            target.nodes[0].game_count_delta = -static_cast<std::int64_t>(start.game_count);
            target.source_game_delta = -static_cast<std::int64_t>(start.game_count);
        })}));
    // node count above the source game count.
    CHECK(rejects({forged([](tree_patch_run& target) -> void { target.nodes[0].game_count_delta = 50; })}));
    // the start node no longer counts every game.
    CHECK(rejects({forged([](tree_patch_run& target) -> void { target.source_game_delta = -1; })}));
    // an edge whose exact minimum ply is beyond the horizon on a non-complete node:
    // candidates on a fresh edge of a shallow node.
    {
        auto const non_start = std::ranges::find_if(base_nodes, [](raw_node const& n) -> bool { return !n.complete && !n.edges.empty(); });
        REQUIRE(non_start != base_nodes.end());
        auto run = tree_patch_run {.identity = patch_identity(),
                                   .operations = tree_patch_operation_membership,
                                   .from_exclusive_seq = 0,
                                   .to_inclusive_seq = 1,
                                   .nodes = {},
                                   .edges = {}};
        run.nodes.push_back(tree_patch_node {.root_hash = non_start->root_hash, .game_count_delta = 0, .edge_begin = 0, .edge_count = 1});
        constexpr auto novel_move = std::uint16_t {0x7777};
        constexpr auto novel_child = std::uint64_t {0x8888};
        auto edge = edge_of(novel_move,
                            novel_child,
                            raw_edge_stats {.frequency = 1, .white_wins = 1, .white_sum = 0, .white_count = 0},
                            tree_patch_mask_candidates);
        edge.candidate_min_root_ply = beyond_horizon;  // beyond max_root_ply, node not complete
        run.edges.push_back(edge);
        CHECK(rejects({run}));
    }

    // Chain shape: gap, overlap, identity and semantics disagreements.
    constexpr auto edited_index = std::size_t {6};
    auto second_edit = tree.corpus[edited_index];
    second_edit.result = second_edit.result == "1-0" ? "0-1" : "1-0";
    auto const second_run = metadata_run(tree.corpus, edited_index, second_edit, 1);
    CHECK_FALSE(rejects({good_run, second_run}, tree_child_frequency_mode::checkpoint));
    auto gap = metadata_run(tree.corpus, edited_index, second_edit, 2);
    CHECK(rejects({good_run, gap}, tree_child_frequency_mode::checkpoint));
    CHECK(rejects({good_run, good_run}, tree_child_frequency_mode::checkpoint));  // overlapping
    auto foreign = good_run;
    foreign.identity.max_root_ply = foreign_horizon;
    CHECK(rejects({foreign}, tree_child_frequency_mode::checkpoint));
    foreign = good_run;
    foreign.identity.canonical_start_hash ^= 1U;
    CHECK(rejects({foreign}, tree_child_frequency_mode::checkpoint));
    foreign = good_run;
    foreign.identity.index_semantics = 2;
    CHECK(rejects({foreign}, tree_child_frequency_mode::checkpoint));

    // Decoded memory is bounded.
    CHECK_FALSE(open_composite(tree, {good_run}, tree_child_frequency_mode::checkpoint, 8).has_value());
}

// ── Streaming fold ───────────────────────────────────────────────────────────

namespace
{

auto share(std::vector<tree_patch_run> runs) -> std::vector<tree_patch_run_ptr>
{
    auto shared = std::vector<tree_patch_run_ptr> {};
    for (auto& run : runs) {
        shared.push_back(std::make_shared<tree_patch_run const>(std::move(run)));
    }
    return shared;
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("a folded checkpoint equals the uncompacted chain and a rebuild", "[motif-db][pr6a][fold]")
{
    constexpr auto corpus_seed = 35U;
    constexpr auto corpus_size = 20U;
    constexpr auto step_count = 6U;
    small_tree const tree {motif_test::make_corpus(corpus_seed, corpus_size)};
    auto corpus = tree.corpus;
    auto rng = std::mt19937_64 {corpus_seed};  // NOLINT(cert-msc51-cpp,cert-msc32-c) -- a fixed seed keeps the fixtures reproducible.
    auto runs = std::vector<tree_patch_run> {};
    for (auto step = std::uint64_t {0}; step < step_count; ++step) {
        auto const index = std::uniform_int_distribution<std::size_t> {0, corpus.size() - 1}(rng);
        auto after = corpus[index];
        after.result = motif_test::pick_result(rng);
        after.white_elo = motif_test::pick_elo(rng);
        after.black_elo = motif_test::pick_elo(rng);
        runs.push_back(metadata_run(corpus, index, after, step));
        corpus[index] = after;
    }
    auto composite = open_composite(tree, runs);
    REQUIRE(composite.has_value());
    auto const uncompacted = collect_nodes(*composite);

    auto base = opening_tree_index::open(tree.checkpoint);
    REQUIRE(base.has_value());
    auto const shared = share(runs);
    auto const folded_path = tree.dir.path / "folded.idx";
    auto const folded = fold_tree_chain(*base, shared, tree_child_frequency_mode::checkpoint, folded_path);
    REQUIRE(folded.has_value());
    CHECK(folded->node_count == uncompacted.size());
    CHECK(folded->source_game_count == corpus.size());
    CHECK(folded->child_frequency_mode == tree_child_frequency_mode::checkpoint);
    auto folded_index = opening_tree_index::open(folded_path);
    REQUIRE(folded_index.has_value());
    CHECK(folded_index->checkpoint_format_version() == 6U);
    auto const compacted = collect_nodes(*folded_index);
    CHECK(compacted == uncompacted);
    CHECK(compacted == rebuilt_nodes(tree.dir.path / "fresh.idx", corpus));
    CHECK_FALSE(std::filesystem::exists(folded_path.string() + ".tmp"));

    // Folding is idempotent, and a fold of zero runs is a faithful copy.
    auto const again = fold_tree_chain(*folded_index, {}, tree_child_frequency_mode::checkpoint, tree.dir.path / "again.idx");
    REQUIRE(again.has_value());
    CHECK(collect_nodes(*opening_tree_index::open(tree.dir.path / "again.idx")) == compacted);

    // The manager hands in an empty reserved staging file; the fold replaces it.
    auto const reserved = tree.dir.path / "reserved.idx.building";
    {
        std::ofstream const touch {reserved};
    }
    REQUIRE(fold_tree_chain(*base, shared, tree_child_frequency_mode::checkpoint, reserved).has_value());
    CHECK(collect_nodes(*opening_tree_index::open(reserved)) == compacted);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("folding keeps invalidation flags sticky, deletes zero-frequency edges and empty nodes", "[motif-db][pr6a][fold]")
{
    constexpr auto corpus_seed = 36U;
    constexpr auto corpus_size = 6U;
    small_tree tree {motif_test::make_corpus(corpus_seed, corpus_size)};
    auto builder = tree_patch_builder {patch_identity()};
    // Remove every game: all nodes and edges must disappear.
    for (std::size_t index = 0; index < tree.corpus.size(); ++index) {
        REQUIRE(builder.add_game_removal(tree_state_of(tree.corpus[index], static_cast<std::uint32_t>(index + 1U))).has_value());
    }
    auto const everything = builder.build(0, 1);
    REQUIRE(everything.has_value());
    auto base = opening_tree_index::open(tree.checkpoint);
    REQUIRE(base.has_value());
    auto folded = fold_tree_chain(*base, share({*everything}), tree_child_frequency_mode::postings, tree.dir.path / "empty.idx");
    REQUIRE(folded.has_value());
    CHECK(folded->node_count == 0U);
    CHECK(folded->edge_count == 0U);
    CHECK(folded->source_game_count == 0U);
    auto empty = opening_tree_index::open(tree.dir.path / "empty.idx");
    REQUIRE(empty.has_value());
    CHECK(empty->node_count() == 0U);
    // An empty corpus has no positive-count root, and the composite agrees.
    auto const composite = open_composite(tree, {*everything}, tree_child_frequency_mode::postings);
    REQUIRE(composite.has_value());
    CHECK(composite->source_game_count() == 0U);
    CHECK(collect_nodes(*composite).empty());

    // Retract one game: extrema of the edges it shared become inexact and stay so
    // through a fold and a reopen.
    auto single = tree_patch_builder {patch_identity()};
    REQUIRE(single.add_game_removal(tree_state_of(tree.corpus[4], 5)).has_value());
    auto const one_removal = single.build(0, 1);
    REQUIRE(one_removal.has_value());
    auto const merged_before = collect_nodes(*open_composite(tree, {*one_removal}, tree_child_frequency_mode::postings));
    auto const bad_flags = [](std::vector<raw_node> const& nodes) -> std::size_t
    {
        auto count = std::size_t {0};
        for (auto const& node : nodes) {
            count += static_cast<std::size_t>(std::ranges::count_if(
                node.edges, [](raw_edge const& edge) -> bool { return !edge.min_root_ply_valid && !edge.sample_bounds_valid; }));
        }
        return count;
    };
    REQUIRE(bad_flags(merged_before) > 0U);
    REQUIRE(fold_tree_chain(*base, share({*one_removal}), tree_child_frequency_mode::postings, tree.dir.path / "one.idx").has_value());
    auto const after_fold = collect_nodes(*opening_tree_index::open(tree.dir.path / "one.idx"));
    // A postings-mode fold drops the (stale) cached child counts rather than
    // certify them; everything else is carried through exactly.
    auto expected_after = merged_before;
    for (auto& node : expected_after) {
        for (auto& edge : node.edges) {
            edge.child_frequency.reset();
        }
    }
    CHECK(after_fold == expected_after);
    CHECK(bad_flags(after_fold) == bad_flags(merged_before));
}

TEST_CASE("a failed fold leaves no output and rejects a foreign chain", "[motif-db][pr6a][fold]")
{
    constexpr auto corpus_seed = 37U;
    constexpr auto corpus_size = 6U;
    small_tree tree {motif_test::make_corpus(corpus_seed, corpus_size)};
    auto edited = tree.corpus[3];
    edited.result = "0-1";
    auto run = metadata_run(tree.corpus, 3, edited, 0);
    run.identity.max_root_ply = foreign_horizon;  // belongs to another checkpoint
    auto base = opening_tree_index::open(tree.checkpoint);
    REQUIRE(base.has_value());
    auto const out = tree.dir.path / "never.idx";
    CHECK_FALSE(fold_tree_chain(*base, share({run}), tree_child_frequency_mode::checkpoint, out).has_value());
    CHECK_FALSE(std::filesystem::exists(out));
    CHECK_FALSE(std::filesystem::exists(out.string() + ".tmp"));

    // A chain that merges into an invalid node fails mid-stream and cleans up.
    auto builder = tree_patch_builder {patch_identity()};
    REQUIRE(builder.add_game_removal(tree_state_of(tree.corpus[1], 2)).has_value());
    REQUIRE(builder.add_game_removal(tree_state_of(tree.corpus[1], 2)).has_value());  // twice
    auto const double_removal = builder.build(0, 1);
    REQUIRE(double_removal.has_value());
    CHECK_FALSE(fold_tree_chain(*base, share({*double_removal}), tree_child_frequency_mode::postings, out).has_value());
    CHECK_FALSE(std::filesystem::exists(out));
    CHECK_FALSE(std::filesystem::exists(out.string() + ".tmp"));
}

// ── Admission caps ───────────────────────────────────────────────────────────

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("chain growth is classified against the soft and hard thresholds", "[motif-db][pr6a][fold]")
{
    auto const limits = patch_chain_limits {};
    constexpr auto run_encoded_bytes = std::uint64_t {100};
    constexpr auto run_decoded_bytes = std::uint64_t {1'000};
    auto const one = patch_chain_usage {.runs = 1, .encoded_bytes = run_encoded_bytes, .decoded_bytes = run_decoded_bytes};
    auto const usage = [](std::size_t const runs) -> patch_chain_usage
    { return {.runs = runs, .encoded_bytes = runs * run_encoded_bytes, .decoded_bytes = runs * run_decoded_bytes}; };

    CHECK(classify_chain_growth(limits, usage(0), one) == patch_chain_admission::admit);
    CHECK(classify_chain_growth(limits, usage(1), one) == patch_chain_admission::admit);
    CHECK(classify_chain_growth(limits, usage(7), one) == patch_chain_admission::admit);  // reaches the soft cap of 8
    CHECK(classify_chain_growth(limits, usage(8), one) == patch_chain_admission::admit_and_recommend_fold);
    CHECK(classify_chain_growth(limits, usage(15), one) == patch_chain_admission::admit_and_recommend_fold);  // 16 = hard cap, allowed
    CHECK(classify_chain_growth(limits, usage(16), one) == patch_chain_admission::fold_required);

    // Bytes: soft 4 MiB, hard 16 MiB; decoded memory hard 64 MiB.
    auto const mib = std::uint64_t {1} << 20U;
    CHECK(classify_chain_growth(
              limits, {.runs = 1, .encoded_bytes = 4 * mib, .decoded_bytes = 0}, {.runs = 1, .encoded_bytes = 1, .decoded_bytes = 0})
          == patch_chain_admission::admit_and_recommend_fold);
    CHECK(classify_chain_growth(
              limits, {.runs = 1, .encoded_bytes = 16 * mib, .decoded_bytes = 0}, {.runs = 1, .encoded_bytes = 1, .decoded_bytes = 0})
          == patch_chain_admission::fold_required);
    CHECK(classify_chain_growth(
              limits, {.runs = 1, .encoded_bytes = 0, .decoded_bytes = 64 * mib}, {.runs = 1, .encoded_bytes = 0, .decoded_bytes = 1})
          == patch_chain_admission::fold_required);
    // One overlarge candidate on an empty chain folds instead of being appended.
    CHECK(classify_chain_growth(limits, usage(0), {.runs = 1, .encoded_bytes = 20 * mib, .decoded_bytes = 0})
          == patch_chain_admission::fold_required);
    CHECK(classify_chain_growth(limits, usage(0), {.runs = 1, .encoded_bytes = 1, .decoded_bytes = 100 * mib})
          == patch_chain_admission::fold_required);

    auto const tiny = patch_chain_limits {
        .soft_runs = 1, .soft_encoded_bytes = 1'000, .hard_runs = 2, .hard_encoded_bytes = 2'000, .hard_decoded_bytes = 4'000};
    CHECK(classify_chain_growth(tiny, usage(0), one) == patch_chain_admission::admit);
    CHECK(classify_chain_growth(tiny, usage(1), one) == patch_chain_admission::admit_and_recommend_fold);
    CHECK(classify_chain_growth(tiny, usage(2), one) == patch_chain_admission::fold_required);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("folding at the hard cap keeps the chain bounded and equal to a rebuild", "[motif-db][pr6a][fold][property]")
{
    constexpr auto corpus_seed = 38U;
    constexpr auto corpus_size = 18U;
    constexpr auto edit_count = 24;
    small_tree const tree {motif_test::make_corpus(corpus_seed, corpus_size)};
    auto corpus = tree.corpus;
    auto rng = std::mt19937_64 {corpus_seed};  // NOLINT(cert-msc51-cpp,cert-msc32-c) -- a fixed seed keeps the fixtures reproducible.
    auto const limits = patch_chain_limits {
        .soft_runs = 2, .soft_encoded_bytes = 1U << 20U, .hard_runs = 3, .hard_encoded_bytes = 1U << 20U, .hard_decoded_bytes = 1U << 20U};

    auto checkpoint_path = tree.checkpoint;
    auto runs = std::vector<tree_patch_run_ptr> {};
    auto sequence = std::uint64_t {0};
    auto folds = 0;
    auto max_runs_seen = std::size_t {0};
    for (auto step = 0; step < edit_count; ++step) {
        auto const index = std::uniform_int_distribution<std::size_t> {0, corpus.size() - 1}(rng);
        auto after = corpus[index];
        after.result = motif_test::pick_result(rng);
        after.white_elo = motif_test::pick_elo(rng);
        after.black_elo = motif_test::pick_elo(rng);
        auto pending = std::make_shared<tree_patch_run const>(metadata_run(corpus, index, after, sequence));
        ++sequence;
        corpus[index] = after;

        auto usage = patch_chain_usage {};
        for (auto const& run : runs) {
            usage.runs += 1;
            usage.encoded_bytes += run->encoded_bytes;
            usage.decoded_bytes += run->decoded_bytes();
        }
        auto const decision = classify_chain_growth(
            limits,
            usage,
            patch_chain_usage {.runs = 1, .encoded_bytes = pending->encoded_bytes, .decoded_bytes = pending->decoded_bytes()});
        if (decision == patch_chain_admission::fold_required) {
            // Fold the checkpoint, every active run AND the pending delta.
            auto base = opening_tree_index::open(checkpoint_path);
            REQUIRE(base.has_value());
            auto all = runs;
            all.push_back(pending);
            auto const out = tree.dir.path / ("fold_" + std::to_string(folds++) + ".idx");
            REQUIRE(fold_tree_chain(*base, all, tree_child_frequency_mode::checkpoint, out).has_value());
            checkpoint_path = out;
            runs.clear();
        } else {
            runs.push_back(pending);
        }
        max_runs_seen = std::max(max_runs_seen, runs.size());
        REQUIRE(runs.size() <= limits.hard_runs);  // never an oversized chain

        auto composite = composite_opening_tree::open(checkpoint_path, runs, tree_child_frequency_mode::checkpoint);
        REQUIRE(composite.has_value());
        CHECK(collect_nodes(*composite) == rebuilt_nodes(tree.dir.path / "fresh.idx", corpus));
    }
    CHECK(folds >= 5);
    CHECK(max_runs_seen == limits.hard_runs);
}

// ── Rebuild-equivalence properties ───────────────────────────────────────────

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("metadata edit sequences applied as signed runs equal a fresh rebuild", "[motif-db][pr6a][property][equivalence]")
{
    auto result_transitions = std::set<std::pair<std::string, std::string>> {};
    constexpr auto corpus_size = 22U;
    constexpr auto step_count = 60;
    constexpr auto first_compaction_step = 20;
    constexpr auto second_compaction_step = 45;
    constexpr auto max_probe_ply = std::size_t {24};
    auto elo_cleared = 0;
    auto elo_set = 0;
    auto elo_changed = 0;
    auto negative_seen = 0;
    for (auto const seed : {101ULL, 102ULL, 103ULL, 104ULL}) {
        small_tree const tree {motif_test::make_corpus(seed, corpus_size)};
        auto corpus = tree.corpus;
        auto rng = std::mt19937_64 {seed};
        auto checkpoint_path = tree.checkpoint;
        auto runs = std::vector<tree_patch_run_ptr> {};
        auto sequence = std::uint64_t {0};
        auto fold_count = 0;
        for (auto step = 0; step < step_count; ++step) {
            // The first 32 steps cover every ordered result transition honestly: an
            // even step first primes one game to the source result through a real
            // run, the odd step that follows moves it to the target.
            auto const systematic = step < 32;
            auto const pair = static_cast<std::size_t>(step) / 2U;
            auto const index = systematic ? pair % corpus.size() : std::uniform_int_distribution<std::size_t> {0, corpus.size() - 1}(rng);
            auto after = corpus[index];
            if (systematic) {
                after.result = std::string {motif_test::result_strings.at(step % 2 == 0 ? pair / 4U : pair % 4U)};
            } else {
                after.result = motif_test::pick_result(rng);
            }
            auto const& before = corpus[index];
            if (step % 2 == 1 || !systematic) {
                result_transitions.emplace(before.result, after.result);
            }
            auto const edit_white = rng() % 3U;
            if (edit_white == 0U) {
                after.white_elo = motif_test::pick_elo(rng);
            }
            if (rng() % 3U == 0U) {
                after.black_elo = motif_test::pick_elo(rng);
            }
            elo_cleared += (before.white_elo && !after.white_elo) || (before.black_elo && !after.black_elo) ? 1 : 0;
            elo_set += (!before.white_elo && after.white_elo) || (!before.black_elo && after.black_elo) ? 1 : 0;
            elo_changed += (before.white_elo && after.white_elo && before.white_elo != after.white_elo) ? 1 : 0;
            negative_seen += (after.white_elo && *after.white_elo < 0) || (after.black_elo && *after.black_elo < 0) ? 1 : 0;

            // The run is computed from the game's before/after contributions.
            auto pending = std::make_shared<tree_patch_run const>(metadata_run(corpus, index, after, sequence));
            ++sequence;
            corpus[index] = after;
            runs.push_back(pending);

            if (step == first_compaction_step || step == second_compaction_step) {
                // Compaction mid-stream: the new checkpoint replaces checkpoint + runs.
                auto base = opening_tree_index::open(checkpoint_path);
                REQUIRE(base.has_value());
                auto const out = tree.dir.path / ("fold_" + std::to_string(fold_count++) + ".idx");
                REQUIRE(fold_tree_chain(*base, runs, tree_child_frequency_mode::checkpoint, out).has_value());
                auto before_fold = composite_opening_tree::open(checkpoint_path, runs, tree_child_frequency_mode::checkpoint);
                REQUIRE(before_fold.has_value());
                auto folded_index = opening_tree_index::open(out);
                REQUIRE(folded_index.has_value());
                REQUIRE(collect_nodes(*before_fold) == collect_nodes(*folded_index));  // compacted == uncompacted
                checkpoint_path = out;
                runs.clear();
            }

            auto composite = composite_opening_tree::open(checkpoint_path, runs, tree_child_frequency_mode::checkpoint);
            REQUIRE(composite.has_value());
            auto const fresh = rebuilt_nodes(tree.dir.path / "fresh.idx", corpus);
            // Exact raw equality: stats, extrema, completeness, child counts.
            REQUIRE(collect_nodes(*composite) == fresh);

            // Point lookups on a few hashes along a random game.
            auto const probe = corpus[std::uniform_int_distribution<std::size_t> {0, corpus.size() - 1}(rng)];
            for (std::size_t ply = 0; ply <= probe.moves.size() && ply <= max_probe_ply; ply += 3) {
                auto const hash = zobrist_hash {motif_test::hash_after(probe.moves, ply)};
                auto const merged = composite->query_raw_node(hash);
                REQUIRE(merged.has_value());
                auto const want = std::ranges::find_if(fresh, [&](raw_node const& node) -> bool { return node.root_hash == hash.value; });
                if (want == fresh.end()) {
                    CHECK_FALSE(merged->has_value());
                } else {
                    REQUIRE(merged->has_value());
                    CHECK(**merged == *want);
                }
            }
        }
    }
    // All 16 ordered result transitions (including draw <-> '*', an encoded no-op)
    // and every rating transition kind occurred.
    CHECK(result_transitions.size() == 16U);
    CHECK(elo_cleared > 0);
    CHECK(elo_set > 0);
    CHECK(elo_changed > 0);
    CHECK(negative_seen > 0);
}

namespace
{

// Raw equality that ignores what a membership chain legitimately does not
// maintain: cached child counts, and extrema it has marked inexact.
auto equivalent_modulo_witnesses(std::vector<raw_node> const& merged, std::vector<raw_node> const& fresh) -> bool
{
    if (merged.size() != fresh.size()) {
        return false;
    }
    for (std::size_t index = 0; index < merged.size(); ++index) {
        auto const& left = merged[index];
        auto const& right = fresh[index];
        if (left.root_hash != right.root_hash || left.game_count != right.game_count || left.complete != right.complete
            || left.edges.size() != right.edges.size())
        {
            return false;
        }
        for (std::size_t edge = 0; edge < left.edges.size(); ++edge) {
            auto const& got = left.edges[edge];
            auto const& want = right.edges[edge];
            if (got.key != want.key || got.stats != want.stats || (got.min_root_ply_valid && got.min_root_ply != want.min_root_ply)
                || (got.sample_bounds_valid && (got.sample_min != want.sample_min || got.sample_max != want.sample_max)))
            {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("membership edit sequences applied as signed runs equal a fresh rebuild", "[motif-db][pr6a][property][equivalence]")
{
    constexpr auto corpus_size = 14U;
    constexpr auto step_count = 30;
    constexpr auto max_novel_plies = 12U;
    constexpr auto mid_fold_step = 14;
    for (auto const seed : {201ULL, 202ULL, 203ULL}) {
        small_tree tree {motif_test::make_corpus(seed, corpus_size)};
        // ids are preserved across removals, so track (id, game) pairs; the rebuilt
        // corpus renumbers, which only moves sample bounds -- and a replayed id
        // sequence keeps them comparable when we rebuild from the live pairs.
        auto live = std::vector<std::pair<std::uint32_t, motif_test::model_game>> {};
        for (std::size_t index = 0; index < tree.corpus.size(); ++index) {
            live.emplace_back(static_cast<std::uint32_t>(index + 1U), tree.corpus[index]);
        }
        auto next_id = static_cast<std::uint32_t>(live.size() + 1U);
        auto rng = std::mt19937_64 {seed};
        auto runs = std::vector<tree_patch_run_ptr> {};
        auto sequence = std::uint64_t {0};
        auto retractions = 0;
        for (auto step = 0; step < step_count; ++step) {
            auto builder = tree_patch_builder {patch_identity()};
            auto const operations = 1 + static_cast<int>(rng() % 3U);
            for (auto operation = 0; operation < operations; ++operation) {
                if (live.size() > 2 && rng() % 2U == 0U) {
                    auto const victim = std::uniform_int_distribution<std::size_t> {0, live.size() - 1}(rng);
                    REQUIRE(builder.add_game_removal(tree_state_of(live[victim].second, live[victim].first)).has_value());
                    live.erase(live.begin() + static_cast<std::ptrdiff_t>(victim));
                    ++retractions;
                } else {
                    auto novel = motif_test::model_game {.moves = motif_test::random_game_moves(rng, 1 + (rng() % max_novel_plies)),
                                                         .result = motif_test::pick_result(rng),
                                                         .white_elo = motif_test::pick_elo(rng),
                                                         .black_elo = motif_test::pick_elo(rng)};
                    REQUIRE(builder.add_game_insertion(tree_state_of(novel, next_id)).has_value());
                    live.emplace_back(next_id++, std::move(novel));
                }
            }
            auto run = builder.build(sequence, sequence + 1U);
            ++sequence;
            REQUIRE(run.has_value());
            runs.push_back(std::make_shared<tree_patch_run const>(std::move(*run)));

            auto composite = composite_opening_tree::open(tree.checkpoint, runs, tree_child_frequency_mode::postings);
            INFO((composite.has_value() ? std::string {} : composite.error().message));
            REQUIRE(composite.has_value());
            // Rebuild from the live (id, game) pairs with the same ids.
            auto builder_path = tree.dir.path / "fresh_live.idx";
            auto fresh_builder = opening_tree_index_builder {builder_path};
            for (auto const& [game_number, game] : live) {
                REQUIRE(fresh_builder
                            .accumulate(replay_game_record {.id = game_id {game_number},
                                                            .result = game.result,
                                                            .white_elo = game.white_elo,
                                                            .black_elo = game.black_elo,
                                                            .moves = game.moves})
                            .has_value());
            }
            REQUIRE(fresh_builder.finalize().has_value());
            auto const fresh = collect_nodes(*opening_tree_index::open(builder_path));
            REQUIRE(composite->source_game_count() == live.size());
            REQUIRE(equivalent_modulo_witnesses(collect_nodes(*composite), fresh));

            // A fold in the middle does not change the answer.
            if (step == mid_fold_step) {
                auto base = opening_tree_index::open(tree.checkpoint);
                REQUIRE(base.has_value());
                REQUIRE(
                    fold_tree_chain(*base, runs, tree_child_frequency_mode::postings, tree.dir.path / "membership_fold.idx").has_value());
                REQUIRE(
                    equivalent_modulo_witnesses(collect_nodes(*opening_tree_index::open(tree.dir.path / "membership_fold.idx")), fresh));
            }
        }
        CHECK(retractions > 0);
    }
}
