#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "motif/db/postings_metadata_patch.hpp"

#include <tl/expected.hpp>

#include "motif/db/error.hpp"
#include "motif/db/manifest.hpp"
#include "motif/db/opening_tree_stats.hpp"
#include "motif/db/position_postings.hpp"

namespace motif::db
{

namespace
{

constexpr auto metadata_magic = std::string_view {"MOTIFPM1"};
constexpr auto flag_white = std::uint8_t {0x01};
constexpr auto flag_black = std::uint8_t {0x02};
// Bits of the record flags byte that no format version defines; they must be zero.
constexpr auto unknown_flags_mask = std::uint8_t {0xFC};
// magic (8) + format version (4) + header length (4); the header length counts these too.
constexpr auto fixed_prefix_bytes = std::size_t {16};
constexpr auto int64_max_u = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

auto corrupt(char const* what) -> tl::unexpected<error>
{
    return tl::unexpected {error {error_code::io_failure, what}};
}

auto key_of(position_metadata_after_image const& image) -> std::tuple<std::uint32_t, std::uint64_t>
{
    return {image.id.value, image.content_revision};
}

}  // namespace

auto metadata_artifact::decoded_bytes() const noexcept -> std::uint64_t
{
    return sizeof(metadata_artifact) + (records.capacity() * sizeof(position_metadata_after_image));
}

auto validate_metadata_artifact(metadata_artifact const& artifact) -> result<void>
{
    if (artifact.kind != metadata_artifact_kind::run && artifact.kind != metadata_artifact_kind::checkpoint) {
        return corrupt("unknown metadata artifact kind");
    }
    if (artifact.from_exclusive_seq >= artifact.to_inclusive_seq || artifact.to_inclusive_seq > int64_max_u) {
        return corrupt("invalid metadata sequence interval");
    }
    auto previous = std::optional<std::tuple<std::uint32_t, std::uint64_t>> {};
    for (auto const& record : artifact.records) {
        // PR 6: one content revision per document, result in the 3-valued code.
        if (record.id.value == 0U || record.content_revision != 1U || record.result < -1 || record.result > 1) {
            return corrupt("invalid metadata record");
        }
        if (previous && !(*previous < key_of(record))) {
            return corrupt("metadata records are not strictly ascending");
        }
        previous = key_of(record);
    }
    return {};
}

auto encode_metadata_artifact(metadata_artifact const& artifact) -> result<std::string>
{
    if (auto valid = validate_metadata_artifact(artifact); !valid) {
        return tl::unexpected {valid.error()};
    }
    auto tail = std::string {};
    append_fixed_le(tail, artifact.identity.index_semantics);
    for (auto const byte : artifact.identity.bundle_id) {
        tail.push_back(static_cast<char>(byte));
    }
    append_fixed_le(tail, artifact.from_exclusive_seq);
    append_fixed_le(tail, artifact.to_inclusive_seq);
    append_fixed_le(tail, static_cast<std::uint32_t>(artifact.kind));
    append_fixed_le(tail, static_cast<std::uint64_t>(artifact.records.size()));

    auto out = std::string {};
    out.reserve(fixed_prefix_bytes + tail.size() + (artifact.records.size() * metadata_record_bytes));
    out.append(metadata_magic);
    append_fixed_le(out, postings_metadata_format_version);
    append_fixed_le(out, static_cast<std::uint32_t>(fixed_prefix_bytes + tail.size()));
    out.append(tail);
    for (auto const& record : artifact.records) {
        auto flags = std::uint8_t {0};
        if (record.white_elo) {
            flags |= flag_white;
        }
        if (record.black_elo) {
            flags |= flag_black;
        }
        append_fixed_le(out, record.id.value);
        append_fixed_le(out, record.content_revision);
        append_fixed_le(out, static_cast<std::uint8_t>(record.result + 1));
        append_fixed_le(out, flags);
        append_fixed_le(out, static_cast<std::uint16_t>(record.white_elo.value_or(0)));
        append_fixed_le(out, static_cast<std::uint16_t>(record.black_elo.value_or(0)));
    }
    if (out.size() > metadata_artifact_max_encoded_bytes) {
        return corrupt("metadata artifact exceeds the encoded size limit");
    }
    return out;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one strict, sequential parse of an untrusted file.
auto decode_metadata_artifact(std::string_view const bytes, metadata_artifact_expectation const& expected) -> result<metadata_artifact>
{
    if (bytes.size() > metadata_artifact_max_encoded_bytes) {
        return corrupt("metadata artifact exceeds the encoded size limit");
    }
    auto reader = byte_reader {bytes};
    auto const magic_bytes = reader.read_bytes(metadata_magic.size());
    if (!magic_bytes || *magic_bytes != metadata_magic) {
        return corrupt("bad metadata magic");
    }
    auto artifact = metadata_artifact {};
    auto const version = reader.read_u32();
    auto const header_length = reader.read_u32();
    auto const semantics = reader.read_u32();
    auto const bundle = reader.read_bytes(artifact.identity.bundle_id.size());
    auto const from = reader.read_u64();
    auto const to_seq = reader.read_u64();
    auto const kind = reader.read_u32();
    auto const record_count = reader.read_u64();
    if (!version || !header_length || !semantics || !bundle || !from || !to_seq || !kind || !record_count) {
        return corrupt("truncated metadata header");
    }
    if (*version != postings_metadata_format_version || *header_length != reader.position()) {
        return corrupt("unsupported metadata header");
    }
    std::ranges::transform(
        *bundle, artifact.identity.bundle_id.begin(), [](char const byte) -> std::uint8_t { return static_cast<std::uint8_t>(byte); });
    artifact.identity.index_semantics = *semantics;
    if (artifact.identity != expected.identity) {
        return corrupt("metadata header identity disagrees with its bundle");
    }
    if (*kind != static_cast<std::uint32_t>(expected.kind)) {
        return corrupt("metadata artifact kind disagrees with its descriptor slot");
    }
    artifact.kind = expected.kind;
    artifact.from_exclusive_seq = *from;
    artifact.to_inclusive_seq = *to_seq;
    if (artifact.from_exclusive_seq != expected.from_exclusive_seq || artifact.to_inclusive_seq != expected.to_inclusive_seq) {
        return corrupt("metadata header interval disagrees with its descriptor");
    }
    // The records are the whole remainder, exactly: check before allocating.
    if (*record_count > reader.remaining() / metadata_record_bytes || reader.remaining() != *record_count * metadata_record_bytes) {
        return corrupt("metadata record count disagrees with the file size");
    }
    artifact.records.reserve(static_cast<std::size_t>(*record_count));
    for (std::uint64_t index = 0; index < *record_count; ++index) {
        auto const document = reader.read_u32();
        auto const revision = reader.read_u64();
        auto const result_byte = reader.read_u8();
        auto const flags = reader.read_u8();
        auto const white_bits = reader.read_u16();
        auto const black_bits = reader.read_u16();
        if (!document || !revision || !result_byte || !flags || !white_bits || !black_bits) {
            return corrupt("truncated metadata record");
        }
        // Canonical form: an absent rating carries zero payload bits.
        if (*result_byte > 2U || (*flags & unknown_flags_mask) != 0U || ((*flags & flag_white) == 0U && *white_bits != 0U)
            || ((*flags & flag_black) == 0U && *black_bits != 0U))
        {
            return corrupt("invalid metadata record fields");
        }
        artifact.records.push_back(position_metadata_after_image {
            .id = game_id {*document},
            .content_revision = *revision,
            .result = static_cast<std::int8_t>(static_cast<int>(*result_byte) - 1),
            .white_elo = (*flags & flag_white) != 0U ? std::optional<std::int16_t> {static_cast<std::int16_t>(*white_bits)} : std::nullopt,
            .black_elo =
                (*flags & flag_black) != 0U ? std::optional<std::int16_t> {static_cast<std::int16_t>(*black_bits)} : std::nullopt});
    }
    artifact.encoded_bytes = bytes.size();
    if (auto valid = validate_metadata_artifact(artifact); !valid) {
        return tl::unexpected {valid.error()};
    }
    return artifact;
}

auto write_metadata_artifact(std::filesystem::path const& path, metadata_artifact const& artifact) -> result<void>
{
    auto bytes = encode_metadata_artifact(artifact);
    if (!bytes) {
        return tl::unexpected {bytes.error()};
    }
    std::ofstream out {path, std::ios::binary | std::ios::trunc};
    out.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
    out.close();
    if (!out) {
        return corrupt("metadata artifact write failed");
    }
    return {};
}

auto load_metadata_artifact(std::filesystem::path const& path,
                            artifact_descriptor const& descriptor,
                            metadata_artifact_expectation const& expected) -> result<metadata_artifact>
{
    std::error_code size_error;
    auto const size = std::filesystem::file_size(path, size_error);
    if (size_error || size != descriptor.size_bytes || size > metadata_artifact_max_encoded_bytes) {
        return corrupt("metadata artifact size disagrees with its descriptor");
    }
    std::ifstream input {path, std::ios::binary};
    if (!input) {
        return corrupt("cannot open metadata artifact");
    }
    auto bytes = std::string {std::istreambuf_iterator<char> {input}, std::istreambuf_iterator<char> {}};
    if (bytes.size() != size || fnv1a64(bytes) != descriptor.checksum) {
        return corrupt("metadata artifact checksum disagrees with its descriptor");
    }
    return decode_metadata_artifact(bytes, expected);
}

namespace
{

// Sorted two-way merge; on an equal key the newer record wins.
auto merge_two(std::vector<position_metadata_after_image> const& older, std::vector<position_metadata_after_image> const& newer)
    -> std::vector<position_metadata_after_image>
{
    auto merged = std::vector<position_metadata_after_image> {};
    merged.reserve(older.size() + newer.size());
    auto left = std::size_t {0};
    auto right = std::size_t {0};
    while (left < older.size() && right < newer.size()) {
        auto const left_key = key_of(older[left]);
        auto const right_key = key_of(newer[right]);
        if (left_key < right_key) {
            merged.push_back(older[left++]);
        } else if (right_key < left_key) {
            merged.push_back(newer[right++]);
        } else {
            merged.push_back(newer[right++]);
            ++left;
        }
    }
    merged.insert(merged.end(), older.begin() + static_cast<std::ptrdiff_t>(left), older.end());
    merged.insert(merged.end(), newer.begin() + static_cast<std::ptrdiff_t>(right), newer.end());
    return merged;
}

}  // namespace

auto merge_metadata_records(std::span<metadata_artifact const* const> const chronological)
    -> result<std::vector<position_metadata_after_image>>
{
    auto merged = std::vector<position_metadata_after_image> {};
    for (auto const* artifact : chronological) {
        if (artifact == nullptr) {
            return corrupt("null metadata artifact");
        }
        if (auto valid = validate_metadata_artifact(*artifact); !valid) {
            return tl::unexpected {valid.error()};
        }
        merged = merge_two(merged, artifact->records);
    }
    return merged;
}

auto fold_metadata_chain(metadata_artifact_identity const& identity,
                         std::uint64_t const baseline_seq,
                         std::span<metadata_artifact const* const> const chronological) -> result<metadata_artifact>
{
    if (chronological.empty() || chronological.back() == nullptr) {
        return tl::unexpected {error {error_code::invalid_argument, "nothing to fold"}};
    }
    auto merged = merge_metadata_records(chronological);
    if (!merged) {
        return tl::unexpected {merged.error()};
    }
    auto folded = metadata_artifact {.identity = identity,
                                     .kind = metadata_artifact_kind::checkpoint,
                                     .from_exclusive_seq = baseline_seq,
                                     .to_inclusive_seq = chronological.back()->to_inclusive_seq,
                                     .records = std::move(*merged),
                                     .encoded_bytes = 0};
    auto encoded = encode_metadata_artifact(folded);
    if (!encoded) {
        return tl::unexpected {encoded.error()};
    }
    folded.encoded_bytes = encoded->size();
    return folded;
}

}  // namespace motif::db
