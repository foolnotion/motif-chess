#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "motif/db/error.hpp"

namespace motif::db
{

// ── Published snapshot model ─────────────────────────────────────────────────
//
// The published_snapshot row in SQLite is authoritative for which derived
// artifacts exist and which canonical mutation sequence they cover.
// manifest.json is only a rewritable cache of that row (see database_manager).
// These types are the typed, validated form of the row's versioned JSON payload.

// Payload version 1: one postings segment at the covered sequence, an optional
// v5 tree checkpoint at that same sequence, no chains. Payload version 2 adds a
// postings metadata chain, tree patch runs, a child-frequency mode and v6 tree
// checkpoints. Writers emit version 2 only when a payload needs it (see
// required_payload_version); readers accept both, and a build that only knows
// version 1 rejects version 2 as schema_mismatch instead of ignoring patches.
inline constexpr std::uint32_t snapshot_payload_version_1 = 1;
inline constexpr std::uint32_t snapshot_payload_version_2 = 2;
inline constexpr std::string_view postings_artifact_prefix = "positions.postings.";
inline constexpr std::string_view tree_artifact_prefix = "opening_tree.idx.";
inline constexpr std::string_view postings_metadata_artifact_prefix = "positions.metadata.";
inline constexpr std::string_view tree_patch_artifact_prefix = "opening_tree.patch.";
inline constexpr std::string_view postings_format_name = "motif-postings";
inline constexpr std::string_view tree_format_name = "motif-opening-tree";
inline constexpr std::string_view postings_metadata_format_name = "motif-postings-metadata";
inline constexpr std::string_view tree_patch_format_name = "motif-opening-tree-patch";
inline constexpr std::string_view tree_completeness_policy_name = "standard_start_all_occurrences_else_horizon_v1";
inline constexpr std::uint32_t postings_metadata_format_version = 1;
inline constexpr std::uint32_t tree_patch_format_version = 1;
// Version of the indexing rules (horizon/dedup/result mapping) that every
// artifact repeats in its header.
inline constexpr std::uint32_t index_semantics_version = 1;
// Decoder bound on a chain's length (the soft/hard admission caps are lower).
inline constexpr std::size_t max_patch_chain_runs = 256;

// FNV-1a 64-bit, the artifact and payload checksum. Not authentication: it
// detects accidental corruption only.
inline constexpr std::uint64_t fnv1a64_offset_basis = 14695981039346656037ULL;
inline constexpr std::uint64_t fnv1a64_prime = 1099511628211ULL;

auto fnv1a64(std::string_view bytes) noexcept -> std::uint64_t;
// 16 lower-case hex digits.
auto checksum_to_hex(std::uint64_t checksum) -> std::string;
auto checksum_from_hex(std::string_view hex) -> std::optional<std::uint64_t>;
// 128 random bits as 32 lower-case hex digits: bundle and artifact identities.
auto generate_random_id() -> std::string;

// A 128-bit identity (bundle or artifact) as raw bytes, for binary artifact
// headers; nullopt unless hex is exactly 32 lower-case hex digits.
inline constexpr std::size_t identity_byte_count = 16;
using identity_bytes = std::array<std::uint8_t, identity_byte_count>;
auto identity_from_hex(std::string_view hex) -> std::optional<identity_bytes>;

struct artifact_descriptor
{
    std::string id;  // 32 lower-case hex digits, unique, stable across mirrors
    std::string filename;  // direct bundle child: <prefix><id>
    std::string format;
    std::uint32_t format_version {};
    std::uint64_t size_bytes {};
    std::uint64_t checksum {};  // FNV-1a-64 of the whole file

    auto operator==(artifact_descriptor const&) const -> bool = default;
};

struct postings_segment
{
    artifact_descriptor artifact;
    // The sequence the embedded per-game metadata is exact at. With a metadata
    // chain this is provenance, not current coverage.
    std::uint64_t source_mutation_seq {};
    std::uint64_t document_count {};
    std::uint64_t min_content_revision {1};
    std::uint64_t max_content_revision {1};

    auto operator==(postings_segment const&) const -> bool = default;
};

// An immutable chain member covering the half-open sequence interval
// (from_exclusive_seq, to_inclusive_seq].
struct patch_run_descriptor
{
    artifact_descriptor artifact;
    std::uint64_t from_exclusive_seq {};
    std::uint64_t to_inclusive_seq {};

