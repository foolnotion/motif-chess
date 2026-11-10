#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "motif/db/opening_tree_index.hpp"

#include <gtl/phmap.hpp>
#include <spdlog/spdlog.h>
#include <tl/expected.hpp>

#include "motif/chess/chess.hpp"
#include "motif/db/error.hpp"
#include "motif/db/game_store.hpp"
#include "motif/db/types.hpp"

namespace motif::db
{

namespace
{

constexpr auto magic = std::array<char, 8> {'M', 'O', 'T', 'I', 'F', 'O', 'T', '1'};
// v1 -> v2: continuation-record fields switched from fixed-width to
// LEB128 varints (write_varint/read_varint) -- most edges have small counts.
// v4 -> v5: each node header gained a trailing "complete" varint flag. A
// complete node's continuations were aggregated from every occurrence of its
// hash in every game (not capped to max_root_ply), so its edge root_ply may
// legitimately exceed max_root_ply -- the per-edge upper-bound check is
// relaxed per-node accordingly. Only the canonical starting position is
// ever marked complete (see full_coverage_root_hash below).
// v5 -> v6: Elo sums are zigzag signed varints (a v5 writer cast signed sums
// to u64, which its own reader rejected for any negative sum), the two
// weighted double fields became exact doubled integer sums (zigzag), and each
// edge gained a validity-flag varint (min ply exact / sample bounds exact /
// cached child frequency present). v5 files are still read and converted.
constexpr auto format_version = opening_tree_index_format_version;
constexpr auto legacy_format_version = opening_tree_index_legacy_format_version;
constexpr auto edge_flag_min_ply_valid = std::uint64_t {1};
constexpr auto edge_flag_samples_valid = std::uint64_t {2};
constexpr auto edge_flag_child_frequency = std::uint64_t {4};
constexpr auto edge_flag_mask = std::uint64_t {7};
constexpr auto header_node_count_offset = std::streamoff {22};
constexpr auto byte_bits = std::size_t {8};
constexpr auto byte_mask = std::uint64_t {0xff};
constexpr auto fixed_header_size = std::uintmax_t {30};
// hash(8) + game_count varint(>=1) + continuation_count varint(>=1) + complete varint(>=1)
constexpr auto minimum_node_size = std::uintmax_t {11};
constexpr auto final_write_buffer_size = std::size_t {1U << 16U};
constexpr auto per_game_scratch_reserve = std::size_t {32};
// Checkpoint reads: one bulk read per buffer instead of one istream::get() per
// byte. Point queries touch a single node, so they use a small buffer.
constexpr auto read_buffer_size = std::size_t {1U << 16U};
constexpr auto point_read_buffer_size = std::size_t {1U << 12U};

class buffered_tree_writer
{
  public:
    explicit buffered_tree_writer(std::ofstream& output) noexcept
        : output_ {&output}
    {
    }

    auto write_bytes(std::span<char const> const bytes) -> bool
    {
        return std::ranges::all_of(bytes, [this](char const byte) -> bool { return put(byte); });
    }

    template<typename Integer>
    auto write_fixed(Integer value) -> bool
    {
        static_assert(std::is_unsigned_v<Integer>);
        auto raw_value = std::uint64_t {value};
        for (std::size_t index = 0; index < sizeof(Integer); ++index) {
            if (!put(static_cast<char>(raw_value & byte_mask))) {
                return false;
            }
            raw_value >>= byte_bits;
        }
        return true;
    }

    auto write_varint(std::uint64_t value) -> bool
    {
        constexpr auto continuation_bit = std::uint8_t {0x80};
        constexpr auto payload_mask = std::uint8_t {0x7F};
        constexpr auto payload_bits = 7U;
        while (true) {
            auto byte = static_cast<std::uint8_t>(value & payload_mask);
            value >>= payload_bits;
            if (value != 0U) {
                byte |= continuation_bit;
            }
            if (!put(static_cast<char>(byte))) {
                return false;
            }
            if (value == 0U) {
                return true;
            }
        }
    }

    auto write_f64(double const value) -> bool { return write_fixed(std::bit_cast<std::uint64_t>(value)); }

    auto flush() -> bool
    {
        if (size_ != 0U) {
            output_->write(buffer_.data(), static_cast<std::streamsize>(size_));
            size_ = 0U;
        }
        return static_cast<bool>(*output_);
    }

  private:
    auto put(char const value) -> bool
    {
        if (size_ == buffer_.size() && !flush()) {
            return false;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index) -- size_ is checked against buffer_.size() above.
        buffer_[size_++] = value;
        return true;
    }

    std::ofstream* output_;
    std::array<char, final_write_buffer_size> buffer_ {};
    std::size_t size_ {};
};

// Sequential byte reader over a checkpoint file. Replaces std::ifstream::get()
// (a sentry construction per byte) and tellg() (an lseek per call) on the decode
// paths. A truncated file or read error surfaces as end of input, exactly as an
// eof from get() did.
class buffered_tree_reader
{
  public:
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- a file offset and a buffer size; both call sites pass named constants.
    buffered_tree_reader(std::ifstream input, std::uint64_t const start_offset, std::size_t const capacity)
        : input_ {std::move(input)}
        , buffer_(capacity)
        , buffer_offset_ {start_offset}
    {
    }

    auto get() -> std::optional<std::uint8_t>
    {
        if (cursor_ == filled_ && !refill()) {
            return std::nullopt;
        }
        return static_cast<std::uint8_t>(buffer_[cursor_++]);
    }

    // File offset of the next unread byte.
    [[nodiscard]] auto position() const noexcept -> std::uint64_t { return buffer_offset_ + cursor_; }

    auto at_end() -> bool { return cursor_ == filled_ && !refill(); }

  private:
    auto refill() -> bool
    {
        buffer_offset_ += filled_;
        cursor_ = 0;
        filled_ = 0;
        input_.read(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
        filled_ = static_cast<std::size_t>(input_.gcount());
        return filled_ != 0U;
    }

    std::ifstream input_;
    std::vector<char> buffer_;
    std::uint64_t buffer_offset_;
    std::size_t cursor_ {};
    std::size_t filled_ {};
};

template<typename Integer>
auto write_little_endian(std::ofstream& output, Integer value) -> bool
{
    static_assert(std::is_unsigned_v<Integer>);
    auto raw_value = std::uint64_t {value};
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        output.put(static_cast<char>(raw_value & byte_mask));
        if constexpr (sizeof(Integer) > 1U) {
            raw_value >>= byte_bits;
        }
    }
    return static_cast<bool>(output);
}

template<typename Integer>
auto read_little_endian(std::ifstream& input, Integer& value) -> bool
{
    static_assert(std::is_unsigned_v<Integer>);
    auto raw_value = std::uint64_t {0};
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        auto const byte = input.get();
        if (byte == std::char_traits<char>::eof()) {
            return false;
        }
        raw_value |= static_cast<std::uint64_t>(static_cast<unsigned char>(byte)) << (index * byte_bits);
    }
    value = static_cast<Integer>(raw_value);
    return true;
}

template<typename Integer>
auto read_little_endian(buffered_tree_reader& input, Integer& value) -> bool
{
    static_assert(std::is_unsigned_v<Integer>);
    auto raw_value = std::uint64_t {0};
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        auto const byte = input.get();
        if (!byte) {
            return false;
        }
        raw_value |= static_cast<std::uint64_t>(*byte) << (index * byte_bits);
    }
    value = static_cast<Integer>(raw_value);
    return true;
}

auto read_f64(buffered_tree_reader& input, double& value) -> bool
{
    std::uint64_t raw {};
    if (!read_little_endian(input, raw)) {
        return false;
    }
    value = std::bit_cast<double>(raw);
    return true;
}

auto read_varint(buffered_tree_reader& input, std::uint64_t& value) -> bool
{
    auto const decoded = decode_uleb128([&input]() -> std::optional<std::uint8_t> { return input.get(); });
    if (!decoded) {
        return false;
    }
    value = *decoded;
    return true;
}

// (root_hash, encoded_move, child_hash) -- one entry per distinct edge.
struct edge_key
{
    std::uint64_t root_hash {};
    std::uint16_t encoded_move {};
    std::uint64_t child_hash {};

    auto operator==(edge_key const&) const -> bool = default;
};

struct edge_key_hash
{
    auto operator()(edge_key const& key) const noexcept -> std::size_t
    {
        constexpr std::size_t phi = 0x9e3779b97f4a7c15ULL;
        constexpr std::size_t lshift = 12U;
        constexpr std::size_t rshift = 4U;
        auto seed = std::hash<std::uint64_t> {}(key.root_hash);
        seed ^= std::hash<std::uint64_t> {}(key.child_hash) + phi + (seed << lshift) + (seed >> rshift);
        seed ^= static_cast<std::size_t>(key.encoded_move) + phi + (seed << lshift) + (seed >> rshift);
        return seed;
    }
};

// In-memory accumulator for one edge, before the child's global
// transposition_frequency is known (attached in a second pass once every
// game has been replayed).
struct edge_agg
{
    std::uint16_t root_ply {std::numeric_limits<std::uint16_t>::max()};
    std::uint32_t frequency {};
    std::uint32_t white_wins {};
    std::uint32_t draws {};
    std::uint32_t black_wins {};
    std::uint32_t white_elo_count {};
    std::uint32_t black_elo_count {};
    std::int64_t white_elo_sum {};
    std::int64_t black_elo_sum {};
    std::int64_t numerator2 {};
    std::int64_t weight2 {};
    std::uint32_t eco_sample_min {std::numeric_limits<std::uint32_t>::max()};
    std::uint32_t eco_sample_max {0};
};

auto stats_to_agg(raw_edge_stats const& stats, std::uint16_t const root_ply, std::uint32_t const sample) -> edge_agg
{
    // One game's contribution: every field is 0 or 1 except the signed sums.
    return edge_agg {.root_ply = root_ply,
                     .frequency = static_cast<std::uint32_t>(stats.frequency),
                     .white_wins = static_cast<std::uint32_t>(stats.white_wins),
                     .draws = static_cast<std::uint32_t>(stats.draws),
                     .black_wins = static_cast<std::uint32_t>(stats.black_wins),
                     .white_elo_count = static_cast<std::uint32_t>(stats.white_count),
                     .black_elo_count = static_cast<std::uint32_t>(stats.black_count),
                     .white_elo_sum = stats.white_sum,
                     .black_elo_sum = stats.black_sum,
                     .numerator2 = stats.numerator2,
                     .weight2 = stats.weight2,
                     .eco_sample_min = sample,
                     .eco_sample_max = sample};
}

