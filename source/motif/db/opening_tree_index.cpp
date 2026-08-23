#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
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
// legitimately exceed max_root_ply -- read_continuation's upper-bound check
// is relaxed per-node accordingly. Only the canonical starting position is
// ever marked complete (see full_coverage_root_hash below).
constexpr auto format_version = std::uint32_t {5};
constexpr auto byte_bits = std::size_t {8};
constexpr auto byte_mask = std::uint64_t {0xff};
constexpr auto fixed_header_size = std::uintmax_t {30};
// hash(8) + game_count varint(>=1) + continuation_count varint(>=1) + complete varint(>=1)
constexpr auto minimum_node_size = std::uintmax_t {11};
constexpr auto final_write_buffer_size = std::size_t {1U << 16U};
constexpr auto per_game_scratch_reserve = std::size_t {32};

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

auto write_f64(std::ofstream& output, double value) -> bool
{
    return write_little_endian(output, std::bit_cast<std::uint64_t>(value));
}

auto read_f64(std::ifstream& input, double& value) -> bool
{
    std::uint64_t raw {};
    if (!read_little_endian(input, raw)) {
        return false;
    }
    value = std::bit_cast<double>(raw);
    return true;
}

auto read_varint(std::ifstream& input, std::uint64_t& value) -> bool
{
    constexpr auto continuation_bit = std::uint8_t {0x80};
    constexpr auto payload_mask = std::uint8_t {0x7F};
    constexpr auto payload_bits = 7U;
    constexpr auto max_shift = 63U;
    // At shift 63 only bit 0 of a payload fits in a u64; reject a 10th byte
    // that sets any other bit instead of silently shifting it out.
    constexpr auto max_shift_payload_limit = std::uint8_t {0x01};

    value = 0;
    auto shift = 0U;
    while (true) {
        auto const byte = input.get();
        if (byte == std::char_traits<char>::eof()) {
            return false;
        }
        if (shift > max_shift) {
            return false;
        }
        auto const payload = static_cast<std::uint8_t>(byte);
        auto const payload_bits_value = static_cast<std::uint8_t>(payload & payload_mask);
        if (shift == max_shift && payload_bits_value > max_shift_payload_limit) {
            return false;
        }
        value |= static_cast<std::uint64_t>(payload_bits_value) << shift;
        if ((payload & continuation_bit) == 0U) {
            return true;
        }
        shift += payload_bits;
    }
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
    std::int64_t white_elo_sum {};
    std::uint32_t white_elo_count {};
    std::int64_t black_elo_sum {};
    std::uint32_t black_elo_count {};
    double weighted_contrib_sum {0.0};
    double elo_weight_sum {0.0};
    std::uint32_t eco_sample_min {std::numeric_limits<std::uint32_t>::max()};
    std::uint32_t eco_sample_max {0};
};

// Mirrors database_manager.cpp's local map_result: draws and unrecognized
// result strings both map to 0. Duplicated rather than shared because that
// function is anonymous-namespace, file-local to database_manager.cpp.
auto map_result(std::string const& pgn_result) -> std::int8_t
{
    if (pgn_result == "1-0") {
        return 1;
    }
    if (pgn_result == "0-1") {
        return -1;
    }
    return 0;
}

// Mirrors note_result/note_elo in source/motif/search/opening_tree.cpp,
// duplicated for the same reason (file-local there).
void note_result(edge_agg& agg, std::int8_t const result)
{
    if (result > 0) {
        ++agg.white_wins;
        return;
    }
    if (result < 0) {
        ++agg.black_wins;
        return;
    }
    ++agg.draws;
}

void note_elo(edge_agg& agg,
              std::int8_t const result,
              std::optional<std::int32_t> const& white_elo,
              std::optional<std::int32_t> const& black_elo)
{
    if (white_elo.has_value()) {
        agg.white_elo_sum += *white_elo;
        ++agg.white_elo_count;
    }
    if (black_elo.has_value()) {
        agg.black_elo_sum += *black_elo;
        ++agg.black_elo_count;
    }
    if (white_elo.has_value() && black_elo.has_value()) {
        auto const avg_elo = (static_cast<double>(*white_elo) + static_cast<double>(*black_elo)) / 2.0;
        agg.weighted_contrib_sum += static_cast<double>(result) * avg_elo;
        agg.elo_weight_sum += avg_elo;
    }
}