    auto operator==(patch_run_descriptor const&) const -> bool = default;
};

// Sparse per-game metadata after-images layered over the segment's embedded
// metadata: an optional accumulated checkpoint starting at the segment's source
// sequence, then runs, together reaching the snapshot's coverage.
struct postings_metadata_chain
{
    std::optional<patch_run_descriptor> checkpoint;
    std::vector<patch_run_descriptor> runs;

    [[nodiscard]] auto empty() const noexcept -> bool { return !checkpoint && runs.empty(); }

    auto operator==(postings_metadata_chain const&) const -> bool = default;
};

// Where child-global transposition counts come from. "checkpoint": the cached
// value in the checkpoint is exact. "postings": membership has changed since
// the checkpoint, so every count is resolved from the current postings.
enum class tree_child_frequency_mode : std::uint8_t
{
    checkpoint,
    postings,
};

[[nodiscard]] constexpr auto to_string(tree_child_frequency_mode const mode) noexcept -> std::string_view
{
    return mode == tree_child_frequency_mode::postings ? "postings" : "checkpoint";
}

[[nodiscard]] constexpr auto parse_child_frequency_mode(std::string_view const text) noexcept -> std::optional<tree_child_frequency_mode>
{
    if (text == "checkpoint") {
        return tree_child_frequency_mode::checkpoint;
    }
    if (text == "postings") {
        return tree_child_frequency_mode::postings;
    }
    return std::nullopt;
}

struct tree_descriptor
{
    std::uint64_t covered_mutation_seq {};
    std::uint16_t max_root_ply {};
    std::string completeness_policy;
    artifact_descriptor checkpoint;
    // The sequence the checkpoint itself is exact at; runs chain from here to
    // covered_mutation_seq.
    std::uint64_t base_mutation_seq {};
    tree_child_frequency_mode child_frequency_mode {tree_child_frequency_mode::checkpoint};
    std::vector<patch_run_descriptor> patch_runs;

    auto operator==(tree_descriptor const&) const -> bool = default;
};

struct bundle_identity
{
    std::string id;  // generated once at creation and preserved
    std::string name;
    std::string created_at;  // ISO 8601, e.g. "2026-04-18T14:30:00Z"
};

// The normalized payload both wire versions decode to.
struct snapshot_payload
{
    bundle_identity bundle;
    std::vector<postings_segment> segments;
    postings_metadata_chain metadata;
    std::optional<tree_descriptor> tree;
};

// A validated published_snapshot row together with its decoded payload. It is
// also exactly what manifest.json mirrors.
struct db_manifest
{
    std::uint32_t schema_version {};
    std::uint64_t publication_generation {};
    // Null for an unbuilt snapshot (no artifacts). Otherwise the mutation
    // sequence the artifact set covers; the artifacts are usable only while it
    // equals the current mutation_state.mutation_seq.
    std::optional<std::uint64_t> covered_mutation_seq;
    // Canonical games at the covered baseline (not refreshed as games change).
    std::uint64_t canonical_game_count {};
    std::uint32_t payload_version {};
    std::string payload_json;  // the exact serialized bytes the checksum covers
    std::string payload_checksum;  // FNV-1a-64 of payload_json, 16 lower-case hex digits
    snapshot_payload payload;

    [[nodiscard]] auto name() const noexcept -> std::string const& { return payload.bundle.name; }

    [[nodiscard]] auto created_at() const noexcept -> std::string const& { return payload.bundle.created_at; }

    [[nodiscard]] auto postings() const noexcept -> postings_segment const*
    {
        return payload.segments.empty() ? nullptr : payload.segments.data();
    }

