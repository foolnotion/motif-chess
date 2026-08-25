#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "motif/db/position_postings.hpp"

#include <spdlog/spdlog.h>
#include <tl/expected.hpp>

#include "motif/chess/chess.hpp"
#include "motif/db/game_store.hpp"

namespace motif::db
{

namespace
{

constexpr auto magic = std::array<char, 8> {'M', 'O', 'T', 'I', 'F', 'P', 'O', '2'};
constexpr auto format_version = std::uint32_t {6};
constexpr auto byte_bits = std::size_t {8};
constexpr auto byte_mask = std::uint64_t {0xff};

// header: magic(8) + version(4) + header_size(4) + indexed_game_count(8) +
// occurrence_count(8) + distinct_hash_count(8) + metadata_offset(8) +
// metadata_count(8) + directory_offset(8) + directory_byte_length(8) +
// sparse_offset(8) + sparse_count(8)
constexpr auto fixed_header_size = std::uint64_t {88};
constexpr auto metadata_record_size = std::uint64_t {10};  // game_id(4) + result(1) + flags(1) + white_elo(2) + black_elo(2)
constexpr auto sparse_record_size = std::uint64_t {22};  // first_hash(8) + block_offset(8) + block_byte_length(4) + entry_count(2)
constexpr auto max_directory_block_entries = std::size_t {256};
constexpr auto directory_block_header_size = std::uint64_t {18};
constexpr auto directory_bitmap_max_size = std::uint64_t {32};
constexpr auto hash_delta_low_bytes = std::size_t {5};
constexpr auto hash_delta_low_bits = std::uint32_t {40};
constexpr auto bitmap_bits_per_byte = std::size_t {8};
constexpr auto directory_entry_max_size = std::uint64_t {60};
constexpr auto max_directory_block_size = directory_block_header_size + (2U * directory_bitmap_max_size)
    + ((max_directory_block_entries - 1U) * hash_delta_low_bytes) + (max_directory_block_entries * directory_entry_max_size);

constexpr auto spill_record_size = std::size_t {14};
constexpr auto spill_read_buffer_size = spill_record_size * std::size_t {4096};
constexpr auto spill_write_buffer_size = spill_record_size * std::size_t {4096};
constexpr auto stream_write_buffer_size = std::size_t {1U << 16U};
constexpr auto hash_delta_40_bit_limit = std::uint64_t {1} << 40U;
constexpr auto hash_delta_48_bit_limit = std::uint64_t {1} << 48U;

auto bitmap_padding_is_zero(std::span<char const> const bitmap, std::size_t const used_bits) -> bool
{
    if (bitmap.empty() || used_bits % bitmap_bits_per_byte == 0U) {
        return true;
    }
    auto const used_in_last = used_bits % bitmap_bits_per_byte;
    auto const padding_mask = static_cast<std::uint8_t>(0xffU << used_in_last);
    return (static_cast<std::uint8_t>(bitmap.back()) & padding_mask) == 0U;
}

// LEB128: 7 payload bits per byte, high bit is the continuation flag. A
// 64-bit value needs at most 10 such bytes (70 bits of capacity); the 10th
// byte's shift lands at bit 63, so only its low payload bit may be set.
constexpr auto uleb128_max_bytes = std::size_t {10};
constexpr auto uleb128_bits_per_byte = std::uint32_t {7};
constexpr auto uleb128_payload_mask = std::uint8_t {0x7f};
constexpr auto uleb128_continuation_bit = std::uint8_t {0x80};
constexpr auto uleb128_value_bit_width = std::uint32_t {64};
constexpr auto uleb128_final_byte_shift = std::uint32_t {63};

constexpr auto result_black_win = std::uint8_t {0};
constexpr auto result_draw_or_other = std::uint8_t {1};
constexpr auto result_white_win = std::uint8_t {2};
constexpr auto metadata_flag_white_elo = std::uint8_t {0x01};
constexpr auto metadata_flag_black_elo = std::uint8_t {0x02};
constexpr auto metadata_flag_mask = std::uint8_t {0x03};

auto result_code(std::string const& value) noexcept -> std::int8_t
{
    if (value == "1-0") {
        return 1;
    }
    if (value == "0-1") {
        return -1;
    }
    return 0;
}

auto encode_result(std::int8_t const result) noexcept -> std::uint8_t
{
    if (result > 0) {
        return result_white_win;
    }
    if (result < 0) {
        return result_black_win;
    }
    return result_draw_or_other;
}

auto decode_result(std::uint8_t const encoded) noexcept -> std::int8_t
{
    if (encoded == result_white_win) {
        return 1;
    }
    if (encoded == result_black_win) {
        return -1;
    }
    return 0;
}

auto checked_add(std::uint64_t const left, std::uint64_t const right, std::uint64_t& result) noexcept -> bool
{
    if (left > std::numeric_limits<std::uint64_t>::max() - right) {
        return false;
    }
    result = left + right;
    return true;
}

auto checked_multiply(std::uint64_t const left, std::uint64_t const right, std::uint64_t& result) noexcept -> bool
{
    if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

auto representable_stream_offset(std::uint64_t const value) noexcept -> bool
{
    return value <= static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max());
}

auto narrow_elo(std::optional<std::int32_t> const& elo) -> result<std::optional<std::int16_t>>
{
    if (!elo) {
        return std::optional<std::int16_t> {};
    }
    if (*elo < std::numeric_limits<std::int16_t>::min() || *elo > std::numeric_limits<std::int16_t>::max()) {
        return tl::unexpected {error_code::io_failure};
    }
    return static_cast<std::int16_t>(*elo);
}

template<typename Integer>
auto write_little_endian(std::ofstream& output, Integer value) -> bool
{
    static_assert(std::is_unsigned_v<Integer>);
    auto raw_value = std::uint64_t {value};
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        output.put(static_cast<char>(raw_value & byte_mask));
        raw_value >>= byte_bits;
    }
    return static_cast<bool>(output);
}

template<typename Integer>
auto write_little_endian(std::fstream& output, Integer value) -> bool
{
    static_assert(std::is_unsigned_v<Integer>);
    auto raw_value = std::uint64_t {value};
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        output.put(static_cast<char>(raw_value & byte_mask));
        raw_value >>= byte_bits;
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

template<typename Integer, std::size_t Size>
void write_little_endian(std::array<char, Size>& output, std::size_t& offset, Integer value)
{
    static_assert(std::is_unsigned_v<Integer>);
    auto raw_value = std::uint64_t {value};
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index) -- fixed record layouts below always fit their fields.
        output[offset++] = static_cast<char>(raw_value & byte_mask);
        raw_value >>= byte_bits;
    }
}

template<typename Integer>
auto read_little_endian(std::span<char const> const input, std::size_t& offset, Integer& value) -> bool
{
    static_assert(std::is_unsigned_v<Integer>);
    if (offset + sizeof(Integer) > input.size()) {
        return false;
    }
    auto raw_value = std::uint64_t {0};
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index) -- bounds are checked immediately above.
        raw_value |= static_cast<std::uint64_t>(static_cast<unsigned char>(input[offset++])) << (index * byte_bits);
    }
    value = static_cast<Integer>(raw_value);
    return true;
}

// Number of bytes the canonical (minimal) uleb128 encoding of value occupies.
auto uleb128_length(std::uint64_t value) noexcept -> std::size_t
{
    auto length = std::size_t {1};
    value >>= uleb128_bits_per_byte;
    while (value != 0U) {
        ++length;
        value >>= uleb128_bits_per_byte;
    }
    return length;
}

// Rejects truncated input, encodings longer than the 10-byte maximum for a
// 64-bit value, values that overflow 64 bits, and non-canonical (overlong)
// encodings.
auto read_uleb128(std::span<char const> const data, std::size_t& offset, std::uint64_t& value) -> bool
{
    auto const start = offset;
    value = 0U;
    auto shift = 0U;
    for (std::size_t index = 0; index < uleb128_max_bytes; ++index) {
        if (offset >= data.size()) {
            return false;
        }
        auto const byte = static_cast<unsigned char>(data[offset]);
        ++offset;
        auto const payload = static_cast<std::uint64_t>(byte & uleb128_payload_mask);
        if (shift >= uleb128_value_bit_width || (shift == uleb128_final_byte_shift && payload > 1U)) {
            return false;
        }
        value |= payload << shift;
        if ((byte & uleb128_continuation_bit) == 0U) {
            return uleb128_length(value) == offset - start;
        }
        shift += uleb128_bits_per_byte;
    }
    return false;
}

// Buffers fixed-width and uleb128-encoded writes to an ofstream and tracks
// the logical byte offset written so far, so callers never need tellp().
class buffered_file_writer
{
  public:
    explicit buffered_file_writer(std::ofstream& stream) noexcept
        : stream_ {&stream}
    {
    }

