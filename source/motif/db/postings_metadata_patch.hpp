#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "motif/db/error.hpp"
#include "motif/db/manifest.hpp"
#include "motif/db/position_postings.hpp"

namespace motif::db
{

// ── Postings metadata artifact, format v1 ────────────────────────────────────
//
// `positions.metadata.<random128hex>`: complete per-game metadata AFTER-IMAGES
// (not numerical differences) for the half-open sequence interval (from, to].
// Little-endian throughout.
//
//   header: "MOTIFPM1"(8) | u32 format_version(=1) | u32 header_length |
//           u32 index_semantics | 16B bundle identity | u64 from_exclusive |
//           u64 to_inclusive | u32 kind (1 = run, 2 = checkpoint) | u64 record_count
//   record (18 bytes, strictly ascending by (game_id, content_revision)):
//           u32 game_id | u64 content_revision | u8 result (0 black win,
//           1 draw/other, 2 white win -- the postings encoding) | u8 flags
//           (bit0 white rating present, bit1 black present) | i16 white_elo bits |
//           i16 black_elo bits (zero when the flag is clear)
//   EOF exactly after the last record; the descriptor's FNV-1a-64 covers it all.
//
// A RUN holds the latest after-image of each document an epoch touched. A
// CHECKPOINT is the accumulated sparse override set relative to the segment's
// embedded baseline (last write wins per document); it is NOT a second full
// metadata table. PR 6 requires content revision 1.
inline constexpr std::uint64_t metadata_record_bytes = 18;
inline constexpr std::uint64_t metadata_artifact_max_encoded_bytes = std::uint64_t {64} << 20U;

enum class metadata_artifact_kind : std::uint8_t
{
    run = 1,
    checkpoint = 2,
};

struct metadata_artifact_identity
{
    identity_bytes bundle_id {};
    std::uint32_t index_semantics {index_semantics_version};

    auto operator==(metadata_artifact_identity const&) const -> bool = default;
};

struct metadata_artifact
{
    metadata_artifact_identity identity;
    metadata_artifact_kind kind {metadata_artifact_kind::run};
    std::uint64_t from_exclusive_seq {};
    std::uint64_t to_inclusive_seq {};
    std::vector<position_metadata_after_image> records;  // strictly ascending by (id, revision)
    std::uint64_t encoded_bytes {};

    // Actual resident bytes of the decoded representation.
    [[nodiscard]] auto decoded_bytes() const noexcept -> std::uint64_t;

    auto operator==(metadata_artifact const&) const -> bool = default;
};

[[nodiscard]] auto validate_metadata_artifact(metadata_artifact const& artifact) -> result<void>;
[[nodiscard]] auto encode_metadata_artifact(metadata_artifact const& artifact) -> result<std::string>;

struct metadata_artifact_expectation
{
    metadata_artifact_identity identity;
    metadata_artifact_kind kind {metadata_artifact_kind::run};
    std::uint64_t from_exclusive_seq {};
    std::uint64_t to_inclusive_seq {};
};

// Strict decode: the record count is checked against the bytes present before
// any allocation; bad flags/results/ids/revisions, unordered or duplicate
// keys, identity/kind/interval disagreement, truncation and trailing bytes are
// all io_failure.
[[nodiscard]] auto decode_metadata_artifact(std::string_view bytes, metadata_artifact_expectation const& expected)
    -> result<metadata_artifact>;
[[nodiscard]] auto write_metadata_artifact(std::filesystem::path const& path, metadata_artifact const& artifact) -> result<void>;
[[nodiscard]] auto load_metadata_artifact(std::filesystem::path const& path,
                                          artifact_descriptor const& descriptor,
                                          metadata_artifact_expectation const& expected) -> result<metadata_artifact>;

// Last write wins per (id, revision) across artifacts given in chronological
// order (checkpoint first); the result is sorted and unique. Streaming sorted
// merge: no hash map, scratch bounded by the output.
[[nodiscard]] auto merge_metadata_records(std::span<metadata_artifact const* const> chronological)
    -> result<std::vector<position_metadata_after_image>>;

// Compaction: folds the chain (existing checkpoint, runs, and any pending run)
// into one new checkpoint covering (baseline_seq, last.to]. Repeated edits to
// one game leave one record; an override that returns to its baseline value
// may remain (no baseline map is kept to optimize it away).
[[nodiscard]] auto fold_metadata_chain(metadata_artifact_identity const& identity,
                                       std::uint64_t baseline_seq,
                                       std::span<metadata_artifact const* const> chronological) -> result<metadata_artifact>;

}  // namespace motif::db
