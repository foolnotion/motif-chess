#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "motif/db/error.hpp"
#include "motif/db/types.hpp"

namespace motif::db
{

// ── Integer codecs ───────────────────────────────────────────────────────────
//
// Every raw sum in the opening tree is a signed 64-bit integer. Signed values
// travel as zigzag(i64) -> ULEB128; unsigned counts travel as plain ULEB128.
// Decoding is strict: truncation, overlong (non-minimal) encodings, and a
// tenth byte carrying more than one payload bit are all rejected.

// (x << 1) ^ (x >> 63) in unsigned arithmetic, so INT64_MIN needs no signed
// negation or shift of a negative value.
[[nodiscard]] constexpr auto zigzag_encode(std::int64_t const value) noexcept -> std::uint64_t
{
    return (static_cast<std::uint64_t>(value) << 1U) ^ (std::uint64_t {0} - static_cast<std::uint64_t>(value < 0));
}

[[nodiscard]] constexpr auto zigzag_decode(std::uint64_t const encoded) noexcept -> std::int64_t
{
    return static_cast<std::int64_t>((encoded >> 1U) ^ (std::uint64_t {0} - (encoded & 1U)));
}

inline void append_uleb128(std::string& out, std::uint64_t value)
{
    constexpr auto continuation_bit = std::uint8_t {0x80};
    constexpr auto payload_mask = std::uint64_t {0x7F};
    constexpr auto payload_bits = 7U;
    while (true) {
        auto byte = static_cast<std::uint8_t>(value & payload_mask);
        value >>= payload_bits;
        if (value != 0U) {
            byte |= continuation_bit;
        }
        out.push_back(static_cast<char>(byte));
        if (value == 0U) {
            return;
        }
    }
}

inline void append_zigzag(std::string& out, std::int64_t const value)
{
    append_uleb128(out, zigzag_encode(value));
}

template<typename Integer>
void append_fixed_le(std::string& out, Integer const value)
{
    static_assert(std::is_unsigned_v<Integer>);
    constexpr auto byte_mask = std::uint64_t {0xFF};
    constexpr auto byte_bits = 8U;
    auto raw = static_cast<std::uint64_t>(value);
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        out.push_back(static_cast<char>(raw & byte_mask));
        raw >>= byte_bits;
    }
}

// next_byte() yields std::optional<std::uint8_t> (nullopt at end of input).
// Returns nullopt on truncation, overlong encoding or 64-bit overflow.
template<typename NextByte>
[[nodiscard]] constexpr auto decode_uleb128(NextByte next_byte) -> std::optional<std::uint64_t>
{
    constexpr auto continuation_bit = std::uint8_t {0x80};
    constexpr auto payload_mask = std::uint8_t {0x7F};
    constexpr auto payload_bits = 7U;
    constexpr auto last_shift = 63U;
    auto value = std::uint64_t {0};
    for (auto shift = 0U; shift <= last_shift; shift += payload_bits) {
        auto const byte = next_byte();
        if (!byte) {
            return std::nullopt;
        }
        auto const payload = static_cast<std::uint8_t>(*byte & payload_mask);
        // At shift 63 only bit 0 of the payload still fits a 64-bit value.
        if (shift == last_shift && payload > 1U) {
            return std::nullopt;
        }
        value |= static_cast<std::uint64_t>(payload) << shift;
        if ((*byte & continuation_bit) == 0U) {
            // A terminating zero group after the first byte is a redundant
            // (non-minimal) encoding of a shorter value.
            if (payload == 0U && shift != 0U) {
                return std::nullopt;
            }
            return value;
        }
    }
    return std::nullopt;
}

// Sequential little-endian / LEB128 reader over an in-memory byte range.
class byte_reader
{
  public:
    explicit byte_reader(std::string_view data) noexcept
        : data_ {data}
    {
    }

    [[nodiscard]] auto position() const noexcept -> std::size_t { return position_; }

    [[nodiscard]] auto remaining() const noexcept -> std::size_t { return data_.size() - position_; }

    [[nodiscard]] auto at_end() const noexcept -> bool { return position_ == data_.size(); }

    [[nodiscard]] auto read_u8() noexcept -> std::optional<std::uint8_t>
    {
        if (position_ >= data_.size()) {
            return std::nullopt;
        }
        return static_cast<std::uint8_t>(data_[position_++]);
    }