    auto put_byte(char const value) -> bool
    {
        if (buffer_size_ == buffer_.size() && !flush()) {
            return false;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index) -- buffer_size_ is checked against buffer_.size() above.
        buffer_[buffer_size_++] = value;
        ++offset_;
        return true;
    }

    template<typename Integer>
    auto write_fixed(Integer value) -> bool
    {
        static_assert(std::is_unsigned_v<Integer>);
        auto raw_value = std::uint64_t {value};
        for (std::size_t index = 0; index < sizeof(Integer); ++index) {
            if (!put_byte(static_cast<char>(raw_value & byte_mask))) {
                return false;
            }
            raw_value >>= byte_bits;
        }
        return true;
    }

    auto write_uleb128(std::uint64_t value) -> bool
    {
        while (true) {
            auto byte = static_cast<std::uint8_t>(value & uleb128_payload_mask);
            value >>= uleb128_bits_per_byte;
            if (value != 0U) {
                byte |= uleb128_continuation_bit;
            }
            if (!put_byte(static_cast<char>(byte))) {
                return false;
            }
            if (value == 0U) {
                return true;
            }
        }
    }

    auto flush() -> bool
    {
        if (buffer_size_ > 0U) {
            stream_->write(buffer_.data(), static_cast<std::streamsize>(buffer_size_));
            buffer_size_ = 0U;
        }
        return static_cast<bool>(*stream_);
    }

    // Advances the tracked logical offset for bytes written directly to the
    // wrapped stream outside this writer (e.g. a bulk rdbuf() copy). The
    // buffer must be empty (flush() called) before calling this.
    void skip_offset(std::uint64_t const bytes) noexcept { offset_ += bytes; }

    [[nodiscard]] auto offset() const noexcept -> std::uint64_t { return offset_; }

  private:
    std::ofstream* stream_;
    std::array<char, stream_write_buffer_size> buffer_ {};
    std::size_t buffer_size_ {0};
    std::uint64_t offset_ {0};
};

// One decoded compressed-directory-block entry. Reader-transient: never
// retained beyond the query or open() validation that produced it.
struct directory_block_entry
{
    zobrist_hash hash {};
    std::uint64_t posting_offset {};
    std::uint64_t posting_byte_length {};
    std::uint32_t occurrence_count {};
    std::uint32_t distinct_game_count {};
    std::uint16_t min_ply {};
    std::uint16_t max_ply {};
};

// Decodes one complete compressed directory block already read into memory,
// validating internal ordering and bounds. Does not validate cross-block
// invariants (those require the caller's running state).
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- validates each dependent field of one compact binary block in decode order.
auto decode_directory_block(std::span<char const> const bytes) -> result<std::vector<directory_block_entry>>
{
    auto offset = std::size_t {0};
    auto first_hash = std::uint64_t {};
    auto first_posting_offset = std::uint64_t {};
    auto entry_count = std::uint16_t {};
    if (!read_little_endian(bytes, offset, first_hash) || !read_little_endian(bytes, offset, first_posting_offset)
        || !read_little_endian(bytes, offset, entry_count) || entry_count == 0U
        || entry_count > static_cast<std::uint16_t>(max_directory_block_entries))
    {
        return tl::unexpected {error_code::schema_mismatch};
    }
    std::vector<directory_block_entry> entries;
    entries.reserve(entry_count);
    auto const hash_delta_count = static_cast<std::size_t>(entry_count - 1U);
    auto const hash_exception_bitmap_size = (hash_delta_count + bitmap_bits_per_byte - 1U) / bitmap_bits_per_byte;
    auto const occurrence_exception_bitmap_size =
        (static_cast<std::size_t>(entry_count) + bitmap_bits_per_byte - 1U) / bitmap_bits_per_byte;
    if (offset + hash_exception_bitmap_size + occurrence_exception_bitmap_size > bytes.size()) {
        return tl::unexpected {error_code::schema_mismatch};
    }
    auto const hash_exception_bitmap = bytes.subspan(offset, hash_exception_bitmap_size);
    offset += hash_exception_bitmap_size;
    auto const occurrence_exception_bitmap = bytes.subspan(offset, occurrence_exception_bitmap_size);
    offset += occurrence_exception_bitmap_size;
    if (!bitmap_padding_is_zero(hash_exception_bitmap, hash_delta_count)
        || !bitmap_padding_is_zero(occurrence_exception_bitmap, entry_count))
    {
        return tl::unexpected {error_code::schema_mismatch};
    }

    std::vector<std::uint64_t> hash_deltas;
    hash_deltas.reserve(hash_delta_count);
    for (std::size_t index = 0; index < hash_delta_count; ++index) {
        if (offset + hash_delta_low_bytes > bytes.size()) {
            return tl::unexpected {error_code::schema_mismatch};
        }
        auto low = std::uint64_t {0};
        for (std::size_t byte_index = 0; byte_index < hash_delta_low_bytes; ++byte_index) {
            low |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset++])) << (byte_index * byte_bits);
        }
        hash_deltas.push_back(low);
    }
    for (std::size_t index = 0; index < hash_delta_count; ++index) {
        auto const mask = static_cast<std::uint8_t>(1U << (index % bitmap_bits_per_byte));
        if ((static_cast<std::uint8_t>(hash_exception_bitmap[index / bitmap_bits_per_byte]) & mask) == 0U) {
            continue;
        }
        auto high = std::uint64_t {0};
        if (!read_uleb128(bytes, offset, high) || high == 0U || high > (std::numeric_limits<std::uint64_t>::max() >> hash_delta_low_bits)) {
            return tl::unexpected {error_code::schema_mismatch};
        }
        hash_deltas[index] |= high << hash_delta_low_bits;
    }

    auto current_hash = first_hash;
    auto current_offset = first_posting_offset;
    auto previous_posting_byte_length = std::uint64_t {0};
    for (std::uint16_t index = 0; index < entry_count; ++index) {
        if (index > 0U) {
            auto const hash_delta = hash_deltas[static_cast<std::size_t>(index - 1U)];
            if (hash_delta == 0U || current_hash > std::numeric_limits<std::uint64_t>::max() - hash_delta
                || current_offset > std::numeric_limits<std::uint64_t>::max() - previous_posting_byte_length)
            {
                return tl::unexpected {error_code::schema_mismatch};
            }
            current_hash += hash_delta;
            current_offset += previous_posting_byte_length;
        }
        auto posting_byte_length = std::uint64_t {};
        auto distinct_game_count = std::uint64_t {};
        auto occurrence_count = std::uint64_t {};
        auto min_ply = std::uint64_t {};
        auto max_ply = std::uint64_t {};
        if (!read_uleb128(bytes, offset, posting_byte_length) || posting_byte_length == 0U
            || !read_uleb128(bytes, offset, distinct_game_count) || distinct_game_count == 0U)
        {
            return tl::unexpected {error_code::schema_mismatch};
        }
        auto const occurrence_mask = static_cast<std::uint8_t>(1U << (index % bitmap_bits_per_byte));
        auto const occurrence_differs =
            (static_cast<std::uint8_t>(occurrence_exception_bitmap[static_cast<std::size_t>(index) / bitmap_bits_per_byte])
             & occurrence_mask)
            != 0U;
        occurrence_count = distinct_game_count;
        if (occurrence_differs && (!read_uleb128(bytes, offset, occurrence_count) || occurrence_count <= distinct_game_count)) {
            return tl::unexpected {error_code::schema_mismatch};
        }
        if (occurrence_count > posting_byte_length || occurrence_count > std::numeric_limits<std::uint32_t>::max()
            || distinct_game_count > std::numeric_limits<std::uint32_t>::max() || !read_uleb128(bytes, offset, min_ply)
            || !read_uleb128(bytes, offset, max_ply) || min_ply > max_ply || max_ply > std::numeric_limits<std::uint16_t>::max())
        {
            return tl::unexpected {error_code::schema_mismatch};
        }
        entries.push_back(directory_block_entry {.hash = zobrist_hash {current_hash},
                                                 .posting_offset = current_offset,
                                                 .posting_byte_length = posting_byte_length,
                                                 .occurrence_count = static_cast<std::uint32_t>(occurrence_count),
                                                 .distinct_game_count = static_cast<std::uint32_t>(distinct_game_count),
                                                 .min_ply = static_cast<std::uint16_t>(min_ply),
                                                 .max_ply = static_cast<std::uint16_t>(max_ply)});
        previous_posting_byte_length = posting_byte_length;
    }
    if (offset != bytes.size()) {
        return tl::unexpected {error_code::schema_mismatch};
    }
    return entries;
}