auto agg_to_stats(edge_agg const& agg) -> raw_edge_stats
{
    return raw_edge_stats {.frequency = agg.frequency,
                           .white_wins = agg.white_wins,
                           .draws = agg.draws,
                           .black_wins = agg.black_wins,
                           .white_sum = agg.white_elo_sum,
                           .white_count = agg.white_elo_count,
                           .black_sum = agg.black_elo_sum,
                           .black_count = agg.black_elo_count,
                           .numerator2 = agg.numerator2,
                           .weight2 = agg.weight2};
}

auto corrupt(char const* what) -> tl::unexpected<error>
{
    return tl::unexpected {error {error_code::io_failure, what}};
}

// The v5 writer stored signed sums as unsigned varints (two's complement), so a
// value above INT64_MAX is a negative sum; the population bound in
// validate_merged_edge rejects anything implausible.
auto v5_signed(std::uint64_t const raw) noexcept -> std::int64_t
{
    return static_cast<std::int64_t>(raw);
}

// A v5 weighted double is accepted only when 2x is a finite, integral, in-range
// value whose int64 round trip is exact; anything else is corruption, never
// silently rounded.
auto v5_doubled(double const value) noexcept -> std::optional<std::int64_t>
{
    constexpr auto two_pow_63 = 9223372036854775808.0;
    auto const doubled = value * 2.0;
    if (!std::isfinite(doubled) || doubled != std::trunc(doubled) || doubled >= two_pow_63 || doubled < -two_pow_63) {
        return std::nullopt;
    }
    auto const integer = static_cast<std::int64_t>(doubled);
    if (static_cast<double>(integer) != doubled) {
        return std::nullopt;
    }
    return integer;
}

constexpr auto u32_limit = std::uint64_t {std::numeric_limits<std::uint32_t>::max()};

// Reads and fully validates one v5 continuation, converting it to the raw row.
auto read_continuation_v5(buffered_tree_reader& input, merged_edge_context const& context) -> result<raw_edge>
{
    std::uint16_t encoded_move {};
    std::uint64_t child_hash {};
    double weighted_contrib_sum {};
    double elo_weight_sum {};
    std::uint64_t root_ply {};
    std::uint64_t frequency {};
    std::uint64_t white_wins {};
    std::uint64_t draws {};
    std::uint64_t black_wins {};
    std::uint64_t white_elo_sum {};
    std::uint64_t white_elo_count {};
    std::uint64_t black_elo_sum {};
    std::uint64_t black_elo_count {};
    std::uint64_t eco_sample_min {};
    std::uint64_t eco_sample_max {};
    std::uint64_t transposition_frequency {};
    auto const read_ok = read_little_endian(input, encoded_move) && read_little_endian(input, child_hash) && read_varint(input, root_ply)
        && read_varint(input, frequency) && read_varint(input, white_wins) && read_varint(input, draws) && read_varint(input, black_wins)
        && read_varint(input, white_elo_sum) && read_varint(input, white_elo_count) && read_varint(input, black_elo_sum)
        && read_varint(input, black_elo_count) && read_f64(input, weighted_contrib_sum) && read_f64(input, elo_weight_sum)
        && read_varint(input, eco_sample_min) && read_varint(input, eco_sample_max) && read_varint(input, transposition_frequency);
    auto const numerator2 = v5_doubled(weighted_contrib_sum);
    auto const weight2 = v5_doubled(elo_weight_sum);
    if (!read_ok || !numerator2 || !weight2 || root_ply > std::numeric_limits<std::uint16_t>::max() || frequency > u32_limit
        || white_wins > u32_limit || draws > u32_limit || black_wins > u32_limit || white_elo_count > u32_limit
        || black_elo_count > u32_limit || eco_sample_min > u32_limit || eco_sample_max > u32_limit || transposition_frequency > u32_limit)
    {
        return corrupt("invalid continuation record");
    }
    auto edge = raw_edge {
        .key = {.encoded_move = encoded_move, .child_hash = child_hash},
        .stats = {.frequency = static_cast<std::int64_t>(frequency),
                  .white_wins = static_cast<std::int64_t>(white_wins),
                  .draws = static_cast<std::int64_t>(draws),
                  .black_wins = static_cast<std::int64_t>(black_wins),
                  .white_sum = v5_signed(white_elo_sum),
                  .white_count = static_cast<std::int64_t>(white_elo_count),
                  .black_sum = v5_signed(black_elo_sum),
                  .black_count = static_cast<std::int64_t>(black_elo_count),
                  .numerator2 = *numerator2,
                  .weight2 = *weight2},
        .min_root_ply = static_cast<std::uint16_t>(root_ply),
        .min_root_ply_valid = true,
        .sample_min = static_cast<std::uint32_t>(eco_sample_min),
        .sample_max = static_cast<std::uint32_t>(eco_sample_max),
        .sample_bounds_valid = true,
        .child_frequency = static_cast<std::uint32_t>(transposition_frequency),
    };
    if (auto valid = validate_merged_edge(edge, context); !valid) {
        return tl::unexpected {valid.error()};
    }
    return edge;
}

// Reads and fully validates one v6 continuation.
auto read_continuation_v6(buffered_tree_reader& input, merged_edge_context const& context) -> result<raw_edge>
{
    std::uint16_t encoded_move {};
    std::uint64_t child_hash {};
    std::uint64_t flags {};
    std::uint64_t root_ply {};
    std::uint64_t frequency {};
    std::uint64_t white_wins {};
    std::uint64_t draws {};
    std::uint64_t black_wins {};
    std::uint64_t white_sum {};
    std::uint64_t white_count {};
    std::uint64_t black_sum {};
    std::uint64_t black_count {};
    std::uint64_t numerator2 {};
    std::uint64_t weight2 {};
    std::uint64_t sample_min {};
    std::uint64_t sample_max {};
    std::uint64_t child_frequency {};
    auto const read_ok = read_little_endian(input, encoded_move) && read_little_endian(input, child_hash) && read_varint(input, flags)
        && read_varint(input, root_ply) && read_varint(input, frequency) && read_varint(input, white_wins) && read_varint(input, draws)
        && read_varint(input, black_wins) && read_varint(input, white_sum) && read_varint(input, white_count)
        && read_varint(input, black_sum) && read_varint(input, black_count) && read_varint(input, numerator2) && read_varint(input, weight2)
        && read_varint(input, sample_min) && read_varint(input, sample_max) && read_varint(input, child_frequency);
    if (!read_ok || (flags & ~edge_flag_mask) != 0U || root_ply > std::numeric_limits<std::uint16_t>::max() || frequency > u32_limit
        || white_wins > u32_limit || draws > u32_limit || black_wins > u32_limit || white_count > u32_limit || black_count > u32_limit
        || sample_min > u32_limit || sample_max > u32_limit || child_frequency > u32_limit)
    {
        return corrupt("invalid continuation record");
    }
    auto const min_ply_valid = (flags & edge_flag_min_ply_valid) != 0U;
    auto const samples_valid = (flags & edge_flag_samples_valid) != 0U;
    auto const has_child_frequency = (flags & edge_flag_child_frequency) != 0U;
    // Canonical form: a field whose flag is clear is zero.
    if ((!min_ply_valid && root_ply != 0U) || (!samples_valid && (sample_min != 0U || sample_max != 0U))
        || (!has_child_frequency && child_frequency != 0U))
    {
        return corrupt("non-canonical continuation record");
    }
    auto edge = raw_edge {
        .key = {.encoded_move = encoded_move, .child_hash = child_hash},
        .stats = {.frequency = static_cast<std::int64_t>(frequency),
                  .white_wins = static_cast<std::int64_t>(white_wins),
                  .draws = static_cast<std::int64_t>(draws),
                  .black_wins = static_cast<std::int64_t>(black_wins),
                  .white_sum = zigzag_decode(white_sum),
                  .white_count = static_cast<std::int64_t>(white_count),
                  .black_sum = zigzag_decode(black_sum),
                  .black_count = static_cast<std::int64_t>(black_count),
                  .numerator2 = zigzag_decode(numerator2),
                  .weight2 = zigzag_decode(weight2)},
        .min_root_ply = static_cast<std::uint16_t>(root_ply),
        .min_root_ply_valid = min_ply_valid,
        .sample_min = static_cast<std::uint32_t>(sample_min),
        .sample_max = static_cast<std::uint32_t>(sample_max),
        .sample_bounds_valid = samples_valid,
        .child_frequency = has_child_frequency ? std::optional<std::uint32_t> {static_cast<std::uint32_t>(child_frequency)} : std::nullopt,
    };
    if (auto valid = validate_merged_edge(edge, context); !valid) {
        return tl::unexpected {valid.error()};
    }
    return edge;
}

auto read_continuation(buffered_tree_reader& input, std::uint32_t const version, merged_edge_context const& context) -> result<raw_edge>
{
    return version == legacy_format_version ? read_continuation_v5(input, context) : read_continuation_v6(input, context);
}

// Whether every field of an edge fits the v6 record (counts are u32, signed
// sums are i64). Checked before writing so a bad row can never be stored.
auto edge_is_encodable(raw_edge const& edge) noexcept -> bool
{
    auto const& stats = edge.stats;
    auto const in_u32 = [](std::int64_t const value) noexcept -> bool { return value >= 0 && std::cmp_less_equal(value, u32_limit); };
    return in_u32(stats.frequency) && in_u32(stats.white_wins) && in_u32(stats.draws) && in_u32(stats.black_wins)
        && in_u32(stats.white_count) && in_u32(stats.black_count);
}