auto average_elo(std::int64_t const sum, std::uint32_t const count) -> std::optional<double>
{
    if (count == 0U) {
        return std::nullopt;
    }
    return static_cast<double>(sum) / static_cast<double>(count);
}

// max_root_ply_for_validation is the per-node effective cap: max_root_ply()
// for a normal node, or numeric_limits<uint16_t>::max() (root_ply's own
// representable range, i.e. no cap) for a node marked complete.
auto read_continuation(std::ifstream& input,
                       std::uint16_t const max_root_ply_for_validation,
                       std::uint32_t const root_game_count = std::numeric_limits<std::uint32_t>::max()) -> result<opening_stat_agg_row>
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
    if (!read_ok || root_ply > max_root_ply_for_validation || frequency == 0U || frequency > root_game_count
        || frequency > std::numeric_limits<std::uint32_t>::max() || white_wins > frequency || draws > frequency || black_wins > frequency
        || white_wins + draws + black_wins != frequency
        || white_elo_sum > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
        || black_elo_sum > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) || white_elo_count > frequency
        || black_elo_count > frequency || eco_sample_min == 0U || eco_sample_min > eco_sample_max
        || eco_sample_max > std::numeric_limits<std::uint32_t>::max() || transposition_frequency < frequency
        || transposition_frequency > std::numeric_limits<std::uint32_t>::max())
    {
        return tl::unexpected {error {error_code::io_failure, "invalid continuation record"}};
    }
    return opening_stat_agg_row {
        .cont_encoded_move = encoded_move,
        .cont_hash = zobrist_hash {child_hash},
        .root_ply = static_cast<std::uint16_t>(root_ply),
        .frequency = static_cast<std::uint32_t>(frequency),
        .transposition_frequency = static_cast<std::uint32_t>(transposition_frequency),
        .white_wins = static_cast<std::uint32_t>(white_wins),
        .draws = static_cast<std::uint32_t>(draws),
        .black_wins = static_cast<std::uint32_t>(black_wins),
        .avg_white_elo = average_elo(static_cast<std::int64_t>(white_elo_sum), static_cast<std::uint32_t>(white_elo_count)),
        .avg_black_elo = average_elo(static_cast<std::int64_t>(black_elo_sum), static_cast<std::uint32_t>(black_elo_count)),
        .eco_sample_min = game_id {static_cast<std::uint32_t>(eco_sample_min)},
        .eco_sample_max = game_id {static_cast<std::uint32_t>(eco_sample_max)},
        .elo_weighted_score = elo_weight_sum > 0.0 ? std::optional<double> {weighted_contrib_sum / elo_weight_sum} : std::nullopt,
    };
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
        && write_f64(output, agg.weighted_contrib_sum) && write_f64(output, agg.elo_weight_sum)
        && write_little_endian(output, agg.eco_sample_min) && write_little_endian(output, agg.eco_sample_max);
}

auto read_edge_record(std::ifstream& input, edge_record& record) -> bool
{
    auto& agg = record.agg;
    std::uint64_t white_elo_sum {};
    std::uint64_t black_elo_sum {};
    if (!read_little_endian(input, record.key.root_hash) || !read_little_endian(input, record.key.encoded_move)
        || !read_little_endian(input, record.key.child_hash) || !read_little_endian(input, agg.root_ply)
        || !read_little_endian(input, agg.frequency) || !read_little_endian(input, agg.white_wins) || !read_little_endian(input, agg.draws)
        || !read_little_endian(input, agg.black_wins) || !read_little_endian(input, white_elo_sum)
        || !read_little_endian(input, agg.white_elo_count) || !read_little_endian(input, black_elo_sum)
        || !read_little_endian(input, agg.black_elo_count) || !read_f64(input, agg.weighted_contrib_sum)
        || !read_f64(input, agg.elo_weight_sum) || !read_little_endian(input, agg.eco_sample_min)
        || !read_little_endian(input, agg.eco_sample_max))
    {
        return false;
    }
    agg.white_elo_sum = static_cast<std::int64_t>(white_elo_sum);
    agg.black_elo_sum = static_cast<std::int64_t>(black_elo_sum);
    return true;
}

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
    target.weighted_contrib_sum += source.weighted_contrib_sum;
    target.elo_weight_sum += source.elo_weight_sum;
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