// One (game_id, ply) occurrence decoded from a posting block, in the
// canonical ascending (game_id, ply) order.
struct decoded_occurrence
{
    game_id id {};
    std::uint16_t ply {};
};

// Decodes and validates one posting block while retaining only the requested
// occurrence window. The complete byte stream is still checked so pagination
// cannot hide corruption after the returned rows.
auto decode_posting_block_window(std::span<char const> const bytes,
                                 directory_block_entry const& entry,
                                 std::size_t const limit,
                                 std::size_t const skip) -> result<std::vector<decoded_occurrence>>
{
    auto const begin = std::min(skip, static_cast<std::size_t>(entry.occurrence_count));
    auto const remaining = static_cast<std::size_t>(entry.occurrence_count) - begin;
    auto const count = limit == 0U ? remaining : std::min(limit, remaining);
    std::vector<decoded_occurrence> occurrences;
    occurrences.reserve(count);
    auto occurrence_index = std::size_t {0};
    auto offset = std::size_t {0};
    auto previous_game_id = std::uint64_t {0};
    auto min_ply = std::numeric_limits<std::uint16_t>::max();
    auto max_ply = std::uint16_t {0};
    for (std::uint32_t group = 0; group < entry.distinct_game_count; ++group) {
        auto game_id_delta = std::uint64_t {};
        if (!read_uleb128(bytes, offset, game_id_delta) || (group > 0U && game_id_delta == 0U)
            || previous_game_id > std::numeric_limits<std::uint64_t>::max() - game_id_delta)
        {
            return tl::unexpected {error_code::schema_mismatch};
        }
        auto const current_game_id = previous_game_id + game_id_delta;
        if (current_game_id == 0U || current_game_id > std::numeric_limits<std::uint32_t>::max()) {
            return tl::unexpected {error_code::schema_mismatch};
        }
        previous_game_id = current_game_id;
        auto ply_count = std::uint64_t {};
        auto first_ply = std::uint64_t {};
        if (!read_uleb128(bytes, offset, ply_count) || ply_count == 0U || !read_uleb128(bytes, offset, first_ply)
            || first_ply > std::numeric_limits<std::uint16_t>::max())
        {
            return tl::unexpected {error_code::schema_mismatch};
        }
        auto ply = static_cast<std::uint16_t>(first_ply);
        auto append_if_requested = [&]() -> void
        {
            if (occurrence_index >= begin && occurrences.size() < count) {
                occurrences.push_back(decoded_occurrence {.id = game_id {static_cast<std::uint32_t>(current_game_id)}, .ply = ply});
            }
            ++occurrence_index;
            min_ply = std::min(min_ply, ply);
            max_ply = std::max(max_ply, ply);
        };
        append_if_requested();
        for (std::uint64_t index = 1; index < ply_count; ++index) {
            auto ply_delta = std::uint64_t {};
            if (!read_uleb128(bytes, offset, ply_delta) || ply_delta == 0U
                || static_cast<std::uint64_t>(ply) + ply_delta > std::numeric_limits<std::uint16_t>::max())
            {
                return tl::unexpected {error_code::schema_mismatch};
            }
            ply = static_cast<std::uint16_t>(ply + ply_delta);
            append_if_requested();
        }
    }
    if (offset != bytes.size() || occurrence_index != entry.occurrence_count || min_ply != entry.min_ply || max_ply != entry.max_ply) {
        return tl::unexpected {error_code::schema_mismatch};
    }
    return occurrences;
}

// Decodes only the distinct game ids from a posting block, skipping (but
// still validating the varint framing of) every ply value.
auto decode_posting_block_game_ids(std::span<char const> const bytes, directory_block_entry const& entry) -> result<std::vector<game_id>>
{
    std::vector<game_id> game_ids;
    game_ids.reserve(entry.distinct_game_count);
    auto offset = std::size_t {0};
    auto previous_game_id = std::uint64_t {0};
    auto total_occurrences = std::uint64_t {0};
    auto min_ply = std::numeric_limits<std::uint16_t>::max();
    auto max_ply = std::uint16_t {0};
    for (std::uint32_t group = 0; group < entry.distinct_game_count; ++group) {
        auto game_id_delta = std::uint64_t {};
        if (!read_uleb128(bytes, offset, game_id_delta) || (group > 0U && game_id_delta == 0U)
            || previous_game_id > std::numeric_limits<std::uint64_t>::max() - game_id_delta)
        {
            return tl::unexpected {error_code::schema_mismatch};
        }
        auto const current_game_id = previous_game_id + game_id_delta;
        if (current_game_id == 0U || current_game_id > std::numeric_limits<std::uint32_t>::max()) {
            return tl::unexpected {error_code::schema_mismatch};
        }
        previous_game_id = current_game_id;
        auto ply_count = std::uint64_t {};
        auto first_ply = std::uint64_t {};
        if (!read_uleb128(bytes, offset, ply_count) || ply_count == 0U || !read_uleb128(bytes, offset, first_ply)
            || first_ply > std::numeric_limits<std::uint16_t>::max())
        {
            return tl::unexpected {error_code::schema_mismatch};
        }
        auto ply = static_cast<std::uint16_t>(first_ply);
        min_ply = std::min(min_ply, ply);
        max_ply = std::max(max_ply, ply);
        for (std::uint64_t index = 1; index < ply_count; ++index) {
            auto ply_delta = std::uint64_t {};
            if (!read_uleb128(bytes, offset, ply_delta) || ply_delta == 0U
                || static_cast<std::uint64_t>(ply) + ply_delta > std::numeric_limits<std::uint16_t>::max())
            {
                return tl::unexpected {error_code::schema_mismatch};
            }
            ply = static_cast<std::uint16_t>(ply + ply_delta);
            min_ply = std::min(min_ply, ply);
            max_ply = std::max(max_ply, ply);
        }
        if (!checked_add(total_occurrences, ply_count, total_occurrences)) {
            return tl::unexpected {error_code::schema_mismatch};
        }
        game_ids.push_back(game_id {static_cast<std::uint32_t>(current_game_id)});
    }
    if (offset != bytes.size() || total_occurrences != entry.occurrence_count || min_ply != entry.min_ply || max_ply != entry.max_ply) {
        return tl::unexpected {error_code::schema_mismatch};
    }
    return game_ids;
}

// Removes a fixed set of owned scratch paths on destruction, always --
// success or failure -- so a merge that fails partway through never leaks
// .spill*/.dirspool/.tmp files. Never touches the final artifact path
// itself, which is only ever reached via an explicit rename on success.
class temp_artifact_guard
{
  public:
    explicit temp_artifact_guard(std::vector<std::filesystem::path>& spill_paths) noexcept
        : spill_paths_ {&spill_paths}
    {
    }

    temp_artifact_guard(temp_artifact_guard const&) = delete;
    auto operator=(temp_artifact_guard const&) -> temp_artifact_guard& = delete;
    temp_artifact_guard(temp_artifact_guard&&) = delete;
    auto operator=(temp_artifact_guard&&) -> temp_artifact_guard& = delete;

    ~temp_artifact_guard()
    {
        std::error_code ignored;
        for (auto const& path : *spill_paths_) {
            std::filesystem::remove(path, ignored);
            ignored.clear();
        }
        spill_paths_->clear();
        for (auto const& path : extra_paths_) {
            std::filesystem::remove(path, ignored);
            ignored.clear();
        }
    }

    void track(std::filesystem::path path) { extra_paths_.push_back(std::move(path)); }

  private:
    std::vector<std::filesystem::path>* spill_paths_;
    std::vector<std::filesystem::path> extra_paths_;
};

}  // namespace

position_postings::position_postings(std::filesystem::path path, std::size_t spill_threshold)
    : path_ {std::move(path)}
    , spill_threshold_ {spill_threshold}
{
}

position_postings::~position_postings()
{
    std::error_code ignored;
    for (auto const& spill_path : spill_paths_) {
        std::filesystem::remove(spill_path, ignored);
        ignored.clear();
    }
}

position_postings::position_postings(position_postings&& other) noexcept = default;

auto position_postings::operator=(position_postings&& other) noexcept -> position_postings&
{
    if (this != &other) {
        std::error_code ignored;
        for (auto const& spill_path : spill_paths_) {
            std::filesystem::remove(spill_path, ignored);
            ignored.clear();
        }
        path_ = std::move(other.path_);
        spill_threshold_ = other.spill_threshold_;
        records_ = std::move(other.records_);
        game_metadata_ = std::move(other.game_metadata_);
        spill_paths_ = std::move(other.spill_paths_);
        indexed_game_count_ = other.indexed_game_count_;
        metrics_ = other.metrics_;
        is_open_ = other.is_open_;
        metadata_ = std::move(other.metadata_);
        sparse_directory_ = std::move(other.sparse_directory_);
        metadata_offset_ = other.metadata_offset_;
        directory_offset_ = other.directory_offset_;
        directory_byte_length_ = other.directory_byte_length_;
        sparse_offset_ = other.sparse_offset_;
        distinct_hash_count_ = other.distinct_hash_count_;
        occurrence_count_ = other.occurrence_count_;
    }
    return *this;
}