auto write_continuation(buffered_tree_writer& output, raw_edge const& edge) -> bool
{
    auto const& stats = edge.stats;
    auto flags = std::uint64_t {0};
    if (edge.min_root_ply_valid) {
        flags |= edge_flag_min_ply_valid;
    }
    if (edge.sample_bounds_valid) {
        flags |= edge_flag_samples_valid;
    }
    if (edge.child_frequency) {
        flags |= edge_flag_child_frequency;
    }
    return output.write_fixed(edge.key.encoded_move) && output.write_fixed(edge.key.child_hash) && output.write_varint(flags)
        && output.write_varint(edge.min_root_ply_valid ? edge.min_root_ply : std::uint64_t {0})
        && output.write_varint(static_cast<std::uint64_t>(stats.frequency))
        && output.write_varint(static_cast<std::uint64_t>(stats.white_wins)) && output.write_varint(static_cast<std::uint64_t>(stats.draws))
        && output.write_varint(static_cast<std::uint64_t>(stats.black_wins)) && output.write_varint(zigzag_encode(stats.white_sum))
        && output.write_varint(static_cast<std::uint64_t>(stats.white_count)) && output.write_varint(zigzag_encode(stats.black_sum))
        && output.write_varint(static_cast<std::uint64_t>(stats.black_count)) && output.write_varint(zigzag_encode(stats.numerator2))
        && output.write_varint(zigzag_encode(stats.weight2))
        && output.write_varint(edge.sample_bounds_valid ? edge.sample_min : std::uint64_t {0})
        && output.write_varint(edge.sample_bounds_valid ? edge.sample_max : std::uint64_t {0})
        && output.write_varint(edge.child_frequency.value_or(0U));
}

// Bounded-memory external-merge counter: counts distinct-game visits per
// hash (callers call visit() at most once per game per hash). Unlike the
// ply-capped edge map below, transposition_frequency spans every ply of
// every game, so this can't be a flat in-memory map. Generalizes
// position_prefix_postings.cpp's spill/k-way-merge to counting, not dedup.
class hash_visit_counter
{
  public:
    hash_visit_counter(std::filesystem::path scratch_prefix,
                       std::size_t const spill_threshold,
                       opening_tree_index_build_metrics* const metrics)
        : scratch_prefix_ {std::move(scratch_prefix)}
        , spill_threshold_ {spill_threshold}
        , metrics_ {metrics}
    {
    }

    // Best-effort cleanup of spill runs left behind by an error return from
    // spill_current_buffer()/finalize() -- a no-op on the success path,
    // which already removes and clears run_paths_.
    ~hash_visit_counter()
    {
        for (auto const& run_path : run_paths_) {
            std::error_code remove_err;
            std::filesystem::remove(run_path, remove_err);
        }
    }

    hash_visit_counter(hash_visit_counter const&) = delete;
    auto operator=(hash_visit_counter const&) -> hash_visit_counter& = delete;
    hash_visit_counter(hash_visit_counter&&) = delete;
    auto operator=(hash_visit_counter&&) -> hash_visit_counter& = delete;

    auto visit(std::uint64_t const hash) -> result<void>
    {
        buffer_.push_back(hash);
        ++metrics_->child_visit_count;
        if (buffer_.size() < spill_threshold_) {
            return {};
        }
        return spill_current_buffer();
    }

    // Merges every distinct-game visit into a sorted on-disk (hash, count)
    // stream. Keeping this stream on disk avoids making its cardinality part
    // of the builder's peak memory.
    [[nodiscard]] auto finalize(std::filesystem::path const& output_path) -> result<void>;

  private:
    [[nodiscard]] auto spill_current_buffer() -> result<void>;

    std::filesystem::path scratch_prefix_;
    std::size_t spill_threshold_;
    std::vector<std::uint64_t> buffer_;
    std::vector<std::filesystem::path> run_paths_;
    opening_tree_index_build_metrics* metrics_;
};

// Sorts and run-length-encodes the buffer into (hash, count) pairs before
// writing -- a hot position (e.g. the start position) visited by millions of
// games would otherwise spill millions of duplicate raw hash values instead
// of one pair.
auto hash_visit_counter::spill_current_buffer() -> result<void>
{
    if (buffer_.empty()) {
        return {};
    }

    std::ranges::sort(buffer_);
    auto const run_path = std::filesystem::path {scratch_prefix_.string() + ".spill" + std::to_string(run_paths_.size())};
    run_paths_.push_back(run_path);
    std::ofstream output {run_path, std::ios::binary | std::ios::trunc};
    auto unique_count = std::uint64_t {0};
    for (std::size_t index = 0; index < buffer_.size();) {
        ++unique_count;
        auto const hash = buffer_[index];
        while (index < buffer_.size() && buffer_[index] == hash) {
            ++index;
        }
    }
    if (!output || !write_little_endian(output, unique_count)) {
        return tl::unexpected {error {error_code::io_failure, "spill write failed"}};
    }
    for (std::size_t index = 0; index < buffer_.size();) {
        auto const hash = buffer_[index];
        auto count = std::uint32_t {0};
        while (index < buffer_.size() && buffer_[index] == hash) {
            if (count == std::numeric_limits<std::uint32_t>::max()) {
                return tl::unexpected {error {error_code::io_failure, "child count overflow"}};
            }
            ++count;
            ++index;
        }
        if (!write_little_endian(output, hash) || !write_little_endian(output, count)) {
            return tl::unexpected {error {error_code::io_failure, "spill write failed"}};
        }
    }
    if (!output) {
        return tl::unexpected {error {error_code::io_failure, "spill write failed"}};
    }

    buffer_.clear();
    ++metrics_->child_spill_run_count;
    return {};
}

struct hash_count_run_cursor
{
    std::ifstream input;
    std::uint64_t remaining {};
    std::uint64_t current_hash {};
    std::uint32_t current_count {};
};

