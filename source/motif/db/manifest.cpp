#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "motif/db/manifest.hpp"

#include <fmt/chrono.h>  // NOLINT(misc-include-cleaner) — provides chrono format specializations
#include <fmt/format.h>
#include <glaze/json/read.hpp>
#include <glaze/json/write.hpp>
#include <tl/expected.hpp>
#ifdef _WIN32
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#else
#    include <fcntl.h>
#    include <unistd.h>
#endif

#include "motif/db/error.hpp"
#include "motif/db/fault_injection.hpp"
#include "motif/db/opening_tree_index.hpp"
#include "motif/db/position_postings.hpp"
#include "motif/db/schema.hpp"

namespace motif::db
{

namespace
{

constexpr auto checksum_algorithm_name = std::string_view {"fnv1a64"};
constexpr auto document_encoding_name = std::string_view {"implicit_revision_1"};
constexpr auto scope_name = std::string_view {"mainline_only"};
constexpr auto initial_position_name = std::string_view {"standard"};
constexpr auto occurrence_locator_name = std::string_view {"ply_u16"};
constexpr auto metadata_encoding_name = std::string_view {"result_i8_optional_elo_i16"};
constexpr auto random_id_hex_digits = std::size_t {32};
constexpr auto counter_max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
constexpr auto checksum_hex_digits = std::size_t {16};
constexpr auto hex_base = 16;

}  // namespace

// Glaze reflects aggregates by mangled type name, which needs external
// linkage, so the wire model lives in a named (not anonymous) namespace.
namespace manifest_wire
{

// ── Wire model ───────────────────────────────────────────────────────────────
// Decimal counters travel as JSON strings so no consumer rounds at 2^53; the
// typed model in manifest.hpp holds real integers.

struct wire_bundle
{
    std::string id;
    std::string name;
    std::string created_at;
};

struct wire_semantics
{
    std::uint32_t version {};
    std::string scope;
    std::string initial_position;
    std::string occurrence_locator;
    std::string metadata_encoding;
};

struct wire_segment
{
    std::string id;
    std::string filename;
    std::string format;
    std::uint32_t format_version {};
    std::string size_bytes;
    std::string checksum_algorithm;
    std::string checksum;
    std::string source_mutation_seq;
    std::string document_count;
    std::string document_encoding;
    std::string min_content_revision;
    std::string max_content_revision;
};

struct wire_postings
{
    std::vector<wire_segment> segments;
};

struct wire_checkpoint
{
    std::string id;
    std::string filename;
    std::string format;
    std::uint32_t format_version {};
    std::string size_bytes;
    std::string checksum_algorithm;
    std::string checksum;
    std::string base_mutation_seq;
};

struct wire_tree
{
    std::string covered_mutation_seq;
    std::uint16_t max_root_ply {};
    std::string completeness_policy;
    wire_checkpoint checkpoint;
    std::vector<std::string> patch_runs;
    std::vector<std::string> invalid_fields;
};

struct wire_payload
{
    wire_bundle bundle;
    wire_semantics index_semantics;
    wire_postings postings;
    std::optional<wire_tree> tree;
};

// Payload version 2 wire model: a metadata chain on the postings, and patch runs
// plus a child-frequency mode on the tree. Run descriptors repeat the artifact
// fields and add the half-open sequence interval (from, to] they cover.
struct wire_run
{
    std::string id;
    std::string filename;
    std::string format;
    std::uint32_t format_version {};
    std::string size_bytes;
    std::string checksum_algorithm;
    std::string checksum;
    std::string from_exclusive_seq;
    std::string to_inclusive_seq;
};

struct wire_metadata_chain
{
    std::optional<wire_run> checkpoint;
    std::vector<wire_run> patch_runs;
};

struct wire_postings_v2
{
    std::vector<wire_segment> segments;
    wire_metadata_chain metadata;
};

struct wire_tree_v2
{
    std::string covered_mutation_seq;
    std::uint16_t max_root_ply {};
    std::string completeness_policy;
    std::string child_frequency_mode;
    wire_checkpoint checkpoint;
    std::vector<wire_run> patch_runs;
};

struct wire_payload_v2
{
    wire_bundle bundle;
    wire_semantics index_semantics;
    wire_postings_v2 postings;
    std::optional<wire_tree_v2> tree;
};

struct wire_envelope
{
    std::uint32_t schema_version {};
    std::string publication_generation;
    std::optional<std::string> covered_mutation_seq;
    std::string canonical_game_count;
    std::uint32_t payload_version {};
    std::string payload_checksum;
    std::string payload;
};

}  // namespace manifest_wire

namespace
{

using manifest_wire::wire_bundle;
using manifest_wire::wire_checkpoint;
using manifest_wire::wire_envelope;
using manifest_wire::wire_payload;
using manifest_wire::wire_payload_v2;
using manifest_wire::wire_run;
using manifest_wire::wire_segment;
using manifest_wire::wire_semantics;
using manifest_wire::wire_tree;
using manifest_wire::wire_tree_v2;

constexpr auto strict_read_opts = glz::opts {.error_on_unknown_keys = true, .error_on_missing_keys = true};
constexpr auto lenient_read_opts = glz::opts {.error_on_unknown_keys = false};

auto fail(std::string message) -> tl::unexpected<error>
{
    return tl::unexpected {error {error_code::io_failure, std::move(message)}};
}

// Parses the whole of `text` as an integer in `base`; nullopt on any trailing or missing characters.
template<typename Integer>
auto parse_integer(std::string_view const text, int const base = 10) -> std::optional<Integer>
{
    auto value = Integer {};
    auto const* const first = text.data();
    auto const* const last = std::next(first, static_cast<std::ptrdiff_t>(text.size()));
    auto const [end, ec] = std::from_chars(first, last, value, base);
    if (ec != std::errc {} || end != last) {
        return std::nullopt;
    }
    return value;
}

auto parse_counter(std::string_view text) -> std::optional<std::uint64_t>
{
    if (text.empty() || (text.size() > 1 && text.front() == '0')) {
        return std::nullopt;
    }
    auto const value = parse_integer<std::uint64_t>(text);
    if (!value || *value > counter_max) {
        return std::nullopt;
    }
    return value;
}

auto is_lower_hex(std::string_view text, std::size_t const length) -> bool
{
    return text.size() == length
        && std::ranges::all_of(text, [](char const chr) -> bool { return (chr >= '0' && chr <= '9') || (chr >= 'a' && chr <= 'f'); });
}

auto to_wire(artifact_descriptor const& artifact) -> wire_checkpoint
{
    return wire_checkpoint {.id = artifact.id,
                            .filename = artifact.filename,
                            .format = artifact.format,
                            .format_version = artifact.format_version,
                            .size_bytes = std::to_string(artifact.size_bytes),
                            .checksum_algorithm = std::string {checksum_algorithm_name},
                            .checksum = checksum_to_hex(artifact.checksum),
                            .base_mutation_seq = {}};
}

auto to_wire(snapshot_payload const& payload) -> wire_payload
{
    wire_payload wire;
    wire.bundle = {.id = payload.bundle.id, .name = payload.bundle.name, .created_at = payload.bundle.created_at};
    wire.index_semantics = {.version = index_semantics_version,
                            .scope = std::string {scope_name},
                            .initial_position = std::string {initial_position_name},
                            .occurrence_locator = std::string {occurrence_locator_name},
                            .metadata_encoding = std::string {metadata_encoding_name}};
    for (auto const& segment : payload.segments) {
        auto const& artifact = segment.artifact;
        wire.postings.segments.push_back(wire_segment {.id = artifact.id,
                                                       .filename = artifact.filename,
                                                       .format = artifact.format,
                                                       .format_version = artifact.format_version,
                                                       .size_bytes = std::to_string(artifact.size_bytes),
                                                       .checksum_algorithm = std::string {checksum_algorithm_name},
                                                       .checksum = checksum_to_hex(artifact.checksum),
                                                       .source_mutation_seq = std::to_string(segment.source_mutation_seq),
                                                       .document_count = std::to_string(segment.document_count),
                                                       .document_encoding = std::string {document_encoding_name},
                                                       .min_content_revision = std::to_string(segment.min_content_revision),
                                                       .max_content_revision = std::to_string(segment.max_content_revision)});
    }
    if (payload.tree) {
        auto checkpoint = to_wire(payload.tree->checkpoint);
        checkpoint.base_mutation_seq = std::to_string(payload.tree->base_mutation_seq);
        wire.tree = wire_tree {.covered_mutation_seq = std::to_string(payload.tree->covered_mutation_seq),
                               .max_root_ply = payload.tree->max_root_ply,
                               .completeness_policy = payload.tree->completeness_policy,
                               .checkpoint = std::move(checkpoint),
                               .patch_runs = {},
                               .invalid_fields = {}};
    }
    return wire;
}

auto to_wire_run(patch_run_descriptor const& run) -> wire_run
{
    return wire_run {.id = run.artifact.id,
                     .filename = run.artifact.filename,
                     .format = run.artifact.format,
                     .format_version = run.artifact.format_version,
                     .size_bytes = std::to_string(run.artifact.size_bytes),
                     .checksum_algorithm = std::string {checksum_algorithm_name},
                     .checksum = checksum_to_hex(run.artifact.checksum),
                     .from_exclusive_seq = std::to_string(run.from_exclusive_seq),
                     .to_inclusive_seq = std::to_string(run.to_inclusive_seq)};
}

auto to_wire_segments(snapshot_payload const& payload) -> std::vector<wire_segment>
{
    auto segments = std::vector<wire_segment> {};
    for (auto const& segment : payload.segments) {
        auto const& artifact = segment.artifact;
        segments.push_back(wire_segment {.id = artifact.id,
                                         .filename = artifact.filename,
                                         .format = artifact.format,
                                         .format_version = artifact.format_version,
                                         .size_bytes = std::to_string(artifact.size_bytes),
                                         .checksum_algorithm = std::string {checksum_algorithm_name},
                                         .checksum = checksum_to_hex(artifact.checksum),
                                         .source_mutation_seq = std::to_string(segment.source_mutation_seq),
                                         .document_count = std::to_string(segment.document_count),
                                         .document_encoding = std::string {document_encoding_name},
                                         .min_content_revision = std::to_string(segment.min_content_revision),
                                         .max_content_revision = std::to_string(segment.max_content_revision)});
    }
    return segments;
}

auto to_wire_semantics() -> wire_semantics
{
    return wire_semantics {.version = index_semantics_version,
                           .scope = std::string {scope_name},
                           .initial_position = std::string {initial_position_name},
                           .occurrence_locator = std::string {occurrence_locator_name},
                           .metadata_encoding = std::string {metadata_encoding_name}};
}

auto to_wire_v2(snapshot_payload const& payload) -> wire_payload_v2
{
    wire_payload_v2 wire;
    wire.bundle = {.id = payload.bundle.id, .name = payload.bundle.name, .created_at = payload.bundle.created_at};
    wire.index_semantics = to_wire_semantics();
    wire.postings.segments = to_wire_segments(payload);
    if (payload.metadata.checkpoint) {
        wire.postings.metadata.checkpoint = to_wire_run(*payload.metadata.checkpoint);
    }
    for (auto const& run : payload.metadata.runs) {
        wire.postings.metadata.patch_runs.push_back(to_wire_run(run));
    }
    if (payload.tree) {
        auto const& tree = *payload.tree;
        auto checkpoint = to_wire(tree.checkpoint);
        checkpoint.base_mutation_seq = std::to_string(tree.base_mutation_seq);
        auto wire_tree = wire_tree_v2 {.covered_mutation_seq = std::to_string(tree.covered_mutation_seq),
                                       .max_root_ply = tree.max_root_ply,
                                       .completeness_policy = tree.completeness_policy,
                                       .child_frequency_mode = std::string {to_string(tree.child_frequency_mode)},
                                       .checkpoint = std::move(checkpoint),
                                       .patch_runs = {}};
        for (auto const& run : tree.patch_runs) {
            wire_tree.patch_runs.push_back(to_wire_run(run));
        }
        wire.tree = std::move(wire_tree);
    }
    return wire;
}

// Shared descriptor checks: identity shape, filename = prefix + id (so no path
// component and no traversal can exist), known format, supported version, the
// one supported checksum algorithm.
auto decode_artifact(std::string const& artifact_id,
                     std::string const& filename,
                     std::string const& format,
                     std::uint32_t const format_version,
                     std::string const& size_bytes,
                     std::string const& algorithm,
                     std::string const& checksum,
                     std::string_view const prefix,
                     std::string_view const expected_format,
                     std::initializer_list<std::uint32_t> const accepted_versions) -> result<artifact_descriptor>
{
    auto const size = parse_counter(size_bytes);
    auto const sum = checksum_from_hex(checksum);
    auto const version_ok = std::ranges::find(accepted_versions, format_version) != accepted_versions.end();
    if (!is_lower_hex(artifact_id, random_id_hex_digits) || filename != std::string {prefix} + artifact_id || format != expected_format
        || !version_ok || algorithm != checksum_algorithm_name || !size || !sum)
    {
        return fail("invalid artifact descriptor");
    }
    return artifact_descriptor {
        .id = artifact_id, .filename = filename, .format = format, .format_version = format_version, .size_bytes = *size, .checksum = *sum};
}

auto decode_run(wire_run const& run, std::string_view const prefix, std::string_view const format, std::uint32_t const version)
    -> result<patch_run_descriptor>
{
    auto artifact = decode_artifact(run.id,
                                    run.filename,
                                    run.format,
                                    run.format_version,
                                    run.size_bytes,
                                    run.checksum_algorithm,
                                    run.checksum,
                                    prefix,
                                    format,
                                    {version});
    if (!artifact) {
        return tl::unexpected {artifact.error()};
    }
    auto const from_seq = parse_counter(run.from_exclusive_seq);
    auto const to_seq = parse_counter(run.to_inclusive_seq);
    if (!from_seq || !to_seq) {
        return fail("invalid patch run interval");
    }
    return patch_run_descriptor {.artifact = std::move(*artifact), .from_exclusive_seq = *from_seq, .to_inclusive_seq = *to_seq};
}

auto decode_runs(std::vector<wire_run> const& wires,
                 std::string_view const prefix,
                 std::string_view const format,
                 std::uint32_t const version) -> result<std::vector<patch_run_descriptor>>
{
    if (wires.size() > max_patch_chain_runs) {
        return fail("too many patch runs");
    }
    auto runs = std::vector<patch_run_descriptor> {};
    runs.reserve(wires.size());
    for (auto const& wire : wires) {
        auto run = decode_run(wire, prefix, format, version);
        if (!run) {
            return tl::unexpected {run.error()};
        }
        runs.push_back(std::move(*run));
    }
    return runs;
}

// Bundle identity, index semantics and the postings segment: identical in both
// payload versions.
auto decode_common(wire_semantics const& semantics, wire_bundle const& bundle, std::vector<wire_segment> const& segments)
    -> result<snapshot_payload>
{
    if (semantics.version != index_semantics_version || semantics.scope != scope_name || semantics.initial_position != initial_position_name
        || semantics.occurrence_locator != occurrence_locator_name || semantics.metadata_encoding != metadata_encoding_name)
    {
        return tl::unexpected {error {error_code::schema_mismatch, "unsupported index semantics"}};
    }
    if (!is_lower_hex(bundle.id, random_id_hex_digits)) {
        return fail("invalid bundle id");
    }

    snapshot_payload payload;
    payload.bundle = {.id = bundle.id, .name = bundle.name, .created_at = bundle.created_at};

    if (segments.size() > 1) {
        return tl::unexpected {error {error_code::schema_mismatch, "snapshot payload holds at most one postings segment"}};
    }
    for (auto const& segment : segments) {
        auto artifact = decode_artifact(segment.id,
                                        segment.filename,
                                        segment.format,
                                        segment.format_version,
                                        segment.size_bytes,
                                        segment.checksum_algorithm,
                                        segment.checksum,
                                        postings_artifact_prefix,
                                        postings_format_name,
                                        {position_postings_format_version});
        if (!artifact) {
            return tl::unexpected {artifact.error()};
        }
        auto const source_seq = parse_counter(segment.source_mutation_seq);
        auto const documents = parse_counter(segment.document_count);
        auto const min_revision = parse_counter(segment.min_content_revision);
        auto const max_revision = parse_counter(segment.max_content_revision);
        if (!source_seq || !documents || !min_revision || !max_revision || segment.document_encoding != document_encoding_name
            || *min_revision != 1U || *max_revision != 1U)
        {
            return fail("invalid postings segment");
        }
        payload.segments.push_back(postings_segment {.artifact = std::move(*artifact),
                                                     .source_mutation_seq = *source_seq,
                                                     .document_count = *documents,
                                                     .min_content_revision = *min_revision,
                                                     .max_content_revision = *max_revision});
    }
    return payload;
}

// Payload version 1: exactly zero or one postings segment, an optional v5 tree
// checkpoint, no metadata chain and no patch runs.
auto decode_wire(wire_payload const& wire) -> result<snapshot_payload>
{
    auto payload = decode_common(wire.index_semantics, wire.bundle, wire.postings.segments);
    if (!payload) {
        return payload;
    }
    if (wire.tree) {
        auto const& tree = *wire.tree;
        if (!tree.patch_runs.empty() || !tree.invalid_fields.empty()) {
            return tl::unexpected {error {error_code::schema_mismatch, "payload version 1 holds no tree patch runs"}};
        }
        auto checkpoint = decode_artifact(tree.checkpoint.id,
                                          tree.checkpoint.filename,
                                          tree.checkpoint.format,
                                          tree.checkpoint.format_version,
                                          tree.checkpoint.size_bytes,
                                          tree.checkpoint.checksum_algorithm,
                                          tree.checkpoint.checksum,
                                          tree_artifact_prefix,
                                          tree_format_name,
                                          {opening_tree_index_legacy_format_version});
        if (!checkpoint) {
            return tl::unexpected {checkpoint.error()};
        }
        auto const covered = parse_counter(tree.covered_mutation_seq);
        auto const base = parse_counter(tree.checkpoint.base_mutation_seq);
        if (!covered || !base || tree.completeness_policy != tree_completeness_policy_name) {
            return fail("invalid tree descriptor");
        }
        payload->tree = tree_descriptor {.covered_mutation_seq = *covered,
                                         .max_root_ply = tree.max_root_ply,
                                         .completeness_policy = tree.completeness_policy,
                                         .checkpoint = std::move(*checkpoint),
                                         .base_mutation_seq = *base};
    }
    return payload;
}

// Payload version 2: the normalized form with a metadata chain, tree patch runs,
// a child-frequency mode, and a v5 or v6 tree checkpoint.
auto decode_wire_v2(wire_payload_v2 const& wire) -> result<snapshot_payload>
{
    auto payload = decode_common(wire.index_semantics, wire.bundle, wire.postings.segments);
    if (!payload) {
        return payload;
    }
    auto const& metadata = wire.postings.metadata;
    if (metadata.checkpoint) {
        auto checkpoint = decode_run(
            *metadata.checkpoint, postings_metadata_artifact_prefix, postings_metadata_format_name, postings_metadata_format_version);
        if (!checkpoint) {
            return tl::unexpected {checkpoint.error()};
        }
        payload->metadata.checkpoint = std::move(*checkpoint);
    }
    auto metadata_runs = decode_runs(
        metadata.patch_runs, postings_metadata_artifact_prefix, postings_metadata_format_name, postings_metadata_format_version);
    if (!metadata_runs) {
        return tl::unexpected {metadata_runs.error()};
    }
    payload->metadata.runs = std::move(*metadata_runs);

    if (wire.tree) {
        auto const& tree = *wire.tree;
        auto checkpoint = decode_artifact(tree.checkpoint.id,
                                          tree.checkpoint.filename,
                                          tree.checkpoint.format,
                                          tree.checkpoint.format_version,
                                          tree.checkpoint.size_bytes,
                                          tree.checkpoint.checksum_algorithm,
                                          tree.checkpoint.checksum,
                                          tree_artifact_prefix,
                                          tree_format_name,
                                          {opening_tree_index_legacy_format_version, opening_tree_index_format_version});
        if (!checkpoint) {
            return tl::unexpected {checkpoint.error()};
        }
        auto const covered = parse_counter(tree.covered_mutation_seq);
        auto const base = parse_counter(tree.checkpoint.base_mutation_seq);
        auto const mode = parse_child_frequency_mode(tree.child_frequency_mode);
        if (!covered || !base || !mode || tree.completeness_policy != tree_completeness_policy_name) {
            return fail("invalid tree descriptor");
        }
        auto runs = decode_runs(tree.patch_runs, tree_patch_artifact_prefix, tree_patch_format_name, tree_patch_format_version);
        if (!runs) {
            return tl::unexpected {runs.error()};
        }
        payload->tree = tree_descriptor {.covered_mutation_seq = *covered,
                                         .max_root_ply = tree.max_root_ply,
                                         .completeness_policy = tree.completeness_policy,
                                         .checkpoint = std::move(*checkpoint),
                                         .base_mutation_seq = *base,
                                         .child_frequency_mode = *mode,
                                         .patch_runs = std::move(*runs)};
    }
    return payload;
}

// Every artifact descriptor a payload references, in a fixed order.
auto collect_descriptors(snapshot_payload const& payload) -> std::vector<artifact_descriptor const*>
{
    auto all = std::vector<artifact_descriptor const*> {};
    for (auto const& segment : payload.segments) {
        all.push_back(&segment.artifact);
    }
    if (payload.metadata.checkpoint) {
        all.push_back(&payload.metadata.checkpoint->artifact);
    }
    for (auto const& run : payload.metadata.runs) {
        all.push_back(&run.artifact);
    }
    if (payload.tree) {
        all.push_back(&payload.tree->checkpoint);
        for (auto const& run : payload.tree->patch_runs) {
            all.push_back(&run.artifact);
        }
    }
    return all;
}

auto interval_is_valid(patch_run_descriptor const& run) -> bool
{
    return run.from_exclusive_seq < run.to_inclusive_seq && run.to_inclusive_seq <= counter_max;
}

// A chain of runs must start exactly at `baseline` (after the optional
// checkpoint, which itself starts there) and be contiguous. Returns where it
// ends.
auto validate_chain(std::uint64_t const baseline,
                    std::optional<patch_run_descriptor> const& checkpoint,
                    std::vector<patch_run_descriptor> const& runs) -> result<std::uint64_t>
{
    if (runs.size() > max_patch_chain_runs) {
        return fail("too many patch runs");
    }
    auto cursor = baseline;
    if (checkpoint) {
        if (!interval_is_valid(*checkpoint) || checkpoint->from_exclusive_seq != cursor) {
            return fail("metadata checkpoint does not start at its baseline");
        }
        cursor = checkpoint->to_inclusive_seq;
    }
    for (auto const& run : runs) {
        if (!interval_is_valid(run) || run.from_exclusive_seq != cursor) {
            return fail("patch runs are not a contiguous chain");
        }
        cursor = run.to_inclusive_seq;
    }
    return cursor;
}

// The tree's patch chain must start at its base and end exactly at its coverage.
auto validate_tree_chain(tree_descriptor const& tree, std::uint64_t const covered) -> result<void>
{
    if (tree.covered_mutation_seq > covered || tree.base_mutation_seq > tree.covered_mutation_seq) {
        return fail("tree descriptor disagrees with snapshot coverage");
    }
    auto const tree_end = validate_chain(tree.base_mutation_seq, std::nullopt, tree.patch_runs);
    if (!tree_end) {
        return tl::unexpected {tree_end.error()};
    }
    if (*tree_end != tree.covered_mutation_seq) {
        return fail("tree patch runs do not reach tree coverage");
    }
    return {};
}

// Identities and filenames are unique across the whole payload: a file may
// be referenced exactly once.
auto validate_unique_artifacts(snapshot_payload const& payload) -> result<void>
{
    auto const descriptors = collect_descriptors(payload);
    for (std::size_t outer = 0; outer < descriptors.size(); ++outer) {
        for (std::size_t inner = outer + 1; inner < descriptors.size(); ++inner) {
            if (descriptors[outer]->id == descriptors[inner]->id || descriptors[outer]->filename == descriptors[inner]->filename) {
                return fail("artifact referenced more than once");
            }
        }
    }
    return {};
}

// Cross-field invariants that must hold for any stored or about-to-be-stored
// snapshot. Validation against the live mutation sequence is the manager's.
auto validate_manifest(db_manifest const& manifest) -> result<void>
{
    if (manifest.publication_generation > counter_max || manifest.canonical_game_count > counter_max
        || (manifest.covered_mutation_seq && *manifest.covered_mutation_seq > counter_max))
    {
        return fail("snapshot counter out of range");
    }
    auto const& payload = manifest.payload;
    if (!manifest.covered_mutation_seq) {
        if (!payload.segments.empty() || payload.tree || !payload.metadata.empty()) {
            return fail("unbuilt snapshot must not reference artifacts");
        }
        return {};
    }
    auto const covered = *manifest.covered_mutation_seq;
    if (payload.segments.size() != 1) {
        return fail("built snapshot must reference exactly one postings segment");
    }
    auto const& segment = payload.segments.front();
    if (segment.document_count != manifest.canonical_game_count || segment.source_mutation_seq > covered) {
        return fail("postings segment disagrees with snapshot coverage");
    }
    auto const metadata_end = validate_chain(segment.source_mutation_seq, payload.metadata.checkpoint, payload.metadata.runs);
    if (!metadata_end) {
        return tl::unexpected {metadata_end.error()};
    }
    if (*metadata_end != covered) {
        return fail("postings metadata chain does not reach snapshot coverage");
    }
    if (payload.tree) {
        if (auto tree_valid = validate_tree_chain(*payload.tree, covered); !tree_valid) {
            return tl::unexpected {tree_valid.error()};
        }
    }
    if (manifest.payload_version == snapshot_payload_version_1) {
        // Version 1 predates chains: everything is at the baseline.
        if (segment.source_mutation_seq != covered
            || (payload.tree && payload.tree->base_mutation_seq != payload.tree->covered_mutation_seq))
        {
            return fail("snapshot payload version 1 holds no patch chains");
        }
    }
    return validate_unique_artifacts(payload);
}

// (validate_manifest is defined above, next to its helpers.)

auto read_file_bytes(std::filesystem::path const& path) -> result<std::string>
{
    std::error_code fs_err;
    auto const exists = std::filesystem::exists(path, fs_err);
    if (fs_err) {
        return tl::unexpected {error_code::io_failure};
    }
    if (!exists) {
        return tl::unexpected {error_code::not_found};
    }
    std::ifstream file {path, std::ios::binary};
    if (!file.is_open()) {
        return tl::unexpected {error_code::io_failure};
    }
    std::ostringstream buffer_stream;
    buffer_stream << file.rdbuf();
    if (!file) {
        return tl::unexpected {error_code::io_failure};
    }
    return buffer_stream.str();
}

}  // namespace

// ── Checksums and identities ─────────────────────────────────────────────────

auto fnv1a64(std::string_view const bytes) noexcept -> std::uint64_t
{
    auto checksum = fnv1a64_offset_basis;
    for (auto const byte : bytes) {
        checksum ^= static_cast<unsigned char>(byte);
        checksum *= fnv1a64_prime;
    }
    return checksum;
}

auto checksum_to_hex(std::uint64_t const checksum) -> std::string
{
    return fmt::format("{:016x}", checksum);
}

auto checksum_from_hex(std::string_view const hex) -> std::optional<std::uint64_t>
{
    if (!is_lower_hex(hex, checksum_hex_digits)) {
        return std::nullopt;
    }
    return parse_integer<std::uint64_t>(hex, hex_base);
}

auto generate_random_id() -> std::string
{
    std::random_device source;
    auto random_id = std::string {};
    random_id.reserve(random_id_hex_digits);
    for (auto word = 0; word < 4; ++word) {
        random_id += fmt::format("{:08x}", static_cast<std::uint32_t>(source()));
    }
    return random_id;
}

auto identity_from_hex(std::string_view const hex) -> std::optional<identity_bytes>
{
    if (!is_lower_hex(hex, random_id_hex_digits)) {
        return std::nullopt;
    }
    auto bytes = identity_bytes {};
    auto index = std::size_t {0};
    for (auto& byte : bytes) {
        auto const parsed = parse_integer<std::uint8_t>(hex.substr(2U * index, 2U), hex_base);
        if (!parsed) {
            return std::nullopt;
        }
        byte = *parsed;
        ++index;
    }
    return bytes;
}

auto now_iso8601() -> std::string
{
    auto const now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return fmt::format("{:%Y-%m-%dT%H:%M:%S}Z", now);
}

// ── Payload codec ────────────────────────────────────────────────────────────

auto required_payload_version(snapshot_payload const& payload) noexcept -> std::uint32_t
{
    auto const tree_needs_v2 = payload.tree
        && (payload.tree->checkpoint.format_version != opening_tree_index_legacy_format_version || !payload.tree->patch_runs.empty()
            || payload.tree->child_frequency_mode != tree_child_frequency_mode::checkpoint
            || payload.tree->base_mutation_seq != payload.tree->covered_mutation_seq);
    return tree_needs_v2 || !payload.metadata.empty() ? snapshot_payload_version_2 : snapshot_payload_version_1;
}

auto referenced_artifacts(snapshot_payload const& payload) -> std::vector<artifact_descriptor>
{
    auto out = std::vector<artifact_descriptor> {};
    for (auto const* descriptor : collect_descriptors(payload)) {
        out.push_back(*descriptor);
    }
    return out;
}

auto referenced_artifact_filenames(snapshot_payload const& payload) -> std::vector<std::string>
{
    auto names = std::vector<std::string> {};
    for (auto const* descriptor : collect_descriptors(payload)) {
        names.push_back(descriptor->filename);
    }
    return names;
}

auto encode_payload(snapshot_payload const& payload) -> result<std::string>
{
    std::string buffer;
    auto const failed = required_payload_version(payload) == snapshot_payload_version_2
        ? static_cast<bool>(glz::write_json(to_wire_v2(payload), buffer))
        : static_cast<bool>(glz::write_json(to_wire(payload), buffer));
    if (failed) {
        return fail("snapshot payload serialization failed");
    }
    return buffer;
}

auto decode_payload(std::uint32_t const payload_version, std::string_view const payload_json) -> result<snapshot_payload>
{
    if (payload_version == snapshot_payload_version_1) {
        wire_payload wire;
        if (glz::read<strict_read_opts>(wire, payload_json)) {
            return fail("snapshot payload is malformed");
        }
        return decode_wire(wire);
    }
    if (payload_version == snapshot_payload_version_2) {
        wire_payload_v2 wire;
        if (glz::read<strict_read_opts>(wire, payload_json)) {
            return fail("snapshot payload is malformed");
        }
        return decode_wire_v2(wire);
    }
    return tl::unexpected {error {error_code::schema_mismatch, "unsupported snapshot payload version"}};
}

auto build_manifest(std::uint64_t const publication_generation,
                    std::optional<std::uint64_t> const covered_mutation_seq,
                    std::uint64_t const canonical_game_count,
                    snapshot_payload payload) -> result<db_manifest>
{
    auto encoded = encode_payload(payload);
    if (!encoded) {
        return tl::unexpected {encoded.error()};
    }
    db_manifest manifest {.schema_version = schema::current_version,
                          .publication_generation = publication_generation,
                          .covered_mutation_seq = covered_mutation_seq,
                          .canonical_game_count = canonical_game_count,
                          .payload_version = required_payload_version(payload),
                          .payload_json = std::move(*encoded),
                          .payload_checksum = {},
                          .payload = std::move(payload)};
    manifest.payload_checksum = checksum_to_hex(fnv1a64(manifest.payload_json));
    if (auto valid = validate_manifest(manifest); !valid) {
        return tl::unexpected {valid.error()};
    }
    return manifest;
}

auto parse_manifest(std::uint64_t const publication_generation,
                    std::optional<std::uint64_t> const covered_mutation_seq,
                    std::uint64_t const canonical_game_count,
                    std::uint32_t const payload_version,
                    std::string payload_json,
                    std::string payload_checksum) -> result<db_manifest>
{
    if (checksum_to_hex(fnv1a64(payload_json)) != payload_checksum) {
        return fail("snapshot payload checksum mismatch");
    }
    auto payload = decode_payload(payload_version, payload_json);
    if (!payload) {
        return tl::unexpected {payload.error()};
    }
    db_manifest manifest {.schema_version = schema::current_version,
                          .publication_generation = publication_generation,
                          .covered_mutation_seq = covered_mutation_seq,
                          .canonical_game_count = canonical_game_count,
                          .payload_version = payload_version,
                          .payload_json = std::move(payload_json),
                          .payload_checksum = std::move(payload_checksum),
                          .payload = std::move(*payload)};
    if (auto valid = validate_manifest(manifest); !valid) {
        return tl::unexpected {valid.error()};
    }
    return manifest;
}

// ── manifest.json cache ──────────────────────────────────────────────────────

auto sync_filesystem_path(std::filesystem::path const& path, bool const directory) -> bool
{
#ifdef _WIN32
    if (directory) {
        return true;
    }
    auto const flags = directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;
    auto const handle = CreateFileW(path.c_str(),
                                    GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    flags,
                                    nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    auto const result = FlushFileBuffers(handle) != 0;
    CloseHandle(handle);
    return result;
#else
    auto const flags = directory ? O_RDONLY | O_DIRECTORY | O_CLOEXEC : O_RDONLY | O_CLOEXEC;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg) -- POSIX open is required for fsync durability.
    auto const descriptor = ::open(path.c_str(), flags);
    if (descriptor < 0) {
        return false;
    }
    auto const result = ::fsync(descriptor) == 0;
    ::close(descriptor);
    return result;
#endif
}

auto encode_manifest_cache(db_manifest const& manifest) -> result<std::string>
{
    wire_envelope envelope {.schema_version = manifest.schema_version,
                            .publication_generation = std::to_string(manifest.publication_generation),
                            .covered_mutation_seq = manifest.covered_mutation_seq
                                ? std::optional<std::string> {std::to_string(*manifest.covered_mutation_seq)}
                                : std::nullopt,
                            .canonical_game_count = std::to_string(manifest.canonical_game_count),
                            .payload_version = manifest.payload_version,
                            .payload_checksum = manifest.payload_checksum,
                            .payload = manifest.payload_json};
    std::string buffer;
    if (glz::write_json(envelope, buffer)) {
        return fail("manifest serialization failed");
    }
    return buffer;
}

auto write_manifest(std::filesystem::path const& path, db_manifest const& manifest) -> manifest_write_result
{
    auto const failed = [](manifest_write_state const state) -> manifest_write_result
    { return manifest_write_result {.state = state, .failure = error {error_code::io_failure}}; };
    auto const generation = manifest.publication_generation;
    auto const injected = [generation](fault::point const where) -> bool
    { return fault::inject(where, fault::artifact_kind::none, generation) == fault::fault_action::fail_io; };

    auto encoded = encode_manifest_cache(manifest);
    if (!encoded) {
        return failed(manifest_write_state::not_published);
    }
    if (injected(fault::point::before_manifest_write)) {
        return failed(manifest_write_state::not_published);
    }

    auto const temp_path = path.string() + ".tmp";
    std::error_code fs_err;
    auto const discard_temp = [&temp_path]() -> void
    {
        std::error_code ignored;
        std::filesystem::remove(temp_path, ignored);
    };
    {
        std::ofstream file {temp_path, std::ios::trunc | std::ios::binary};
        if (!file.is_open()) {
            return failed(manifest_write_state::not_published);
        }
        file << *encoded;
        if (!file) {
            discard_temp();
            return failed(manifest_write_state::not_published);
        }
        file.close();
        if (!file) {
            discard_temp();
            return failed(manifest_write_state::not_published);
        }
    }
    if (injected(fault::point::after_manifest_temp_write)) {
        discard_temp();
        return failed(manifest_write_state::not_published);
    }
    if (!sync_filesystem_path(temp_path, /*directory=*/false)) {
        discard_temp();
        return failed(manifest_write_state::not_published);
    }
    if (injected(fault::point::after_manifest_temp_fsync) || injected(fault::point::before_manifest_rename)) {
        discard_temp();
        return failed(manifest_write_state::not_published);
    }
#ifdef _WIN32
    if (MoveFileExW(std::filesystem::path {temp_path}.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        discard_temp();
        return failed(manifest_write_state::not_published);
    }
#else
    std::filesystem::rename(temp_path, path, fs_err);
    if (fs_err) {
        discard_temp();
        return failed(manifest_write_state::not_published);
    }
#endif
    if (injected(fault::point::after_manifest_rename) || injected(fault::point::before_manifest_directory_fsync)) {
        return failed(manifest_write_state::published_not_durable);
    }
    auto const directory_path = path.parent_path().empty() ? std::filesystem::path {"."} : path.parent_path();
    if (!sync_filesystem_path(directory_path, /*directory=*/true)) {
        return failed(manifest_write_state::published_not_durable);
    }
    if (injected(fault::point::after_manifest_write)) {
        return failed(manifest_write_state::published_not_durable);
    }
    return manifest_write_result {.state = manifest_write_state::published, .failure = error {error_code::ok}};
}

auto read_manifest(std::filesystem::path const& path) -> result<db_manifest>
{
    auto bytes = read_file_bytes(path);
    if (!bytes) {
        return tl::unexpected {bytes.error()};
    }
    wire_envelope envelope;
    if (glz::read<strict_read_opts>(envelope, *bytes)) {
        return fail("manifest.json is malformed");
    }
    if (envelope.schema_version > schema::current_version) {
        return tl::unexpected {error {error_code::schema_mismatch, "manifest.json schema is newer than supported"}};
    }
    auto const generation = parse_counter(envelope.publication_generation);
    auto const games = parse_counter(envelope.canonical_game_count);
    auto covered = std::optional<std::uint64_t> {};
    if (envelope.covered_mutation_seq) {
        covered = parse_counter(*envelope.covered_mutation_seq);
        if (!covered) {
            return fail("manifest.json coverage is malformed");
        }
    }
    if (!generation || !games) {
        return fail("manifest.json counters are malformed");
    }
    auto manifest = parse_manifest(
        *generation, covered, *games, envelope.payload_version, std::move(envelope.payload), std::move(envelope.payload_checksum));
    if (manifest) {
        manifest->schema_version = envelope.schema_version;
    }
    return manifest;
}

auto manifest_cache_current(std::filesystem::path const& path, db_manifest const& manifest) -> bool
{
    auto const expected = encode_manifest_cache(manifest);
    if (!expected) {
        return false;
    }
    auto const actual = read_file_bytes(path);
    return actual && *actual == *expected;
}

// ── Legacy manifest ──────────────────────────────────────────────────────────

auto read_legacy_manifest(std::filesystem::path const& path) -> result<legacy_manifest>
{
    auto bytes = read_file_bytes(path);
    if (!bytes) {
        return tl::unexpected {bytes.error()};
    }
    legacy_manifest parsed;
    if (glz::read<lenient_read_opts>(parsed, *bytes)) {
        return fail("manifest.json is not a legacy manifest");
    }
    return parsed;
}

}  // namespace motif::db