auto position_postings::build(game_store const& store,
                              std::filesystem::path path,
                              std::size_t const spill_threshold,
                              position_postings_build_metrics* const metrics) -> result<void>
{
    auto postings = position_postings {std::move(path), spill_threshold};
    auto const game_count = store.count_games();
    if (!game_count || *game_count < 0) {
        return tl::unexpected {error_code::io_failure};
    }
    postings.indexed_game_count_ = static_cast<std::uint64_t>(*game_count);
    postings.metrics_.game_count = postings.indexed_game_count_;
    auto replayed_game_count = std::uint64_t {0};
    auto const replay_started = std::chrono::steady_clock::now();
    auto const replay_result = store.for_each_replay_game(
        [&postings, &replayed_game_count](replay_game_record const& game) -> result<void>
        {
            ++replayed_game_count;
            auto const white_elo = narrow_elo(game.white_elo);
            auto const black_elo = narrow_elo(game.black_elo);
            if (!white_elo || !black_elo) {
                return tl::unexpected {error_code::io_failure};
            }
            auto board = motif::chess::board {};
            auto rows = std::vector<position_row> {};
            rows.reserve(game.moves.size() + 1U);
            rows.push_back(position_row {.zobrist_hash = zobrist_hash {board.hash()},
                                         .game_id = game.id,
                                         .ply = 0U,
                                         .encoded_move = 0U,
                                         .result = result_code(game.result),
                                         .white_elo = *white_elo,
                                         .black_elo = *black_elo});
            for (std::size_t index = 0; index < game.moves.size(); ++index) {
                if (index >= std::numeric_limits<std::uint16_t>::max()) {
                    return tl::unexpected {error_code::io_failure};
                }
                auto const encoded_move = game.moves[index];
                motif::chess::apply_encoded_move(board, encoded_move);
                rows.push_back(position_row {.zobrist_hash = zobrist_hash {board.hash()},
                                             .game_id = game.id,
                                             .ply = static_cast<std::uint16_t>(index + 1U),
                                             .encoded_move = encoded_move,
                                             .result = result_code(game.result),
                                             .white_elo = *white_elo,
                                             .black_elo = *black_elo});
            }
            return postings.append(rows);
        });
    if (!replay_result) {
        return tl::unexpected {replay_result.error()};
    }
    if (replayed_game_count != postings.indexed_game_count_) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const replay_with_spills =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - replay_started);
    postings.metrics_.replay_elapsed = replay_with_spills - postings.metrics_.spill_elapsed;
    auto finalized = postings.finalize();
    if (!finalized) {
        return finalized;
    }
    if (auto const log = spdlog::get("motif.db"); log != nullptr) {
        log->info("position_postings build games {} occurrences {} hashes {} spills {} replay_ms {} spill_ms {} merge_ms {} "
                  "metadata_ms {} directory_ms {} write_ms {} posting_bytes {} metadata_bytes {} directory_bytes {} "
                  "sparse_bytes {} artifact_bytes {}",
                  postings.metrics_.game_count,
                  postings.metrics_.occurrence_count,
                  postings.metrics_.distinct_hash_count,
                  postings.metrics_.spill_run_count,
                  postings.metrics_.replay_elapsed.count(),
                  postings.metrics_.spill_elapsed.count(),
                  postings.metrics_.merge_elapsed.count(),
                  postings.metrics_.metadata_write_elapsed.count(),
                  postings.metrics_.directory_write_elapsed.count(),
                  postings.metrics_.final_write_elapsed.count(),
                  postings.metrics_.posting_bytes,
                  postings.metrics_.metadata_bytes,
                  postings.metrics_.directory_bytes,
                  postings.metrics_.sparse_directory_bytes,
                  postings.metrics_.final_artifact_bytes);
    }
    if (metrics != nullptr) {
        *metrics = postings.metrics_;
    }
    return {};
}

auto position_postings::append(std::span<position_row const> rows) -> result<void>
{
    if (spill_threshold_ == 0U) {
        return tl::unexpected {error_code::invalid_argument};
    }
    records_.reserve(std::min(records_.size() + rows.size(), spill_threshold_));
    for (auto const& row : rows) {
        if (row.game_id.value == 0U) {
            return tl::unexpected {error_code::invalid_argument};
        }
        if (row.result < -1 || row.result > 1) {
            return tl::unexpected {error_code::invalid_argument};
        }
        auto& metadata = game_metadata_[row.game_id.value];
        if (metadata.is_set
            && (metadata.result != row.result || metadata.white_elo != row.white_elo || metadata.black_elo != row.black_elo))
        {
            return tl::unexpected {error_code::invalid_argument};
        }
        metadata = {.result = row.result, .white_elo = row.white_elo, .black_elo = row.black_elo, .is_set = true};
        records_.push_back(record {.hash = row.zobrist_hash, .game_key = row.game_id, .ply = row.ply});
        ++metrics_.input_occurrence_count;
        if (records_.size() >= spill_threshold_) {
            if (auto const spill = spill_current_buffer(); !spill) {
                return spill;
            }
        }
    }
    indexed_game_count_ = game_metadata_.size();
    metrics_.game_count = indexed_game_count_;
    return {};
}

auto position_postings::spill_current_buffer() -> result<void>
{
    if (records_.empty()) {
        return {};
    }
    auto const started = std::chrono::steady_clock::now();
    std::ranges::sort(records_);
    auto const spill_path = std::filesystem::path {path_.string() + ".spill" + std::to_string(spill_paths_.size())};
    auto remove_partial_spill = [&spill_path]() -> void
    {
        std::error_code ignored;
        std::filesystem::remove(spill_path, ignored);
    };
    std::ofstream output {spill_path, std::ios::binary | std::ios::trunc};
    if (!output || !write_little_endian(output, static_cast<std::uint64_t>(records_.size()))) {
        remove_partial_spill();
        return tl::unexpected {error_code::io_failure};
    }
    auto buffer = std::array<char, spill_write_buffer_size> {};
    auto buffer_size = std::size_t {0};
    for (auto const& item : records_) {
        if (buffer_size + spill_record_size > buffer.size()) {
            output.write(buffer.data(), static_cast<std::streamsize>(buffer_size));
            if (!output) {
                remove_partial_spill();
                return tl::unexpected {error_code::io_failure};
            }
            buffer_size = 0U;
        }
        auto encoded = std::array<char, spill_record_size> {};
        auto offset = std::size_t {0};
        write_little_endian(encoded, offset, item.hash.value);
        write_little_endian(encoded, offset, item.game_key.value);
        write_little_endian(encoded, offset, item.ply);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic) -- buffer_size advances by fixed encoded record sizes.
        std::ranges::copy(encoded, buffer.begin() + static_cast<std::ptrdiff_t>(buffer_size));
        buffer_size += encoded.size();
    }
    if (buffer_size > 0U) {
        output.write(buffer.data(), static_cast<std::streamsize>(buffer_size));
        if (!output) {
            remove_partial_spill();
            return tl::unexpected {error_code::io_failure};
        }
    }
    output.close();
    if (!output) {
        remove_partial_spill();
        return tl::unexpected {error_code::io_failure};
    }
    metrics_.spill_bytes += sizeof(std::uint64_t) + (records_.size() * spill_record_size);
    spill_paths_.push_back(spill_path);
    ++metrics_.spill_run_count;
    records_.clear();
    metrics_.spill_elapsed += std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    return {};
}