auto advance_run_cursor(hash_count_run_cursor& cursor) -> result<bool>
{
    if (cursor.remaining == 0U) {
        return false;
    }
    if (!read_little_endian(cursor.input, cursor.current_hash) || !read_little_endian(cursor.input, cursor.current_count)) {
        return tl::unexpected {error {error_code::io_failure, "spill read failed"}};
    }
    --cursor.remaining;
    return true;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- k-way merge w/ per-step checks, mirrors position_prefix_postings.cpp
auto hash_visit_counter::finalize(std::filesystem::path const& output_path) -> result<void>
{
    if (auto spill_res = spill_current_buffer(); !spill_res) {
        return tl::unexpected {spill_res.error()};
    }

    std::vector<hash_count_run_cursor> cursors;
    cursors.reserve(run_paths_.size());
    for (auto const& run_path : run_paths_) {
        hash_count_run_cursor cursor {.input = std::ifstream {run_path, std::ios::binary}};
        if (!cursor.input || !read_little_endian(cursor.input, cursor.remaining)) {
            return tl::unexpected {error {error_code::io_failure, "spill read failed"}};
        }
        cursors.push_back(std::move(cursor));
    }

    auto compare = [&cursors](std::size_t const lhs, std::size_t const rhs) -> bool
    { return cursors[rhs].current_hash < cursors[lhs].current_hash; };
    std::vector<std::size_t> heap;
    heap.reserve(cursors.size());
    for (std::size_t index = 0; index < cursors.size(); ++index) {
        auto const has_record = advance_run_cursor(cursors[index]);
        if (!has_record) {
            return tl::unexpected {has_record.error()};
        }
        if (*has_record) {
            heap.push_back(index);
        }
    }
    std::ranges::make_heap(heap, compare);

    auto const temp_path = std::filesystem::path {output_path.string() + ".tmp"};
    std::ofstream output {temp_path, std::ios::binary | std::ios::trunc};
    if (!output || !write_little_endian(output, std::uint64_t {0})) {
        return tl::unexpected {error {error_code::io_failure, "spill write failed"}};
    }
    auto merged_count = std::uint64_t {0};
    auto current_hash = std::uint64_t {0};
    auto current_sum = std::uint32_t {0};
    auto has_current = false;

    auto flush_current = [&]() -> bool
    {
        if (!has_current) {
            return true;
        }
        ++merged_count;
        return write_little_endian(output, current_hash) && write_little_endian(output, current_sum);
    };

    while (!heap.empty()) {
        std::ranges::pop_heap(heap, compare);
        auto const index = heap.back();
        heap.pop_back();

        auto const hash = cursors[index].current_hash;
        if (!has_current || hash != current_hash) {
            if (!flush_current()) {
                return tl::unexpected {error {error_code::io_failure, "spill write failed"}};
            }
            current_hash = hash;
            current_sum = 0;
            has_current = true;
        }
        current_sum += cursors[index].current_count;

        auto const has_next = advance_run_cursor(cursors[index]);
        if (!has_next) {
            return tl::unexpected {has_next.error()};
        }
        if (*has_next) {
            heap.push_back(index);
            std::ranges::push_heap(heap, compare);
        }
    }
    if (!flush_current()) {
        return tl::unexpected {error {error_code::io_failure, "spill write failed"}};
    }
    output.seekp(0);
    if (!write_little_endian(output, merged_count)) {
        return tl::unexpected {error {error_code::io_failure, "spill write failed"}};
    }
    output.close();
    if (!output) {
        return tl::unexpected {error {error_code::io_failure, "spill write failed"}};
    }
    std::error_code rename_error;
    std::filesystem::rename(temp_path, output_path, rename_error);
    if (rename_error) {
        std::filesystem::remove(temp_path, rename_error);
        return tl::unexpected {error {error_code::io_failure, "spill publish failed"}};
    }

    for (auto const& run_path : run_paths_) {
        std::error_code remove_err;
        std::filesystem::remove(run_path, remove_err);
    }
    run_paths_.clear();

    return {};
}

auto materialize_child_counts(opening_tree_child_count_stream const& stream, std::filesystem::path const& output_path) -> result<void>
{
    auto const temp_path = std::filesystem::path {output_path.string() + ".tmp"};
    std::ofstream output {temp_path, std::ios::binary | std::ios::trunc};
    if (!output || !write_little_endian(output, std::uint64_t {0})) {
        return tl::unexpected {error {error_code::io_failure, "child count write failed"}};
    }

    auto count = std::uint64_t {0};
    auto previous_hash = std::uint64_t {0};
    auto has_previous = false;
    auto streamed = stream(
        [&](zobrist_hash const hash, std::uint32_t const frequency) -> result<void>
        {
            if (frequency == 0U || (has_previous && hash.value <= previous_hash) || !write_little_endian(output, hash.value)
                || !write_little_endian(output, frequency))
            {
                return tl::unexpected {error_code::io_failure};
            }
            previous_hash = hash.value;
            has_previous = true;
            ++count;
            return {};
        });
    if (!streamed) {
        output.close();
        std::error_code ignored;
        std::filesystem::remove(temp_path, ignored);
        return tl::unexpected {streamed.error()};
    }
    output.seekp(0);
    if (!write_little_endian(output, count)) {
        output.close();
        std::error_code ignored;
        std::filesystem::remove(temp_path, ignored);
        return tl::unexpected {error {error_code::io_failure, "child count write failed"}};
    }
    output.close();
    if (!output) {
        std::error_code ignored;
        std::filesystem::remove(temp_path, ignored);
        return tl::unexpected {error {error_code::io_failure, "child count write failed"}};
    }
    std::error_code rename_error;
    std::filesystem::rename(temp_path, output_path, rename_error);
    if (rename_error) {
        std::filesystem::remove(temp_path, rename_error);
        return tl::unexpected {error {error_code::io_failure, "child count publish failed"}};
    }
    return {};
}

struct edge_record
{
    edge_key key;
    edge_agg agg;
};

auto edge_less(edge_record const& lhs, edge_record const& rhs) -> bool
{
    return std::tie(lhs.key.root_hash, lhs.key.encoded_move, lhs.key.child_hash)
        < std::tie(rhs.key.root_hash, rhs.key.encoded_move, rhs.key.child_hash);
}

auto write_edge_record(std::ofstream& output, edge_record const& record) -> bool
{
    auto const& agg = record.agg;
    return write_little_endian(output, record.key.root_hash) && write_little_endian(output, record.key.encoded_move)
        && write_little_endian(output, record.key.child_hash) && write_little_endian(output, agg.root_ply)
        && write_little_endian(output, agg.frequency) && write_little_endian(output, agg.white_wins)
        && write_little_endian(output, agg.draws) && write_little_endian(output, agg.black_wins)
        && write_little_endian(output, static_cast<std::uint64_t>(agg.white_elo_sum)) && write_little_endian(output, agg.white_elo_count)
        && write_little_endian(output, static_cast<std::uint64_t>(agg.black_elo_sum)) && write_little_endian(output, agg.black_elo_count)
        && write_little_endian(output, static_cast<std::uint64_t>(agg.numerator2))
        && write_little_endian(output, static_cast<std::uint64_t>(agg.weight2)) && write_little_endian(output, agg.eco_sample_min)
        && write_little_endian(output, agg.eco_sample_max);
}

auto read_edge_record(std::ifstream& input, edge_record& record) -> bool
{
    auto& agg = record.agg;
    std::uint64_t white_elo_sum {};
    std::uint64_t black_elo_sum {};
    std::uint64_t numerator2 {};
    std::uint64_t weight2 {};
    if (!read_little_endian(input, record.key.root_hash) || !read_little_endian(input, record.key.encoded_move)
        || !read_little_endian(input, record.key.child_hash) || !read_little_endian(input, agg.root_ply)
        || !read_little_endian(input, agg.frequency) || !read_little_endian(input, agg.white_wins) || !read_little_endian(input, agg.draws)
        || !read_little_endian(input, agg.black_wins) || !read_little_endian(input, white_elo_sum)
        || !read_little_endian(input, agg.white_elo_count) || !read_little_endian(input, black_elo_sum)
        || !read_little_endian(input, agg.black_elo_count) || !read_little_endian(input, numerator2) || !read_little_endian(input, weight2)
        || !read_little_endian(input, agg.eco_sample_min) || !read_little_endian(input, agg.eco_sample_max))
    {
        return false;
    }
    agg.white_elo_sum = static_cast<std::int64_t>(white_elo_sum);
    agg.black_elo_sum = static_cast<std::int64_t>(black_elo_sum);
    agg.numerator2 = static_cast<std::int64_t>(numerator2);
    agg.weight2 = static_cast<std::int64_t>(weight2);
    return true;
}

// Spill-run merge of two partial aggregates of the same edge. Sums stay within
// i64 by the supported-domain bound (|sum| <= 2^17 * 2^32 games), so no
// checked arithmetic is needed on this trusted scratch path.
void merge_edge_agg(edge_agg& target, edge_agg const& source)
{
    target.root_ply = std::min(target.root_ply, source.root_ply);
    target.frequency += source.frequency;
    target.white_wins += source.white_wins;
    target.draws += source.draws;
    target.black_wins += source.black_wins;
    target.white_elo_sum += source.white_elo_sum;
    target.white_elo_count += source.white_elo_count;
    target.black_elo_sum += source.black_elo_sum;
    target.black_elo_count += source.black_elo_count;
    target.numerator2 += source.numerator2;
    target.weight2 += source.weight2;
    target.eco_sample_min = std::min(target.eco_sample_min, source.eco_sample_min);
    target.eco_sample_max = std::max(target.eco_sample_max, source.eco_sample_max);
}

class build_spills
{
  public:
    build_spills(std::filesystem::path prefix, std::size_t threshold, opening_tree_index_build_metrics* const metrics)
        : prefix_ {std::move(prefix)}
        , threshold_ {threshold}
        , metrics_ {metrics}
    {
    }

    ~build_spills();
    build_spills(build_spills const&) = delete;
    auto operator=(build_spills const&) -> build_spills& = delete;
    build_spills(build_spills&&) = delete;
    auto operator=(build_spills&&) -> build_spills& = delete;
    auto add_root(std::uint64_t hash) -> result<void>;
    auto add_edge(edge_record record) -> result<void>;
    auto merge_roots(std::filesystem::path const& output_path) -> result<std::uint64_t>;
    auto merge_edges(std::filesystem::path const& output_path) -> result<void>;

  private:
    auto spill_roots() -> result<void>;
    auto spill_edges() -> result<void>;
    std::filesystem::path prefix_;
    std::size_t threshold_;
    std::vector<std::uint64_t> roots_;
    std::vector<edge_record> edges_;
    std::vector<std::filesystem::path> root_runs_;
    std::vector<std::filesystem::path> edge_runs_;
    opening_tree_index_build_metrics* metrics_;
};

build_spills::~build_spills()
{
    for (auto const& path : root_runs_) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    for (auto const& path : edge_runs_) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
}

auto build_spills::add_root(std::uint64_t const hash) -> result<void>
{
    roots_.push_back(hash);
    ++metrics_->root_record_count;
    return roots_.size() >= threshold_ ? spill_roots() : result<void> {};
}

auto build_spills::add_edge(edge_record record) -> result<void>
{
    edges_.push_back(record);
    ++metrics_->edge_record_count;
    return edges_.size() >= threshold_ ? spill_edges() : result<void> {};
}

auto build_spills::spill_roots() -> result<void>
{
    if (roots_.empty()) {
        return {};
    }
    std::ranges::sort(roots_);
    auto const path = std::filesystem::path {prefix_.string() + ".roots." + std::to_string(root_runs_.size())};
    root_runs_.push_back(path);
    std::ofstream output {path, std::ios::binary | std::ios::trunc};
    auto unique_count = std::uint64_t {0};
    for (std::size_t index = 0; index < roots_.size();) {
        ++unique_count;
        auto const hash = roots_[index];
        while (index < roots_.size() && roots_[index] == hash) {
            ++index;
        }
    }
    if (!output || !write_little_endian(output, unique_count)) {
        return tl::unexpected {error {error_code::io_failure, "spill write failed"}};
    }
    for (std::size_t index = 0; index < roots_.size();) {
        auto const hash = roots_[index];
        auto count = std::uint32_t {0};
        while (index < roots_.size() && roots_[index] == hash) {
            if (count == std::numeric_limits<std::uint32_t>::max()) {
                return tl::unexpected {error {error_code::io_failure, "root count overflow"}};
            }
            ++count;
            ++index;
        }
        if (!write_little_endian(output, hash) || !write_little_endian(output, count)) {
            return tl::unexpected {error {error_code::io_failure, "spill write failed"}};
        }
    }
    if (!output) {
        return tl::unexpected {error {error_code::io_failure, "spill write failed"}};
    }
    ++metrics_->root_spill_run_count;
    roots_.clear();
    return {};
}

auto build_spills::spill_edges() -> result<void>
{
    if (edges_.empty()) {
        return {};
    }
    std::ranges::sort(edges_, edge_less);
    auto write_index = std::size_t {0};
    for (auto const& record : edges_) {
        if (write_index == 0U || edges_[write_index - 1U].key != record.key) {
            edges_[write_index++] = record;
        } else {
            merge_edge_agg(edges_[write_index - 1U].agg, record.agg);
        }
    }
    edges_.resize(write_index);
    auto const path = std::filesystem::path {prefix_.string() + ".edges." + std::to_string(edge_runs_.size())};
    edge_runs_.push_back(path);
    std::ofstream output {path, std::ios::binary | std::ios::trunc};
    if (!output || !write_little_endian(output, static_cast<std::uint64_t>(edges_.size()))) {
        return tl::unexpected {error {error_code::io_failure, "spill write failed"}};
    }
    for (auto const& record : edges_) {
        if (!write_edge_record(output, record)) {
            return tl::unexpected {error {error_code::io_failure, "spill write failed"}};
        }
    }
    if (!output) {
        return tl::unexpected {error {error_code::io_failure, "spill write failed"}};
    }
    ++metrics_->edge_spill_run_count;
    edges_.clear();
    return {};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- streaming k-way merge validates every cursor transition.
auto build_spills::merge_roots(std::filesystem::path const& output_path) -> result<std::uint64_t>
{
    if (auto result = spill_roots(); !result) {
        return tl::unexpected {result.error()};
    }

    struct cursor
    {
        std::ifstream input;
        std::uint64_t remaining {};
        std::uint64_t current {};
        std::uint32_t current_count {};
    };

    std::vector<cursor> cursors;
    for (auto const& path : root_runs_) {
        cursor item {.input = std::ifstream {path, std::ios::binary}};
        if (!item.input || !read_little_endian(item.input, item.remaining)) {
            return tl::unexpected {error {error_code::io_failure, "spill read failed"}};
        }
        cursors.push_back(std::move(item));
    }
    auto advance = [](cursor& item) -> result<bool>
    {
        if (item.remaining == 0U) {
            return false;
        }
        if (!read_little_endian(item.input, item.current) || !read_little_endian(item.input, item.current_count)) {
            return tl::unexpected {error {error_code::io_failure, "spill read failed"}};
        }
        --item.remaining;
        return true;
    };
    auto compare = [&cursors](std::size_t lhs, std::size_t rhs) -> bool { return cursors[rhs].current < cursors[lhs].current; };
    std::vector<std::size_t> heap;
    for (std::size_t index = 0; index < cursors.size(); ++index) {
        auto has = advance(cursors[index]);
        if (!has) {
            return tl::unexpected {has.error()};
        }
        if (*has) {
            heap.push_back(index);
        }
    }
    std::ranges::make_heap(heap, compare);
    std::ofstream output {output_path, std::ios::binary | std::ios::trunc};
    if (!output || !write_little_endian(output, std::uint64_t {0})) {
        return tl::unexpected {error {error_code::io_failure, "spill write failed"}};
    }
    std::uint64_t count {};
    while (!heap.empty()) {
        std::ranges::pop_heap(heap, compare);
        auto index = heap.back();
        heap.pop_back();
        auto const hash = cursors[index].current;
        auto games = std::uint64_t {0};
        while (true) {
            games += cursors[index].current_count;
            if (games > std::numeric_limits<std::uint32_t>::max()) {
                return tl::unexpected {error {error_code::io_failure, "root count overflow"}};
            }
            auto has = advance(cursors[index]);
            if (!has) {
                return tl::unexpected {has.error()};
            }
            if (*has) {
                heap.push_back(index);
                std::ranges::push_heap(heap, compare);
            }
            if (heap.empty() || cursors[heap.front()].current != hash) {
                break;
            }
            std::ranges::pop_heap(heap, compare);
            index = heap.back();
            heap.pop_back();
        }
        if (!write_little_endian(output, hash) || !write_little_endian(output, static_cast<std::uint32_t>(games))) {
            return tl::unexpected {error {error_code::io_failure, "spill write failed"}};
        }
        ++count;
    }
    output.seekp(0);
    if (!write_little_endian(output, count)) {
        return tl::unexpected {error {error_code::io_failure, "spill write failed"}};
    }
    output.close();
    if (!output) {
        return tl::unexpected {error {error_code::io_failure, "spill write failed"}};
    }
    return count;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- streaming k-way merge validates every cursor transition.
auto build_spills::merge_edges(std::filesystem::path const& output_path) -> result<void>
{
    if (auto result = spill_edges(); !result) {
        return result;
    }

    struct cursor
    {
        std::ifstream input;
        std::uint64_t remaining {};
        edge_record current {};
    };

    std::vector<cursor> cursors;
    for (auto const& path : edge_runs_) {
        cursor item {.input = std::ifstream {path, std::ios::binary}};
        if (!item.input || !read_little_endian(item.input, item.remaining)) {
            return tl::unexpected {error {error_code::io_failure, "spill read failed"}};
        }
        cursors.push_back(std::move(item));
    }
    auto advance = [](cursor& item) -> result<bool>
    {
        if (item.remaining == 0U) {
            return false;
        }
        if (!read_edge_record(item.input, item.current)) {
            return tl::unexpected {error {error_code::io_failure, "spill read failed"}};
        }
        --item.remaining;
        return true;
    };
    auto compare = [&cursors](std::size_t lhs, std::size_t rhs) -> bool { return edge_less(cursors[rhs].current, cursors[lhs].current); };
    std::vector<std::size_t> heap;
    for (std::size_t index = 0; index < cursors.size(); ++index) {
        auto has = advance(cursors[index]);
        if (!has) {
            return tl::unexpected {has.error()};
        }
        if (*has) {
            heap.push_back(index);
        }
    }
    std::ranges::make_heap(heap, compare);
    std::ofstream output {output_path, std::ios::binary | std::ios::trunc};
    if (!output || !write_little_endian(output, std::uint64_t {0})) {
        return tl::unexpected {error {error_code::io_failure, "spill write failed"}};
    }
    auto count = std::uint64_t {0};
    while (!heap.empty()) {
        std::ranges::pop_heap(heap, compare);
        auto index = heap.back();
        heap.pop_back();
        auto merged = cursors[index].current;
        auto has = advance(cursors[index]);
        if (!has) {
            return tl::unexpected {has.error()};
        }
        if (*has) {
            heap.push_back(index);
            std::ranges::push_heap(heap, compare);
        }
        while (!heap.empty() && cursors[heap.front()].current.key == merged.key) {
            std::ranges::pop_heap(heap, compare);
            index = heap.back();
            heap.pop_back();
            merge_edge_agg(merged.agg, cursors[index].current.agg);
            has = advance(cursors[index]);
            if (!has) {
                return tl::unexpected {has.error()};
            }
            if (*has) {
                heap.push_back(index);
                std::ranges::push_heap(heap, compare);
            }
        }
        if (!write_edge_record(output, merged)) {
            return tl::unexpected {error {error_code::io_failure, "spill write failed"}};
        }
        ++count;
    }
    output.seekp(0);
    if (!write_little_endian(output, count)) {
        return tl::unexpected {error {error_code::io_failure, "spill write failed"}};
    }
    output.close();
    if (!output) {
        return tl::unexpected {error {error_code::io_failure, "spill write failed"}};
    }
    return {};
}

// Replays one game into `spills` and `visits`: `visits`
// (transposition_frequency) is uncapped, the edge set is capped at
// max_root_ply because root positions at the boundary still contribute their
// immediate continuation, except the canonical starting position (see
// tree_contribution_enumerator for the exact rule, shared with patch producers).
auto accumulate_game(replay_game_record const& current_game,
                     tree_contribution_enumerator& enumerator,
                     hash_visit_counter* const visits,
                     build_spills& spills) -> result<void>
{
    auto const stats = make_game_stats(encode_result_code(current_game.result), current_game.white_elo, current_game.black_elo);
    if (!stats) {
        return tl::unexpected {stats.error()};
    }
    if (auto enumerated = enumerator.enumerate(current_game.moves); !enumerated) {
        return enumerated;
    }
    if (visits != nullptr) {
        for (auto const hash : enumerator.visited()) {
            if (auto visit_res = visits->visit(hash); !visit_res) {
                return visit_res;
            }
        }
    }
    for (auto const root_hash : enumerator.roots()) {
        if (auto result = spills.add_root(root_hash); !result) {
            return result;
        }
    }
    for (auto const& edge : enumerator.edges()) {
        auto record = edge_record {.key = {.root_hash = edge.root_hash, .encoded_move = edge.encoded_move, .child_hash = edge.child_hash},
                                   .agg = stats_to_agg(*stats, edge.min_root_ply, current_game.id.value)};
        if (auto result = spills.add_edge(record); !result) {
            return result;
        }
    }
    return {};
}

struct child_frequency_record
{
    std::uint64_t hash {};
    std::uint32_t frequency {};
};

// NOLINTBEGIN(bugprone-easily-swappable-parameters) -- count indexes a stream while hash is its lookup key.
auto child_frequency(std::ifstream& counts, std::uint64_t const count, std::uint64_t const hash, opening_tree_index_build_metrics& metrics)
    -> result<std::uint32_t>
{
    ++metrics.child_frequency_lookup_count;
    constexpr auto record_size = std::streamoff {sizeof(std::uint64_t) + sizeof(std::uint32_t)};
    auto lower = std::uint64_t {0};
    auto upper = count;
    while (lower < upper) {
        auto const middle = lower + ((upper - lower) / 2U);
        counts.clear();
        counts.seekg(static_cast<std::streamoff>(sizeof(std::uint64_t)) + (static_cast<std::streamoff>(middle) * record_size));
        std::uint64_t candidate {};
        std::uint32_t frequency {};
        if (!counts || !read_little_endian(counts, candidate) || !read_little_endian(counts, frequency)) {
            return tl::unexpected {error {error_code::io_failure, "spill read failed"}};
        }
        if (candidate < hash) {
            lower = middle + 1U;
        } else {
            upper = middle;
        }
    }
    if (lower == count) {
        return tl::unexpected {error {error_code::io_failure, "missing child frequency"}};
    }
    counts.clear();
    counts.seekg(static_cast<std::streamoff>(sizeof(std::uint64_t)) + (static_cast<std::streamoff>(lower) * record_size));
    std::uint64_t candidate {};
    std::uint32_t frequency {};
    if (!counts || !read_little_endian(counts, candidate) || !read_little_endian(counts, frequency)) {
        return tl::unexpected {error {error_code::io_failure, "spill read failed"}};
    }
    if (candidate != hash) {
        return tl::unexpected {error {error_code::io_failure, "missing child frequency"}};
    }
    return frequency;
}

// NOLINTEND(bugprone-easily-swappable-parameters)

struct referenced_child_frequencies
{
    std::vector<std::uint64_t> hashes;
    std::vector<std::uint32_t> frequencies;
};

// NOLINTBEGIN(bugprone-easily-swappable-parameters) -- each count indexes its distinct input stream.
auto load_referenced_child_frequencies(std::filesystem::path const& edges_path,
                                       std::ifstream& counts,
                                       std::uint64_t const child_count,
                                       std::uint64_t const edge_count,
                                       std::size_t const memory_limit_bytes,
                                       opening_tree_index_build_metrics& metrics) -> result<referenced_child_frequencies>
{
    constexpr auto bytes_per_referenced_edge = sizeof(std::uint64_t) + sizeof(std::uint32_t);
    if (edge_count > memory_limit_bytes / bytes_per_referenced_edge) {
        return referenced_child_frequencies {};
    }
    auto const started = std::chrono::steady_clock::now();
    std::ifstream edges {edges_path, std::ios::binary};
    std::uint64_t stored_edge_count {};
    if (!edges || !read_little_endian(edges, stored_edge_count) || stored_edge_count != edge_count) {
        return tl::unexpected {error {error_code::io_failure, "spill read failed"}};
    }
    auto referenced = referenced_child_frequencies {};
    referenced.hashes.reserve(static_cast<std::size_t>(edge_count));
    for (auto edge_index = std::uint64_t {0}; edge_index < edge_count; ++edge_index) {
        auto edge = edge_record {};
        if (!read_edge_record(edges, edge)) {
            return tl::unexpected {error {error_code::io_failure, "spill read failed"}};
        }
        referenced.hashes.push_back(edge.key.child_hash);
    }
    std::ranges::sort(referenced.hashes);
    auto const unique_end = std::ranges::unique(referenced.hashes);
    referenced.hashes.erase(unique_end.begin(), unique_end.end());
    referenced.frequencies.assign(referenced.hashes.size(), std::uint32_t {0});
    counts.clear();
    counts.seekg(static_cast<std::streamoff>(sizeof(std::uint64_t)));
    auto referenced_index = std::size_t {0};
    for (auto index = std::uint64_t {0}; index < child_count && referenced_index < referenced.hashes.size(); ++index) {
        auto hash = std::uint64_t {};
        auto frequency = std::uint32_t {};
        if (!read_little_endian(counts, hash) || !read_little_endian(counts, frequency)) {
            return tl::unexpected {error {error_code::io_failure, "spill read failed"}};
        }
        while (referenced_index < referenced.hashes.size() && referenced.hashes[referenced_index] < hash) {
            ++referenced_index;
        }
        if (referenced_index < referenced.hashes.size() && referenced.hashes[referenced_index] == hash) {
            referenced.frequencies[referenced_index] = frequency;
            ++referenced_index;
        }
    }
    metrics.child_frequency_loaded_count = referenced.hashes.size();
    metrics.child_frequency_load_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    return referenced;
}

// NOLINTEND(bugprone-easily-swappable-parameters)

auto referenced_child_frequency(referenced_child_frequencies const& frequencies, std::uint64_t const hash) -> result<std::uint32_t>
{
    auto const entry = std::ranges::lower_bound(frequencies.hashes, hash);
    if (entry == frequencies.hashes.end() || *entry != hash) {
        return tl::unexpected {error {error_code::io_failure, "missing child frequency"}};
    }
    auto const index = static_cast<std::size_t>(entry - frequencies.hashes.begin());
    if (frequencies.frequencies[index] == 0U) {
        return tl::unexpected {error {error_code::io_failure, "missing child frequency"}};
    }
    return frequencies.frequencies[index];
}

// NOLINTBEGIN(readability-function-cognitive-complexity,bugprone-easily-swappable-parameters) -- streams three distinct sorted artifacts
// into one index.
auto write_index_file(std::filesystem::path const& path,
                      opening_tree_index_build_options const& opts,
                      std::uint64_t const source_game_count,
                      std::uint64_t const full_coverage_root_hash,
                      std::filesystem::path const& roots_path,
                      std::filesystem::path const& edges_path,
                      std::filesystem::path const& child_counts_path,
                      std::size_t const child_frequency_memory_limit_bytes,
                      opening_tree_index_build_metrics& metrics) -> result<void>
{
    std::ifstream roots {roots_path, std::ios::binary};
    std::ifstream edges {edges_path, std::ios::binary};
    std::ifstream child_counts {child_counts_path, std::ios::binary};
    std::uint64_t root_count {};
    std::uint64_t edge_count {};
    std::uint64_t child_count {};
    if (!roots || !edges || !child_counts || !read_little_endian(roots, root_count) || !read_little_endian(edges, edge_count)
        || !read_little_endian(child_counts, child_count))
    {
        return tl::unexpected {error {error_code::io_failure, "spill read failed"}};
    }
    auto writer = opening_tree_checkpoint_writer::create(path, opts.max_root_ply, source_game_count);
    if (!writer) {
        return tl::unexpected {writer.error()};
    }

    auto child_frequencies =
        load_referenced_child_frequencies(edges_path, child_counts, child_count, edge_count, child_frequency_memory_limit_bytes, metrics);
    if (!child_frequencies) {
        return tl::unexpected {child_frequencies.error()};
    }
    edge_record current {};
    auto has_edge = edge_count != 0U && read_edge_record(edges, current);
    if (edge_count != 0U && !has_edge) {
        return tl::unexpected {error {error_code::io_failure, "spill read failed"}};
    }
    for (std::uint64_t root_index = 0; root_index < root_count; ++root_index) {
        std::uint64_t root_hash {};
        std::uint32_t game_count {};
        if (!read_little_endian(roots, root_hash) || !read_little_endian(roots, game_count)) {
            return tl::unexpected {error {error_code::io_failure, "spill read failed"}};
        }
        auto node =
            raw_node {.root_hash = root_hash, .game_count = game_count, .complete = root_hash == full_coverage_root_hash, .edges = {}};
        while (has_edge && current.key.root_hash == root_hash) {
            auto const frequency = child_frequencies->hashes.empty()
                ? child_frequency(child_counts, child_count, current.key.child_hash, metrics)
                : referenced_child_frequency(*child_frequencies, current.key.child_hash);
            if (!frequency || *frequency < current.agg.frequency) {
                return tl::unexpected {error {error_code::io_failure, "write failed"}};
            }
            node.edges.push_back(raw_edge {.key = {.encoded_move = current.key.encoded_move, .child_hash = current.key.child_hash},
                                           .stats = agg_to_stats(current.agg),
                                           .min_root_ply = current.agg.root_ply,
                                           .min_root_ply_valid = true,
                                           .sample_min = current.agg.eco_sample_min,
                                           .sample_max = current.agg.eco_sample_max,
                                           .sample_bounds_valid = true,
                                           .child_frequency = *frequency});
            --edge_count;
            has_edge = edge_count != 0U && read_edge_record(edges, current);
            if (edge_count != 0U && !has_edge) {
                return tl::unexpected {error {error_code::io_failure, "spill read failed"}};
            }
        }
        if (auto written = writer->write_node(node); !written) {
            return written;
        }
    }
    return writer->finish();
}

// NOLINTEND(readability-function-cognitive-complexity,bugprone-easily-swappable-parameters)

}  // namespace

struct opening_tree_index_builder::state
{
    std::filesystem::path path;
    opening_tree_index_build_options options;
    opening_tree_index_build_metrics metrics;
    hash_visit_counter visits;
    opening_tree_child_count_stream child_counts;
    tree_contribution_enumerator enumerator;
    build_spills spills;
    // The only node aggregated past max_root_ply -- see the format_version
    // comment above and tree_contribution_enumerator.
    std::uint64_t full_coverage_root_hash {opening_tree_start_hash()};
    std::uint64_t source_game_count {};
    std::optional<error> failure;
    bool finalized {false};

    state(std::filesystem::path output_path,
          opening_tree_index_build_options build_options,
          opening_tree_child_count_stream external_child_counts)
        : path {std::move(output_path)}
        , options {build_options}
        , visits {std::filesystem::path {path.string() + ".childfreq"}, options.spill_threshold, &metrics}
        , child_counts {std::move(external_child_counts)}
        , enumerator {options.max_root_ply, opening_tree_start_hash(), !child_counts}
        , spills {std::filesystem::path {path.string() + ".build"}, options.spill_threshold, &metrics}
    {
        metrics.child_counts_external = static_cast<bool>(child_counts);
    }
};

opening_tree_index_builder::opening_tree_index_builder(std::filesystem::path path,
                                                       opening_tree_index_build_options options,
                                                       opening_tree_child_count_stream child_counts)
    : state_ {std::make_unique<state>(std::move(path), options, std::move(child_counts))}
{
}

opening_tree_index_builder::~opening_tree_index_builder() = default;
opening_tree_index_builder::opening_tree_index_builder(opening_tree_index_builder&&) noexcept = default;
auto opening_tree_index_builder::operator=(opening_tree_index_builder&&) noexcept -> opening_tree_index_builder& = default;

auto opening_tree_index_builder::accumulate(replay_game_record const& game) -> result<void>
{
    if (state_->failure) {
        return tl::unexpected {*state_->failure};
    }
    if (state_->finalized) {
        return tl::unexpected {error_code::invalid_argument};
    }
    if (state_->options.spill_threshold == 0U) {
        return tl::unexpected {error_code::invalid_argument};
    }
    auto* const visits = state_->child_counts ? nullptr : &state_->visits;
    auto accumulated = accumulate_game(game, state_->enumerator, visits, state_->spills);
    if (!accumulated) {
        state_->failure = accumulated.error();
        return accumulated;
    }
    ++state_->source_game_count;
    state_->metrics.game_count = state_->source_game_count;
    return {};
}

auto opening_tree_index_builder::finalize() -> result<void>
{
    if (state_->finalized) {
        return tl::unexpected {error_code::invalid_argument};
    }
    if (state_->failure) {
        return tl::unexpected {*state_->failure};
    }
    state_->finalized = true;

    auto const roots_path = std::filesystem::path {state_->path.string() + ".roots"};
    auto const edges_path = std::filesystem::path {state_->path.string() + ".edges"};
    auto const child_counts_path = std::filesystem::path {state_->path.string() + ".childcounts"};
    auto cleanup = [&]() -> void
    {
        std::error_code ignored;
        std::filesystem::remove(roots_path, ignored);
        std::filesystem::remove(edges_path, ignored);
        std::filesystem::remove(child_counts_path, ignored);
    };
    auto const roots_started = std::chrono::steady_clock::now();
    auto roots = state_->spills.merge_roots(roots_path);
    if (!roots) {
        cleanup();
        return tl::unexpected {roots.error()};
    }
    state_->metrics.root_merge_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - roots_started);
    auto const edges_started = std::chrono::steady_clock::now();
    auto edges = state_->spills.merge_edges(edges_path);
    if (!edges) {
        cleanup();
        return tl::unexpected {edges.error()};
    }
    state_->metrics.edge_merge_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - edges_started);
    auto const counts_started = std::chrono::steady_clock::now();
    auto counts = state_->child_counts ? materialize_child_counts(state_->child_counts, child_counts_path)
                                       : state_->visits.finalize(child_counts_path);
    if (!counts) {
        cleanup();
        return tl::unexpected {counts.error()};
    }
    state_->metrics.child_merge_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - counts_started);
    auto const write_started = std::chrono::steady_clock::now();
    auto written = write_index_file(state_->path,
                                    state_->options,
                                    state_->source_game_count,
                                    state_->full_coverage_root_hash,
                                    roots_path,
                                    edges_path,
                                    child_counts_path,
                                    state_->options.child_frequency_memory_limit_bytes,
                                    state_->metrics);
    state_->metrics.index_write_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - write_started);
    cleanup();
    return written;
}

