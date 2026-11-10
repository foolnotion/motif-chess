#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include "motif/db/manifest.hpp"

#include <catch2/catch_test_macros.hpp>

#include "composite_test_support.hpp"
#include "motif/db/error.hpp"
#include "motif/db/fault_injection.hpp"
#include "motif/db/opening_tree_index.hpp"
#include "motif/db/position_postings.hpp"
#include "motif/db/schema.hpp"

namespace
{

struct tmp_file
{
    std::filesystem::path path;

    explicit tmp_file(std::string const& suffix)
    {
        path = std::filesystem::temp_directory_path() / ("motif_manifest_test_" + suffix + ".json");
    }

    ~tmp_file()
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + ".tmp", ignored);
    }

    tmp_file(tmp_file const&) = delete;
    auto operator=(tmp_file const&) -> tmp_file& = delete;
    tmp_file(tmp_file&&) = delete;
    auto operator=(tmp_file&&) -> tmp_file& = delete;
};

constexpr auto artifact_id_a = "b074048b02154d23a0fb126c7601ba5e";
constexpr auto artifact_id_b = "bb89d780941b4ab0a5424a85335ffab7";

constexpr auto postings_artifact_size = std::uint64_t {1'085'643'669};
constexpr auto postings_artifact_checksum = std::uint64_t {0xfeedfacecafebeefULL};
constexpr auto tree_artifact_size = std::uint64_t {258'501'913};
constexpr auto tree_artifact_checksum = std::uint64_t {0x0123456789abcdefULL};
constexpr auto tree_horizon = std::uint16_t {20};
// The snapshot most tests build: coverage at mutation sequence 7 over 10 documents.
constexpr auto covered_seq = std::uint64_t {7};
constexpr auto game_total = std::uint64_t {10};
// The chain fixtures build generation 5 covering sequence 9.
constexpr auto chain_generation = std::uint64_t {5};
constexpr auto chain_seq = std::uint64_t {9};
constexpr auto id_hex_digits = std::size_t {32};

auto postings_artifact(std::string const& artifact_id) -> motif::db::artifact_descriptor
{
    return {.id = artifact_id,
            .filename = std::string {motif::db::postings_artifact_prefix} + artifact_id,
            .format = std::string {motif::db::postings_format_name},
            .format_version = motif::db::position_postings_format_version,
            .size_bytes = postings_artifact_size,
            .checksum = postings_artifact_checksum};
}

auto tree_artifact(std::string const& artifact_id) -> motif::db::artifact_descriptor
{
    return {.id = artifact_id,
            .filename = std::string {motif::db::tree_artifact_prefix} + artifact_id,
            .format = std::string {motif::db::tree_format_name},
            .format_version = motif::db::opening_tree_index_legacy_format_version,
            .size_bytes = tree_artifact_size,
            .checksum = tree_artifact_checksum};
}

auto unbuilt_payload() -> motif::db::snapshot_payload
{
    return {.bundle = {.id = "0123456789abcdef0123456789abcdef", .name = "Reference", .created_at = "2026-11-07T00:00:00Z"},
            .segments = {},
            .metadata = {},
            .tree = std::nullopt};
}

auto built_payload(std::uint64_t const seq, std::uint64_t const documents, bool const with_tree) -> motif::db::snapshot_payload
{
    auto payload = unbuilt_payload();
    payload.segments.push_back(motif::db::postings_segment {.artifact = postings_artifact(artifact_id_a),
                                                            .source_mutation_seq = seq,
                                                            .document_count = documents,
                                                            .min_content_revision = 1,
                                                            .max_content_revision = 1});
    if (with_tree) {
        payload.tree = motif::db::tree_descriptor {.covered_mutation_seq = seq,
                                                   .max_root_ply = tree_horizon,
                                                   .completeness_policy = std::string {motif::db::tree_completeness_policy_name},
                                                   .checkpoint = tree_artifact(artifact_id_b),
                                                   .base_mutation_seq = seq,
                                                   .patch_runs = {}};
    }
    return payload;
}

// Replaces the first occurrence of from in payload_json; used to forge variants
// of a valid payload while keeping the checksum honest.
auto forged(std::string json, std::string const& from, std::string const& replacement) -> std::string
{
    auto const position = json.find(from);
    REQUIRE(position != std::string::npos);
    json.replace(position, from.size(), replacement);
    return json;
}

auto parse_with_checksum(std::string const& json, std::uint32_t const version = motif::db::snapshot_payload_version_1)
    -> motif::db::result<motif::db::db_manifest>
{
    return motif::db::parse_manifest(3, covered_seq, game_total, version, json, motif::db::checksum_to_hex(motif::db::fnv1a64(json)));
}

}  // namespace