auto position_postings::finalize() -> result<void>
{
    if (spill_threshold_ == 0U) {
        return tl::unexpected {error_code::invalid_argument};
    }
    if (auto const spill = spill_current_buffer(); !spill) {
        return spill;
    }
    return merge_runs();
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- streaming merge validates every input and output boundary.
auto position_postings::merge_runs() -> result<void>
{
    auto const merge_started = std::chrono::steady_clock::now();

    auto const dirspool_path = std::filesystem::path {path_.string() + ".dirspool"};
    auto const temp_output_path = std::filesystem::path {path_.string() + ".tmp"};
    // Declared before every local that opens one of the owned files, so its
    // destructor -- which always removes owned spill/.dirspool/.tmp paths,
    // success or failure -- runs only after those handles are closed. Never
    // removes path_ itself: the final artifact only replaces path_ via the
    // rename below, after every write has succeeded.
    auto cleanup_guard = temp_artifact_guard {spill_paths_};
    cleanup_guard.track(dirspool_path);
    cleanup_guard.track(temp_output_path);

    struct cursor
    {
        std::ifstream input;
        std::uint64_t remaining {};
        record current {};
        std::array<char, spill_read_buffer_size> buffer {};
        std::size_t buffer_offset {};
        std::size_t buffer_size {};
    };

    auto const& game_metadata_table = game_metadata_;
    auto advance = [&game_metadata_table](cursor& item) -> bool
    {
        if (item.remaining == 0U) {
            return false;
        }
        if (item.buffer_offset == item.buffer_size) {
            auto const records_to_read = std::min(item.remaining, static_cast<std::uint64_t>(spill_read_buffer_size / spill_record_size));
            auto const bytes_to_read = static_cast<std::streamsize>(records_to_read * spill_record_size);
            item.input.read(item.buffer.data(), bytes_to_read);
            if (!item.input) {
                item.remaining = 0U;
                return false;
            }
            item.buffer_offset = 0U;
            item.buffer_size = static_cast<std::size_t>(bytes_to_read);
        }
        auto const encoded = std::span {item.buffer}.subspan(item.buffer_offset, spill_record_size);
        item.buffer_offset += spill_record_size;
        auto offset = std::size_t {0};
        read_little_endian(encoded, offset, item.current.hash.value);
        read_little_endian(encoded, offset, item.current.game_key.value);
        read_little_endian(encoded, offset, item.current.ply);
        auto const metadata = game_metadata_table.find(item.current.game_key.value);
        if (item.current.game_key.value == 0U || metadata == game_metadata_table.end() || !metadata->second.is_set) {
            item.input.setstate(std::ios::failbit);
            item.remaining = 0U;
            return false;
        }
        --item.remaining;
        return true;
    };

    std::vector<cursor> cursors;
    cursors.reserve(spill_paths_.size());
    for (auto const& spill_path : spill_paths_) {
        cursor item {.input = std::ifstream {spill_path, std::ios::binary}};
        if (!item.input || !read_little_endian(item.input, item.remaining)) {
            return tl::unexpected {error_code::io_failure};
        }
        cursors.push_back(std::move(item));
    }

    auto compare = [&cursors](std::size_t left, std::size_t right) -> bool { return cursors[right].current < cursors[left].current; };
    std::vector<std::size_t> heap;
    for (std::size_t index = 0; index < cursors.size(); ++index) {
        if (advance(cursors[index])) {
            heap.push_back(index);
        } else if (!cursors[index].input.good() && !cursors[index].input.eof()) {
            return tl::unexpected {error_code::io_failure};
        }
    }
    std::ranges::make_heap(heap, compare);

    std::ofstream output {temp_output_path, std::ios::binary | std::ios::trunc};
    if (!output) {
        return tl::unexpected {error_code::io_failure};
    }
    auto writer = buffered_file_writer {output};
    // Placeholder header, patched after every section has closed successfully.
    for (std::size_t index = 0; index < fixed_header_size; ++index) {
        if (!writer.put_byte('\0')) {
            return tl::unexpected {error_code::io_failure};
        }
    }

    std::ofstream dirspool {dirspool_path, std::ios::binary | std::ios::trunc};
    if (!dirspool) {
        return tl::unexpected {error_code::io_failure};
    }
    auto dirspool_writer = buffered_file_writer {dirspool};

    struct directory_entry
    {
        zobrist_hash hash {};
        std::uint64_t offset {};
        std::uint64_t byte_length {};
        std::uint64_t occurrence_count {};
        std::uint32_t distinct_game_count {};
        std::uint16_t min_ply {};
        std::uint16_t max_ply {};
    };

    struct sparse_entry
    {
        zobrist_hash first_hash {};
        std::uint64_t spool_offset {};
        std::uint32_t byte_length {};
        std::uint16_t entry_count {};
    };

    std::vector<directory_entry> pending_directory_entries;
    pending_directory_entries.reserve(max_directory_block_entries);
    std::vector<sparse_entry> sparse_entries;

    // NOLINTNEXTLINE(readability-function-cognitive-complexity) -- emits one self-contained block in persisted field order.
    auto flush_directory_block = [&]() -> bool
    {
        if (pending_directory_entries.empty()) {
            return true;
        }
        auto const block_start = dirspool_writer.offset();
        auto const& first = pending_directory_entries.front();
        if (!dirspool_writer.write_fixed(first.hash.value) || !dirspool_writer.write_fixed(first.offset)
            || !dirspool_writer.write_fixed(static_cast<std::uint16_t>(pending_directory_entries.size())))
        {
            return false;
        }
        auto const hash_delta_count = pending_directory_entries.size() - 1U;
        auto const hash_exception_bitmap_size = (hash_delta_count + bitmap_bits_per_byte - 1U) / bitmap_bits_per_byte;
        auto const occurrence_exception_bitmap_size = (pending_directory_entries.size() + bitmap_bits_per_byte - 1U) / bitmap_bits_per_byte;
        metrics_.directory_block_header_bytes += directory_block_header_size;
        metrics_.directory_bitmap_bytes += hash_exception_bitmap_size + occurrence_exception_bitmap_size;
        auto hash_exception_bitmap = std::vector<std::uint8_t>(hash_exception_bitmap_size, 0U);
        auto occurrence_exception_bitmap = std::vector<std::uint8_t>(occurrence_exception_bitmap_size, 0U);
        auto hash_deltas = std::vector<std::uint64_t> {};
        hash_deltas.reserve(hash_delta_count);
        for (std::size_t index = 1; index < pending_directory_entries.size(); ++index) {
            auto const hash_delta = pending_directory_entries[index].hash.value - pending_directory_entries[index - 1U].hash.value;
            hash_deltas.push_back(hash_delta);
            if ((hash_delta >> hash_delta_low_bits) != 0U) {
                hash_exception_bitmap[(index - 1U) / bitmap_bits_per_byte] |=
                    static_cast<std::uint8_t>(1U << ((index - 1U) % bitmap_bits_per_byte));
            }
        }
        for (std::size_t index = 0; index < pending_directory_entries.size(); ++index) {
            if (pending_directory_entries[index].occurrence_count != pending_directory_entries[index].distinct_game_count) {
                occurrence_exception_bitmap[index / bitmap_bits_per_byte] |=
                    static_cast<std::uint8_t>(1U << (index % bitmap_bits_per_byte));
            }
        }
        for (auto const byte : hash_exception_bitmap) {
            if (!dirspool_writer.put_byte(static_cast<char>(byte))) {
                return false;
            }
        }
        for (auto const byte : occurrence_exception_bitmap) {
            if (!dirspool_writer.put_byte(static_cast<char>(byte))) {
                return false;
            }
        }
        for (auto const hash_delta : hash_deltas) {
            auto low = hash_delta;
            for (std::size_t byte_index = 0; byte_index < hash_delta_low_bytes; ++byte_index) {
                if (!dirspool_writer.put_byte(static_cast<char>(low & byte_mask))) {
                    return false;
                }
                low >>= byte_bits;
            }
        }
        for (std::size_t index = 0; index < hash_deltas.size(); ++index) {
            if ((hash_exception_bitmap[index / bitmap_bits_per_byte] & static_cast<std::uint8_t>(1U << (index % bitmap_bits_per_byte)))
                    != 0U
                && !dirspool_writer.write_uleb128(hash_deltas[index] >> hash_delta_low_bits))
            {
                return false;
            }
        }
        for (std::size_t index = 0; index < pending_directory_entries.size(); ++index) {
            auto const& entry = pending_directory_entries[index];
            if (index > 0U) {
                auto const& previous = pending_directory_entries[index - 1U];
                auto const hash_delta = entry.hash.value - previous.hash.value;
                metrics_.directory_hash_encoding_bytes += hash_delta_low_bytes;
                if ((hash_delta >> hash_delta_low_bits) != 0U) {
                    metrics_.directory_hash_encoding_bytes += uleb128_length(hash_delta >> hash_delta_low_bits);
                }
                metrics_.directory_hash_delta_over_32_bits += hash_delta > std::numeric_limits<std::uint32_t>::max() ? 1U : 0U;
                metrics_.directory_hash_delta_over_40_bits += hash_delta >= hash_delta_40_bit_limit ? 1U : 0U;
                metrics_.directory_hash_delta_over_48_bits += hash_delta >= hash_delta_48_bit_limit ? 1U : 0U;
            }
            metrics_.directory_posting_length_bytes += uleb128_length(entry.byte_length);
            if (entry.occurrence_count != entry.distinct_game_count) {
                metrics_.directory_occurrence_count_bytes += uleb128_length(entry.occurrence_count);
            }
            metrics_.directory_distinct_game_count_bytes += uleb128_length(entry.distinct_game_count);
            metrics_.directory_equal_occurrence_game_count += entry.occurrence_count == entry.distinct_game_count ? 1U : 0U;
            metrics_.directory_min_ply_bytes += uleb128_length(entry.min_ply);
            metrics_.directory_max_ply_bytes += uleb128_length(entry.max_ply);
            if (!dirspool_writer.write_uleb128(entry.byte_length) || !dirspool_writer.write_uleb128(entry.distinct_game_count)
                || (entry.occurrence_count != entry.distinct_game_count && !dirspool_writer.write_uleb128(entry.occurrence_count))
                || !dirspool_writer.write_uleb128(entry.min_ply) || !dirspool_writer.write_uleb128(entry.max_ply))
            {
                return false;
            }
        }
        sparse_entries.push_back(sparse_entry {.first_hash = first.hash,
                                               .spool_offset = block_start,
                                               .byte_length = static_cast<std::uint32_t>(dirspool_writer.offset() - block_start),
                                               .entry_count = static_cast<std::uint16_t>(pending_directory_entries.size())});
        pending_directory_entries.clear();
        return true;
    };

    // Active hash/game-group accumulation state for the current streaming
    // posting block. Reset each time a new distinct hash starts.
    auto have_active_hash = false;
    auto active_hash = zobrist_hash {};
    auto active_hash_offset = std::uint64_t {0};
    auto active_occurrence_count = std::uint64_t {0};
    auto active_distinct_game_count = std::uint32_t {0};
    auto active_min_ply = std::uint16_t {0};
    auto active_max_ply = std::uint16_t {0};
    auto have_active_game = false;
    auto active_game_id = game_id {};
    auto have_flushed_game_in_hash = false;
    auto previous_flushed_game_id = game_id {};
    std::vector<std::uint16_t> active_plies;

    auto flush_game_group = [&]() -> bool
    {
        if (!have_active_game) {
            return true;
        }
        auto const delta = have_flushed_game_in_hash ? active_game_id.value - previous_flushed_game_id.value : active_game_id.value;
        if (!writer.write_uleb128(delta) || !writer.write_uleb128(active_plies.size()) || !writer.write_uleb128(active_plies.front())) {
            return false;
        }
        for (std::size_t index = 1; index < active_plies.size(); ++index) {
            if (!writer.write_uleb128(active_plies[index] - active_plies[index - 1U])) {
                return false;
            }
        }
        have_flushed_game_in_hash = true;
        previous_flushed_game_id = active_game_id;
        have_active_game = false;
        active_plies.clear();
        return true;
    };

    auto distinct_hash_count = std::uint64_t {0};
    auto total_occurrences = std::uint64_t {0};

    auto flush_hash = [&]() -> bool
    {
        if (!have_active_hash) {
            return true;
        }
        if (!flush_game_group()) {
            return false;
        }
        pending_directory_entries.push_back(directory_entry {.hash = active_hash,
                                                             .offset = active_hash_offset,
                                                             .byte_length = writer.offset() - active_hash_offset,
                                                             .occurrence_count = active_occurrence_count,
                                                             .distinct_game_count = active_distinct_game_count,
                                                             .min_ply = active_min_ply,
                                                             .max_ply = active_max_ply});
        ++distinct_hash_count;
        have_active_hash = false;
        have_flushed_game_in_hash = false;
        if (pending_directory_entries.size() == max_directory_block_entries) {
            return flush_directory_block();
        }
        return true;
    };

    auto previous = record {};
    auto have_previous = false;
    while (!heap.empty()) {
        std::ranges::pop_heap(heap, compare);
        auto const index = heap.back();
        heap.pop_back();
        auto const current = cursors[index].current;
        if (!have_previous || current != previous) {
            if (!have_active_hash || current.hash != active_hash) {
                if (!flush_hash()) {
                    return tl::unexpected {error_code::io_failure};
                }
                have_active_hash = true;
                active_hash = current.hash;
                active_hash_offset = writer.offset();
                active_occurrence_count = 0U;
                active_distinct_game_count = 0U;
                active_min_ply = current.ply;
                active_max_ply = current.ply;
            }
            if (!have_active_game || current.game_key != active_game_id) {
                if (!flush_game_group()) {
                    return tl::unexpected {error_code::io_failure};
                }
                have_active_game = true;
                active_game_id = current.game_key;
                ++active_distinct_game_count;
            }
            active_plies.push_back(current.ply);
            active_min_ply = std::min(active_min_ply, current.ply);
            active_max_ply = std::max(active_max_ply, current.ply);
            ++active_occurrence_count;
            ++total_occurrences;
            previous = current;
            have_previous = true;
        }
        if (advance(cursors[index])) {
            heap.push_back(index);
            std::ranges::push_heap(heap, compare);
        } else if (!cursors[index].input.good() && !cursors[index].input.eof()) {
            return tl::unexpected {error_code::io_failure};
        }
    }
    if (!flush_hash() || !flush_directory_block()) {
        return tl::unexpected {error_code::io_failure};
    }

    // Every merged occurrence and posting block is now written. Close all run
    // readers and remove consumed spill files before assembling the rest of
    // the artifact so peak temporary storage is spills+dirspool, not
    // spills+dirspool+final.
    cursors.clear();
    std::error_code remove_error;
    for (auto const& spill_path : spill_paths_) {
        std::filesystem::remove(spill_path, remove_error);
        if (remove_error) {
            return tl::unexpected {error_code::io_failure};
        }
    }
    spill_paths_.clear();

    auto const metadata_offset = writer.offset();
    auto const metadata_write_started = std::chrono::steady_clock::now();
    std::vector<std::pair<std::uint32_t, game_metadata>> sorted_metadata {game_metadata_.begin(), game_metadata_.end()};
    std::ranges::sort(sorted_metadata, {}, [](auto const& item) -> std::uint32_t { return item.first; });
    for (auto const& [game_id_key, metadata] : sorted_metadata) {
        auto flags = std::uint8_t {0};
        if (metadata.white_elo) {
            flags |= metadata_flag_white_elo;
        }
        if (metadata.black_elo) {
            flags |= metadata_flag_black_elo;
        }
        if (!writer.write_fixed(game_id_key) || !writer.write_fixed(encode_result(metadata.result)) || !writer.write_fixed(flags)
            || !writer.write_fixed(static_cast<std::uint16_t>(metadata.white_elo.value_or(0)))
            || !writer.write_fixed(static_cast<std::uint16_t>(metadata.black_elo.value_or(0))))
        {
            return tl::unexpected {error_code::io_failure};
        }
    }
    metrics_.metadata_write_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - metadata_write_started);

    auto const directory_write_started = std::chrono::steady_clock::now();
    auto const directory_offset = writer.offset();
    if (!writer.flush() || !dirspool_writer.flush()) {
        return tl::unexpected {error_code::io_failure};
    }
    dirspool.close();
    if (!dirspool) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const directory_byte_length = dirspool_writer.offset();
    if (directory_byte_length > 0U) {
        // std::ostream::operator<<(streambuf*) sets failbit if it transfers
        // zero characters, so an empty spool (no distinct hashes) must skip
        // this copy entirely rather than being treated as a write failure.
        std::ifstream const dirspool_input {dirspool_path, std::ios::binary};
        if (!dirspool_input) {
            return tl::unexpected {error_code::io_failure};
        }
        output << dirspool_input.rdbuf();
        if (!output) {
            return tl::unexpected {error_code::io_failure};
        }
    }
    writer.skip_offset(directory_byte_length);
    auto const sparse_offset = writer.offset();
    for (auto const& entry : sparse_entries) {
        auto const absolute_block_offset = directory_offset + entry.spool_offset;
        if (!writer.write_fixed(entry.first_hash.value) || !writer.write_fixed(absolute_block_offset)
            || !writer.write_fixed(entry.byte_length) || !writer.write_fixed(entry.entry_count))
        {
            return tl::unexpected {error_code::io_failure};
        }
    }
    metrics_.directory_write_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - directory_write_started);

    auto const final_write_started = std::chrono::steady_clock::now();
    if (!writer.flush()) {
        return tl::unexpected {error_code::io_failure};
    }
    output.close();
    if (!output) {
        return tl::unexpected {error_code::io_failure};
    }

    {
        std::fstream header_patch {temp_output_path, std::ios::binary | std::ios::in | std::ios::out};
        if (!header_patch) {
            return tl::unexpected {error_code::io_failure};
        }
        header_patch.seekp(0, std::ios::beg);
        header_patch.write(magic.data(), static_cast<std::streamsize>(magic.size()));
        if (!header_patch || !write_little_endian(header_patch, format_version)
            || !write_little_endian(header_patch, static_cast<std::uint32_t>(fixed_header_size))
            || !write_little_endian(header_patch, indexed_game_count_) || !write_little_endian(header_patch, total_occurrences)
            || !write_little_endian(header_patch, distinct_hash_count) || !write_little_endian(header_patch, metadata_offset)
            || !write_little_endian(header_patch, static_cast<std::uint64_t>(sorted_metadata.size()))
            || !write_little_endian(header_patch, directory_offset) || !write_little_endian(header_patch, directory_byte_length)
            || !write_little_endian(header_patch, sparse_offset)
            || !write_little_endian(header_patch, static_cast<std::uint64_t>(sparse_entries.size())))
        {
            return tl::unexpected {error_code::io_failure};
        }
        header_patch.close();
        if (!header_patch) {
            return tl::unexpected {error_code::io_failure};
        }
    }

    {
        auto staged = position_postings {temp_output_path};
        if (auto const validation = staged.open(); !validation) {
            return tl::unexpected {validation.error()};
        }
    }

    std::error_code rename_error;
    std::filesystem::rename(temp_output_path, path_, rename_error);
    if (rename_error) {
        return tl::unexpected {error_code::io_failure};
    }

    std::error_code size_error;
    auto const final_size = std::filesystem::file_size(path_, size_error);
    if (size_error) {
        return tl::unexpected {error_code::io_failure};
    }
    metrics_.final_write_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - final_write_started);
    metrics_.merge_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(directory_write_started - merge_started);
    metrics_.occurrence_count = total_occurrences;
    metrics_.distinct_hash_count = distinct_hash_count;
    metrics_.posting_bytes = metadata_offset - fixed_header_size;
    metrics_.metadata_bytes = directory_offset - metadata_offset;
    metrics_.directory_bytes = directory_byte_length;
    metrics_.sparse_directory_bytes = sparse_entries.size() * sparse_record_size;
    // All spill runs, the completed posting section, and the directory spool
    // coexist immediately before consumed spills are removed.
    metrics_.peak_temp_bytes = metrics_.spill_bytes + fixed_header_size + metrics_.posting_bytes + directory_byte_length;
    metrics_.final_artifact_bytes = final_size;
    return {};
    // cleanup_guard removes the now-consumed spill runs and .dirspool file
    // here (temp_output_path is already gone via the rename above; removing
    // an absent path is a harmless no-op).
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- open() validates every section boundary before trusting any count.
auto position_postings::open() -> result<void>
{
    is_open_ = false;
    metadata_.clear();
    sparse_directory_.clear();

    std::ifstream input {path_, std::ios::binary};
    auto file_magic = std::array<char, magic.size()> {};
    std::uint32_t version {};
    std::uint32_t header_size {};
    std::uint64_t metadata_count {};
    std::uint64_t sparse_count {};
    std::error_code size_error;
    auto const raw_file_size = std::filesystem::file_size(path_, size_error);
    if (size_error || !input) {
        return tl::unexpected {error_code::io_failure};
    }
    if (raw_file_size > std::numeric_limits<std::uint64_t>::max()) {
        return tl::unexpected {error_code::schema_mismatch};
    }
    auto const file_size = static_cast<std::uint64_t>(raw_file_size);
    if (file_size < fixed_header_size || !input.read(file_magic.data(), static_cast<std::streamsize>(file_magic.size()))
        || file_magic != magic || !read_little_endian(input, version) || !read_little_endian(input, header_size)
        || version != format_version || header_size != fixed_header_size || !read_little_endian(input, indexed_game_count_)
        || !read_little_endian(input, occurrence_count_) || !read_little_endian(input, distinct_hash_count_)
        || !read_little_endian(input, metadata_offset_) || !read_little_endian(input, metadata_count)
        || !read_little_endian(input, directory_offset_) || !read_little_endian(input, directory_byte_length_)
        || !read_little_endian(input, sparse_offset_) || !read_little_endian(input, sparse_count))
    {
        return tl::unexpected {error_code::schema_mismatch};
    }

    // Section contiguity: posting blocks in [fixed_header_size, metadata_offset_),
    // metadata in [metadata_offset_, directory_offset_), directory blocks in
    // [directory_offset_, sparse_offset_), sparse directory in [sparse_offset_, file_size).
    auto metadata_bytes = std::uint64_t {};
    auto metadata_end = std::uint64_t {};
    auto directory_end = std::uint64_t {};
    auto sparse_bytes = std::uint64_t {};
    auto sparse_end = std::uint64_t {};
    if (metadata_count != indexed_game_count_ || metadata_count > std::numeric_limits<std::size_t>::max()
        || sparse_count > std::numeric_limits<std::size_t>::max() || metadata_offset_ < fixed_header_size
        || !checked_multiply(metadata_count, metadata_record_size, metadata_bytes)
        || !checked_add(metadata_offset_, metadata_bytes, metadata_end) || metadata_end != directory_offset_
        || !checked_add(directory_offset_, directory_byte_length_, directory_end) || directory_end != sparse_offset_
        || !checked_multiply(sparse_count, sparse_record_size, sparse_bytes) || !checked_add(sparse_offset_, sparse_bytes, sparse_end)
        || sparse_end != file_size || !representable_stream_offset(metadata_offset_) || !representable_stream_offset(sparse_offset_))
    {
        return tl::unexpected {error_code::schema_mismatch};
    }

    metadata_.reserve(metadata_count);
    auto have_previous_game_id = false;
    auto previous_game_id = game_id {};
    input.seekg(static_cast<std::streamoff>(metadata_offset_));
    if (!input) {
        return tl::unexpected {error_code::io_failure};
    }
    for (std::uint64_t index = 0; index < metadata_count; ++index) {
        std::uint32_t raw_game_id {};
        std::uint8_t encoded_result {};
        std::uint8_t flags {};
        std::uint16_t white_elo_bits {};
        std::uint16_t black_elo_bits {};
        if (!read_little_endian(input, raw_game_id) || !read_little_endian(input, encoded_result) || !read_little_endian(input, flags)
            || !read_little_endian(input, white_elo_bits) || !read_little_endian(input, black_elo_bits) || raw_game_id == 0U
            || encoded_result > result_white_win || (flags & static_cast<std::uint8_t>(~metadata_flag_mask)) != 0U)
        {
            return tl::unexpected {error_code::schema_mismatch};
        }
        auto const current_id = game_id {raw_game_id};
        if (have_previous_game_id && previous_game_id >= current_id) {
            return tl::unexpected {error_code::schema_mismatch};
        }
        metadata_.push_back(metadata_record {
            .id = current_id,
            .result = decode_result(encoded_result),
            .white_elo = (flags & metadata_flag_white_elo) != 0U ? std::optional<std::int16_t> {static_cast<std::int16_t>(white_elo_bits)}
                                                                 : std::optional<std::int16_t> {},
            .black_elo = (flags & metadata_flag_black_elo) != 0U ? std::optional<std::int16_t> {static_cast<std::int16_t>(black_elo_bits)}
                                                                 : std::optional<std::int16_t> {}});
        previous_game_id = current_id;
        have_previous_game_id = true;
    }

    sparse_directory_.reserve(sparse_count);
    auto have_previous_hash = false;
    auto previous_hash = zobrist_hash {};
    auto expected_block_offset = directory_offset_;
    auto total_sparse_entries = std::uint64_t {0};
    input.seekg(static_cast<std::streamoff>(sparse_offset_));
    if (!input) {
        return tl::unexpected {error_code::io_failure};
    }
    for (std::uint64_t index = 0; index < sparse_count; ++index) {
        std::uint64_t first_hash {};
        std::uint64_t block_offset {};
        std::uint32_t block_byte_length {};
        std::uint16_t entry_count {};
        if (!read_little_endian(input, first_hash) || !read_little_endian(input, block_offset)
            || !read_little_endian(input, block_byte_length) || !read_little_endian(input, entry_count) || block_byte_length == 0U
            || block_byte_length > max_directory_block_size || entry_count == 0U
            || entry_count > static_cast<std::uint16_t>(max_directory_block_entries) || block_offset != expected_block_offset
            || !representable_stream_offset(block_offset))
        {
            return tl::unexpected {error_code::schema_mismatch};
        }
        auto const hash = zobrist_hash {first_hash};
        if (have_previous_hash && previous_hash >= hash) {
            return tl::unexpected {error_code::schema_mismatch};
        }
        sparse_directory_.push_back(sparse_entry {
            .first_hash = hash, .block_offset = block_offset, .block_byte_length = block_byte_length, .entry_count = entry_count});
        if (!checked_add(expected_block_offset, block_byte_length, expected_block_offset)
            || !checked_add(total_sparse_entries, entry_count, total_sparse_entries))
        {
            return tl::unexpected {error_code::schema_mismatch};
        }
        previous_hash = hash;
        have_previous_hash = true;
    }
    if (expected_block_offset != sparse_offset_ || total_sparse_entries != distinct_hash_count_) {
        return tl::unexpected {error_code::schema_mismatch};
    }

    auto expected_posting_offset = fixed_header_size;
    auto have_previous_directory_hash = false;
    auto previous_directory_hash = zobrist_hash {};
    auto total_occurrences = std::uint64_t {0};
    for (auto const& sparse : sparse_directory_) {
        std::vector<char> block(sparse.block_byte_length);
        input.clear();
        input.seekg(static_cast<std::streamoff>(sparse.block_offset));
        if (!input || !input.read(block.data(), static_cast<std::streamsize>(block.size()))) {
            return tl::unexpected {error_code::io_failure};
        }
        auto entries = decode_directory_block(block);
        if (!entries) {
            return tl::unexpected {entries.error()};
        }
        if (entries->size() != sparse.entry_count || entries->front().hash != sparse.first_hash) {
            return tl::unexpected {error_code::schema_mismatch};
        }
        for (auto const& entry : *entries) {
            if ((have_previous_directory_hash && previous_directory_hash >= entry.hash) || entry.posting_offset != expected_posting_offset)
            {
                return tl::unexpected {error_code::schema_mismatch};
            }
            if (!representable_stream_offset(entry.posting_offset)
                || !checked_add(expected_posting_offset, entry.posting_byte_length, expected_posting_offset)
                || !checked_add(total_occurrences, entry.occurrence_count, total_occurrences))
            {
                return tl::unexpected {error_code::schema_mismatch};
            }
            previous_directory_hash = entry.hash;
            have_previous_directory_hash = true;
        }
    }
    if (expected_posting_offset != metadata_offset_ || total_occurrences != occurrence_count_) {
        return tl::unexpected {error_code::schema_mismatch};
    }

    is_open_ = true;
    return {};
}