    [[nodiscard]] auto tree() const noexcept -> tree_descriptor const* { return payload.tree ? &*payload.tree : nullptr; }
};

// The lowest payload version able to express payload: 2 when it carries a
// metadata chain, tree patch runs, a non-default child-frequency mode or a
// checkpoint that is not v5, otherwise 1 (so bundles that need nothing new keep
// their PR 5 bytes).
auto required_payload_version(snapshot_payload const& payload) noexcept -> std::uint32_t;

// Every artifact descriptor the payload references, in a fixed order.
auto referenced_artifacts(snapshot_payload const& payload) -> std::vector<artifact_descriptor>;

// Every artifact filename the payload references (segment, metadata chain, tree
// checkpoint and runs). The garbage collector's authoritative reference set.
auto referenced_artifact_filenames(snapshot_payload const& payload) -> std::vector<std::string>;

// Serialize a payload to its canonical bytes (typed Glaze, decimal counters as
// strings so no consumer rounds at 2^53) in required_payload_version's form.
auto encode_payload(snapshot_payload const& payload) -> result<std::string>;

// Strict decode of version 1 or 2: unknown fields, unknown payload semantics,
// more than one segment, nonconforming ids/filenames/counters are all rejected;
// version 1 additionally rejects any chain. An unsupported payload_version is
// schema_mismatch; malformed content is io_failure.
auto decode_payload(std::uint32_t payload_version, std::string_view payload_json) -> result<snapshot_payload>;

// Builds a validated manifest around a new payload: encodes it, checksums the
// exact bytes, and cross-validates against covered/count.
auto build_manifest(std::uint64_t publication_generation,
                    std::optional<std::uint64_t> covered_mutation_seq,
                    std::uint64_t canonical_game_count,
                    snapshot_payload payload) -> result<db_manifest>;

// Validates raw row fields (checksum of the exact payload bytes, payload
// version, strict payload structure, cross-field invariants) and returns the
// typed manifest. A checksum mismatch or malformed payload is io_failure; an
// unsupported payload_version is schema_mismatch.
auto parse_manifest(std::uint64_t publication_generation,
                    std::optional<std::uint64_t> covered_mutation_seq,
                    std::uint64_t canonical_game_count,
                    std::uint32_t payload_version,
                    std::string payload_json,
                    std::string payload_checksum) -> result<db_manifest>;

// ── manifest.json cache ──────────────────────────────────────────────────────

enum class manifest_write_state : std::uint8_t
{
    not_published,
    published,
    published_not_durable,
};

// Outcome of the atomic cache-file writer only. It says nothing about whether
// a derived-index publication committed: that is decided by the SQLite COMMIT.
struct manifest_write_result
{
    manifest_write_state state {manifest_write_state::not_published};
    motif::db::error failure {error_code::ok};

    [[nodiscard]] auto has_value() const noexcept -> bool { return state == manifest_write_state::published; }

    explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] auto was_published() const noexcept -> bool { return state != manifest_write_state::not_published; }

    [[nodiscard]] auto error() const noexcept -> motif::db::error const& { return failure; }
};

// The envelope bytes manifest.json holds for manifest: schema version,
// generation, coverage, canonical count, payload version, exact payload bytes
// and payload checksum. Deterministic, so a cache is "current" if and only if its bytes
// equal this.
auto encode_manifest_cache(db_manifest const& manifest) -> result<std::string>;

// Atomically replaces the cache file: temp write -> fsync -> rename ->
// directory fsync. Invokes the fault-injection seam at each step.
auto write_manifest(std::filesystem::path const& path, db_manifest const& manifest) -> manifest_write_result;

// Reads and fully validates a manifest.json written by write_manifest(). The
// cache is never authoritative; this exists for diagnostics and tests.
// not_found when absent; io_failure when unreadable or malformed;
// schema_mismatch for a payload this build does not understand.
auto read_manifest(std::filesystem::path const& path) -> result<db_manifest>;

// True when path exists and holds exactly encode_manifest_cache(manifest).
auto manifest_cache_current(std::filesystem::path const& path, db_manifest const& manifest) -> bool;

// ── Legacy (schema <= 6) manifest, migration only ────────────────────────────

struct legacy_manifest_entry
{
    std::string filename;
    std::uint64_t source_generation {};
    std::uint64_t game_count {};
    std::uint64_t file_size {};
    std::uint64_t checksum {};
};

// The pre-v7 manifest.json. It is read only by open() to reject a bundle
// written by a newer build before any in-place migration and to recover the
// bundle name and creation time. Its artifact entries are never adopted.
struct legacy_manifest
{
    std::string name;
    std::uint32_t schema_version {1};
    std::uint64_t game_count {0};
    std::string created_at;
    bool position_index_dirty {false};
    std::uint64_t source_generation {};
    std::uint64_t derived_index_build_seq {};
    std::optional<legacy_manifest_entry> position_postings;
    std::optional<legacy_manifest_entry> opening_tree_index;
};

// Lenient about unknown fields (a newer manifest must still yield its
// schema_version so it can be rejected). not_found when absent; io_failure
// when unreadable or not a manifest.
auto read_legacy_manifest(std::filesystem::path const& path) -> result<legacy_manifest>;

// ── Filesystem durability helpers shared with database_manager ───────────────

// fsync a regular file or (directory = true) a directory. False on failure.
auto sync_filesystem_path(std::filesystem::path const& path, bool directory) -> bool;

// Current UTC time as ISO 8601, e.g. "2026-04-18T14:30:00Z".
auto now_iso8601() -> std::string;

}  // namespace motif::db