TEST_CASE("manifest: envelope round-trip preserves the published snapshot", "[motif-db][manifest]")
{
    auto manifest = motif::db::build_manifest(3, covered_seq, game_total, built_payload(covered_seq, game_total, /*with_tree=*/true));
    REQUIRE(manifest.has_value());

    tmp_file const tmp {"roundtrip"};
    auto const write_res = motif::db::write_manifest(tmp.path, *manifest);
    REQUIRE(write_res.has_value());
    CHECK(motif::db::manifest_cache_current(tmp.path, *manifest));

    auto const read_res = motif::db::read_manifest(tmp.path);
    REQUIRE(read_res.has_value());
    CHECK(read_res->schema_version == motif::db::schema::current_version);
    CHECK(read_res->publication_generation == 3U);
    CHECK(read_res->covered_mutation_seq == std::optional<std::uint64_t> {7});
    CHECK(read_res->canonical_game_count == 10U);
    CHECK(read_res->payload_json == manifest->payload_json);
    CHECK(read_res->payload_checksum == manifest->payload_checksum);
    CHECK(read_res->name() == "Reference");
    CHECK(read_res->created_at() == "2026-11-07T00:00:00Z");
    REQUIRE(read_res->postings() != nullptr);
    CHECK(read_res->postings()->artifact.checksum == 0xfeedfacecafebeefULL);
    CHECK(read_res->postings()->artifact.size_bytes == 1'085'643'669ULL);
    CHECK(read_res->postings()->artifact.filename == std::string {"positions.postings."} + artifact_id_a);
    REQUIRE(read_res->tree() != nullptr);
    CHECK(read_res->tree()->max_root_ply == 20U);
    CHECK(read_res->tree()->covered_mutation_seq == 7U);
}

TEST_CASE("manifest: an unbuilt snapshot round-trips with null coverage", "[motif-db][manifest]")
{
    auto manifest = motif::db::build_manifest(0, std::nullopt, 0, unbuilt_payload());
    REQUIRE(manifest.has_value());
    tmp_file const tmp {"unbuilt"};
    REQUIRE(motif::db::write_manifest(tmp.path, *manifest).has_value());
    auto const read_res = motif::db::read_manifest(tmp.path);
    REQUIRE(read_res.has_value());
    CHECK_FALSE(read_res->covered_mutation_seq.has_value());
    CHECK(read_res->postings() == nullptr);
    CHECK(read_res->tree() == nullptr);
}

TEST_CASE("manifest: read_manifest returns not_found for missing file", "[motif-db][manifest]")
{
    auto const missing_path = std::filesystem::temp_directory_path() / "motif_manifest_does_not_exist_xyz.json";
    auto res = motif::db::read_manifest(missing_path);
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error() == motif::db::error_code::not_found);
}

TEST_CASE("manifest: cache equality is byte exact and sees any difference", "[motif-db][manifest]")
{
    auto manifest = motif::db::build_manifest(3, covered_seq, game_total, built_payload(covered_seq, game_total, /*with_tree=*/false));
    REQUIRE(manifest.has_value());
    auto newer = motif::db::build_manifest(4, covered_seq, game_total, built_payload(covered_seq, game_total, /*with_tree=*/false));
    REQUIRE(newer.has_value());

    tmp_file const tmp {"equality"};
    CHECK_FALSE(motif::db::manifest_cache_current(tmp.path, *manifest));  // absent
    REQUIRE(motif::db::write_manifest(tmp.path, *newer).has_value());
    CHECK_FALSE(motif::db::manifest_cache_current(tmp.path, *manifest));  // different generation
    CHECK(motif::db::manifest_cache_current(tmp.path, *newer));
    {
        std::ofstream truncated {tmp.path, std::ios::trunc};
        truncated << "{\"schema_version\":";
    }
    CHECK_FALSE(motif::db::manifest_cache_current(tmp.path, *newer));  // truncated
}

TEST_CASE("manifest: payload checksum covers the exact bytes", "[motif-db][manifest]")
{
    auto manifest = motif::db::build_manifest(1, covered_seq, game_total, built_payload(covered_seq, game_total, /*with_tree=*/true));
    REQUIRE(manifest.has_value());
    CHECK(manifest->payload_checksum == motif::db::checksum_to_hex(motif::db::fnv1a64(manifest->payload_json)));
    CHECK(manifest->payload_checksum.size() == 16U);

    // A single flipped byte fails closed, not silently.
    auto corrupt = manifest->payload_json;
    corrupt[corrupt.size() / 2] = corrupt[corrupt.size() / 2] == 'x' ? 'y' : 'x';
    auto const bad = motif::db::parse_manifest(1, covered_seq, game_total, 1, corrupt, manifest->payload_checksum);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error() == motif::db::error_code::io_failure);

    auto const good = motif::db::parse_manifest(1, covered_seq, game_total, 1, manifest->payload_json, manifest->payload_checksum);
    CHECK(good.has_value());
}

TEST_CASE("manifest: an unknown payload version is rejected as schema_mismatch", "[motif-db][manifest]")
{
    auto manifest = motif::db::build_manifest(1, covered_seq, game_total, built_payload(covered_seq, game_total, /*with_tree=*/false));
    REQUIRE(manifest.has_value());
    auto const future = parse_with_checksum(manifest->payload_json, 3);
    REQUIRE_FALSE(future.has_value());
    CHECK(future.error() == motif::db::error_code::schema_mismatch);
}