auto position_postings::indexed_game_count() const noexcept -> std::uint64_t
{
    return indexed_game_count_;
}

namespace
{

// Finds the sparse block whose range could contain hash: the last block with
// first_hash <= hash.
auto find_sparse_block(std::span<position_postings::sparse_entry const> const sparse_directory, zobrist_hash const hash)
    -> std::optional<position_postings::sparse_entry>
{
    auto const iterator =
        std::ranges::upper_bound(sparse_directory, hash, {}, [](auto const& entry) -> zobrist_hash { return entry.first_hash; });
    if (iterator == sparse_directory.begin()) {
        return std::nullopt;
    }
    return *std::ranges::prev(iterator);
}

}  // namespace

auto position_postings::for_each_summary(std::function<result<void>(zobrist_hash, position_postings_summary const&)> const& visitor) const
    -> result<void>
{
    if (!is_open_) {
        return tl::unexpected {error_code::invalid_argument};
    }
    std::ifstream input {path_, std::ios::binary};
    if (!input) {
        return tl::unexpected {error_code::io_failure};
    }
    for (auto const& sparse : sparse_directory_) {
        std::vector<char> block(sparse.block_byte_length);
        input.clear();
        input.seekg(static_cast<std::streamoff>(sparse.block_offset));
        if (!input || !input.read(block.data(), static_cast<std::streamsize>(block.size()))) {
            return tl::unexpected {error_code::io_failure};
        }
        auto entries = decode_directory_block(block);
        if (!entries) {
            return tl::unexpected {entries.error()};
        }
        for (auto const& entry : *entries) {
            auto const visited = visitor(entry.hash,
                                         position_postings_summary {.occurrence_count = entry.occurrence_count,
                                                                    .distinct_game_count = entry.distinct_game_count,
                                                                    .min_ply = entry.min_ply,
                                                                    .max_ply = entry.max_ply});
            if (!visited) {
                return tl::unexpected {visited.error()};
            }
        }
    }
    return {};
}