    [[nodiscard]] auto read_u16() noexcept -> std::optional<std::uint16_t> { return read_fixed<std::uint16_t>(); }

    [[nodiscard]] auto read_u32() noexcept -> std::optional<std::uint32_t> { return read_fixed<std::uint32_t>(); }

    [[nodiscard]] auto read_u64() noexcept -> std::optional<std::uint64_t> { return read_fixed<std::uint64_t>(); }

    [[nodiscard]] auto read_uleb128() noexcept -> std::optional<std::uint64_t>
    {
        return decode_uleb128([this]() noexcept -> std::optional<std::uint8_t> { return read_u8(); });
    }

    [[nodiscard]] auto read_zigzag() noexcept -> std::optional<std::int64_t>
    {
        auto const encoded = read_uleb128();
        if (!encoded) {
            return std::nullopt;
        }
        return zigzag_decode(*encoded);
    }

    [[nodiscard]] auto read_bytes(std::size_t const count) noexcept -> std::optional<std::string_view>
    {
        if (count > remaining()) {
            return std::nullopt;
        }
        auto const view = data_.substr(position_, count);
        position_ += count;
        return view;
    }

  private:
    template<typename Integer>
    [[nodiscard]] auto read_fixed() noexcept -> std::optional<Integer>
    {
        if (remaining() < sizeof(Integer)) {
            return std::nullopt;
        }
        constexpr auto byte_bits = 8U;
        auto raw = std::uint64_t {0};
        for (std::size_t index = 0; index < sizeof(Integer); ++index) {
            raw |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(data_[position_ + index])) << (byte_bits * index);
        }
        position_ += sizeof(Integer);
        return static_cast<Integer>(raw);
    }

    std::string_view data_;
    std::size_t position_ {};
};

// ── Checked signed arithmetic ────────────────────────────────────────────────

[[nodiscard]] constexpr auto checked_add_i64(std::int64_t const lhs, std::int64_t const rhs) noexcept -> std::optional<std::int64_t>
{
    if ((rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() - rhs)
        || (rhs < 0 && lhs < std::numeric_limits<std::int64_t>::min() - rhs))
    {
        return std::nullopt;
    }
    return lhs + rhs;
}

[[nodiscard]] constexpr auto checked_sub_i64(std::int64_t const lhs, std::int64_t const rhs) noexcept -> std::optional<std::int64_t>
{
    if ((rhs < 0 && lhs > std::numeric_limits<std::int64_t>::max() + rhs)
        || (rhs > 0 && lhs < std::numeric_limits<std::int64_t>::min() + rhs))
    {
        return std::nullopt;
    }
    return lhs - rhs;
}

// ── Raw sufficient statistics ────────────────────────────────────────────────
//
// The ten additive quantities that define one tree edge. Merged rows hold them
// as non-negative totals; patch runs hold them as signed deltas. weight2 and
// numerator2 are TWICE the historical "elo_weight_sum"/"weighted_contrib_sum"
// (white + black rating instead of their average), so every value is an exact
// integer and the ratio is unchanged.
//
//   frequency  = 1                                (one distinct game per edge)
//   white_wins/draws/black_wins = [result is +1/0/-1]
//   white_sum/white_count = rating, 1 when present; 0/0 when absent
//   black_sum/black_count likewise
//   weight2    = Iw*Ib*(white + black)
//   numerator2 = result * weight2
struct raw_edge_stats
{
    std::int64_t frequency {};
    std::int64_t white_wins {};
    std::int64_t draws {};
    std::int64_t black_wins {};
    std::int64_t white_sum {};
    std::int64_t white_count {};
    std::int64_t black_sum {};
    std::int64_t black_count {};
    std::int64_t numerator2 {};
    std::int64_t weight2 {};

    auto operator==(raw_edge_stats const&) const -> bool = default;
};

inline constexpr auto raw_edge_stat_fields = std::array {&raw_edge_stats::frequency,
                                                         &raw_edge_stats::white_wins,
                                                         &raw_edge_stats::draws,
                                                         &raw_edge_stats::black_wins,
                                                         &raw_edge_stats::white_sum,
                                                         &raw_edge_stats::white_count,
                                                         &raw_edge_stats::black_sum,
                                                         &raw_edge_stats::black_count,
                                                         &raw_edge_stats::numerator2,
                                                         &raw_edge_stats::weight2};

