#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "motif/db/position_prefix_postings.hpp"

#include <tl/expected.hpp>

#include "motif/db/error.hpp"
#include "motif/db/types.hpp"

namespace motif::db
{

namespace
{

constexpr auto magic = std::array<char, 8> {'M', 'O', 'T', 'I', 'F', 'P', 'P', '1'};
constexpr auto format_version = std::uint32_t {1};
constexpr auto max_prefix_bits = std::uint8_t {24};
constexpr auto byte_bits = std::size_t {8};
constexpr auto byte_mask = std::uint64_t {0xff};
constexpr auto zobrist_hash_bits = std::uint8_t {64};

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

auto valid_prefix_bits(std::uint8_t prefix_bits) noexcept -> bool
{
    return prefix_bits > 0U && prefix_bits <= max_prefix_bits;
}

}  // namespace

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- distinct config knobs, not positionally confusable in practice
position_prefix_postings::position_prefix_postings(std::filesystem::path path, std::uint8_t prefix_bits, std::size_t spill_threshold)
    : path_ {std::move(path)}
    , prefix_bits_ {prefix_bits}
    , spill_threshold_ {spill_threshold}
{
}

auto position_prefix_postings::append(std::span<position_row const> rows) -> result<void>
{
    if (!valid_prefix_bits(prefix_bits_)) {
        return tl::unexpected {error_code::invalid_argument};
    }

    records_.reserve(std::min(records_.size() + rows.size(), spill_threshold_));
    for (auto const& row : rows) {
        records_.push_back(record {.prefix = prefix(row.zobrist_hash), .game_key = row.game_id});
        if (records_.size() >= spill_threshold_) {
            if (auto spill_res = spill_current_buffer(); !spill_res) {
                return spill_res;
            }
        }
    }
    return {};
}

auto position_prefix_postings::spill_current_buffer() -> result<void>
{
    if (records_.empty()) {
        return {};
    }

    std::ranges::sort(records_);
    auto const unique_end = std::ranges::unique(records_);
    records_.erase(unique_end.begin(), unique_end.end());

    auto const spill_path = std::filesystem::path {path_.string() + ".spill" + std::to_string(spill_paths_.size())};
    std::ofstream output {spill_path, std::ios::binary | std::ios::trunc};
    if (!output || !write_little_endian(output, static_cast<std::uint64_t>(records_.size()))) {
        return tl::unexpected {error_code::io_failure};
    }
    for (auto const& item : records_) {
        if (!write_little_endian(output, item.prefix) || !write_little_endian(output, item.game_key.value)) {
            return tl::unexpected {error_code::io_failure};
        }
    }
    if (!output) {
        return tl::unexpected {error_code::io_failure};
    }

    spill_paths_.push_back(spill_path);
    records_.clear();
    return {};
}

auto position_prefix_postings::finalize() -> result<void>
{
    if (!valid_prefix_bits(prefix_bits_)) {
        return tl::unexpected {error_code::invalid_argument};
    }

    return spill_paths_.empty() ? finalize_in_memory() : finalize_external_merge();
}

auto position_prefix_postings::finalize_in_memory() -> result<void>
{
    std::ranges::sort(records_);
    auto const unique_end = std::ranges::unique(records_);
    records_.erase(unique_end.begin(), unique_end.end());

    auto const bucket_count = std::size_t {1} << prefix_bits_;
    offsets_.assign(bucket_count + 1U, 0U);
    for (auto const& item : records_) {
        ++offsets_[static_cast<std::size_t>(item.prefix) + 1U];
    }
    for (std::size_t index = 1U; index < offsets_.size(); ++index) {
        offsets_[index] += offsets_[index - 1U];
    }

    postings_.clear();
    postings_.reserve(records_.size());
    for (auto const& item : records_) {
        postings_.push_back(item.game_key);
    }

    std::ofstream output {path_, std::ios::binary | std::ios::trunc};
    if (!output) {
        return tl::unexpected {error_code::io_failure};
    }
    output.write(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!output || !write_little_endian(output, format_version) || !write_little_endian(output, prefix_bits_)
        || !write_little_endian(output, std::uint8_t {0}) || !write_little_endian(output, std::uint16_t {0})
        || !write_little_endian(output, static_cast<std::uint64_t>(bucket_count))
        || !write_little_endian(output, static_cast<std::uint64_t>(postings_.size())))
    {
        return tl::unexpected {error_code::io_failure};
    }
    for (auto const offset : offsets_) {
        if (!write_little_endian(output, offset)) {
            return tl::unexpected {error_code::io_failure};
        }
    }
    for (auto const game_key : postings_) {
        if (!write_little_endian(output, game_key.value)) {
            return tl::unexpected {error_code::io_failure};
        }
    }
    records_.clear();
    return {};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- streaming k-way merge with per-step error checks inflates complexity
auto position_prefix_postings::finalize_external_merge() -> result<void>
{
    if (auto spill_res = spill_current_buffer(); !spill_res) {
        return spill_res;
    }

    auto const bucket_count = std::size_t {1} << prefix_bits_;

    struct run_cursor
    {
        std::ifstream input;
        std::uint64_t remaining {};
        record current {};
    };

    std::vector<run_cursor> cursors;
    cursors.reserve(spill_paths_.size());
    for (auto const& spill_path : spill_paths_) {
        run_cursor cursor {.input = std::ifstream {spill_path, std::ios::binary}};
        if (!cursor.input || !read_little_endian(cursor.input, cursor.remaining)) {
            return tl::unexpected {error_code::io_failure};
        }
        cursors.push_back(std::move(cursor));
    }

    auto advance_cursor = [](run_cursor& cursor) -> result<bool>
    {
        if (cursor.remaining == 0U) {
            return false;
        }
        if (!read_little_endian(cursor.input, cursor.current.prefix) || !read_little_endian(cursor.input, cursor.current.game_key.value)) {
            return tl::unexpected {error_code::io_failure};
        }
        --cursor.remaining;
        return true;
    };

    auto compare = [&cursors](std::size_t lhs, std::size_t rhs) -> bool { return cursors[rhs].current < cursors[lhs].current; };
    std::vector<std::size_t> heap;
    heap.reserve(cursors.size());
    for (std::size_t index = 0; index < cursors.size(); ++index) {
        auto const has_record = advance_cursor(cursors[index]);
        if (!has_record) {
            return tl::unexpected {has_record.error()};
        }
        if (*has_record) {
            heap.push_back(index);
        }
    }
    std::ranges::make_heap(heap, compare);

    auto const scratch_path = std::filesystem::path {path_.string() + ".postings.tmp"};
    std::vector<std::uint64_t> bucket_counts(bucket_count, 0U);
    std::uint64_t total_count {0};
    {
        std::ofstream scratch {scratch_path, std::ios::binary | std::ios::trunc};
        if (!scratch) {
            return tl::unexpected {error_code::io_failure};
        }

        bool has_prev {false};
        record prev {};

        while (!heap.empty()) {
            std::ranges::pop_heap(heap, compare);
            auto const index = heap.back();
            heap.pop_back();

            auto const current = cursors[index].current;
            if (!has_prev || current != prev) {
                if (!write_little_endian(scratch, current.game_key.value)) {
                    return tl::unexpected {error_code::io_failure};
                }
                ++bucket_counts[current.prefix];
                ++total_count;
                prev = current;
                has_prev = true;
            }

            auto const has_next = advance_cursor(cursors[index]);
            if (!has_next) {
                return tl::unexpected {has_next.error()};
            }
            if (*has_next) {
                heap.push_back(index);
                std::ranges::push_heap(heap, compare);
            }
        }
        if (!scratch) {
            return tl::unexpected {error_code::io_failure};
        }
    }  // scratch flushed and closed here, before it is reopened for reading below

    offsets_.assign(bucket_count + 1U, 0U);
    for (std::size_t index = 0; index < bucket_count; ++index) {
        offsets_[index + 1U] = offsets_[index] + bucket_counts[index];
    }

    {
        std::ofstream output {path_, std::ios::binary | std::ios::trunc};
        if (!output) {
            return tl::unexpected {error_code::io_failure};
        }
        output.write(magic.data(), static_cast<std::streamsize>(magic.size()));
        if (!output || !write_little_endian(output, format_version) || !write_little_endian(output, prefix_bits_)
            || !write_little_endian(output, std::uint8_t {0}) || !write_little_endian(output, std::uint16_t {0})
            || !write_little_endian(output, static_cast<std::uint64_t>(bucket_count)) || !write_little_endian(output, total_count))
        {
            return tl::unexpected {error_code::io_failure};
        }
        for (auto const offset : offsets_) {
            if (!write_little_endian(output, offset)) {
                return tl::unexpected {error_code::io_failure};
            }
        }

        std::ifstream const scratch_input {scratch_path, std::ios::binary};
        if (!scratch_input) {
            return tl::unexpected {error_code::io_failure};
        }
        output << scratch_input.rdbuf();
        if (!output) {
            return tl::unexpected {error_code::io_failure};
        }
    }

    std::error_code remove_err;
    std::filesystem::remove(scratch_path, remove_err);
    for (auto const& spill_path : spill_paths_) {
        std::filesystem::remove(spill_path, remove_err);
    }
    spill_paths_.clear();
    postings_.clear();
    records_.clear();
    return {};
}

auto position_prefix_postings::open() -> result<void>
{
    std::ifstream input {path_, std::ios::binary};
    if (!input) {
        return tl::unexpected {error_code::io_failure};
    }

    auto file_magic = std::array<char, magic.size()> {};
    input.read(file_magic.data(), static_cast<std::streamsize>(file_magic.size()));
    std::uint32_t version {};
    std::uint8_t stored_prefix_bits {};
    std::uint8_t reserved_byte {};
    std::uint16_t reserved_word {};
    std::uint64_t bucket_count {};
    std::uint64_t postings_count {};
    if (!input || file_magic != magic || !read_little_endian(input, version) || !read_little_endian(input, stored_prefix_bits)
        || !read_little_endian(input, reserved_byte) || !read_little_endian(input, reserved_word)
        || !read_little_endian(input, bucket_count) || !read_little_endian(input, postings_count))
    {
        return tl::unexpected {error_code::schema_mismatch};
    }
    if (version != format_version || stored_prefix_bits != prefix_bits_ || !valid_prefix_bits(stored_prefix_bits)
        || bucket_count != (std::uint64_t {1} << stored_prefix_bits)
        || postings_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
    {
        return tl::unexpected {error_code::schema_mismatch};
    }

    offsets_.assign(static_cast<std::size_t>(bucket_count) + 1U, 0U);
    for (auto& offset : offsets_) {
        if (!read_little_endian(input, offset)) {
            return tl::unexpected {error_code::schema_mismatch};
        }
    }
    if (offsets_.front() != 0U || offsets_.back() != postings_count || !std::ranges::is_sorted(offsets_)) {
        return tl::unexpected {error_code::schema_mismatch};
    }

    postings_.assign(static_cast<std::size_t>(postings_count), game_id {});
    for (auto& game_key : postings_) {
        if (!read_little_endian(input, game_key.value)) {
            return tl::unexpected {error_code::schema_mismatch};
        }
    }
    return {};
}

auto position_prefix_postings::candidates(zobrist_hash hash) const -> result<std::vector<game_id>>
{
    if (offsets_.empty()) {
        return tl::unexpected {error_code::invalid_argument};
    }
    auto const prefix_value = prefix(hash);
    auto const begin = static_cast<std::size_t>(offsets_[prefix_value]);
    auto const end = static_cast<std::size_t>(offsets_[static_cast<std::size_t>(prefix_value) + 1U]);
    return std::vector<game_id> {postings_.begin() + static_cast<std::ptrdiff_t>(begin),
                                 postings_.begin() + static_cast<std::ptrdiff_t>(end)};
}

auto position_prefix_postings::prefix(zobrist_hash hash) const noexcept -> std::uint32_t
{
    auto const shift = static_cast<std::uint64_t>(zobrist_hash_bits) - static_cast<std::uint64_t>(prefix_bits_);
    return static_cast<std::uint32_t>(hash.value >> shift);
}

}  // namespace motif::db