TEST_CASE("manifest: a v2-shaped payload with segment sets and patch runs is rejected, not truncated", "[motif-db][manifest]")
{
    auto manifest = motif::db::build_manifest(3, covered_seq, game_total, built_payload(covered_seq, game_total, /*with_tree=*/true));
    REQUIRE(manifest.has_value());

    // Nonempty tree patch runs belong to a later payload version.
    auto const with_patch_runs = forged(manifest->payload_json, R"("patch_runs":[])", R"("patch_runs":["run"])");
    auto const patched = parse_with_checksum(with_patch_runs);
    REQUIRE_FALSE(patched.has_value());
    CHECK(patched.error() == motif::db::error_code::schema_mismatch);

    // Unknown (v2) fields are an error under a known-version strict parser.
    auto const with_unknown = forged(manifest->payload_json, R"("bundle":{)", R"("retired_documents":[],"bundle":{)");
    CHECK_FALSE(parse_with_checksum(with_unknown).has_value());

    // Two segments: PR 5 accepts exactly one.
    auto two_segments = built_payload(covered_seq, game_total, /*with_tree=*/false);
    two_segments.segments.push_back(two_segments.segments.front());
    two_segments.segments.back().artifact = postings_artifact(artifact_id_b);
    auto encoded = motif::db::encode_payload(two_segments);
    REQUIRE(encoded.has_value());
    auto const multi = motif::db::decode_payload(1, *encoded);
    REQUIRE_FALSE(multi.has_value());
    CHECK(multi.error() == motif::db::error_code::schema_mismatch);

    // Unsupported index semantics.
    auto const semantics = forged(manifest->payload_json, "mainline_only", "all_branches");
    auto const unsupported = parse_with_checksum(semantics);
    REQUIRE_FALSE(unsupported.has_value());
    CHECK(unsupported.error() == motif::db::error_code::schema_mismatch);
}

TEST_CASE("manifest: artifact descriptors reject path escapes, duplicates and bad counters", "[motif-db][manifest]")
{
    auto manifest = motif::db::build_manifest(3, covered_seq, game_total, built_payload(covered_seq, game_total, /*with_tree=*/true));
    REQUIRE(manifest.has_value());
    auto const& json = manifest->payload_json;

    auto const traversal = forged(json, std::string {"positions.postings."} + artifact_id_a, std::string {"../victim"});
    CHECK_FALSE(parse_with_checksum(traversal).has_value());

    auto const nested =
        forged(json, std::string {"positions.postings."} + artifact_id_a, std::string {"sub/positions.postings."} + artifact_id_a);
    CHECK_FALSE(parse_with_checksum(nested).has_value());

    auto const wrong_prefix = forged(json, std::string {"positions.postings."} + artifact_id_a, std::string {"games.db.x"});
    CHECK_FALSE(parse_with_checksum(wrong_prefix).has_value());

    // Filename must match the descriptor id.
    auto const mismatched_id =
        forged(json, std::string {R"("id":")"} + artifact_id_a, std::string {R"("id":")"} + "ffffffffffffffffffffffffffffffff");
    CHECK_FALSE(parse_with_checksum(mismatched_id).has_value());

    // Counters travel as canonical decimal strings within int64.
    auto const overflow = forged(json, R"("size_bytes":"1085643669")", R"("size_bytes":"9223372036854775808")");
    CHECK_FALSE(parse_with_checksum(overflow).has_value());
    auto const leading_zero = forged(json, R"("size_bytes":"1085643669")", R"("size_bytes":"01085643669")");
    CHECK_FALSE(parse_with_checksum(leading_zero).has_value());
    auto const negative = forged(json, R"("size_bytes":"1085643669")", R"("size_bytes":"-1")");
    CHECK_FALSE(parse_with_checksum(negative).has_value());

    // Tree and postings must be distinct artifacts.
    auto shared = built_payload(covered_seq, game_total, /*with_tree=*/true);
    motif_test::require_value(shared.tree).checkpoint.id = artifact_id_a;
    motif_test::require_value(shared.tree).checkpoint.filename = std::string {motif::db::tree_artifact_prefix} + artifact_id_a;
    CHECK_FALSE(motif::db::build_manifest(1, 7, 10, shared).has_value());
    auto same_file = built_payload(covered_seq, game_total, /*with_tree=*/true);
    motif_test::require_value(same_file.tree).checkpoint = tree_artifact(artifact_id_a);
    motif_test::require_value(same_file.tree).checkpoint.filename = same_file.segments.front().artifact.filename;
    // A tree descriptor that points at the postings file is invalid; the typed
    // builder refuses it before it can reach SQLite.
    CHECK_FALSE(motif::db::build_manifest(1, 7, 10, same_file).has_value());
}