[[nodiscard]] auto is_zero(raw_edge_stats const& stats) noexcept -> bool;
// nullopt on any i64 overflow.
[[nodiscard]] auto add_stats(raw_edge_stats const& lhs, raw_edge_stats const& rhs) noexcept -> std::optional<raw_edge_stats>;
[[nodiscard]] auto subtract_stats(raw_edge_stats const& lhs, raw_edge_stats const& rhs) noexcept -> std::optional<raw_edge_stats>;

// Result mapping shared by every index: 1-0 -> +1, 0-1 -> -1, everything else 0.
[[nodiscard]] auto encode_result_code(std::string_view pgn_result) noexcept -> std::int8_t;

inline constexpr auto min_supported_elo = std::int32_t {std::numeric_limits<std::int16_t>::min()};
inline constexpr auto max_supported_elo = std::int32_t {std::numeric_limits<std::int16_t>::max()};

// The one contribution of a single game to one distinct edge. Ratings outside
// the signed 16-bit domain are rejected (the postings metadata cannot hold
// them), never clamped.
[[nodiscard]] auto make_game_stats(std::int8_t outcome, std::optional<std::int32_t> white_elo, std::optional<std::int32_t> black_elo)
    -> result<raw_edge_stats>;

// ── Raw edge / node rows ─────────────────────────────────────────────────────

struct tree_edge_key
{
    std::uint16_t encoded_move {};
    std::uint64_t child_hash {};

    auto operator<=>(tree_edge_key const&) const = default;
};

// One merged edge: statistics plus the non-additive fields with explicit
// validity. Extrema (minimum root ply, sample game-ID bounds) are exact only
// while no retraction has invalidated them; child-global frequency is a cached
// value, valid only when the snapshot says it is.
struct raw_edge
{
    tree_edge_key key {};
    raw_edge_stats stats {};
    std::uint16_t min_root_ply {};
    bool min_root_ply_valid {true};
    std::uint32_t sample_min {};
    std::uint32_t sample_max {};
    bool sample_bounds_valid {true};
    std::optional<std::uint32_t> child_frequency;

    auto operator==(raw_edge const&) const -> bool = default;
};

struct raw_node
{
    std::uint64_t root_hash {};
    std::uint32_t game_count {};
    bool complete {false};
    std::vector<raw_edge> edges;  // strictly ascending by key

    auto operator==(raw_node const&) const -> bool = default;
};

struct merged_edge_context
{
    std::uint32_t node_game_count {};
    // Exact root plies must not exceed this unless the node is complete, whose
    // edges may sit at any representable ply.
    std::uint16_t max_root_ply {};
    bool node_complete {false};
};

// Invariants of a MERGED (never a delta) positive-frequency edge; see the
// design's merged-row rules. io_failure on violation.
[[nodiscard]] auto validate_merged_edge(raw_edge const& edge, merged_edge_context const& context) -> result<void>;

// ── Presentation (the single raw -> opening_stat_agg_row conversion) ─────────

// elo_weighted_score: numerator2 / weight2 when weight2 > 0, else nullopt.
// The one rule for every path (tree, filtered stats, tree slice).
[[nodiscard]] auto weighted_score(std::int64_t numerator2, std::int64_t weight2) noexcept -> std::optional<double>;
[[nodiscard]] auto average_elo(std::int64_t sum, std::int64_t count) noexcept -> std::optional<double>;

// Converts one merged edge; requires exact extrema and a resolved child count.
[[nodiscard]] auto to_opening_stat_row(raw_edge const& edge, std::uint32_t transposition_frequency) -> result<opening_stat_agg_row>;

// Shared by edge conversion and the postings-replay filtered path.
[[nodiscard]] auto make_stat_row(std::uint16_t encoded_move,
                                 std::uint64_t child_hash,
                                 std::uint16_t root_ply,
                                 raw_edge_stats const& stats,
                                 std::uint32_t sample_min,
                                 std::uint32_t sample_max,
                                 std::uint32_t transposition_frequency) -> result<opening_stat_agg_row>;

}  // namespace motif::db
