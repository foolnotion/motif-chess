#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

#include "motif/db/opening_tree_stats.hpp"

#include <tl/expected.hpp>

#include "motif/db/error.hpp"
#include "motif/db/types.hpp"

namespace motif::db
{

namespace
{

auto invalid(char const* what) -> tl::unexpected<error>
{
    return tl::unexpected {error {error_code::io_failure, what}};
}

constexpr auto u32_max = static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max());

}  // namespace

auto is_zero(raw_edge_stats const& stats) noexcept -> bool
{
    return std::ranges::all_of(raw_edge_stat_fields, [&stats](auto const field) -> bool { return stats.*field == 0; });
}

auto add_stats(raw_edge_stats const& lhs, raw_edge_stats const& rhs) noexcept -> std::optional<raw_edge_stats>
{
    auto out = raw_edge_stats {};
    for (auto const field : raw_edge_stat_fields) {
        auto const sum = checked_add_i64(lhs.*field, rhs.*field);
        if (!sum) {
            return std::nullopt;
        }
        out.*field = *sum;
    }
    return out;
}

auto subtract_stats(raw_edge_stats const& lhs, raw_edge_stats const& rhs) noexcept -> std::optional<raw_edge_stats>
{
    auto out = raw_edge_stats {};
    for (auto const field : raw_edge_stat_fields) {
        auto const difference = checked_sub_i64(lhs.*field, rhs.*field);
        if (!difference) {
            return std::nullopt;
        }
        out.*field = *difference;
    }
    return out;
}

auto encode_result_code(std::string_view const pgn_result) noexcept -> std::int8_t
{
    if (pgn_result == "1-0") {
        return 1;
    }
    if (pgn_result == "0-1") {
        return -1;
    }
    return 0;
}

