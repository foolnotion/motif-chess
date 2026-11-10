// PR 6a: signed codec, checked raw statistics, the shared per-game contribution
// enumerator, and the v5/v6 checkpoint formats.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "composite_test_support.hpp"
#include "motif/chess/chess.hpp"
#include "motif/db/database_manager.hpp"
#include "motif/db/game_store.hpp"
#include "motif/db/opening_tree_index.hpp"
#include "motif/db/opening_tree_stats.hpp"

namespace
{

using namespace motif::db;  // NOLINT(google-build-using-namespace) -- the file exercises most of motif::db; qualifying every name buries
                            // the assertions.
using motif_test::collect_nodes;
using motif_test::temp_dir;

constexpr auto i64_min = std::numeric_limits<std::int64_t>::min();
constexpr auto i64_max = std::numeric_limits<std::int64_t>::max();
// The horizon, root hash and varint run length most fixtures in this file use.
constexpr auto test_horizon = std::uint16_t {20};
constexpr auto sample_root_hash = std::uint64_t {0x1111};
// Nine continuation bytes: a ten-byte varint minus its final byte.
constexpr auto varint_prefix_bytes = std::size_t {9};
constexpr auto v5_format_version = std::uint32_t {5};
// The v5 writer stored half of every doubled weighted sum.
constexpr auto weight_scale = 2.0;
// Offset of the format version inside a v6 checkpoint header.
constexpr auto v6_version_offset = std::size_t {8};
constexpr auto u64_max = std::numeric_limits<std::uint64_t>::max();

auto encode_uleb(std::uint64_t const value) -> std::string
{
    auto out = std::string {};
    append_uleb128(out, value);
    return out;
}

auto decode_all(std::string const& bytes) -> std::optional<std::uint64_t>
{
    auto reader = byte_reader {bytes};
    auto const value = reader.read_uleb128();
    if (!value || !reader.at_end()) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("zigzag maps the int64 endpoints exactly", "[motif-db][pr6a][codec]")
{
    CHECK(zigzag_encode(i64_min) == u64_max);
    CHECK(zigzag_encode(i64_max) == u64_max - 1U);
    CHECK(zigzag_encode(0) == 0U);
    CHECK(zigzag_encode(-1) == 1U);
    CHECK(zigzag_encode(1) == 2U);
    CHECK(zigzag_encode(-2) == 3U);
    CHECK(zigzag_decode(u64_max) == i64_min);
    CHECK(zigzag_decode(u64_max - 1U) == i64_max);
    CHECK(zigzag_decode(1U) == -1);
    CHECK(zigzag_decode(0U) == 0);

    for (auto const value : {i64_min,
                             i64_min + 1,
                             std::int64_t {-65'537},
                             std::int64_t {-2},
                             std::int64_t {-1},
                             std::int64_t {0},
                             std::int64_t {1},
                             std::int64_t {2},
                             std::int64_t {65'536},
                             i64_max - 1,
                             i64_max})
    {
        CHECK(zigzag_decode(zigzag_encode(value)) == value);
        auto bytes = std::string {};
        append_zigzag(bytes, value);
        auto reader = byte_reader {bytes};
        auto const decoded = reader.read_zigzag();
        REQUIRE(decoded.has_value());
        CHECK(motif_test::require_value(decoded) == value);
        CHECK(reader.at_end());
    }
}

TEST_CASE("zigzag and uleb round-trip random values at every bit length", "[motif-db][pr6a][codec]")
{
    constexpr auto codec_seed = std::uint64_t {0xC0DEC};
    auto rng = std::mt19937_64 {codec_seed};  // NOLINT(cert-msc51-cpp,cert-msc32-c) -- a fixed seed keeps the fixtures reproducible.
    constexpr auto round_count = 4'000;
    for (auto round = 0; round < round_count; ++round) {
        auto const width = std::uniform_int_distribution<int> {0, 63}(rng);
        auto const mask = width == 0 ? std::uint64_t {0} : (u64_max >> static_cast<unsigned>(64 - width));
        auto const unsigned_value = rng() & mask;
        auto const encoded = encode_uleb(unsigned_value);
        CHECK(decode_all(encoded) == std::optional<std::uint64_t> {unsigned_value});
        // Minimal length: ceil(bits / 7) bytes, at least one.
        auto const bits = unsigned_value == 0U ? 1 : std::bit_width(unsigned_value);
        CHECK(encoded.size() == static_cast<std::size_t>((bits + 6) / 7));

        auto const signed_value = static_cast<std::int64_t>(rng());
        auto bytes = std::string {};
        append_zigzag(bytes, signed_value);
        auto reader = byte_reader {bytes};
        CHECK(reader.read_zigzag() == std::optional<std::int64_t> {signed_value});
    }
}

TEST_CASE("uleb128 decoding rejects overlong, nonminimal, overflowing and truncated input", "[motif-db][pr6a][codec]")
{
    CHECK(decode_all(std::string {'\x00'}) == std::optional<std::uint64_t> {0});
    CHECK(decode_all(std::string {'\x7f'}) == std::optional<std::uint64_t> {127});
    CHECK(decode_all(std::string {'\x80', '\x01'}) == std::optional<std::uint64_t> {128});

    // Non-minimal encodings of 0, 1, 127 and 128: a terminating zero group.
    CHECK_FALSE(decode_all(std::string {'\x80', '\x00'}).has_value());
    CHECK_FALSE(decode_all(std::string {'\x81', '\x00'}).has_value());
    CHECK_FALSE(decode_all(std::string {'\xff', '\x00'}).has_value());
    CHECK_FALSE(decode_all(std::string {'\x80', '\x81', '\x00'}).has_value());

    // UINT64_MAX is nine 0xFF bytes then 0x01; a tenth byte above 1 overflows.
    auto max_encoding = std::string(varint_prefix_bytes, '\xff');
    max_encoding.push_back('\x01');
    CHECK(decode_all(max_encoding) == std::optional<std::uint64_t> {u64_max});
    auto overflow = std::string(varint_prefix_bytes, '\xff');
    overflow.push_back('\x02');
    CHECK_FALSE(decode_all(overflow).has_value());
    auto overflow_high = std::string(varint_prefix_bytes, '\xff');
    overflow_high.push_back('\x7f');
    CHECK_FALSE(decode_all(overflow_high).has_value());

    // An eleventh byte (tenth with the continuation bit) cannot be 64-bit.
    auto too_long = std::string(varint_prefix_bytes, '\x80');
    too_long.push_back('\x81');
    too_long.push_back('\x00');
    CHECK_FALSE(decode_all(too_long).has_value());

    // Truncation: empty input and a dangling continuation bit.
    CHECK_FALSE(decode_all(std::string {}).has_value());
    CHECK_FALSE(decode_all(std::string {'\x80'}).has_value());
    CHECK_FALSE(decode_all(std::string {'\xff', '\xff'}).has_value());

    // The signed decoder shares every rejection (INT64_MIN needs the full tenth byte).
    auto signed_min = std::string {};
    append_zigzag(signed_min, i64_min);
    CHECK(signed_min == max_encoding);
    auto reader = byte_reader {overflow};
    CHECK_FALSE(reader.read_zigzag().has_value());
}

TEST_CASE("byte_reader reads little-endian fixed-width fields and refuses to run past the end", "[motif-db][pr6a][codec]")
{
    auto bytes = std::string {};
    // The bytes 0x01..0x0E in order: every fixed-width read must consume them little-endian.
    append_fixed_le(bytes,
                    std::uint16_t {0x0201});  // NOLINT(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers) -- bytes 01 02
    append_fixed_le(bytes,
                    std::uint32_t {0x06050403});  // NOLINT(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers) -- bytes 03..06
    append_fixed_le(
        bytes,
        std::uint64_t {0x0E0D0C0B0A090807ULL});  // NOLINT(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers) -- bytes 07..0E
    REQUIRE(bytes.size() == 14U);
    CHECK(bytes.front() == '\x01');
    auto reader = byte_reader {bytes};
    CHECK(reader.read_u16() == std::optional<std::uint16_t> {0x0201});
    CHECK(reader.read_u32() == std::optional<std::uint32_t> {0x06050403});
    CHECK(reader.read_u64() == std::optional<std::uint64_t> {0x0E0D0C0B0A090807ULL});
    CHECK(reader.at_end());
    CHECK_FALSE(reader.read_u8().has_value());
    CHECK_FALSE(reader.read_u16().has_value());

    auto short_reader = byte_reader {std::string_view {bytes}.substr(0, 3)};
    CHECK(short_reader.read_u16().has_value());
    CHECK_FALSE(short_reader.read_u32().has_value());
    CHECK(short_reader.remaining() == 1U);
}

TEST_CASE("checked int64 arithmetic rejects overflow in both directions", "[motif-db][pr6a][stats]")
{
    CHECK_FALSE(checked_add_i64(i64_max, 1).has_value());
    CHECK_FALSE(checked_add_i64(i64_min, -1).has_value());
    CHECK(checked_add_i64(i64_max, -1) == std::optional<std::int64_t> {i64_max - 1});
    CHECK(checked_add_i64(i64_min, 1) == std::optional<std::int64_t> {i64_min + 1});
    CHECK(checked_add_i64(i64_max, i64_min) == std::optional<std::int64_t> {-1});
    CHECK_FALSE(checked_sub_i64(i64_min, 1).has_value());
    CHECK_FALSE(checked_sub_i64(0, i64_min).has_value());
    CHECK_FALSE(checked_sub_i64(i64_max, -1).has_value());
    CHECK(checked_sub_i64(-1, i64_min) == std::optional<std::int64_t> {i64_max});
    CHECK(checked_sub_i64(i64_min, i64_min) == std::optional<std::int64_t> {0});

    auto big = raw_edge_stats {};
    big.weight2 = i64_max;
    auto one = raw_edge_stats {};
    one.weight2 = 1;
    CHECK_FALSE(add_stats(big, one).has_value());
    CHECK(add_stats(big, raw_edge_stats {}).has_value());
    auto low = raw_edge_stats {};
    low.numerator2 = i64_min;
    CHECK_FALSE(subtract_stats(raw_edge_stats {}, low).has_value());
}

TEST_CASE("per-game stats follow the design's contribution algebra", "[motif-db][pr6a][stats]")
{
    auto const white_win = make_game_stats(1, 2300, 2500);
    REQUIRE(white_win.has_value());
    CHECK(white_win->frequency == 1);
    CHECK(white_win->white_wins == 1);
    CHECK(white_win->draws == 0);
    CHECK(white_win->white_sum == 2300);
    CHECK(white_win->white_count == 1);
    CHECK(white_win->black_sum == 2500);
    CHECK(white_win->black_count == 1);
    CHECK(white_win->weight2 == 4800);
    CHECK(white_win->numerator2 == 4800);

    auto const draw = make_game_stats(0, 2300, 2500);
    auto const black_win = make_game_stats(-1, 2300, 2500);
    REQUIRE(draw.has_value());
    REQUIRE(black_win.has_value());
    CHECK(draw->draws == 1);
    CHECK(draw->numerator2 == 0);
    CHECK(draw->weight2 == 4800);
    CHECK(black_win->black_wins == 1);
    CHECK(black_win->numerator2 == -4800);

    // `1-0 -> 1/2-1/2` (design example): only the tally and numerator2 move.
    auto const result_delta = subtract_stats(*draw, *white_win);
    REQUIRE(result_delta.has_value());
    auto const& result_stats = motif_test::require_value(result_delta);
    CHECK(result_stats.white_wins == -1);
    CHECK(result_stats.draws == 1);
    CHECK(result_stats.numerator2 == -4800);
    CHECK(result_stats.weight2 == 0);
    CHECK(result_stats.white_sum == 0);
    CHECK(result_stats.frequency == 0);

    // White Elo 2300 -> 2400 for a white win.
    auto const raised = make_game_stats(1, 2400, 2500);
    auto const elo_delta = subtract_stats(*raised, *white_win);
    REQUIRE(elo_delta.has_value());
    auto const& elo_stats = motif_test::require_value(elo_delta);
    CHECK(elo_stats.white_sum == 100);
    CHECK(elo_stats.white_count == 0);
    CHECK(elo_stats.weight2 == 100);
    CHECK(elo_stats.numerator2 == 100);

    // White Elo 2300 -> absent for a white win.
    auto const cleared = make_game_stats(1, std::nullopt, 2500);
    auto const clear_delta = subtract_stats(*cleared, *white_win);
    REQUIRE(clear_delta.has_value());
    auto const& clear_stats = motif_test::require_value(clear_delta);
    CHECK(clear_stats.white_sum == -2300);
    CHECK(clear_stats.white_count == -1);
    CHECK(clear_stats.weight2 == -4800);
    CHECK(clear_stats.numerator2 == -4800);
    CHECK(clear_stats.black_sum == 0);
    CHECK(clear_stats.black_count == 0);

    // A present zero rating counts; an absent one does not.
    auto const zero = make_game_stats(1, 0, std::nullopt);
    REQUIRE(zero.has_value());
    CHECK(zero->white_count == 1);
    CHECK(zero->white_sum == 0);
    CHECK(zero->weight2 == 0);
    CHECK(zero->numerator2 == 0);

    // The whole signed-16-bit domain is accepted; one past either end is not.
    CHECK(make_game_stats(1, -32768, 32767).has_value());
    CHECK_FALSE(make_game_stats(1, 32768, 0).has_value());
    CHECK_FALSE(make_game_stats(1, 0, -32769).has_value());
    CHECK(encode_result_code("1-0") == 1);
    CHECK(encode_result_code("0-1") == -1);
    CHECK(encode_result_code("1/2-1/2") == 0);
    CHECK(encode_result_code("*") == 0);
    CHECK(encode_result_code("garbage") == 0);
}

TEST_CASE("the weighted score is defined only for a positive denominator, everywhere", "[motif-db][pr6a][stats]")
{
    CHECK_FALSE(weighted_score(5, 0).has_value());
    CHECK_FALSE(weighted_score(5, -4).has_value());
    CHECK_FALSE(weighted_score(-5, -1).has_value());
    CHECK(weighted_score(4800, 4800) == std::optional<double> {1.0});
    constexpr auto negative_half = -0.5;
    CHECK(weighted_score(-2400, 4800) == std::optional<double> {negative_half});
    CHECK_FALSE(average_elo(10, 0).has_value());
    constexpr auto negative_two_and_a_half = -2.5;
    CHECK(average_elo(-10, 4) == std::optional<double> {negative_two_and_a_half});
}

namespace
{

constexpr auto sample_edge_move = std::uint16_t {5};

auto valid_edge() -> raw_edge
{
    // The sample edge's field values only need to satisfy the merged-edge invariants for a 3-game node.
    return raw_edge {
        .key = {.encoded_move = sample_edge_move,
                .child_hash =
                    0x2222},  // NOLINT(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers) -- arbitrary move and child hash
        .stats = {.frequency = 2,
                  .white_wins = 1,
                  .draws = 1,
                  .black_wins = 0,
                  .white_sum = 10,  // NOLINT(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers) -- one rated game
                  .white_count = 1,
                  .black_sum = -3,
                  .black_count = 1,
                  .numerator2 = 0,
                  .weight2 = 7},  // NOLINT(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers) -- (white + black) rating pair
        .min_root_ply = 2,
        .min_root_ply_valid = true,
        .sample_min = 1,
        .sample_max = 2,
        .sample_bounds_valid = true,
        .child_frequency = 3};
}

constexpr auto context_of(std::uint32_t const count) -> merged_edge_context
{
    return merged_edge_context {.node_game_count = count, .max_root_ply = test_horizon, .node_complete = false};
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("validate_merged_edge accepts a good edge and rejects every invariant violation", "[motif-db][pr6a][stats]")
{
    CHECK(validate_merged_edge(valid_edge(), context_of(3)).has_value());

    auto const rejects = [](auto const& mutate, merged_edge_context const context = context_of(3)) -> bool
    {
        auto edge = valid_edge();
        mutate(edge);
        return !validate_merged_edge(edge, context).has_value();
    };
    CHECK(rejects([](raw_edge& edge) -> void { edge.stats.frequency = 0; }));
    CHECK(rejects([](raw_edge& edge) -> void { edge.stats.frequency = -1; }));
    CHECK(rejects([](raw_edge& edge) -> void { edge.stats.frequency = 4; }));  // above node count
    CHECK(rejects([](raw_edge& edge) -> void { edge.stats.frequency = 4'294'967'296LL; }));
    CHECK(rejects([](raw_edge& edge) -> void { edge.stats.draws = 0; }));  // W+D+B != frequency
    CHECK(rejects(
        [](raw_edge& edge) -> void
        {
            edge.stats.white_wins = -1;
            edge.stats.draws = 3;
        }));
    CHECK(rejects([](raw_edge& edge) -> void { edge.stats.white_count = 3; }));  // above frequency
    CHECK(rejects([](raw_edge& edge) -> void { edge.stats.black_count = -1; }));
    CHECK(rejects([](raw_edge& edge) -> void { edge.stats.white_sum = 32'768; }));  // above 32767 * count
    CHECK(rejects([](raw_edge& edge) -> void { edge.stats.black_sum = -32'769; }));
    CHECK(rejects([](raw_edge& edge) -> void { edge.stats.white_count = 0; }));  // zero count, nonzero sum
    CHECK(rejects([](raw_edge& edge) -> void { edge.stats.weight2 = 65'537; }));
    CHECK(rejects([](raw_edge& edge) -> void { edge.stats.numerator2 = -65'537; }));
    CHECK(rejects(
        [](raw_edge& edge) -> void
        {
            edge.stats.black_count = 0;
            edge.stats.black_sum = 0;
        }));  // weight without a pair
    CHECK(rejects([](raw_edge& edge) -> void { edge.min_root_ply = 21; }));
    CHECK(rejects([](raw_edge& edge) -> void { edge.sample_min = 0; }));
    CHECK(rejects([](raw_edge& edge) -> void { edge.sample_min = 3; }));  // above sample_max
    CHECK(rejects([](raw_edge& edge) -> void { edge.child_frequency = 1; }));  // below the edge frequency

    // Invalid extrema are representable and not rejected for lacking a witness.
    CHECK_FALSE(rejects(
        [](raw_edge& edge) -> void
        {
            edge.min_root_ply_valid = false;
            edge.min_root_ply = 0;
        }));
    CHECK_FALSE(rejects(
        [](raw_edge& edge) -> void
        {
            edge.sample_bounds_valid = false;
            edge.sample_min = 0;
            edge.sample_max = 0;
        }));
    CHECK_FALSE(rejects([](raw_edge& edge) -> void { edge.child_frequency = std::nullopt; }));
    // A complete node's edges may sit past the horizon.
    CHECK_FALSE(rejects([](raw_edge& edge) -> void { edge.min_root_ply = 4'000; },
                        merged_edge_context {.node_game_count = 3, .max_root_ply = 20, .node_complete = true}));
}

TEST_CASE("presentation conversion needs exact extrema and a resolved child count", "[motif-db][pr6a][stats]")
{
    auto edge = valid_edge();
    auto row = to_opening_stat_row(edge, 3);
    REQUIRE(row.has_value());
    CHECK(row->cont_encoded_move == 5U);
    CHECK(row->cont_hash.value == 0x2222U);
    CHECK(row->root_ply == 2U);
    CHECK(row->frequency == 2U);
    CHECK(row->transposition_frequency == 3U);
    CHECK(row->white_wins == 1U);
    CHECK(row->draws == 1U);
    CHECK(row->avg_white_elo == std::optional<double> {static_cast<double>(edge.stats.white_sum)});
    CHECK(row->avg_black_elo == std::optional<double> {static_cast<double>(edge.stats.black_sum)});
    CHECK(row->elo_weighted_score == std::optional<double> {0.0});
    CHECK(row->eco_sample_min.value == 1U);
    CHECK(row->eco_sample_max.value == 2U);

    CHECK_FALSE(to_opening_stat_row(edge, 1).has_value());  // child count below the frequency
    edge.min_root_ply_valid = false;
    CHECK_FALSE(to_opening_stat_row(edge, 3).has_value());
    edge = valid_edge();
    edge.sample_bounds_valid = false;
    CHECK_FALSE(to_opening_stat_row(edge, 3).has_value());
}

// ── The shared per-game contribution enumerator ──────────────────────────────

namespace
{

struct oracle_contribution
{
    std::set<std::uint64_t> roots;
    std::map<std::tuple<std::uint64_t, std::uint16_t, std::uint64_t>, std::uint16_t> edges;
    std::set<std::uint64_t> visited;
};

// An independent, deliberately naive restatement of the design's traversal rule.
auto oracle_contribution_of(std::vector<std::uint16_t> const& moves, std::uint16_t const horizon) -> oracle_contribution
{
    auto out = oracle_contribution {};
    auto board = motif::chess::board {};
    auto const start = board.hash();
    out.roots.insert(start);
    out.visited.insert(start);
    for (std::size_t ply = 0; ply < moves.size(); ++ply) {
        auto const root = board.hash();
        motif::chess::apply_encoded_move(board, moves[ply]);
        auto const child = board.hash();
        out.visited.insert(child);
        if (ply <= horizon || root == start) {
            auto const key = std::tuple {root, moves[ply], child};
            auto const found = out.edges.find(key);
            if (found == out.edges.end() || ply < found->second) {
                out.edges[key] = static_cast<std::uint16_t>(ply);
            }
        }
        if (ply + 1U <= horizon) {
            out.roots.insert(child);
        }
    }
    return out;
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("the contribution enumerator matches an independent oracle on random and structured games", "[motif-db][pr6a][tree][property]")
{
    for (auto const seed : {1ULL, 2ULL, 3ULL}) {
        auto const corpus = motif_test::make_corpus(seed, 40);
        for (auto const horizon : {std::uint16_t {0}, std::uint16_t {2}, std::uint16_t {20}}) {
            auto enumerator = tree_contribution_enumerator {horizon, motif_test::start_hash(), /*collect_visited=*/true};
            for (auto const& game : corpus) {
                REQUIRE(enumerator.enumerate(game.moves).has_value());
                auto const expected = oracle_contribution_of(game.moves, horizon);

                auto roots = std::set<std::uint64_t> {enumerator.roots().begin(), enumerator.roots().end()};
                CHECK(roots.size() == enumerator.roots().size());  // already deduplicated
                CHECK(roots == expected.roots);

                auto edges = std::map<std::tuple<std::uint64_t, std::uint16_t, std::uint64_t>, std::uint16_t> {};
                for (auto const& edge : enumerator.edges()) {
                    auto const inserted =
                        edges.emplace(std::tuple {edge.root_hash, edge.encoded_move, edge.child_hash}, edge.min_root_ply).second;
                    CHECK(inserted);  // each distinct edge appears once
                }
                CHECK(edges == expected.edges);

                auto visited = std::set<std::uint64_t> {enumerator.visited().begin(), enumerator.visited().end()};
                CHECK(visited.size() == enumerator.visited().size());
                CHECK(visited == expected.visited);
            }
        }
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("the enumerator captures late returns to the start beyond the horizon, zero-move and terminal games", "[motif-db][pr6a][tree]")
{
    auto const fixtures = motif_test::fixture_games();
    auto enumerator = tree_contribution_enumerator {test_horizon, motif_test::start_hash(), /*collect_visited=*/false};

    // Zero-move game: the start root still contributes, no edge.
    REQUIRE(enumerator.enumerate(fixtures[0].moves).has_value());
    CHECK(enumerator.roots().size() == 1U);
    CHECK(enumerator.roots()[0] == motif_test::start_hash());
    CHECK(enumerator.edges().empty());

    // Terminal root: fool's mate reaches checkmate at ply 4, a root with no edge out.
    REQUIRE(enumerator.enumerate(fixtures[1].moves).has_value());
    CHECK(enumerator.roots().size() == 5U);
    CHECK(enumerator.edges().size() == 4U);

    // 24 plies of knight shuffles, then e2e4 from the start position at ply 24:
    // beyond the horizon, so only the complete start node may capture it.
    REQUIRE(enumerator.enumerate(fixtures[2].moves).has_value());
    auto const late_move = fixtures[2].moves.back();
    auto const late_edge = std::ranges::find_if(enumerator.edges(),
                                                [&](game_tree_edge const& edge) -> bool
                                                { return edge.root_hash == motif_test::start_hash() && edge.encoded_move == late_move; });
    REQUIRE(late_edge != enumerator.edges().end());
    CHECK(late_edge->min_root_ply == 24U);
    // No edge rooted at any non-start position beyond the horizon.
    for (auto const& edge : enumerator.edges()) {
        if (edge.root_hash != motif_test::start_hash()) {
            CHECK(edge.min_root_ply <= 20U);
        }
    }
    // The shuffle's repeated start->Nf3 edge is deduplicated with its minimum ply.
    auto const repeated = std::ranges::count_if(
        enumerator.edges(), [&](game_tree_edge const& edge) -> bool { return edge.root_hash == motif_test::start_hash(); });
    CHECK(repeated == 2);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("the full builder's checkpoint equals the sum of per-game contributions", "[motif-db][pr6a][tree][property]")
{
    for (auto const seed : {11ULL, 12ULL, 13ULL}) {
        temp_dir const dir {"builder-eq"};
        auto const corpus = motif_test::make_corpus(seed, 30);
        auto const horizon = opening_tree_index_default_max_root_ply;

        auto builder = opening_tree_index_builder {dir.path / "tree.idx"};
        auto enumerator = tree_contribution_enumerator {horizon, opening_tree_start_hash(), /*collect_visited=*/true};

        struct node_expectation
        {
            std::uint32_t games {};
            std::map<std::pair<std::uint16_t, std::uint64_t>, raw_edge> edges;
        };

        auto expected = std::map<std::uint64_t, node_expectation> {};
        auto child_games = std::map<std::uint64_t, std::uint32_t> {};
        for (std::size_t index = 0; index < corpus.size(); ++index) {
            auto const& game = corpus[index];
            auto const document = static_cast<std::uint32_t>(index + 1U);
            REQUIRE(builder
                        .accumulate(replay_game_record {.id = game_id {document},
                                                        .result = game.result,
                                                        .white_elo = game.white_elo,
                                                        .black_elo = game.black_elo,
                                                        .moves = game.moves})
                        .has_value());
            auto const stats = make_game_stats(encode_result_code(game.result), game.white_elo, game.black_elo);
            REQUIRE(stats.has_value());
            REQUIRE(enumerator.enumerate(game.moves).has_value());
            for (auto const root : enumerator.roots()) {
                ++expected[root].games;
            }
            for (auto const hash : enumerator.visited()) {
                ++child_games[hash];
            }
            for (auto const& edge : enumerator.edges()) {
                auto& slot = expected[edge.root_hash].edges[{edge.encoded_move, edge.child_hash}];
                if (slot.stats.frequency == 0) {
                    slot = raw_edge {.key = {.encoded_move = edge.encoded_move, .child_hash = edge.child_hash},
                                     .stats = {},
                                     .min_root_ply = edge.min_root_ply,
                                     .min_root_ply_valid = true,
                                     .sample_min = document,
                                     .sample_max = document,
                                     .sample_bounds_valid = true,
                                     .child_frequency = std::nullopt};
                }
                slot.min_root_ply = std::min(slot.min_root_ply, edge.min_root_ply);
                slot.sample_min = std::min(slot.sample_min, document);
                slot.sample_max = std::max(slot.sample_max, document);
                slot.stats = motif_test::require_value(add_stats(slot.stats, stats.value()));
            }
        }
        REQUIRE(builder.finalize().has_value());

        auto opened = opening_tree_index::open(dir.path / "tree.idx");
        REQUIRE(opened.has_value());
        CHECK(opened->checkpoint_format_version() == opening_tree_index_format_version);
        auto const nodes = collect_nodes(*opened);
        REQUIRE(nodes.size() == expected.size());
        auto position = std::size_t {0};
        for (auto const& [root, node_expected] : expected) {  // std::map iterates ascending, like the file
            auto const& node = nodes[position++];
            CHECK(node.root_hash == root);
            CHECK(node.game_count == node_expected.games);
            CHECK(node.complete == (root == opening_tree_start_hash()));
            REQUIRE(node.edges.size() == node_expected.edges.size());
            auto edge_position = std::size_t {0};
            for (auto const& [key, edge_expected] : node_expected.edges) {
                auto const& edge = node.edges[edge_position++];
                CHECK(edge.key.encoded_move == key.first);
                CHECK(edge.key.child_hash == key.second);
                CHECK(edge.stats == edge_expected.stats);
                CHECK(edge.min_root_ply == edge_expected.min_root_ply);
                CHECK(edge.sample_min == edge_expected.sample_min);
                CHECK(edge.sample_max == edge_expected.sample_max);
                CHECK(edge.min_root_ply_valid);
                CHECK(edge.sample_bounds_valid);
                CHECK(edge.child_frequency == std::optional<std::uint32_t> {child_games[key.second]});
            }
            // query_raw_node agrees with the streaming reader.
            auto const direct = opened->query_raw_node(zobrist_hash {root});
            REQUIRE(direct.has_value());
            REQUIRE(direct->has_value());
            CHECK(motif_test::require_value(*direct) == node);
        }
        CHECK(opened->source_game_count() == corpus.size());
    }
}

// ── v5 / v6 checkpoint formats ───────────────────────────────────────────────

namespace
{

void put_u16(std::string& out, std::uint16_t const value)
{
    append_fixed_le(out, value);
}

// Writes a checkpoint exactly as the PR 5 (v5) writer did: unsigned varint sums
// (signed sums cast to u64) and the weighted sums as little-endian doubles. Used
// to prove the converting reader and to forge corrupt files.
struct v5_overrides
{
    std::optional<double> weighted_contrib_sum;
    std::optional<double> elo_weight_sum;
};

auto encode_v5(std::vector<raw_node> const& nodes,
               std::uint16_t const horizon,  // NOLINT(bugprone-easily-swappable-parameters) -- a ply horizon and a source game count; every
                                             // caller passes them in this order.
               std::uint64_t const source_games,
               v5_overrides const overrides = {}) -> std::string
{
    auto out = std::string {"MOTIFOT1"};
    append_fixed_le(out, v5_format_version);
    put_u16(out, horizon);
    append_fixed_le(out, source_games);
    append_fixed_le(out, static_cast<std::uint64_t>(nodes.size()));
    auto const put_double = [&out](double const value) -> void { append_fixed_le(out, std::bit_cast<std::uint64_t>(value)); };
    for (auto const& node : nodes) {
        append_fixed_le(out, node.root_hash);
        append_uleb128(out, node.game_count);
        append_uleb128(out, node.edges.size());
        append_uleb128(out, node.complete ? 1U : 0U);
        for (auto const& edge : node.edges) {
            put_u16(out, edge.key.encoded_move);
            append_fixed_le(out, edge.key.child_hash);
            append_uleb128(out, edge.min_root_ply);
            append_uleb128(out, static_cast<std::uint64_t>(edge.stats.frequency));
            append_uleb128(out, static_cast<std::uint64_t>(edge.stats.white_wins));
            append_uleb128(out, static_cast<std::uint64_t>(edge.stats.draws));
            append_uleb128(out, static_cast<std::uint64_t>(edge.stats.black_wins));
            append_uleb128(out, static_cast<std::uint64_t>(edge.stats.white_sum));  // two's complement for negatives
            append_uleb128(out, static_cast<std::uint64_t>(edge.stats.white_count));
            append_uleb128(out, static_cast<std::uint64_t>(edge.stats.black_sum));
            append_uleb128(out, static_cast<std::uint64_t>(edge.stats.black_count));
            put_double(overrides.weighted_contrib_sum.value_or(static_cast<double>(edge.stats.numerator2) / weight_scale));
            put_double(overrides.elo_weight_sum.value_or(static_cast<double>(edge.stats.weight2) / weight_scale));
            append_uleb128(out, edge.sample_min);
            append_uleb128(out, edge.sample_max);
            append_uleb128(out, edge.child_frequency.value_or(0U));
        }
    }
    return out;
}

void write_file(std::filesystem::path const& path, std::string const& bytes)
{
    std::ofstream out {path, std::ios::binary | std::ios::trunc};
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

auto read_file(std::filesystem::path const& path) -> std::string
{
    std::ifstream input {path, std::ios::binary};
    return std::string {std::istreambuf_iterator<char> {input}, std::istreambuf_iterator<char> {}};
}

// Builds a tree checkpoint (v6) over the corpus using only the builder.
auto build_tree_file(std::filesystem::path const& path, std::vector<motif_test::model_game> const& corpus) -> void
{
    auto builder = opening_tree_index_builder {path};
    for (std::size_t index = 0; index < corpus.size(); ++index) {
        auto const& game = corpus[index];
        REQUIRE(builder
                    .accumulate(replay_game_record {.id = game_id {static_cast<std::uint32_t>(index + 1U)},
                                                    .result = game.result,
                                                    .white_elo = game.white_elo,
                                                    .black_elo = game.black_elo,
                                                    .moves = game.moves})
                    .has_value());
    }
    REQUIRE(builder.finalize().has_value());
}

auto any_negative_rating_sum(std::vector<raw_node> const& nodes) -> bool
{
    return std::ranges::any_of(nodes,
                               [](raw_node const& node) -> bool
                               {
                                   return std::ranges::any_of(node.edges,
                                                              [](raw_edge const& edge) -> bool
                                                              { return edge.stats.white_sum < 0 || edge.stats.black_sum < 0; });
                               });
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("a v5 checkpoint with negative Elo sums is read correctly and v6 round-trips them", "[motif-db][pr6a][tree][v5]")
{
    temp_dir const dir {"v5-negative"};
    // The fixtures carry ratings -1, -32768 and 32767; the builder writes v6.
    constexpr auto corpus_seed = 21U;
    constexpr auto corpus_size = 20U;
    auto const corpus = motif_test::make_corpus(corpus_seed, corpus_size);
    build_tree_file(dir.path / "v6.idx", corpus);
    auto checkpoint_v6 = opening_tree_index::open(dir.path / "v6.idx");
    REQUIRE(checkpoint_v6.has_value());
    CHECK(checkpoint_v6->checkpoint_format_version() == 6U);
    auto const nodes = collect_nodes(*checkpoint_v6);
    REQUIRE(any_negative_rating_sum(nodes));  // the defect's trigger is present

    // v5 as the old writer produced it: two's-complement varints for negative sums.
    // The old reader rejected exactly this (value above INT64_MAX).
    write_file(dir.path / "v5.idx", encode_v5(nodes, checkpoint_v6->max_root_ply(), checkpoint_v6->source_game_count()));
    auto checkpoint_v5 = opening_tree_index::open(dir.path / "v5.idx");
    REQUIRE(checkpoint_v5.has_value());
    CHECK(checkpoint_v5->checkpoint_format_version() == 5U);
    auto const converted = collect_nodes(*checkpoint_v5);
    CHECK(converted == nodes);  // identical raw rows, negatives included

    auto const root = zobrist_hash {opening_tree_start_hash()};
    auto const rows_v6 = checkpoint_v6->query_opening_stats(root);
    auto const rows_v5 = checkpoint_v5->query_opening_stats(root);
    REQUIRE(rows_v6.has_value());
    REQUIRE(rows_v5.has_value());
    REQUIRE(rows_v5->size() == rows_v6->size());
    for (std::size_t index = 0; index < rows_v6->size(); ++index) {
        CHECK((*rows_v5)[index].avg_white_elo == (*rows_v6)[index].avg_white_elo);
        CHECK((*rows_v5)[index].avg_black_elo == (*rows_v6)[index].avg_black_elo);
        CHECK((*rows_v5)[index].elo_weighted_score == (*rows_v6)[index].elo_weighted_score);
        CHECK((*rows_v5)[index].frequency == (*rows_v6)[index].frequency);
    }

    // A v5 checkpoint is converted, never eagerly rewritten; re-emitting its raw
    // rows through the v6 writer and reading them back is exact.
    {
        auto writer = opening_tree_checkpoint_writer::create(
            dir.path / "rewritten.idx", checkpoint_v5->max_root_ply(), checkpoint_v5->source_game_count());
        REQUIRE(writer.has_value());
        for (auto const& node : converted) {
            REQUIRE(writer->write_node(node).has_value());
        }
        REQUIRE(writer->finish().has_value());
        auto rewritten = opening_tree_index::open(dir.path / "rewritten.idx");
        REQUIRE(rewritten.has_value());
        CHECK(rewritten->checkpoint_format_version() == 6U);
        CHECK(collect_nodes(*rewritten) == nodes);
    }
}

TEST_CASE("a v5 sum that is not a plausible signed rating total is rejected", "[motif-db][pr6a][tree][v5]")
{
    temp_dir const dir {"v5-bad-sums"};
    // One node, one edge with a negative black sum and a valid half-integer weight.
    auto node = raw_node {.root_hash = sample_root_hash, .game_count = 3, .complete = false, .edges = {valid_edge()}};
    auto const good = encode_v5({node}, test_horizon, 3);
    write_file(dir.path / "good.idx", good);
    REQUIRE(opening_tree_index::open(dir.path / "good.idx").has_value());

    // Invalid weighted doubles: non-integral doubled value, NaN, infinity, out of range.
    for (auto const bad : {0.3, std::nan(""), std::numeric_limits<double>::infinity(), 1e30, -1e30}) {
        write_file(dir.path / "bad.idx",
                   encode_v5({node}, test_horizon, 3, v5_overrides {.weighted_contrib_sum = bad, .elo_weight_sum = std::nullopt}));
        CHECK_FALSE(opening_tree_index::open(dir.path / "bad.idx").has_value());
        write_file(dir.path / "bad.idx",
                   encode_v5({node}, test_horizon, 3, v5_overrides {.weighted_contrib_sum = std::nullopt, .elo_weight_sum = bad}));
        CHECK_FALSE(opening_tree_index::open(dir.path / "bad.idx").has_value());
    }
    // A half-integer is exactly representable as a doubled integer and accepted.
    constexpr auto half_integer_weight = 3.5;
    write_file(
        dir.path / "half.idx",
        encode_v5({node}, test_horizon, 3, v5_overrides {.weighted_contrib_sum = std::nullopt, .elo_weight_sum = half_integer_weight}));
    CHECK(opening_tree_index::open(dir.path / "half.idx").has_value());

    // A "negative" sum that wraps to an implausible magnitude is corruption.
    auto huge = node;
    huge.edges[0].stats.white_sum = i64_min;
    write_file(dir.path / "huge.idx", encode_v5({huge}, test_horizon, 3));
    CHECK_FALSE(opening_tree_index::open(dir.path / "huge.idx").has_value());
}

namespace
{

// One node, one single-byte-varint edge: every field's offset is fixed.
auto small_v6_file(std::filesystem::path const& path) -> std::string
{
    auto writer = opening_tree_checkpoint_writer::create(path, test_horizon, 3);
    REQUIRE(writer.has_value());
    auto const node = raw_node {.root_hash = sample_root_hash, .game_count = 3, .complete = false, .edges = {valid_edge()}};
    REQUIRE(writer->write_node(node).has_value());
    REQUIRE(writer->finish().has_value());
    return read_file(path);
}

constexpr auto v6_header_bytes = std::size_t {30};
constexpr auto v6_node_header_bytes = std::size_t {8 + 1 + 1 + 1};
constexpr auto v6_edge_flags_offset = v6_header_bytes + v6_node_header_bytes + 2 + 8;  // after move and child hash
// The inverted sample bounds live twelve bytes after the edge flags.
constexpr auto v6_sample_bounds_offset = v6_edge_flags_offset + 12;

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("a v6 checkpoint rejects non-canonical, overflowing and truncated records", "[motif-db][pr6a][tree][v6]")
{
    temp_dir dir {"v6-corrupt"};
    auto const path = dir.path / "small.idx";
    auto const good = small_v6_file(path);
    REQUIRE(opening_tree_index::open(path).has_value());
    REQUIRE(good.size() == v6_edge_flags_offset + 15);  // flags + 14 one-byte varints
    CHECK(static_cast<std::uint8_t>(good[v6_edge_flags_offset]) == 7U);  // all three validity flags

    auto const opens = [&](std::string const& bytes) -> bool
    {
        write_file(dir.path / "variant.idx", bytes);
        return opening_tree_index::open(dir.path / "variant.idx").has_value();
    };

    // Every strict prefix is a truncation.
    for (std::size_t length = 0; length < good.size(); ++length) {
        CHECK_FALSE(opens(good.substr(0, length)));
    }
    CHECK_FALSE(opens(good + '\x00'));  // trailing byte

    auto mutated = good;
    mutated[v6_edge_flags_offset] = '\x0f';  // unknown validity bit
    CHECK_FALSE(opens(mutated));

    // Non-canonical: the min-ply flag is clear but a value remains.
    mutated = good;
    mutated[v6_edge_flags_offset] = '\x06';
    CHECK_FALSE(opens(mutated));

    // frequency (offset + 2) rewritten as the non-minimal two-byte form of 2.
    mutated = good;
    mutated.replace(v6_edge_flags_offset + 2, 1, std::string {'\x82', '\x00'});
    CHECK_FALSE(opens(mutated));

    // A ten-byte varint whose last byte carries more than one payload bit.
    mutated = good;
    auto overflow = std::string(varint_prefix_bytes, '\xff');
    overflow.push_back('\x02');
    mutated.replace(v6_edge_flags_offset + 2, 1, overflow);
    CHECK_FALSE(opens(mutated));

    // Result tallies no longer sum to the frequency.
    mutated = good;
    mutated[v6_edge_flags_offset + 3] = '\x02';
    CHECK_FALSE(opens(mutated));

    // Sample bounds inverted (sample_min above sample_max).
    mutated = good;
    mutated[v6_sample_bounds_offset] = '\x05';
    CHECK_FALSE(opens(mutated));

    // Wrong magic and an unknown format version.
    mutated = good;
    mutated[0] = 'X';
    CHECK_FALSE(opens(mutated));
    mutated = good;
    mutated[v6_version_offset] = '\x07';
    CHECK_FALSE(opens(mutated));
}

TEST_CASE("the checkpoint writer enforces ordering and encodability", "[motif-db][pr6a][tree][v6]")
{
    temp_dir const dir {"writer-rules"};
    auto writer = opening_tree_checkpoint_writer::create(dir.path / "w.idx", test_horizon, 3);
    REQUIRE(writer.has_value());
    auto first = raw_node {.root_hash = sample_root_hash, .game_count = 3, .complete = false, .edges = {valid_edge()}};
    REQUIRE(writer->write_node(first).has_value());
    CHECK_FALSE(writer->write_node(first).has_value());  // not strictly ascending
    auto second = first;
    second.root_hash = sample_root_hash + 1U;
    second.edges.push_back(second.edges.front());  // duplicate edge key
    CHECK_FALSE(writer->write_node(second).has_value());
    second.edges.pop_back();
    second.edges[0].stats.frequency = -1;  // not encodable
    CHECK_FALSE(writer->write_node(second).has_value());
    second.edges[0] = valid_edge();
    second.game_count = 0;
    CHECK_FALSE(writer->write_node(second).has_value());
    CHECK(writer->node_count() == 1U);
    // Dropping an unfinished writer leaves nothing behind.
    writer = opening_tree_checkpoint_writer::create(dir.path / "other.idx", test_horizon, 0);
    CHECK_FALSE(std::filesystem::exists(dir.path / "w.idx"));
    CHECK_FALSE(std::filesystem::exists(dir.path / "w.idx.tmp"));
}

// ── Reader refill boundaries ─────────────────────────────────────────────────
//
// open() and stream_nodes() read a checkpoint in 64 KiB blocks; a point query
// reads in 4 KiB blocks. The fixtures below are valid checkpoints whose sizes and
// node layouts are tuned, from file sizes the writer actually produced, so block
// ends fall inside records, exactly on record ends, and one byte either side of
// the end of file. If the reader's block sizes change the tests stay correct but
// probe different offsets.

namespace
{

constexpr auto stream_refill_bytes = std::uintmax_t {1U << 16U};
constexpr auto point_refill_bytes = std::uintmax_t {1U << 12U};
constexpr auto refill_horizon = std::uint16_t {20};
constexpr auto refill_source_games = std::uint64_t {1'000'000};
// The smallest tail node the size tuner can always fit, with room to spare.
constexpr auto refill_tail_margin = std::uintmax_t {400};
constexpr auto refill_root_stride = std::uint64_t {0x0000'0100'0000'0001ULL};
constexpr auto varied_state_base = std::uint64_t {0x5EED0000ULL};
constexpr auto slack_hash_base = std::uint64_t {0x1000};
constexpr auto smallest_edge_record_bytes = std::uintmax_t {20};
// splitmix64's constants.
constexpr auto splitmix_increment = std::uint64_t {0x9e3779b97f4a7c15ULL};
constexpr auto splitmix_first_multiplier = std::uint64_t {0xbf58476d1ce4e5b9ULL};
constexpr auto splitmix_second_multiplier = std::uint64_t {0x94d049bb133111ebULL};
constexpr auto splitmix_first_shift = 30U;
constexpr auto splitmix_second_shift = 27U;
constexpr auto splitmix_final_shift = 31U;

auto refill_root(std::size_t const index) -> std::uint64_t
{
    return (static_cast<std::uint64_t>(index) + 1U) * refill_root_stride;
}

// A fixed, portable generator: the fixtures must not depend on the standard library's distributions.
auto splitmix64(std::uint64_t& state) -> std::uint64_t
{
    state += splitmix_increment;
    auto value = state;
    value = (value ^ (value >> splitmix_first_shift)) * splitmix_first_multiplier;
    value = (value ^ (value >> splitmix_second_shift)) * splitmix_second_multiplier;
    return value ^ (value >> splitmix_final_shift);
}

// An edge with every field varied (so varints take 1..5 bytes, validity flags
// are mixed) that still satisfies every merged-edge invariant.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- an edge move and a node's game count; every caller passes them in this order.
auto varied_edge(std::uint64_t& state, std::uint16_t const move, std::uint32_t const node_games) -> raw_edge
{
    auto const draw = [&state](std::int64_t const bound) -> std::int64_t
    { return static_cast<std::int64_t>(splitmix64(state) % static_cast<std::uint64_t>(bound)); };
    auto const child_hash = splitmix64(state);
    auto const frequency = std::int64_t {1} + draw(node_games);
    auto const white_wins = draw(frequency + 1);
    auto const draws = draw(frequency - white_wins + 1);
    auto const black_wins = frequency - white_wins - draws;
    auto const white_count = draw(frequency + 1);
    auto const black_count = draw(frequency + 1);
    auto const white_sum = draw((white_count * 65'535) + 1) - (white_count * 32'768);
    auto const black_sum = draw((black_count * 65'535) + 1) - (black_count * 32'768);
    auto const weight_limit = std::min(white_count, black_count) * 65'536;
    auto const weight2 = draw((2 * weight_limit) + 1) - weight_limit;
    auto const numerator2 = draw((2 * weight_limit) + 1) - weight_limit;
    auto const ply_valid = draw(8) != 0;
    auto const ply = ply_valid ? draw(21) : std::int64_t {0};
    auto const samples_valid = draw(8) != 0;
    auto const sample_min = samples_valid ? 1 + draw(1'000'000) : std::int64_t {0};
    auto const sample_max = samples_valid ? sample_min + draw(1'000'000) : std::int64_t {0};
    auto const has_child_frequency = draw(8) != 0;
    auto const child_frequency = has_child_frequency ? frequency + draw(static_cast<std::int64_t>(1U << 24U)) : std::int64_t {0};
    return raw_edge {
        .key = {.encoded_move = move, .child_hash = child_hash},
        .stats = {.frequency = frequency,
                  .white_wins = white_wins,
                  .draws = draws,
                  .black_wins = black_wins,
                  .white_sum = white_sum,
                  .white_count = white_count,
                  .black_sum = black_sum,
                  .black_count = black_count,
                  .numerator2 = numerator2,
                  .weight2 = weight2},
        .min_root_ply = static_cast<std::uint16_t>(ply),
        .min_root_ply_valid = ply_valid,
        .sample_min = static_cast<std::uint32_t>(sample_min),
        .sample_max = static_cast<std::uint32_t>(sample_max),
        .sample_bounds_valid = samples_valid,
        .child_frequency = has_child_frequency ? std::optional<std::uint32_t> {static_cast<std::uint32_t>(child_frequency)} : std::nullopt};
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- a node index and an edge count; every caller passes them in this order.
auto varied_node(std::size_t const index, std::size_t const edge_count) -> raw_node
{
    auto state = varied_state_base + index;
    auto const game_count = static_cast<std::uint32_t>(400'000U + index);
    auto node = raw_node {.root_hash = refill_root(index), .game_count = game_count, .complete = false, .edges = {}};
    node.edges.reserve(edge_count);
    for (std::size_t edge = 0; edge < edge_count; ++edge) {
        node.edges.push_back(varied_edge(state, static_cast<std::uint16_t>(edge + 1U), game_count));
    }
    return node;
}

// A node of `edges` minimal edges whose child-frequency varints are widened to
// absorb exactly `extra` additional bytes (at most four per edge). Its encoded
// size is therefore adjustable one byte at a time.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- root hash, edge count and byte budget; every caller passes them in this order
auto slack_node(std::uint64_t const root, std::size_t const edges, std::size_t extra) -> raw_node
{
    constexpr auto widths = std::array<std::uint32_t, 5> {1U, 128U, 16'384U, 2'097'152U, 268'435'456U};  // 1..5-byte varints
    auto node = raw_node {.root_hash = root, .game_count = 1, .complete = false, .edges = {}};
    node.edges.reserve(edges);
    for (std::size_t index = 0; index < edges; ++index) {
        auto const step = std::min<std::size_t>(extra, 4U);
        extra -= step;
        node.edges.push_back(
            raw_edge {.key = {.encoded_move = static_cast<std::uint16_t>(index + 1U), .child_hash = slack_hash_base + index},
                      .stats = {.frequency = 1, .white_wins = 1},
                      .min_root_ply = 0,
                      .min_root_ply_valid = true,
                      .sample_min = 1,
                      .sample_max = 1,
                      .sample_bounds_valid = true,
                      .child_frequency = widths.at(step)});
    }
    REQUIRE(extra == 0U);
    return node;
}

// Smallest node of the slack family whose measure is exactly `target`. `measure`
// grows strictly with the edge count, and so does measure + 4 * edges, so the
// first edge count whose widest form reaches the target is the only candidate.
template<typename Measure>
auto tuned_node(std::uint64_t const root, std::uintmax_t const target, Measure const& measure) -> raw_node
{
    auto low = std::size_t {1};
    auto high = static_cast<std::size_t>((target / smallest_edge_record_bytes) + 1U);  // an edge record is at least 25 bytes
    while (low < high) {
        auto const middle = low + ((high - low) / 2U);
        if (measure(slack_node(root, middle, 0)) + (4U * middle) >= target) {
            high = middle;
        } else {
            low = middle + 1U;
        }
    }
    auto const floor = static_cast<std::uintmax_t>(measure(slack_node(root, low, 0)));
    REQUIRE(floor <= target);
    REQUIRE(target - floor <= 4U * low);
    auto node = slack_node(root, low, static_cast<std::size_t>(target - floor));
    REQUIRE(measure(node) == target);
    return node;
}

auto write_checkpoint(std::filesystem::path const& path, std::vector<raw_node> const& nodes) -> std::uintmax_t
{
    auto writer = opening_tree_checkpoint_writer::create(path, refill_horizon, refill_source_games);
    REQUIRE(writer.has_value());
    for (auto const& node : nodes) {
        REQUIRE(writer->write_node(node).has_value());
    }
    REQUIRE(writer->finish().has_value());
    return std::filesystem::file_size(path);
}

// Everything needed to size nodes by measurement rather than by format knowledge.
struct refill_prefix
{
    std::filesystem::path scratch;
    std::uintmax_t empty_size {};  // an empty checkpoint: just the header
    std::vector<raw_node> nodes;
    std::vector<std::uintmax_t> sizes;  // encoded size of each node
};

// Bytes this node adds to a checkpoint.
auto node_bytes(refill_prefix const& prefix, raw_node const& node) -> std::uintmax_t
{
    return write_checkpoint(prefix.scratch / "probe.idx", std::vector<raw_node> {node}) - prefix.empty_size;
}

// Bytes of the node's edge section (what a point query reads after the node header).
auto edge_section_bytes(refill_prefix const& prefix, raw_node const& node) -> std::uintmax_t
{
    constexpr auto root_bytes = std::uintmax_t {8};
    constexpr auto complete_flag_bytes = std::uintmax_t {1};
    auto const header = root_bytes + encode_uleb(node.game_count).size() + encode_uleb(node.edges.size()).size() + complete_flag_bytes;
    return node_bytes(prefix, node) - header;
}

// Nodes whose combined checkpoint leaves at least refill_tail_margin bytes of
// `budget`: four nodes whose edge sections are 4095, 4096, 4097 and 8192 bytes
// (a point query's 4 KiB block ends one byte short of, exactly at, and one byte
// past a node's last byte), then nodes cycling through edge counts from none to
// several hundred.
auto make_refill_prefix(std::filesystem::path const& scratch, std::uintmax_t const budget) -> refill_prefix
{
    auto prefix = refill_prefix {.scratch = scratch, .empty_size = 0, .nodes = {}, .sizes = {}};
    prefix.empty_size = write_checkpoint(scratch / "probe.idx", {});
    auto used = prefix.empty_size;
    auto const append = [&](raw_node node) -> void
    {
        auto const size = node_bytes(prefix, node);
        used += size;
        prefix.sizes.push_back(size);
        prefix.nodes.push_back(std::move(node));
    };
    for (auto const section : std::array<std::uintmax_t, 4> {4095U, 4096U, 4097U, 8192U}) {
        auto node = tuned_node(refill_root(prefix.nodes.size()),
                               section,
                               [&](raw_node const& candidate) -> std::uintmax_t { return edge_section_bytes(prefix, candidate); });
        append(std::move(node));
    }
    constexpr auto edge_counts = std::array<std::size_t, 8> {0U, 1U, 3U, 17U, 120U, 2U, 260U, 5U};
    while (true) {
        auto node = varied_node(prefix.nodes.size(), edge_counts.at(prefix.nodes.size() % edge_counts.size()));
        if (used + node_bytes(prefix, node) + refill_tail_margin > budget) {
            break;
        }
        append(std::move(node));
    }
    return prefix;
}

struct refill_checkpoint
{
    std::filesystem::path path;
    std::vector<raw_node> nodes;
    std::vector<std::uintmax_t> end_offsets;  // file offset one past each node
    std::uintmax_t size {};
};

// The prefix nodes plus one tuned tail node, so the file is exactly `target` bytes.
auto build_refill_checkpoint(refill_prefix const& prefix, std::string const& name, std::uintmax_t const target) -> refill_checkpoint
{
    auto prefix_bytes = prefix.empty_size;
    for (auto const size : prefix.sizes) {
        prefix_bytes += size;
    }
    REQUIRE(target >= prefix_bytes + refill_tail_margin);
    auto tail = tuned_node(refill_root(prefix.nodes.size()),
                           target - prefix_bytes,
                           [&](raw_node const& candidate) -> std::uintmax_t { return node_bytes(prefix, candidate); });

    auto checkpoint = refill_checkpoint {.path = prefix.scratch / (name + ".idx"), .nodes = prefix.nodes, .end_offsets = {}, .size = 0};
    checkpoint.nodes.push_back(std::move(tail));
    auto offset = prefix.empty_size;
    for (auto const size : prefix.sizes) {
        offset += size;
        checkpoint.end_offsets.push_back(offset);
    }
    checkpoint.end_offsets.push_back(target);
    checkpoint.size = write_checkpoint(checkpoint.path, checkpoint.nodes);
    REQUIRE(checkpoint.size == target);
    return checkpoint;
}

// Lengths straddling every multiple of `step` below `size`, plus the extremes.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- a file size and a stride; every caller passes them in this order.
auto boundary_lengths(std::uintmax_t const size, std::uintmax_t const step) -> std::set<std::uintmax_t>
{
    auto lengths = std::set<std::uintmax_t> {0U, 1U, size - 1U};
    for (auto at = step; at < size; at += step) {
        for (auto const length : {at - 1U, at, at + 1U}) {
            if (length < size) {
                lengths.insert(length);
            }
        }
    }
    return lengths;
}

}  // namespace

TEST_CASE("streamed and point-read nodes agree across 64 KiB and 4 KiB refill boundaries", "[motif-db][tree][v6][refill]")
{
    temp_dir const dir {"refill-agree"};
    constexpr auto target = 5U * stream_refill_bytes;
    auto const prefix = make_refill_prefix(dir.path, target - 1U);
    // Fixture shape: nodes whose edge sections need several 4 KiB point reads exist.
    REQUIRE(std::ranges::any_of(prefix.sizes, [](std::uintmax_t const size) -> bool { return size > 2U * point_refill_bytes; }));
    REQUIRE(std::ranges::any_of(prefix.nodes, [](raw_node const& node) -> bool { return node.edges.empty(); }));

    // Ending one byte short of, exactly on, and one byte past a 64 KiB block end.
    for (auto const size : {target - 1U, target, target + 1U}) {
        INFO("checkpoint size " << size);
        auto const checkpoint = build_refill_checkpoint(prefix, "agree-" + std::to_string(size), size);
        auto opened = opening_tree_index::open(checkpoint.path);
        REQUIRE(opened.has_value());
        CHECK(opened->node_count() == checkpoint.nodes.size());

        auto const streamed = collect_nodes(*opened);
        REQUIRE(streamed.size() == checkpoint.nodes.size());
        CHECK(streamed == checkpoint.nodes);

        for (std::size_t index = 0; index < checkpoint.nodes.size(); ++index) {
            auto const& expected = checkpoint.nodes[index];
            INFO("node " << index << " with " << expected.edges.size() << " edges");
            auto const point = opened->query_raw_node(zobrist_hash {expected.root_hash});
            REQUIRE(point.has_value());
            REQUIRE(point->has_value());
            auto const& point_node = motif_test::require_value(*point);
            CHECK(point_node == expected);
            CHECK(point_node == streamed[index]);
            auto const games = opened->game_count(zobrist_hash {expected.root_hash});
            REQUIRE(games.has_value());
            CHECK(*games == expected.game_count);
        }

        auto const absent = opened->query_raw_node(zobrist_hash {refill_root(checkpoint.nodes.size())});
        REQUIRE(absent.has_value());
        CHECK_FALSE(absent->has_value());
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("truncating or extending a checkpoint at refill boundaries is rejected at open", "[motif-db][tree][v6][refill]")
{
    temp_dir const dir {"refill-reject"};
    constexpr auto target = 5U * stream_refill_bytes;
    auto const prefix = make_refill_prefix(dir.path, target - 1U);
    auto const variant = dir.path / "variant.idx";
    auto const opens = [&](std::string const& bytes) -> bool
    {
        write_file(variant, bytes);
        return opening_tree_index::open(variant).has_value();
    };

    for (auto const size : {target - 1U, target, target + 1U}) {
        INFO("checkpoint size " << size);
        auto const checkpoint = build_refill_checkpoint(prefix, "reject-" + std::to_string(size), size);
        auto const bytes = read_file(checkpoint.path);
        REQUIRE(bytes.size() == size);
        CHECK(opens(bytes));  // the unmodified file is the control

        // Every prefix is a truncation: on a block end, one byte either side of it, and the empty file.
        // The exact-size file is probed at every 4 KiB boundary, the others at every 64 KiB boundary.
        for (auto const length : boundary_lengths(size, size == target ? point_refill_bytes : stream_refill_bytes)) {
            INFO("truncated to " << length);
            CHECK_FALSE(opens(bytes.substr(0, static_cast<std::size_t>(length))));
        }
        // Cutting between two nodes still claims more nodes than the file holds.
        for (auto const end : checkpoint.end_offsets) {
            if (end < size) {
                INFO("truncated at node end " << end);
                CHECK_FALSE(opens(bytes.substr(0, static_cast<std::size_t>(end))));
            }
        }

        // Trailing data of one byte, a block, and several blocks, and a complete extra node record.
        for (auto const extra : {std::size_t {1},
                                 std::size_t {2},
                                 static_cast<std::size_t>(point_refill_bytes),
                                 static_cast<std::size_t>(stream_refill_bytes)})
        {
            INFO("trailing zero bytes " << extra);
            CHECK_FALSE(opens(bytes + std::string(extra, '\0')));
        }
        auto const last_node_start = checkpoint.end_offsets[checkpoint.end_offsets.size() - 2U];
        CHECK_FALSE(opens(bytes + bytes.substr(static_cast<std::size_t>(last_node_start))));
        CHECK_FALSE(opens(bytes + '\x01'));
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("a checkpoint cut after open fails exactly at the cut, for streams and point reads", "[motif-db][tree][v6][refill]")
{
    temp_dir const dir {"refill-cut"};
    constexpr auto target = 5U * stream_refill_bytes;
    auto const prefix = make_refill_prefix(dir.path, target - 1U);
    auto const checkpoint = build_refill_checkpoint(prefix, "cut-source", target);
    auto const cut_path = dir.path / "cut.idx";

    // On each 64 KiB block end and one byte past it.
    for (auto block = std::uintmax_t {1}; block < target / stream_refill_bytes; ++block) {
        for (auto const cut : {block * stream_refill_bytes, (block * stream_refill_bytes) + 1U}) {
            INFO("cut at " << cut);
            std::filesystem::copy_file(checkpoint.path, cut_path, std::filesystem::copy_options::overwrite_existing);
            auto opened = opening_tree_index::open(cut_path);
            REQUIRE(opened.has_value());
            std::filesystem::resize_file(cut_path, cut);

            // The stream yields exactly the nodes that end at or before the cut, then fails.
            auto stream = opened->stream_nodes();
            REQUIRE(stream.has_value());
            for (std::size_t index = 0; index < checkpoint.nodes.size(); ++index) {
                INFO("stream node " << index);
                auto const next = stream->next();
                if (checkpoint.end_offsets[index] <= cut) {
                    REQUIRE(next.has_value());
                    REQUIRE(next->has_value());
                    CHECK(motif_test::require_value(*next) == checkpoint.nodes[index]);
                } else {
                    CHECK_FALSE(next.has_value());
                    break;
                }
            }

            // Point reads of intact nodes are unaffected; a node whose edges were cut off fails.
            for (std::size_t index = 0; index < checkpoint.nodes.size(); ++index) {
                auto const& expected = checkpoint.nodes[index];
                INFO("point node " << index << " ending at " << checkpoint.end_offsets[index]);
                auto const point = opened->query_raw_node(zobrist_hash {expected.root_hash});
                if (checkpoint.end_offsets[index] <= cut) {
                    REQUIRE(point.has_value());
                    REQUIRE(point->has_value());
                    CHECK(motif_test::require_value(*point) == expected);
                } else if (!expected.edges.empty()) {
                    CHECK_FALSE(point.has_value());
                }
            }
        }
    }
}