TEST_CASE("manifest: cross-field invariants reject inconsistent snapshots", "[motif-db][manifest]")
{
    // Null coverage but artifacts present.
    CHECK_FALSE(motif::db::build_manifest(1, std::nullopt, 10, built_payload(7, 10, /*with_tree=*/false)).has_value());
    // Coverage but no artifacts.
    CHECK_FALSE(motif::db::build_manifest(1, 7, 10, unbuilt_payload()).has_value());
    // Segment source sequence disagrees with the scalar coverage.
    CHECK_FALSE(motif::db::build_manifest(1, 8, 10, built_payload(7, 10, /*with_tree=*/false)).has_value());
    // Document count disagrees with the build-time canonical count.
    CHECK_FALSE(motif::db::build_manifest(1, 7, 11, built_payload(7, 10, /*with_tree=*/false)).has_value());
    // A tree may not claim coverage ahead of the postings.
    auto ahead = built_payload(covered_seq, game_total, /*with_tree=*/true);
    motif_test::require_value(ahead.tree).covered_mutation_seq = covered_seq + 1;
    motif_test::require_value(ahead.tree).base_mutation_seq = covered_seq + 1;
    CHECK_FALSE(motif::db::build_manifest(1, 7, 10, ahead).has_value());
    // Counters beyond SQLite's signed range.
    CHECK_FALSE(motif::db::build_manifest(1, 9'223'372'036'854'775'808ULL, 10, built_payload(7, 10, /*with_tree=*/false)).has_value());
    // The valid shapes pass.
    CHECK(motif::db::build_manifest(1, 7, 10, built_payload(7, 10, /*with_tree=*/true)).has_value());
    CHECK(motif::db::build_manifest(0, std::nullopt, 0, unbuilt_payload()).has_value());
}

TEST_CASE("manifest: a forged higher schema version in the cache is not accepted by the reader", "[motif-db][manifest]")
{
    auto manifest = motif::db::build_manifest(3, covered_seq, game_total, built_payload(covered_seq, game_total, /*with_tree=*/false));
    REQUIRE(manifest.has_value());
    tmp_file const tmp {"forged-schema"};
    manifest->schema_version = motif::db::schema::current_version + 1;
    REQUIRE(motif::db::write_manifest(tmp.path, *manifest).has_value());
    auto const read_res = motif::db::read_manifest(tmp.path);
    REQUIRE_FALSE(read_res.has_value());
    CHECK(read_res.error() == motif::db::error_code::schema_mismatch);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("manifest: write failure at each cache step reports the right state", "[motif-db][manifest]")
{
    using motif::db::fault::point;
    auto manifest = motif::db::build_manifest(3, covered_seq, game_total, built_payload(covered_seq, game_total, /*with_tree=*/false));
    REQUIRE(manifest.has_value());

    static point failing_point {};
    auto const hook = [](point const where, motif::db::fault::artifact_kind, std::uint64_t) noexcept -> motif::db::fault::fault_action
    { return where == failing_point ? motif::db::fault::fault_action::fail_io : motif::db::fault::fault_action::proceed; };

    struct expectation
    {
        point where;
        motif::db::manifest_write_state state;
        bool cache_replaced;
    };

    for (auto const& expected :
         {expectation {
              .where = point::before_manifest_write, .state = motif::db::manifest_write_state::not_published, .cache_replaced = false},
          expectation {
              .where = point::after_manifest_temp_write, .state = motif::db::manifest_write_state::not_published, .cache_replaced = false},
          expectation {
              .where = point::after_manifest_temp_fsync, .state = motif::db::manifest_write_state::not_published, .cache_replaced = false},
          expectation {
              .where = point::before_manifest_rename, .state = motif::db::manifest_write_state::not_published, .cache_replaced = false},
          expectation {.where = point::after_manifest_rename,
                       .state = motif::db::manifest_write_state::published_not_durable,
                       .cache_replaced = true},
          expectation {.where = point::before_manifest_directory_fsync,
                       .state = motif::db::manifest_write_state::published_not_durable,
                       .cache_replaced = true},
          expectation {.where = point::after_manifest_write,
                       .state = motif::db::manifest_write_state::published_not_durable,
                       .cache_replaced = true}})
    {
        tmp_file const tmp {"write-fault"};
        failing_point = expected.where;
        motif::db::fault::scoped_hook const scoped {hook};
        auto const result = motif::db::write_manifest(tmp.path, *manifest);
        CHECK(result.state == expected.state);
        CHECK(std::filesystem::exists(tmp.path) == expected.cache_replaced);
        CHECK_FALSE(std::filesystem::exists(tmp.path.string() + ".tmp"));
    }

    // With the seam disabled the full write is reported fully durable.
    tmp_file const tmp {"write-ok"};
    auto const written = motif::db::write_manifest(tmp.path, *manifest);
    CHECK(written.has_value());
}

TEST_CASE("manifest: the legacy parser reads pre-v7 fields and tolerates unknown ones", "[motif-db][manifest]")
{
    tmp_file const tmp {"legacy"};
    {
        std::ofstream output {tmp.path};
        output << R"json({"name":"old","schema_version":6,"game_count":5,"created_at":"2026-01-01T00:00:00Z",
              "position_index_dirty":true,"source_generation":2,"derived_index_build_seq":4,
              "position_postings":{"filename":"positions.postings.3","source_generation":2,"game_count":5,"file_size":10,"checksum":11},
              "opening_tree_index":null,"field_from_the_future":[1,2,3]})json";
    }
    auto const legacy = motif::db::read_legacy_manifest(tmp.path);
    REQUIRE(legacy.has_value());
    CHECK(legacy->name == "old");
    CHECK(legacy->schema_version == 6U);
    CHECK(legacy->created_at == "2026-01-01T00:00:00Z");
    CHECK(legacy->position_index_dirty);
    REQUIRE(legacy->position_postings.has_value());
    CHECK(motif_test::require_value(legacy->position_postings).filename == "positions.postings.3");

    auto const missing = motif::db::read_legacy_manifest(tmp.path.string() + ".absent");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error() == motif::db::error_code::not_found);

    {
        std::ofstream garbage {tmp.path, std::ios::trunc};
        garbage << "not json";
    }
    CHECK_FALSE(motif::db::read_legacy_manifest(tmp.path).has_value());
}