auto opening_tree_index_builder::metrics() noexcept -> opening_tree_index_build_metrics&
{
    return state_->metrics;
}

auto opening_tree_index::build(game_store const& store,
                               std::filesystem::path const& path,
                               build_options const& opts,
                               opening_tree_index_build_metrics* const metrics) -> result<void>
{
    return build(store, path, opts, metrics, {});
}

auto opening_tree_index::build(game_store const& store,
                               std::filesystem::path const& path,
                               build_options const& opts,
                               opening_tree_index_build_metrics* const metrics,
                               opening_tree_child_count_stream child_counts) -> result<void>
{
    auto builder = opening_tree_index_builder {path, opts, std::move(child_counts)};
    auto const replay_started = std::chrono::steady_clock::now();
    auto const replay_res =
        store.for_each_replay_game([&builder](replay_game_record const& game) -> result<void> { return builder.accumulate(game); });
    if (!replay_res) {
        return tl::unexpected {replay_res.error()};
    }
    builder.metrics().replay_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - replay_started);
    auto finalized = builder.finalize();
    if (!finalized) {
        return finalized;
    }
    auto const& build_metrics = builder.metrics();
    if (auto const log = spdlog::get("motif.db"); log != nullptr) {
        log
            ->info(
                "opening_tree_index build games {} roots {} edges {} child_visits {} root_spills {} edge_spills {} child_spills {} "
                 "child_loaded {} external_counts {} replay_ms {} root_merge_ms {} edge_merge_ms {} child_merge_ms {} child_load_ms {} write_ms {}",
                build_metrics.game_count,
                build_metrics.root_record_count,
                build_metrics.edge_record_count,
                build_metrics.child_visit_count,
                build_metrics.root_spill_run_count,
                build_metrics.edge_spill_run_count,
                build_metrics.child_spill_run_count,
                 build_metrics.child_frequency_loaded_count,
                 build_metrics.child_counts_external,
                build_metrics.replay_elapsed.count(),
                build_metrics.root_merge_elapsed.count(),
                build_metrics.edge_merge_elapsed.count(),
                build_metrics.child_merge_elapsed.count(),
                build_metrics.child_frequency_load_elapsed.count(),
                build_metrics.index_write_elapsed.count());
    }
    if (metrics != nullptr) {
        *metrics = build_metrics;
    }
    return {};
}