// Replays one game into `state` and `visits`: `visits`
// (transposition_frequency) is uncapped, `state.edges` is capped at
// max_root_ply because root positions at the boundary still contribute their
// immediate continuation. The one
// exception is full_coverage_root_hash (the canonical starting position):
// edges taken from that exact hash are captured at every occurrence in the
// game, not only within max_root_ply, so its continuations stay complete
// even for a game that transposes back to the start position later and
// keeps playing from there.
struct replay_scratch
{
    std::optional<gtl::flat_hash_set<std::uint64_t>> visited;
    gtl::flat_hash_map<edge_key, std::uint16_t, edge_key_hash> edges;
    gtl::flat_hash_set<std::uint64_t> roots;
};

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one replay pass updates dedup, edge, and full-coverage state per ply.
auto accumulate_game(replay_game_record const& current_game,
                     std::uint16_t const max_root_ply,
                     std::uint64_t const full_coverage_root_hash,
                     hash_visit_counter* const visits,
                     replay_scratch& scratch,
                     build_spills& spills) -> result<void>
{
    auto const result_code = map_result(current_game.result);
    auto const& white_elo = current_game.white_elo;
    auto const& black_elo = current_game.black_elo;

    auto board = motif::chess::board {};
    auto prev_hash = board.hash();

    if (scratch.visited) {
        scratch.visited->clear();
    }
    scratch.edges.clear();
    scratch.roots.clear();
    // Per-game dedup + min-root-ply-within-this-game tracking, matching
    // edge_deduped's "GROUP BY p_root.game_id, ..." + MIN(p_root.ply).

    if (scratch.visited && scratch.visited->insert(prev_hash).second) {
        if (auto visit_res = visits->visit(prev_hash); !visit_res) {
            return visit_res;
        }
    }
    scratch.roots.insert(prev_hash);

    for (std::size_t root_ply = 0; root_ply < current_game.moves.size(); ++root_ply) {
        if (root_ply > std::numeric_limits<std::uint16_t>::max()) {
            return tl::unexpected {error {error_code::io_failure, "game exceeds max representable ply"}};
        }

        auto const encoded_move = current_game.moves[root_ply];
        motif::chess::apply_encoded_move(board, encoded_move);
        auto const child_hash = board.hash();

        if (scratch.visited && scratch.visited->insert(child_hash).second) {
            if (auto visit_res = visits->visit(child_hash); !visit_res) {
                return visit_res;
            }
        }

        if (root_ply <= max_root_ply || prev_hash == full_coverage_root_hash) {
            auto const key = edge_key {.root_hash = prev_hash, .encoded_move = encoded_move, .child_hash = child_hash};
            auto const ply16 = static_cast<std::uint16_t>(root_ply);
            auto [entry, inserted] = scratch.edges.try_emplace(key, ply16);
            if (!inserted && ply16 < entry->second) {
                entry->second = ply16;
            }
        }

        if (root_ply + 1U <= max_root_ply) {
            scratch.roots.insert(child_hash);
        }

        prev_hash = child_hash;
    }

    for (auto const root_hash : scratch.roots) {
        if (auto result = spills.add_root(root_hash); !result) {
            return result;
        }
    }

    for (auto const& [key, min_ply_this_game] : scratch.edges) {
        auto record = edge_record {.key = key, .agg = {}};
        record.agg.root_ply = min_ply_this_game;
        record.agg.frequency = 1;
        note_result(record.agg, result_code);
        note_elo(record.agg, result_code, white_elo, black_elo);
        record.agg.eco_sample_min = current_game.id.value;
        record.agg.eco_sample_max = current_game.id.value;
        if (auto result = spills.add_edge(record); !result) {
            return result;
        }
    }

    return {};
}