namespace
{

// Reads and decodes the single compressed directory block that could contain
// hash, returning the matching entry (if any).
auto find_directory_block_entry(std::filesystem::path const& path,
                                std::span<position_postings::sparse_entry const> const sparse_directory,
                                zobrist_hash const hash) -> result<std::optional<directory_block_entry>>
{
    auto const block = find_sparse_block(sparse_directory, hash);
    if (!block) {
        return std::optional<directory_block_entry> {};
    }
    std::ifstream input {path, std::ios::binary};
    if (!input) {
        return tl::unexpected {error_code::io_failure};
    }
    input.seekg(static_cast<std::streamoff>(block->block_offset));
    std::vector<char> bytes(block->block_byte_length);
    if (!input || !input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        return tl::unexpected {error_code::io_failure};
    }
    auto entries = decode_directory_block(bytes);
    if (!entries) {
        return tl::unexpected {entries.error()};
    }
    auto const iterator = std::ranges::lower_bound(*entries, hash, {}, [](auto const& entry) -> zobrist_hash { return entry.hash; });
    if (iterator == entries->end() || iterator->hash != hash) {
        return std::optional<directory_block_entry> {};
    }
    return std::optional<directory_block_entry> {*iterator};
}

}  // namespace

auto position_postings::summary(zobrist_hash const hash) const -> result<std::optional<position_postings_summary>>
{
    if (!is_open_) {
        return tl::unexpected {error_code::invalid_argument};
    }
    auto entry_result = find_directory_block_entry(path_, sparse_directory_, hash);
    if (!entry_result) {
        return tl::unexpected {entry_result.error()};
    }
    auto entry_option = *entry_result;
    if (!entry_option.has_value()) {
        return std::optional<position_postings_summary> {};
    }
    auto const entry = *entry_option;
    return position_postings_summary {.occurrence_count = entry.occurrence_count,
                                      .distinct_game_count = entry.distinct_game_count,
                                      .min_ply = entry.min_ply,
                                      .max_ply = entry.max_ply};
}