// ── Contribution enumerator ──────────────────────────────────────────────────

auto opening_tree_start_hash() -> std::uint64_t
{
    return motif::chess::board {}.hash();
}

struct tree_contribution_enumerator::state
{
    std::uint16_t max_root_ply {};
    std::uint64_t full_coverage_root_hash {};
    bool collect_visited {false};
    gtl::flat_hash_set<std::uint64_t> visited_set;
    gtl::flat_hash_map<edge_key, std::uint16_t, edge_key_hash> edge_map;
    gtl::flat_hash_set<std::uint64_t> root_set;
    std::vector<std::uint64_t> visited_out;
    std::vector<std::uint64_t> roots_out;
    std::vector<game_tree_edge> edges_out;
};

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- a ply horizon and a root hash; every caller passes named values in this order.
tree_contribution_enumerator::tree_contribution_enumerator(std::uint16_t const max_root_ply,
                                                           std::uint64_t const full_coverage_root_hash,
                                                           bool const collect_visited)
    : state_ {std::make_unique<state>()}
{
    state_->max_root_ply = max_root_ply;
    state_->full_coverage_root_hash = full_coverage_root_hash;
    state_->collect_visited = collect_visited;
    state_->edge_map.reserve(per_game_scratch_reserve);
    state_->root_set.reserve(per_game_scratch_reserve);
}