auto write_continuation(buffered_tree_writer& output, edge_key const& key, edge_agg const& agg, std::uint32_t const transposition_frequency)
    -> bool
{
    // white_elo_sum/black_elo_sum only ever accumulate non-negative int32
    // elo values (see note_elo), so they fit an unsigned varint without a
    // zigzag scheme. weighted_contrib_sum/elo_weight_sum are genuinely
    // fractional/signed doubles with no natural varint mapping -- left fixed.
    return output.write_fixed(key.encoded_move) && output.write_fixed(key.child_hash) && output.write_varint(agg.root_ply)
        && output.write_varint(agg.frequency) && output.write_varint(agg.white_wins) && output.write_varint(agg.draws)
        && output.write_varint(agg.black_wins) && output.write_varint(static_cast<std::uint64_t>(agg.white_elo_sum))
        && output.write_varint(agg.white_elo_count) && output.write_varint(static_cast<std::uint64_t>(agg.black_elo_sum))
        && output.write_varint(agg.black_elo_count) && output.write_f64(agg.weighted_contrib_sum) && output.write_f64(agg.elo_weight_sum)
        && output.write_varint(agg.eco_sample_min) && output.write_varint(agg.eco_sample_max)
        && output.write_varint(transposition_frequency);
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
    auto const temp_path = std::filesystem::path {path.string() + ".tmp"};
    std::ofstream output {temp_path, std::ios::binary | std::ios::trunc};
    if (!output) {
        return tl::unexpected {error {error_code::io_failure, "cannot open output file"}};
    }
    auto remove_temp = [&output, &temp_path]() -> void
    {
        output.close();
        std::error_code remove_error;
        std::filesystem::remove(temp_path, remove_error);
    };
    auto writer = buffered_tree_writer {output};

    if (!writer.write_bytes(magic) || !writer.write_fixed(format_version) || !writer.write_fixed(opts.max_root_ply)
        || !writer.write_fixed(source_game_count) || !writer.write_fixed(root_count))
    {
        remove_temp();
        return tl::unexpected {error {error_code::io_failure, "write failed"}};
    }

    auto child_frequencies =
        load_referenced_child_frequencies(edges_path, child_counts, child_count, edge_count, child_frequency_memory_limit_bytes, metrics);
    if (!child_frequencies) {
        remove_temp();
        return tl::unexpected {child_frequencies.error()};
    }
    edge_record current {};
    auto has_edge = edge_count != 0U && read_edge_record(edges, current);
    if (edge_count != 0U && !has_edge) {
        remove_temp();
        return tl::unexpected {error {error_code::io_failure, "spill read failed"}};
    }
    for (std::uint64_t root_index = 0; root_index < root_count; ++root_index) {
        std::uint64_t root_hash {};
        std::uint32_t game_count {};
        if (!read_little_endian(roots, root_hash) || !read_little_endian(roots, game_count)) {
            remove_temp();
            return tl::unexpected {error {error_code::io_failure, "spill read failed"}};
        }
        auto continuations = std::vector<edge_record> {};
        while (has_edge && current.key.root_hash == root_hash) {
            continuations.push_back(current);
            --edge_count;
            has_edge = edge_count != 0U && read_edge_record(edges, current);
            if (edge_count != 0U && !has_edge) {
                remove_temp();
                return tl::unexpected {error {error_code::io_failure, "spill read failed"}};
            }
        }
        auto const is_complete = root_hash == full_coverage_root_hash;
        if (!writer.write_fixed(root_hash) || !writer.write_varint(game_count)
            || !writer.write_varint(static_cast<std::uint64_t>(continuations.size()))
            || !writer.write_varint(is_complete ? std::uint64_t {1} : std::uint64_t {0}))
        {
            remove_temp();
            return tl::unexpected {error {error_code::io_failure, "write failed"}};
        }
        for (auto const& continuation : continuations) {
            auto const frequency = child_frequencies->hashes.empty()
                ? child_frequency(child_counts, child_count, continuation.key.child_hash, metrics)
                : referenced_child_frequency(*child_frequencies, continuation.key.child_hash);
            if (!frequency || *frequency < continuation.agg.frequency
                || !write_continuation(writer, continuation.key, continuation.agg, *frequency))
            {
                remove_temp();
                return tl::unexpected {error {error_code::io_failure, "write failed"}};
            }
        }
    }

    if (!writer.flush()) {
        remove_temp();
        return tl::unexpected {error {error_code::io_failure, "write failed"}};
    }
    output.close();
    if (!output) {
        remove_temp();
        return tl::unexpected {error {error_code::io_failure, "write failed"}};
    }
    std::error_code rename_error;
    std::filesystem::rename(temp_path, path, rename_error);
    if (rename_error) {
        remove_temp();
        return tl::unexpected {error {error_code::io_failure, "publish failed"}};
    }
    return {};
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
    replay_scratch scratch;
    build_spills spills;
    // The only node aggregated past max_root_ply -- see the format_version
    // comment above and accumulate_game's full_coverage_root_hash parameter.
    std::uint64_t full_coverage_root_hash {motif::chess::board {}.hash()};
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
        , spills {std::filesystem::path {path.string() + ".build"}, options.spill_threshold, &metrics}
    {
        metrics.child_counts_external = static_cast<bool>(child_counts);
        scratch.edges.reserve(per_game_scratch_reserve);
        scratch.roots.reserve(per_game_scratch_reserve);
        if (!child_counts) {
            scratch.visited.emplace();
        }
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
    auto accumulated =
        accumulate_game(game, state_->options.max_root_ply, state_->full_coverage_root_hash, visits, state_->scratch, state_->spills);
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

auto opening_tree_index::build(game_store& store,
                               std::filesystem::path const& path,
                               build_options const& opts,
                               opening_tree_index_build_metrics* const metrics) -> result<void>
{
    return build(store, path, opts, metrics, {});
}

auto opening_tree_index::build(game_store& store,
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
    if (!read_little_endian(input, version) || version != format_version || !read_little_endian(input, max_root_ply)
        || !read_little_endian(input, source_game_count) || !read_little_endian(input, node_count)
        || node_count > (file_size - fixed_header_size) / minimum_node_size)
    {
        return tl::unexpected {error {error_code::io_failure, "bad header"}};
    }

    opening_tree_index index;
    index.path_ = path;
    index.source_game_count_ = source_game_count;
    index.max_root_ply_ = max_root_ply;
    index.node_hashes_.reserve(static_cast<std::size_t>(node_count));
    index.node_game_counts_.reserve(static_cast<std::size_t>(node_count));
    index.node_data_offsets_.reserve(static_cast<std::size_t>(node_count));
    index.node_continuation_counts_.reserve(static_cast<std::size_t>(node_count));
    index.node_complete_.reserve(static_cast<std::size_t>(node_count));
    auto complete_node_count = std::size_t {0};

    for (std::uint64_t node_index = 0; node_index < node_count; ++node_index) {
        std::uint64_t node_hash {};
        std::uint64_t game_count_raw {};
        std::uint64_t continuation_count {};
        std::uint64_t complete_raw {};
        if (!read_little_endian(input, node_hash) || !read_varint(input, game_count_raw) || !read_varint(input, continuation_count)
            || !read_varint(input, complete_raw) || game_count_raw == 0U || game_count_raw > std::numeric_limits<std::uint32_t>::max()
            || continuation_count > std::numeric_limits<std::uint32_t>::max() || complete_raw > 1U
            || (!index.node_hashes_.empty() && index.node_hashes_.back() >= node_hash))
        {
            return tl::unexpected {error {error_code::io_failure, "truncated node header"}};
        }
        index.node_hashes_.push_back(node_hash);
        index.node_game_counts_.push_back(static_cast<std::uint32_t>(game_count_raw));
        auto const data_offset = input.tellg();
        if (data_offset < 0) {
            return tl::unexpected {error {error_code::io_failure, "invalid node offset"}};
        }
        index.node_data_offsets_.push_back(static_cast<std::uint64_t>(data_offset));
        index.node_continuation_counts_.push_back(static_cast<std::uint32_t>(continuation_count));
        auto const is_complete = complete_raw == 1U;
        if (is_complete && (node_hash != motif::chess::board {}.hash() || game_count_raw != source_game_count || complete_node_count != 0U))
        {
            return tl::unexpected {error {error_code::io_failure, "invalid complete node"}};
        }
        complete_node_count += is_complete ? 1U : 0U;
        index.node_complete_.push_back(is_complete ? std::uint8_t {1} : std::uint8_t {0});

        auto const max_ply_for_validation = is_complete ? std::numeric_limits<std::uint16_t>::max() : max_root_ply;
        for (std::uint64_t cont_index = 0; cont_index < continuation_count; ++cont_index) {
            if (auto continuation = read_continuation(input, max_ply_for_validation, static_cast<std::uint32_t>(game_count_raw));
                !continuation)
            {
                return tl::unexpected {continuation.error()};
            }
        }
    }

    if (input.peek() != std::char_traits<char>::eof()) {
        return tl::unexpected {error {error_code::io_failure, "trailing data"}};
    }

    return index;
}

auto opening_tree_index::query_opening_stats(zobrist_hash const hash) const -> result<std::vector<opening_stat_agg_row>>
{
    auto const lower = std::ranges::lower_bound(node_hashes_, hash.value);
    auto const node_index = static_cast<std::size_t>(lower - node_hashes_.begin());
    if (node_index >= node_hashes_.size() || node_hashes_[node_index] != hash.value) {
        return std::vector<opening_stat_agg_row> {};
    }

    std::ifstream input {path_, std::ios::binary};
    input.seekg(static_cast<std::streamoff>(node_data_offsets_[node_index]));
    if (!input) {
        return tl::unexpected {error {error_code::io_failure, "cannot seek node"}};
    }
    auto const max_ply_for_validation =
        node_complete_[node_index] == std::uint8_t {1} ? std::numeric_limits<std::uint16_t>::max() : max_root_ply_;
    auto rows = std::vector<opening_stat_agg_row> {};
    rows.reserve(node_continuation_counts_[node_index]);
    for (std::uint32_t index = 0; index < node_continuation_counts_[node_index]; ++index) {
        auto row = read_continuation(input, max_ply_for_validation, node_game_counts_[node_index]);
        if (!row) {
            return tl::unexpected {row.error()};
        }
        rows.push_back(*row);
    }
    return rows;
}

auto opening_tree_index::game_count(zobrist_hash const hash) const -> result<std::uint32_t>
{
    auto const lower = std::ranges::lower_bound(node_hashes_, hash.value);
    auto const node_index = static_cast<std::size_t>(lower - node_hashes_.begin());
    if (node_index >= node_hashes_.size() || node_hashes_[node_index] != hash.value) {
        return std::uint32_t {0};
    }
    return node_game_counts_[node_index];
}

auto opening_tree_index::is_complete(zobrist_hash const hash) const -> bool
{
    auto const lower = std::ranges::lower_bound(node_hashes_, hash.value);
    auto const node_index = static_cast<std::size_t>(lower - node_hashes_.begin());
    if (node_index >= node_hashes_.size() || node_hashes_[node_index] != hash.value) {
        return false;
    }
    return node_complete_[node_index] == std::uint8_t {1};
}

auto opening_tree_index::source_game_count() const noexcept -> std::uint64_t
{
    return source_game_count_;
}

auto opening_tree_index::max_root_ply() const noexcept -> std::uint16_t
{
    return max_root_ply_;
}

}  // namespace motif::db