TEST_CASE("manifest: generated ids are 128-bit lower hex and distinct", "[motif-db][manifest]")
{
    auto const first = motif::db::generate_random_id();
    auto const second = motif::db::generate_random_id();
    CHECK(first.size() == 32U);
    CHECK(first != second);
    CHECK(first.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(motif::db::checksum_from_hex("00000000000000ff") == std::optional<std::uint64_t> {255});
    CHECK_FALSE(motif::db::checksum_from_hex("FF").has_value());
    CHECK_FALSE(motif::db::checksum_from_hex("00000000000000fg").has_value());
}

// ── Payload version 2 (PR 6a) ────────────────────────────────────────────────

namespace
{

constexpr std::uint64_t metadata_size_base = 1'000;
constexpr std::uint64_t metadata_checksum_base = 0xabcdef0000ULL;
constexpr std::uint64_t tree_run_size_base = 2'000;
constexpr std::uint64_t tree_run_checksum_base = 0x123456000000ULL;

auto hex_id(std::uint32_t const n) -> std::string
{
    auto text = std::to_string(n);
    return std::string(id_hex_digits - text.size(), '0') + text;
}

auto metadata_run_descriptor(std::uint32_t const n, std::uint64_t const from, std::uint64_t const to_seq) -> motif::db::patch_run_descriptor
{
    return {.artifact = {.id = hex_id(n),
                         .filename = std::string {motif::db::postings_metadata_artifact_prefix} + hex_id(n),
                         .format = std::string {motif::db::postings_metadata_format_name},
                         .format_version = motif::db::postings_metadata_format_version,
                         .size_bytes = metadata_size_base + n,
                         .checksum = metadata_checksum_base + n},
            .from_exclusive_seq = from,
            .to_inclusive_seq = to_seq};
}

auto tree_run_descriptor(std::uint32_t const n, std::uint64_t const from, std::uint64_t const to_seq) -> motif::db::patch_run_descriptor
{
    return {.artifact = {.id = hex_id(n),
                         .filename = std::string {motif::db::tree_patch_artifact_prefix} + hex_id(n),
                         .format = std::string {motif::db::tree_patch_format_name},
                         .format_version = motif::db::tree_patch_format_version,
                         .size_bytes = tree_run_size_base + n,
                         .checksum = tree_run_checksum_base + n},
            .from_exclusive_seq = from,
            .to_inclusive_seq = to_seq};
}

// Postings baseline at sequence 4; metadata chain: checkpoint (4,6], runs (6,7] (7,9];
// tree base 5 with runs (5,8] (8,9]; everything covers sequence 9.
auto chain_payload() -> motif::db::snapshot_payload
{
    // NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers) -- interval endpoints and run ids are spelled out as the
    // comment above lays them out.
    auto payload = built_payload(4, game_total, /*with_tree=*/true);
    payload.metadata.checkpoint = metadata_run_descriptor(101, 4, 6);
    payload.metadata.runs = {metadata_run_descriptor(102, 6, 7), metadata_run_descriptor(103, 7, 9)};
    auto& tree = motif_test::require_value(payload.tree);
    tree.checkpoint.format_version = motif::db::opening_tree_index_format_version;
    tree.base_mutation_seq = 5;
    tree.covered_mutation_seq = 9;
    tree.patch_runs = {tree_run_descriptor(201, 5, 8), tree_run_descriptor(202, 8, 9)};
    // NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
    return payload;
}

}  // namespace

TEST_CASE("manifest: a payload with chains round-trips as version 2 and keeps every descriptor", "[motif-db][manifest][pr6a]")
{
    auto manifest = motif::db::build_manifest(chain_generation, chain_seq, game_total, chain_payload());
    REQUIRE(manifest.has_value());
    CHECK(manifest->payload_version == motif::db::snapshot_payload_version_2);
    CHECK(manifest->payload_json.contains(R"("patch_runs")"));
    CHECK(manifest->payload_json.contains(R"("from_exclusive_seq":"6")"));  // counters are decimal strings
    CHECK(manifest->payload_json.contains(R"("child_frequency_mode":"checkpoint")"));

    auto const parsed =
        motif::db::parse_manifest(chain_generation, chain_seq, game_total, 2, manifest->payload_json, manifest->payload_checksum);
    REQUIRE(parsed.has_value());
    CHECK(parsed->payload.metadata == chain_payload().metadata);
    CHECK(parsed->payload.tree == chain_payload().tree);
    CHECK(parsed->payload.segments == chain_payload().segments);
    CHECK(motif::db::referenced_artifacts(parsed->payload).size() == 1U + 3U + 1U + 2U);
    CHECK(motif::db::referenced_artifact_filenames(parsed->payload).size() == 7U);

    // The cache envelope carries the payload version and re-reads identically.
    tmp_file const tmp {"v2-cache"};
    REQUIRE(motif::db::write_manifest(tmp.path, *manifest).has_value());
    auto const reread = motif::db::read_manifest(tmp.path);
    REQUIRE(reread.has_value());
    CHECK(reread->payload_version == 2U);
    CHECK(reread->payload.tree == chain_payload().tree);

    // child_frequency_mode survives.
    auto postings_mode = chain_payload();
    motif_test::require_value(postings_mode.tree).child_frequency_mode = motif::db::tree_child_frequency_mode::postings;
    auto const postings_manifest = motif::db::build_manifest(chain_generation, chain_seq, game_total, postings_mode);
    REQUIRE(postings_manifest.has_value());
    auto const postings_parsed = motif::db::parse_manifest(
        chain_generation, chain_seq, game_total, 2, postings_manifest->payload_json, postings_manifest->payload_checksum);
    REQUIRE(postings_parsed.has_value());
    CHECK(motif_test::require_value(postings_parsed->payload.tree).child_frequency_mode == motif::db::tree_child_frequency_mode::postings);

    // Both decoders are strict about which shape they accept.
    CHECK_FALSE(motif::db::parse_manifest(5, 9, 10, 1, manifest->payload_json, manifest->payload_checksum).has_value());  // v2 JSON as v1
    auto const v1_manifest =
        motif::db::build_manifest(chain_generation, covered_seq, game_total, built_payload(covered_seq, game_total, /*with_tree=*/true));
    REQUIRE(v1_manifest.has_value());
    CHECK(v1_manifest->payload_version == 1U);
    CHECK(parse_with_checksum(v1_manifest->payload_json, 1).has_value());  // control
    CHECK_FALSE(parse_with_checksum(v1_manifest->payload_json, 2).has_value());  // v1 JSON as v2
}

TEST_CASE("manifest: writers emit version 2 only when the payload needs it", "[motif-db][manifest][pr6a]")
{
    CHECK(motif::db::required_payload_version(unbuilt_payload()) == 1U);
    CHECK(motif::db::required_payload_version(built_payload(7, 10, /*with_tree=*/false)) == 1U);
    CHECK(motif::db::required_payload_version(built_payload(7, 10, /*with_tree=*/true)) == 1U);  // v5 checkpoint, no chain

    auto v6_tree = built_payload(covered_seq, game_total, /*with_tree=*/true);
    motif_test::require_value(v6_tree.tree).checkpoint.format_version = motif::db::opening_tree_index_format_version;
    CHECK(motif::db::required_payload_version(v6_tree) == 2U);
    auto manifest = motif::db::build_manifest(1, covered_seq, game_total, v6_tree);
    REQUIRE(manifest.has_value());
    CHECK(manifest->payload_version == 2U);
    CHECK(parse_with_checksum(manifest->payload_json, 2).has_value());

    constexpr auto metadata_only_baseline = std::uint64_t {5};
    constexpr auto metadata_run_id = 11U;
    auto metadata_only = built_payload(metadata_only_baseline, game_total, /*with_tree=*/false);
    metadata_only.metadata.runs = {metadata_run_descriptor(metadata_run_id, metadata_only_baseline, covered_seq)};
    CHECK(motif::db::required_payload_version(metadata_only) == 2U);
    auto metadata_manifest = motif::db::build_manifest(1, covered_seq, game_total, metadata_only);
    REQUIRE(metadata_manifest.has_value());
    CHECK(metadata_manifest->payload_version == 2U);

    // A v1 payload stays byte-for-byte what PR 5 wrote (no new keys).
    auto const v1_manifest =
        motif::db::build_manifest(1, covered_seq, game_total, built_payload(covered_seq, game_total, /*with_tree=*/true));
    REQUIRE(v1_manifest.has_value());
    CHECK_FALSE(v1_manifest->payload_json.contains(R"("metadata")"));
    CHECK_FALSE(v1_manifest->payload_json.contains("child_frequency_mode"));

    // A v1 decoder never sees a v6 tree descriptor: v1 JSON names only version 5.
    auto forged_v6 = forged(v1_manifest->payload_json, R"("format_version":5)", R"("format_version":6)");
    CHECK_FALSE(parse_with_checksum(forged_v6, 1).has_value());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("manifest: chain intervals must be unique, contiguous, ordered and reach coverage", "[motif-db][manifest][pr6a]")
{
    auto const builds = [](motif::db::snapshot_payload const& payload, std::uint64_t const covered = chain_seq) -> bool
    { return motif::db::build_manifest(1, covered, game_total, payload).has_value(); };
    REQUIRE(builds(chain_payload()));

    auto mutated = [](auto const& mutate) -> motif::db::snapshot_payload
    {
        auto payload = chain_payload();
        mutate(payload);
        return payload;
    };
    // Metadata chain.
    CHECK_FALSE(
        builds(mutated([](auto& target) -> void { target.metadata.runs[0].from_exclusive_seq = 5; })));  // gap/overlap with the checkpoint
    CHECK_FALSE(builds(mutated([](auto& target) -> void { target.metadata.runs[1].from_exclusive_seq = 8; })));  // gap between runs
    CHECK_FALSE(builds(mutated([](auto& target) -> void { target.metadata.runs[1].to_inclusive_seq = 8; })));  // does not reach coverage
    CHECK_FALSE(builds(mutated([](auto& target) -> void { target.metadata.runs[1].to_inclusive_seq = 10; })));  // beyond coverage
    CHECK_FALSE(builds(mutated([](auto& target) -> void { target.metadata.checkpoint->from_exclusive_seq = 3; })));  // not at the baseline
    CHECK_FALSE(builds(mutated([](auto& target) -> void { target.metadata.runs[0].to_inclusive_seq = 6; })));  // empty interval
    CHECK_FALSE(
        builds(mutated([](auto& target) -> void { std::swap(target.metadata.runs[0], target.metadata.runs[1]); })));  // reversed order
    CHECK_FALSE(builds(
        mutated([](auto& target) -> void { target.metadata.runs.push_back(target.metadata.runs.back()); })));  // duplicate / overlapping
    CHECK_FALSE(
        builds(mutated([](auto& target) -> void { target.metadata.checkpoint.reset(); })));  // first run no longer starts at the baseline
    CHECK_FALSE(builds(mutated([](auto& target) -> void { target.segments[0].source_mutation_seq = 10; })));  // baseline beyond coverage
    // Tree chain.
    CHECK_FALSE(
        builds(mutated([](auto& target) -> void { target.tree->patch_runs[0].from_exclusive_seq = 4; })));  // not at the checkpoint base
    CHECK_FALSE(
        builds(mutated([](auto& target) -> void { target.tree->patch_runs[1].from_exclusive_seq = 7; })));  // overlap with the previous run
    CHECK_FALSE(
        builds(mutated([](auto& target) -> void { target.tree->patch_runs[1].to_inclusive_seq = 10; })));  // ends past the tree coverage
    CHECK_FALSE(
        builds(mutated([](auto& target) -> void { target.tree->covered_mutation_seq = 10; })));  // tree coverage beyond the snapshot
    CHECK_FALSE(builds(mutated([](auto& target) -> void { target.tree->base_mutation_seq = 10; })));  // base after coverage
    CHECK_FALSE(builds(mutated([](auto& target) -> void { target.tree->patch_runs.clear(); })));  // base != covered without runs
    // Uniqueness across the whole payload.
    CHECK_FALSE(builds(mutated(
        [](auto& target) -> void
        {
            target.tree->patch_runs[0] = tree_run_descriptor(201, 5, 8);
            target.tree->patch_runs[1] = tree_run_descriptor(201, 8, 9);
        })));
    CHECK_FALSE(
        builds(mutated([](auto& target) -> void { target.metadata.runs[0].artifact.id = target.metadata.checkpoint->artifact.id; })));
    CHECK_FALSE(builds(mutated([](auto& target) -> void { target.tree->checkpoint.id = target.segments[0].artifact.id; })));
    CHECK_FALSE(builds(
        mutated([](auto& target) -> void { target.metadata.runs[0].artifact.filename = target.metadata.runs[1].artifact.filename; })));
    // A pre-v2 shape that needs v2 but claims coverage mismatches the documents.
    CHECK_FALSE(motif::db::build_manifest(1, 9, 11, chain_payload()).has_value());  // document count != canonical count
    // Counters are bounded by INT64_MAX.
    CHECK_FALSE(builds(
        mutated([](auto& target) -> void { target.metadata.runs[1].to_inclusive_seq = std::numeric_limits<std::uint64_t>::max(); })));

    // Decoder bound on chain length.
    constexpr auto first_extra_run_id = 1000U;
    constexpr auto long_chain_baseline = std::uint64_t {4};
    auto long_chain = built_payload(4, game_total, /*with_tree=*/false);
    for (std::uint32_t index = 0; index < motif::db::max_patch_chain_runs + 1; ++index) {
        long_chain.metadata.runs.push_back(
            metadata_run_descriptor(first_extra_run_id + index, long_chain_baseline + index, long_chain_baseline + 1U + index));
    }
    auto const covered_long = 4 + motif::db::max_patch_chain_runs + 1;
    CHECK_FALSE(motif::db::build_manifest(1, covered_long, 10, long_chain).has_value());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("manifest: v2 descriptors reject unknown prefixes, formats, versions, fields and path escapes", "[motif-db][manifest][pr6a]")
{
    auto manifest = motif::db::build_manifest(chain_generation, chain_seq, game_total, chain_payload());
    REQUIRE(manifest.has_value());
    auto const& json = manifest->payload_json;
    auto const parse_at_coverage = [](std::string const& text) -> motif::db::result<motif::db::db_manifest>
    {
        return motif::db::parse_manifest(
            chain_generation, chain_seq, game_total, 2, text, motif::db::checksum_to_hex(motif::db::fnv1a64(text)));
    };
    // Control: the unforged payload parses at its own coverage, so every rejection
    // below is caused by the forgery and nothing else.
    REQUIRE(parse_at_coverage(json).has_value());
    auto const rejects = [&](std::string const& from, std::string const& replacement) -> bool
    { return !parse_at_coverage(forged(json, from, replacement)).has_value(); };

    // Filename must be exactly prefix + id.
    CHECK(rejects(std::string {"\"positions.metadata."} + hex_id(102),
                  std::string {"\"positions.postings."} + hex_id(102)));  // wrong family prefix
    CHECK(rejects(std::string {"\"opening_tree.patch."} + hex_id(201), "\"opening_tree.patch.keepme"));  // look-alike
    CHECK(
        rejects(std::string {"\"positions.metadata."} + hex_id(103), std::string {"\"../positions.metadata."} + hex_id(103)));  // traversal
    CHECK(rejects(std::string {"\"opening_tree.patch."} + hex_id(202),
                  std::string {"\"opening_tree.patch./"} + hex_id(202)));  // path component
    // Formats and versions of runs.
    CHECK(rejects("\"format\":\"motif-postings-metadata\"", "\"format\":\"motif-postings\""));
    CHECK(rejects("\"format\":\"motif-opening-tree-patch\"", "\"format\":\"motif-opening-tree\""));
    CHECK(rejects("\"format\":\"motif-opening-tree-patch\",\"format_version\":1",
                  "\"format\":\"motif-opening-tree-patch\",\"format_version\":2"));
    // Unknown fields and values are errors, never ignored.
    CHECK(rejects("\"patch_runs\":[{", "\"unknown\":1,\"patch_runs\":[{"));
    CHECK(rejects("\"child_frequency_mode\":\"checkpoint\"", "\"child_frequency_mode\":\"approximate\""));
    CHECK(rejects("\"from_exclusive_seq\":\"6\"", "\"from_exclusive_seq\":\"06\""));  // non-canonical counter
    CHECK(rejects("\"from_exclusive_seq\":\"6\"", "\"from_exclusive_seq\":6"));  // not a string
    CHECK(rejects("\"from_exclusive_seq\":\"6\"", "\"from_exclusive_seq\":\"-1\""));
    CHECK(rejects("\"completeness_policy\":\"standard_start_all_occurrences_else_horizon_v1\"", "\"completeness_policy\":\"other\""));
    // Tree checkpoints may be v5 or v6, nothing else.
    CHECK(rejects("\"format\":\"motif-opening-tree\",\"format_version\":6", "\"format\":\"motif-opening-tree\",\"format_version\":4"));
    CHECK(rejects("\"format\":\"motif-opening-tree\",\"format_version\":6", "\"format\":\"motif-opening-tree\",\"format_version\":7"));
    CHECK_FALSE(
        rejects("\"format\":\"motif-opening-tree\",\"format_version\":6", "\"format\":\"motif-opening-tree\",\"format_version\":5"));

    // A v1 payload still decodes only with its v5 checkpoint.
    auto const v1_manifest =
        motif::db::build_manifest(1, covered_seq, game_total, built_payload(covered_seq, game_total, /*with_tree=*/true));
    REQUIRE(v1_manifest.has_value());
    CHECK(parse_with_checksum(v1_manifest->payload_json, 1).has_value());
}

TEST_CASE("manifest: a v1 payload decodes to the normalized form with empty chains", "[motif-db][manifest][pr6a]")
{
    auto const v1_manifest =
        motif::db::build_manifest(3, covered_seq, game_total, built_payload(covered_seq, game_total, /*with_tree=*/true));
    REQUIRE(v1_manifest.has_value());
    auto const parsed = parse_with_checksum(v1_manifest->payload_json, 1);
    REQUIRE(parsed.has_value());
    CHECK(parsed->payload_version == 1U);
    CHECK(parsed->payload.metadata.empty());
    REQUIRE(parsed->tree() != nullptr);
    CHECK(parsed->tree()->patch_runs.empty());
    CHECK(parsed->tree()->base_mutation_seq == parsed->tree()->covered_mutation_seq);
    CHECK(parsed->tree()->child_frequency_mode == motif::db::tree_child_frequency_mode::checkpoint);
    CHECK(parsed->tree()->checkpoint.format_version == motif::db::opening_tree_index_legacy_format_version);
    // v1 keeps its old coverage rules: the segment's source sequence IS the coverage.
    auto payload = built_payload(covered_seq - 1, game_total, /*with_tree=*/false);
    CHECK_FALSE(motif::db::build_manifest(1, 7, 10, payload).has_value());
}