tree_contribution_enumerator::~tree_contribution_enumerator() = default;
tree_contribution_enumerator::tree_contribution_enumerator(tree_contribution_enumerator&&) noexcept = default;
auto tree_contribution_enumerator::operator=(tree_contribution_enumerator&&) noexcept -> tree_contribution_enumerator& = default;

auto tree_contribution_enumerator::enumerate(std::span<std::uint16_t const> const moves) -> result<void>
{
    auto& scratch = *state_;
    scratch.visited_set.clear();
    scratch.edge_map.clear();
    scratch.root_set.clear();
    scratch.visited_out.clear();
    scratch.roots_out.clear();
    scratch.edges_out.clear();

    auto board = motif::chess::board {};
    auto prev_hash = board.hash();
    if (scratch.collect_visited && scratch.visited_set.insert(prev_hash).second) {
        scratch.visited_out.push_back(prev_hash);
    }
    scratch.root_set.insert(prev_hash);

    for (std::size_t root_ply = 0; root_ply < moves.size(); ++root_ply) {
        if (root_ply > std::numeric_limits<std::uint16_t>::max()) {
            return tl::unexpected {error {error_code::io_failure, "game exceeds max representable ply"}};
        }
        auto const encoded_move = moves[root_ply];
        motif::chess::apply_encoded_move(board, encoded_move);
        auto const child_hash = board.hash();

        if (scratch.collect_visited && scratch.visited_set.insert(child_hash).second) {
            scratch.visited_out.push_back(child_hash);
        }
        if (root_ply <= scratch.max_root_ply || prev_hash == scratch.full_coverage_root_hash) {
            auto const key = edge_key {.root_hash = prev_hash, .encoded_move = encoded_move, .child_hash = child_hash};
            auto const ply16 = static_cast<std::uint16_t>(root_ply);
            auto [entry, inserted] = scratch.edge_map.try_emplace(key, ply16);
            if (!inserted && ply16 < entry->second) {
                entry->second = ply16;
            }
        }
        if (root_ply + 1U <= scratch.max_root_ply) {
            scratch.root_set.insert(child_hash);
        }
        prev_hash = child_hash;
    }

    scratch.roots_out.assign(scratch.root_set.begin(), scratch.root_set.end());
    scratch.edges_out.reserve(scratch.edge_map.size());
    for (auto const& [key, min_ply] : scratch.edge_map) {
        scratch.edges_out.push_back(game_tree_edge {
            .root_hash = key.root_hash, .encoded_move = key.encoded_move, .child_hash = key.child_hash, .min_root_ply = min_ply});
    }
    return {};
}

auto tree_contribution_enumerator::roots() const noexcept -> std::span<std::uint64_t const>
{
    return state_->roots_out;
}

auto tree_contribution_enumerator::edges() const noexcept -> std::span<game_tree_edge const>
{
    return state_->edges_out;
}

auto tree_contribution_enumerator::visited() const noexcept -> std::span<std::uint64_t const>
{
    return state_->visited_out;
}

// ── Checkpoint node reading ──────────────────────────────────────────────────

namespace
{

struct node_header
{
    std::uint64_t hash {};
    std::uint32_t game_count {};
    std::uint32_t continuation_count {};
    bool complete {false};
};

// Ranges only; ordering and the complete-node rule are the caller's.
auto read_node_header(buffered_tree_reader& input) -> std::optional<node_header>
{
    std::uint64_t hash {};
    std::uint64_t game_count_raw {};
    std::uint64_t continuation_count {};
    std::uint64_t complete_raw {};
    if (!read_little_endian(input, hash) || !read_varint(input, game_count_raw) || !read_varint(input, continuation_count)
        || !read_varint(input, complete_raw) || game_count_raw == 0U || game_count_raw > u32_limit || continuation_count > u32_limit
        || complete_raw > 1U)
    {
        return std::nullopt;
    }
    return node_header {.hash = hash,
                        .game_count = static_cast<std::uint32_t>(game_count_raw),
                        .continuation_count = static_cast<std::uint32_t>(continuation_count),
                        .complete = complete_raw == 1U};
}

// Reads a node's continuations (validating every one, and that keys strictly
// ascend); appends them to *out when out is non-null.
auto read_node_edges(buffered_tree_reader& input,
                     std::uint32_t const version,
                     node_header const& header,
                     std::uint16_t const max_root_ply,
                     std::vector<raw_edge>* const out) -> result<void>
{
    auto const context =
        merged_edge_context {.node_game_count = header.game_count, .max_root_ply = max_root_ply, .node_complete = header.complete};
    if (out != nullptr) {
        out->reserve(header.continuation_count);
    }
    auto previous = std::optional<tree_edge_key> {};
    for (std::uint32_t index = 0; index < header.continuation_count; ++index) {
        auto edge = read_continuation(input, version, context);
        if (!edge) {
            return tl::unexpected {edge.error()};
        }
        if (previous && !(*previous < edge->key)) {
            return corrupt("continuations are not strictly ascending");
        }
        previous = edge->key;
        if (out != nullptr) {
            out->push_back(*edge);
        }
    }
    return {};
}

}  // namespace

struct opening_tree_node_stream::state
{
    std::optional<buffered_tree_reader> reader;
    std::uint64_t remaining {};
    std::uint32_t version {};
    std::uint16_t max_root_ply {};
    std::uint64_t source_game_count {};
    std::optional<std::uint64_t> previous_hash;
};

opening_tree_node_stream::opening_tree_node_stream(std::unique_ptr<state> stream_state) noexcept
    : state_ {std::move(stream_state)}
{
}

opening_tree_node_stream::opening_tree_node_stream(opening_tree_node_stream&&) noexcept = default;
auto opening_tree_node_stream::operator=(opening_tree_node_stream&&) noexcept -> opening_tree_node_stream& = default;
opening_tree_node_stream::~opening_tree_node_stream() = default;

auto opening_tree_node_stream::next() -> result<std::optional<raw_node>>
{
    if (state_->remaining == 0U) {
        return std::optional<raw_node> {};
    }
    if (!state_->reader) {
        return corrupt("node stream has no open reader");
    }
    auto& reader = *state_->reader;
    auto const header = read_node_header(reader);
    if (!header || (state_->previous_hash && *state_->previous_hash >= header->hash)
        || (header->complete && (header->hash != opening_tree_start_hash() || header->game_count != state_->source_game_count)))
    {
        return corrupt("invalid node header");
    }
    auto node = raw_node {.root_hash = header->hash, .game_count = header->game_count, .complete = header->complete, .edges = {}};
    if (auto edges = read_node_edges(reader, state_->version, *header, state_->max_root_ply, &node.edges); !edges) {
        return tl::unexpected {edges.error()};
    }
    state_->previous_hash = header->hash;
    --state_->remaining;
    return std::optional<raw_node> {std::move(node)};
}

// ── Checkpoint writer ────────────────────────────────────────────────────────

struct opening_tree_checkpoint_writer::state
{
    std::filesystem::path path;
    std::filesystem::path temp_path;
    std::ofstream output;
    buffered_tree_writer writer;
    std::uint64_t node_count {};
    std::optional<std::uint64_t> previous_hash;
    bool finished {false};

    state(std::filesystem::path final_path, std::filesystem::path staging_path)
        : path {std::move(final_path)}
        , temp_path {std::move(staging_path)}
        , output {temp_path, std::ios::binary | std::ios::trunc}
        , writer {output}
    {
    }

    ~state()
    {
        if (!finished) {
            output.close();
            std::error_code ignored;
            std::filesystem::remove(temp_path, ignored);
        }
    }