auto position_postings::distinct_game_ids(zobrist_hash const hash) const -> result<std::vector<game_id>>
{
    if (!is_open_) {
        return tl::unexpected {error_code::invalid_argument};
    }
    auto entry_result = find_directory_block_entry(path_, sparse_directory_, hash);
    if (!entry_result) {
        return tl::unexpected {entry_result.error()};
    }
    auto entry_option = *entry_result;
    if (!entry_option.has_value()) {
        return std::vector<game_id> {};
    }
    auto const entry = *entry_option;
    std::ifstream input {path_, std::ios::binary};
    input.seekg(static_cast<std::streamoff>(entry.posting_offset));
    std::vector<char> bytes(entry.posting_byte_length);
    if (!input || !input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        return tl::unexpected {error_code::io_failure};
    }
    auto game_ids = decode_posting_block_game_ids(bytes, entry);
    if (!game_ids) {
        return tl::unexpected {game_ids.error()};
    }
    for (auto const game_key : *game_ids) {
        auto const metadata_iterator =
            std::ranges::lower_bound(metadata_, game_key, {}, [](auto const& record) -> game_id { return record.id; });
        if (metadata_iterator == metadata_.end() || metadata_iterator->id != game_key) {
            return tl::unexpected {error_code::schema_mismatch};
        }
    }
    return *game_ids;
}

auto position_postings::occurrences(zobrist_hash const hash, std::size_t const limit, std::size_t const offset) const
    -> result<std::vector<position_match>>
{
    if (!is_open_) {
        return tl::unexpected {error_code::invalid_argument};
    }
    auto entry_result = find_directory_block_entry(path_, sparse_directory_, hash);
    if (!entry_result) {
        return tl::unexpected {entry_result.error()};
    }
    auto entry_option = *entry_result;
    if (!entry_option.has_value()) {
        return std::vector<position_match> {};
    }
    auto const entry = *entry_option;
    std::ifstream input {path_, std::ios::binary};
    input.seekg(static_cast<std::streamoff>(entry.posting_offset));
    std::vector<char> bytes(entry.posting_byte_length);
    if (!input || !input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        return tl::unexpected {error_code::io_failure};
    }
    auto decoded = decode_posting_block_window(bytes, entry, limit, offset);
    if (!decoded) {
        return tl::unexpected {decoded.error()};
    }

    std::vector<position_match> matches;
    matches.reserve(decoded->size());
    for (auto const& occurrence : *decoded) {
        auto const metadata_iterator =
            std::ranges::lower_bound(metadata_, occurrence.id, {}, [](auto const& record) -> game_id { return record.id; });
        if (metadata_iterator == metadata_.end() || metadata_iterator->id != occurrence.id) {
            return tl::unexpected {error_code::schema_mismatch};
        }
        matches.push_back(position_match {.game_id = occurrence.id,
                                          .ply = occurrence.ply,
                                          .result = metadata_iterator->result,
                                          .white_elo = metadata_iterator->white_elo,
                                          .black_elo = metadata_iterator->black_elo});
    }
    return matches;
}

}  // namespace motif::db