auto make_game_stats(std::int8_t const outcome, std::optional<std::int32_t> const white_elo, std::optional<std::int32_t> const black_elo)
    -> result<raw_edge_stats>
{
    auto const in_range = [](std::optional<std::int32_t> const& elo) -> bool
    { return !elo || (*elo >= min_supported_elo && *elo <= max_supported_elo); };
    if (!in_range(white_elo) || !in_range(black_elo)) {
        return invalid("rating outside the signed 16-bit domain");
    }
    auto stats = raw_edge_stats {};
    stats.frequency = 1;
    stats.white_wins = outcome > 0 ? 1 : 0;
    stats.draws = outcome == 0 ? 1 : 0;
    stats.black_wins = outcome < 0 ? 1 : 0;
    if (white_elo) {
        stats.white_sum = *white_elo;
        stats.white_count = 1;
    }
    if (black_elo) {
        stats.black_sum = *black_elo;
        stats.black_count = 1;
    }
    if (white_elo && black_elo) {
        stats.weight2 = static_cast<std::int64_t>(*white_elo) + static_cast<std::int64_t>(*black_elo);
        stats.numerator2 = static_cast<std::int64_t>(outcome) * stats.weight2;
    }
    return stats;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one flat list of independent invariants.
auto validate_merged_edge(raw_edge const& edge, merged_edge_context const& context) -> result<void>
{
    auto const& stats = edge.stats;
    if (stats.frequency < 1 || stats.frequency > u32_max || std::cmp_greater(stats.frequency, context.node_game_count)) {
        return invalid("edge frequency out of range");
    }
    auto const in_closed = [](std::int64_t const value, std::int64_t const upper) -> bool { return value >= 0 && value <= upper; };
    if (!in_closed(stats.white_wins, stats.frequency) || !in_closed(stats.draws, stats.frequency)
        || !in_closed(stats.black_wins, stats.frequency) || stats.white_wins + stats.draws + stats.black_wins != stats.frequency)
    {
        return invalid("edge result tallies disagree with its frequency");
    }
    if (!in_closed(stats.white_count, stats.frequency) || !in_closed(stats.black_count, stats.frequency)) {
        return invalid("edge rating population out of range");
    }
    // count * 32768 cannot overflow: count <= 2^32 - 1.
    auto const sum_in_bounds = [](std::int64_t const sum, std::int64_t const count) -> bool
    { return sum >= count * min_supported_elo && sum <= count * max_supported_elo; };
    if (!sum_in_bounds(stats.white_sum, stats.white_count) || !sum_in_bounds(stats.black_sum, stats.black_count)) {
        return invalid("edge rating sum outside its population bound");
    }
    auto const paired = std::min(stats.white_count, stats.black_count);
    auto const weight_limit = paired * 65536;
    if (stats.weight2 < -weight_limit || stats.weight2 > weight_limit || stats.numerator2 < -weight_limit
        || stats.numerator2 > weight_limit)
    {
        return invalid("edge weighted sums outside their population bound");
    }
    if (edge.min_root_ply_valid && !context.node_complete && edge.min_root_ply > context.max_root_ply) {
        return invalid("edge root ply beyond the horizon");
    }
    if (edge.sample_bounds_valid && (edge.sample_min == 0U || edge.sample_min > edge.sample_max)) {
        return invalid("edge sample bounds invalid");
    }
    if (edge.child_frequency && std::cmp_less(*edge.child_frequency, stats.frequency)) {
        return invalid("edge child frequency below its direct frequency");
    }
    return {};
}

auto weighted_score(std::int64_t const numerator2, std::int64_t const weight2) noexcept -> std::optional<double>
{
    if (weight2 <= 0) {
        return std::nullopt;
    }
    return static_cast<double>(numerator2) / static_cast<double>(weight2);
}

auto average_elo(std::int64_t const sum, std::int64_t const count) noexcept -> std::optional<double>
{
    if (count <= 0) {
        return std::nullopt;
    }
    return static_cast<double>(sum) / static_cast<double>(count);
}

auto make_stat_row(std::uint16_t const encoded_move,
                   std::uint64_t const child_hash,
                   std::uint16_t const root_ply,
                   raw_edge_stats const& stats,
                   std::uint32_t const sample_min,
                   std::uint32_t const sample_max,
                   std::uint32_t const transposition_frequency) -> result<opening_stat_agg_row>
{
    auto const in_u32 = [](std::int64_t const value) -> bool { return value >= 0 && value <= u32_max; };
    if (!in_u32(stats.frequency) || !in_u32(stats.white_wins) || !in_u32(stats.draws) || !in_u32(stats.black_wins)) {
        return invalid("edge counts exceed the presentation range");
    }
    return opening_stat_agg_row {
        .cont_encoded_move = encoded_move,
        .cont_hash = zobrist_hash {child_hash},
        .root_ply = root_ply,
        .frequency = static_cast<std::uint32_t>(stats.frequency),
        .transposition_frequency = transposition_frequency,
        .white_wins = static_cast<std::uint32_t>(stats.white_wins),
        .draws = static_cast<std::uint32_t>(stats.draws),
        .black_wins = static_cast<std::uint32_t>(stats.black_wins),
        .avg_white_elo = average_elo(stats.white_sum, stats.white_count),
        .avg_black_elo = average_elo(stats.black_sum, stats.black_count),
        .eco_sample_min = game_id {sample_min},
        .eco_sample_max = game_id {sample_max},
        .elo_weighted_score = weighted_score(stats.numerator2, stats.weight2),
    };
}

auto to_opening_stat_row(raw_edge const& edge, std::uint32_t const transposition_frequency) -> result<opening_stat_agg_row>
{
    if (!edge.min_root_ply_valid || !edge.sample_bounds_valid) {
        return invalid("edge extrema are not exact; a live witness is required");
    }
    if (edge.stats.frequency < 1 || std::cmp_less(transposition_frequency, edge.stats.frequency)) {
        return invalid("child frequency below the edge frequency");
    }
    return make_stat_row(edge.key.encoded_move,
                         edge.key.child_hash,
                         edge.min_root_ply,
                         edge.stats,
                         edge.sample_min,
                         edge.sample_max,
                         transposition_frequency);
}

}  // namespace motif::db