    state(state const&) = delete;
    auto operator=(state const&) -> state& = delete;
    state(state&&) = delete;
    auto operator=(state&&) -> state& = delete;
};

opening_tree_checkpoint_writer::opening_tree_checkpoint_writer(std::unique_ptr<state> writer_state) noexcept
    : state_ {std::move(writer_state)}
{
}

opening_tree_checkpoint_writer::opening_tree_checkpoint_writer(opening_tree_checkpoint_writer&&) noexcept = default;
auto opening_tree_checkpoint_writer::operator=(opening_tree_checkpoint_writer&&) noexcept -> opening_tree_checkpoint_writer& = default;
opening_tree_checkpoint_writer::~opening_tree_checkpoint_writer() = default;

auto opening_tree_checkpoint_writer::create(std::filesystem::path path,
                                            std::uint16_t const max_root_ply,
                                            std::uint64_t const source_game_count) -> result<opening_tree_checkpoint_writer>
{
    auto temp_path = std::filesystem::path {path.string() + ".tmp"};
    auto writer_state = std::make_unique<state>(std::move(path), std::move(temp_path));
    if (!writer_state->output) {
        return tl::unexpected {error {error_code::io_failure, "cannot open output file"}};
    }
    if (!writer_state->writer.write_bytes(magic) || !writer_state->writer.write_fixed(format_version)
        || !writer_state->writer.write_fixed(max_root_ply) || !writer_state->writer.write_fixed(source_game_count)
        || !writer_state->writer.write_fixed(std::uint64_t {0}))
    {
        return tl::unexpected {error {error_code::io_failure, "write failed"}};
    }
    return opening_tree_checkpoint_writer {std::move(writer_state)};
}

auto opening_tree_checkpoint_writer::write_node(raw_node const& node) -> result<void>
{
    auto& self = *state_;
    if (self.finished || (self.previous_hash && *self.previous_hash >= node.root_hash) || node.game_count == 0U
        || node.edges.size() > u32_limit)
    {
        return corrupt("invalid node for checkpoint");
    }
    auto previous = std::optional<tree_edge_key> {};
    for (auto const& edge : node.edges) {
        if (!edge_is_encodable(edge) || (previous && !(*previous < edge.key))) {
            return corrupt("invalid edge for checkpoint");
        }
        previous = edge.key;
    }
    if (!self.writer.write_fixed(node.root_hash) || !self.writer.write_varint(node.game_count)
        || !self.writer.write_varint(static_cast<std::uint64_t>(node.edges.size()))
        || !self.writer.write_varint(node.complete ? std::uint64_t {1} : std::uint64_t {0}))
    {
        return corrupt("write failed");
    }
    for (auto const& edge : node.edges) {
        if (!write_continuation(self.writer, edge)) {
            return corrupt("write failed");
        }
    }
    self.previous_hash = node.root_hash;
    ++self.node_count;
    return {};
}

auto opening_tree_checkpoint_writer::finish() -> result<void>
{
    auto& self = *state_;
    if (self.finished) {
        return tl::unexpected {error_code::invalid_argument};
    }
    if (!self.writer.flush()) {
        return corrupt("write failed");
    }
    self.output.seekp(header_node_count_offset);
    if (!write_little_endian(self.output, self.node_count)) {
        return corrupt("write failed");
    }
    self.output.close();
    if (!self.output) {
        return corrupt("write failed");
    }
    std::error_code rename_error;
    std::filesystem::rename(self.temp_path, self.path, rename_error);
    if (rename_error) {
        return corrupt("publish failed");
    }
    self.finished = true;
    return {};
}

auto opening_tree_checkpoint_writer::node_count() const noexcept -> std::uint64_t
{
    return state_->node_count;
}

// ── Checkpoint reader ────────────────────────────────────────────────────────

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- validates every untrusted binary field before narrowing or allocation.
auto opening_tree_index::open(std::filesystem::path const& path) -> result<opening_tree_index>
{
    std::error_code size_error;
    auto const file_size = std::filesystem::file_size(path, size_error);
    if (size_error || file_size < fixed_header_size) {
        return tl::unexpected {error {error_code::io_failure, "invalid file size"}};
    }
    std::ifstream input {path, std::ios::binary};
    if (!input) {
        return tl::unexpected {error {error_code::io_failure, "cannot open input file"}};
    }

    std::array<char, magic.size()> read_magic {};
    input.read(read_magic.data(), static_cast<std::streamsize>(read_magic.size()));
    if (!input || read_magic != magic) {
        return tl::unexpected {error {error_code::io_failure, "bad magic"}};
    }

    std::uint32_t version {};
    std::uint16_t max_root_ply {};
    std::uint64_t source_game_count {};
    std::uint64_t node_count {};
    if (!read_little_endian(input, version) || (version != format_version && version != legacy_format_version)
        || !read_little_endian(input, max_root_ply) || !read_little_endian(input, source_game_count)
        || !read_little_endian(input, node_count) || node_count > (file_size - fixed_header_size) / minimum_node_size)
    {
        return tl::unexpected {error {error_code::io_failure, "bad header"}};
    }
    auto reader = buffered_tree_reader {std::move(input), fixed_header_size, read_buffer_size};

    opening_tree_index index;
    index.path_ = path;
    index.source_game_count_ = source_game_count;
    index.max_root_ply_ = max_root_ply;
    index.format_version_ = version;
    index.node_hashes_.reserve(static_cast<std::size_t>(node_count));
    index.node_game_counts_.reserve(static_cast<std::size_t>(node_count));
    index.node_data_offsets_.reserve(static_cast<std::size_t>(node_count));
    index.node_continuation_counts_.reserve(static_cast<std::size_t>(node_count));
    index.node_complete_.reserve(static_cast<std::size_t>(node_count));
    auto complete_node_count = std::size_t {0};

    for (std::uint64_t node_index = 0; node_index < node_count; ++node_index) {
        auto const header = read_node_header(reader);
        if (!header || (!index.node_hashes_.empty() && index.node_hashes_.back() >= header->hash)) {
            return tl::unexpected {error {error_code::io_failure, "truncated node header"}};
        }
        index.node_hashes_.push_back(header->hash);
        index.node_game_counts_.push_back(header->game_count);
        index.node_data_offsets_.push_back(reader.position());
        index.node_continuation_counts_.push_back(header->continuation_count);
        if (header->complete
            && (header->hash != opening_tree_start_hash() || header->game_count != source_game_count || complete_node_count != 0U))
        {
            return tl::unexpected {error {error_code::io_failure, "invalid complete node"}};
        }
        complete_node_count += header->complete ? 1U : 0U;
        index.node_complete_.push_back(header->complete ? std::uint8_t {1} : std::uint8_t {0});

        if (auto edges = read_node_edges(reader, version, *header, max_root_ply, nullptr); !edges) {
            return tl::unexpected {edges.error()};
        }
    }

    if (!reader.at_end()) {
        return tl::unexpected {error {error_code::io_failure, "trailing data"}};
    }

    return index;
}

auto opening_tree_index::node_index(std::uint64_t const hash) const noexcept -> std::optional<std::size_t>
{
    auto const lower = std::ranges::lower_bound(node_hashes_, hash);
    if (lower == node_hashes_.end() || *lower != hash) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(lower - node_hashes_.begin());
}

auto opening_tree_index::query_raw_node(zobrist_hash const hash) const -> result<std::optional<raw_node>>
{
    auto const index = node_index(hash.value);
    if (!index) {
        return std::optional<raw_node> {};
    }
    std::ifstream input {path_, std::ios::binary};
    input.seekg(static_cast<std::streamoff>(node_data_offsets_[*index]));
    if (!input) {
        return tl::unexpected {error {error_code::io_failure, "cannot seek node"}};
    }
    auto reader = buffered_tree_reader {std::move(input), node_data_offsets_[*index], point_read_buffer_size};
    auto const header = node_header {.hash = hash.value,
                                     .game_count = node_game_counts_[*index],
                                     .continuation_count = node_continuation_counts_[*index],
                                     .complete = node_complete_[*index] == std::uint8_t {1}};
    auto node = raw_node {.root_hash = hash.value, .game_count = header.game_count, .complete = header.complete, .edges = {}};
    if (auto edges = read_node_edges(reader, format_version_, header, max_root_ply_, &node.edges); !edges) {
        return tl::unexpected {edges.error()};
    }
    return std::optional<raw_node> {std::move(node)};
}

auto opening_tree_index::query_opening_stats(zobrist_hash const hash) const -> result<std::vector<opening_stat_agg_row>>
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
        if (!edge.child_frequency) {
            return corrupt("checkpoint edge has no cached child frequency");
        }
        auto row = to_opening_stat_row(edge, *edge.child_frequency);
        if (!row) {
            return tl::unexpected {row.error()};
        }
        rows.push_back(*row);
    }
    return rows;
}

auto opening_tree_index::stream_nodes() const -> result<opening_tree_node_stream>
{
    auto stream_state = std::make_unique<opening_tree_node_stream::state>();
    auto input = std::ifstream {path_, std::ios::binary};
    input.seekg(static_cast<std::streamoff>(fixed_header_size));
    if (!input) {
        return tl::unexpected {error {error_code::io_failure, "cannot open input file"}};
    }
    stream_state->reader.emplace(std::move(input), fixed_header_size, read_buffer_size);
    stream_state->remaining = node_hashes_.size();
    stream_state->version = format_version_;
    stream_state->max_root_ply = max_root_ply_;
    stream_state->source_game_count = source_game_count_;
    return opening_tree_node_stream {std::move(stream_state)};
}

auto opening_tree_index::game_count(zobrist_hash const hash) const -> result<std::uint32_t>
{
    auto const index = node_index(hash.value);
    return index ? node_game_counts_[*index] : std::uint32_t {0};
}

auto opening_tree_index::is_complete(zobrist_hash const hash) const -> bool
{
    auto const index = node_index(hash.value);
    return index && node_complete_[*index] == std::uint8_t {1};
}

auto opening_tree_index::source_game_count() const noexcept -> std::uint64_t
{
    return source_game_count_;
}

auto opening_tree_index::max_root_ply() const noexcept -> std::uint16_t
{
    return max_root_ply_;
}

auto opening_tree_index::checkpoint_format_version() const noexcept -> std::uint32_t
{
    return format_version_;
}

auto opening_tree_index::node_count() const noexcept -> std::uint64_t
{
    return node_hashes_.size();
}

}  // namespace motif::db
