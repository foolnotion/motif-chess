// PR 6a: the postings metadata artifact (run / sparse checkpoint) and the
// postings reader's overlay / prepare-install API.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "motif/db/postings_metadata_patch.hpp"

#include <catch2/catch_test_macros.hpp>

#include "composite_test_support.hpp"
#include "motif/db/database_manager.hpp"
#include "motif/db/manifest.hpp"
#include "motif/db/opening_tree_stats.hpp"
#include "motif/db/position_postings.hpp"

namespace
{

using namespace motif::db;  // NOLINT(google-build-using-namespace) -- the file exercises most of motif::db; qualifying every name buries
                            // the assertions.
using motif_test::temp_dir;
// Header: magic (8) + format version (4) + header length (4). Record: id (4), revision (8), result (1), flags (1), white bits (2), black
// bits (2).
constexpr auto header_prefix_bytes = std::size_t {16};
constexpr auto record_result_offset = std::size_t {12};
constexpr auto record_flags_offset = std::size_t {13};
constexpr auto record_black_bits_offset = std::size_t {14};
constexpr auto sample_to_seq = 5;
constexpr auto fixture_corpus_size = std::size_t {12};
// Sample record fields: a mid-range rating plus both ends of the signed 16-bit domain, and a document id above 16 bits.
constexpr auto ordinary_rating = std::int16_t {2300};
constexpr auto lowest_rating = std::numeric_limits<std::int16_t>::min();
constexpr auto highest_rating = std::numeric_limits<std::int16_t>::max();
constexpr auto black_win_document = 5U;
constexpr auto draw_document = 9U;
constexpr auto wide_document = 70'000U;

auto test_identity() -> metadata_artifact_identity
{
    return metadata_artifact_identity {.bundle_id = motif_test::require_value(identity_from_hex("fedcba9876543210fedcba9876543210")),
                                       .index_semantics = index_semantics_version};
}

auto image(std::uint32_t const document,
           std::int8_t const result,
           std::optional<std::int16_t> const white = std::nullopt,
           std::optional<std::int16_t> const black = std::nullopt) -> position_metadata_after_image
{
    return position_metadata_after_image {
        .id = game_id {document}, .content_revision = 1, .result = result, .white_elo = white, .black_elo = black};
}

auto sample_artifact(metadata_artifact_kind const kind = metadata_artifact_kind::run) -> metadata_artifact
{
    return metadata_artifact {.identity = test_identity(),
                              .kind = kind,
                              .from_exclusive_seq = 4,
                              .to_inclusive_seq = sample_to_seq,
                              .records = {image(2, 1, ordinary_rating, -1),
                                          image(black_win_document, -1),
                                          image(draw_document, 0, std::nullopt, lowest_rating),
                                          image(wide_document, 1, highest_rating, 0)},
                              .encoded_bytes = 0};
}

auto expectation_of(metadata_artifact const& artifact) -> metadata_artifact_expectation
{
    return metadata_artifact_expectation {.identity = artifact.identity,
                                          .kind = artifact.kind,
                                          .from_exclusive_seq = artifact.from_exclusive_seq,
                                          .to_inclusive_seq = artifact.to_inclusive_seq};
}

// The documented byte layout, written independently and without validation.
auto reference_encode(metadata_artifact const& artifact, std::optional<std::uint64_t> const count_override = std::nullopt) -> std::string
{
    auto tail = std::string {};
    append_fixed_le(tail, artifact.identity.index_semantics);
    for (auto const byte : artifact.identity.bundle_id) {
        tail.push_back(static_cast<char>(byte));
    }
    append_fixed_le(tail, artifact.from_exclusive_seq);
    append_fixed_le(tail, artifact.to_inclusive_seq);
    append_fixed_le(tail, static_cast<std::uint32_t>(artifact.kind));
    append_fixed_le(tail, count_override.value_or(static_cast<std::uint64_t>(artifact.records.size())));
    auto out = std::string {"MOTIFPM1"};
    append_fixed_le(out, std::uint32_t {1});
    append_fixed_le(out, static_cast<std::uint32_t>(header_prefix_bytes + tail.size()));
    out += tail;
    for (auto const& record : artifact.records) {
        append_fixed_le(out, record.id.value);
        append_fixed_le(out, record.content_revision);
        append_fixed_le(out, static_cast<std::uint8_t>(record.result + 1));
        append_fixed_le(out, static_cast<std::uint8_t>((record.white_elo ? 1U : 0U) | (record.black_elo ? 2U : 0U)));
        append_fixed_le(out, static_cast<std::uint16_t>(record.white_elo.value_or(0)));
        append_fixed_le(out, static_cast<std::uint16_t>(record.black_elo.value_or(0)));
    }
    return out;
}

}  // namespace

TEST_CASE("a metadata artifact uses 18-byte records in exactly the documented layout", "[motif-db][pr6a][metadata]")
{
    auto const artifact = sample_artifact();
    auto const encoded = encode_metadata_artifact(artifact);
    REQUIRE(encoded.has_value());
    CHECK(*encoded == reference_encode(artifact));
    CHECK(encoded->substr(0, 8) == "MOTIFPM1");
    auto const header_length = static_cast<std::size_t>(static_cast<std::uint8_t>((*encoded)[12]));
    CHECK(encoded->size() == header_length + (artifact.records.size() * 18U));
    CHECK(metadata_record_bytes == 18U);

    // Pin a record's bytes: id 5, revision 1, black win (0), no ratings, zero payload bits.
    auto const second = encoded->substr(header_length + 18, 18);
    CHECK(second
          == std::string {'\x05',
                          '\x00',
                          '\x00',
                          '\x00',
                          '\x01',
                          '\x00',
                          '\x00',
                          '\x00',
                          '\x00',
                          '\x00',
                          '\x00',
                          '\x00',
                          '\x00',
                          '\x00',
                          '\x00',
                          '\x00',
                          '\x00',
                          '\x00'});
    // Negative ratings are two's-complement i16 bit patterns: -32768 -> 0x8000.
    auto const third = encoded->substr(header_length + 36, 18);
    CHECK(static_cast<std::uint8_t>(third[13]) == 2U);  // black flag only
    CHECK(static_cast<std::uint8_t>(third[16]) == 0x00U);
    CHECK(static_cast<std::uint8_t>(third[17]) == 0x80U);

    auto const decoded = decode_metadata_artifact(*encoded, expectation_of(artifact));
    REQUIRE(decoded.has_value());
    CHECK(decoded->records == artifact.records);
    CHECK(decoded->kind == metadata_artifact_kind::run);
    CHECK(decoded->encoded_bytes == encoded->size());
    CHECK(decoded->decoded_bytes() >= decoded->records.size() * sizeof(position_metadata_after_image));

    // A checkpoint differs only in its kind field, which the expectation must match.
    auto checkpoint = artifact;
    checkpoint.kind = metadata_artifact_kind::checkpoint;
    auto const checkpoint_bytes = *encode_metadata_artifact(checkpoint);
    CHECK(checkpoint_bytes != *encoded);
    CHECK(decode_metadata_artifact(checkpoint_bytes, expectation_of(checkpoint)).has_value());
    CHECK_FALSE(decode_metadata_artifact(checkpoint_bytes, expectation_of(artifact)).has_value());
    CHECK_FALSE(decode_metadata_artifact(*encoded, expectation_of(checkpoint)).has_value());

    // An empty artifact is valid.
    auto empty = artifact;
    empty.records.clear();
    CHECK(decode_metadata_artifact(*encode_metadata_artifact(empty), expectation_of(empty)).has_value());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("every truncation, trailing byte and header disagreement of a metadata artifact is rejected", "[motif-db][pr6a][metadata]")
{
    auto const artifact = sample_artifact();
    auto const bytes = *encode_metadata_artifact(artifact);
    auto const expected = expectation_of(artifact);
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        CHECK_FALSE(decode_metadata_artifact(std::string_view {bytes}.substr(0, length), expected).has_value());
    }
    CHECK_FALSE(decode_metadata_artifact(bytes + '\x00', expected).has_value());

    // Offsets: version 8, header_length 12, semantics 16, bundle 20, from 36, to 44, kind 52, count 56.
    auto const flipped = [&](std::size_t const offset, char const value) -> std::string
    {
        auto copy = bytes;
        copy[offset] = value;
        return copy;
    };
    CHECK_FALSE(decode_metadata_artifact(flipped(0, 'X'), expected).has_value());
    CHECK_FALSE(decode_metadata_artifact(flipped(8, '\x02'), expected).has_value());
    CHECK_FALSE(decode_metadata_artifact(flipped(12, '\x41'), expected).has_value());
    CHECK_FALSE(decode_metadata_artifact(flipped(16, '\x02'), expected).has_value());
    CHECK_FALSE(decode_metadata_artifact(flipped(20, '\x00'), expected).has_value());
    CHECK_FALSE(decode_metadata_artifact(flipped(36, '\x07'), expected).has_value());
    CHECK_FALSE(decode_metadata_artifact(flipped(44, '\x07'), expected).has_value());
    CHECK_FALSE(decode_metadata_artifact(flipped(52, '\x03'), expected).has_value());

    // The record count must match the bytes present exactly (lies up and down).
    CHECK_FALSE(decode_metadata_artifact(reference_encode(artifact, artifact.records.size() + 1), expected).has_value());
    CHECK_FALSE(decode_metadata_artifact(reference_encode(artifact, artifact.records.size() - 1), expected).has_value());
    CHECK_FALSE(decode_metadata_artifact(reference_encode(artifact, 0x1fffffffffffffffULL), expected).has_value());

    // A different bundle or interval is refused.
    auto other = expected;
    other.identity.bundle_id.back() ^= 0x01U;
    CHECK_FALSE(decode_metadata_artifact(bytes, other).has_value());
    other = expected;
    other.to_inclusive_seq = std::uint64_t {sample_to_seq} + 1U;
    CHECK_FALSE(decode_metadata_artifact(bytes, other).has_value());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("malformed metadata records are rejected by the decoder and the encoder", "[motif-db][pr6a][metadata]")
{
    auto const good = sample_artifact();
    auto const rejected = [&](auto const& mutate) -> bool
    {
        auto artifact = good;
        mutate(artifact);
        return !encode_metadata_artifact(artifact).has_value()
            && !decode_metadata_artifact(reference_encode(artifact), expectation_of(artifact)).has_value();
    };
    CHECK_FALSE(rejected([](metadata_artifact& /*artifact*/) -> void {}));
    CHECK(rejected([](metadata_artifact& target) -> void { target.records[0].id = game_id {0}; }));
    CHECK(rejected([](metadata_artifact& target) -> void { target.records[1].content_revision = 2; }));  // PR 6: revision 1 only
    CHECK(rejected([](metadata_artifact& target) -> void { target.records[1].content_revision = 0; }));
    CHECK(rejected([](metadata_artifact& target) -> void { target.records[0].result = 2; }));
    CHECK(rejected([](metadata_artifact& target) -> void { target.records[0].result = -2; }));
    CHECK(rejected([](metadata_artifact& target) -> void { std::swap(target.records[0], target.records[1]); }));  // unsorted
    CHECK(rejected([](metadata_artifact& target) -> void { target.records[1].id = target.records[0].id; }));  // duplicate key
    CHECK(rejected([](metadata_artifact& target) -> void { target.from_exclusive_seq = target.to_inclusive_seq; }));
    CHECK(rejected([](metadata_artifact& target) -> void { target.to_inclusive_seq = std::numeric_limits<std::uint64_t>::max(); }));

    // Decoder-only: raw byte forgery the encoder cannot produce.
    auto bytes = reference_encode(good);
    auto const header_length = static_cast<std::size_t>(static_cast<std::uint8_t>(bytes[12]));
    auto forged = bytes;
    forged[header_length + record_result_offset] = '\x03';  // result byte 3
    CHECK_FALSE(decode_metadata_artifact(forged, expectation_of(good)).has_value());
    forged = bytes;
    forged[header_length + record_flags_offset] = '\x04';  // unknown flag bit
    CHECK_FALSE(decode_metadata_artifact(forged, expectation_of(good)).has_value());
    forged = bytes;
    forged[header_length + metadata_record_bytes + record_black_bits_offset] =
        '\x01';  // second record: rating bits set while the flag is clear
    CHECK_FALSE(decode_metadata_artifact(forged, expectation_of(good)).has_value());
}

TEST_CASE("loading a metadata artifact verifies its descriptor", "[motif-db][pr6a][metadata]")
{
    temp_dir const dir {"metadata-load"};
    auto const artifact = sample_artifact();
    auto const path = dir.path / "positions.metadata.fedcba9876543210fedcba9876543210";
    REQUIRE(write_metadata_artifact(path, artifact).has_value());
    auto const bytes = *encode_metadata_artifact(artifact);
    auto const descriptor = artifact_descriptor {.id = "fedcba9876543210fedcba9876543210",
                                                 .filename = path.filename().string(),
                                                 .format = std::string {postings_metadata_format_name},
                                                 .format_version = postings_metadata_format_version,
                                                 .size_bytes = bytes.size(),
                                                 .checksum = fnv1a64(bytes)};
    CHECK(load_metadata_artifact(path, descriptor, expectation_of(artifact)).has_value());
    auto wrong = descriptor;
    wrong.checksum ^= 1U;
    CHECK_FALSE(load_metadata_artifact(path, wrong, expectation_of(artifact)).has_value());
    wrong = descriptor;
    wrong.size_bytes -= 1;
    CHECK_FALSE(load_metadata_artifact(path, wrong, expectation_of(artifact)).has_value());
    CHECK_FALSE(load_metadata_artifact(dir.path / "absent", descriptor, expectation_of(artifact)).has_value());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("the metadata chain keeps the last after-image per document and folds into a sparse checkpoint",
          "[motif-db][pr6a][metadata][fold]")
{
    auto const identity = test_identity();
    auto make = [&](std::uint64_t const from,
                    std::uint64_t const to_seq,
                    metadata_artifact_kind const kind,
                    std::vector<position_metadata_after_image> records) -> metadata_artifact
    {
        return metadata_artifact {.identity = identity,
                                  .kind = kind,
                                  .from_exclusive_seq = from,
                                  .to_inclusive_seq = to_seq,
                                  .records = std::move(records),
                                  .encoded_bytes = 0};
    };
    auto const checkpoint = make(3, 6, metadata_artifact_kind::checkpoint, {image(1, 1), image(4, 0, 100, 200), image(8, -1)});
    auto const run_a = make(6, 7, metadata_artifact_kind::run, {image(4, 1, 101, std::nullopt), image(9, 1)});
    auto const run_b = make(7, 8, metadata_artifact_kind::run, {image(1, -1, -5, -6), image(4, -1, std::nullopt, std::nullopt)});

    auto const chain = std::array<metadata_artifact const*, 3> {&checkpoint, &run_a, &run_b};
    auto const merged = merge_metadata_records(chain);
    REQUIRE(merged.has_value());
    // Sorted, unique, latest wins (game 4: run_b; game 1: run_b; 8 and 9 survive).
    REQUIRE(merged->size() == 4U);
    CHECK((*merged)[0] == image(1, -1, -5, -6));
    CHECK((*merged)[1] == image(4, -1, std::nullopt, std::nullopt));
    CHECK((*merged)[2] == image(8, -1));
    CHECK((*merged)[3] == image(9, 1));

    // Folding yields one checkpoint over (baseline, last.to] with the same content.
    auto const folded = fold_metadata_chain(identity, 3, chain);
    REQUIRE(folded.has_value());
    CHECK(folded->kind == metadata_artifact_kind::checkpoint);
    CHECK(folded->from_exclusive_seq == 3U);
    CHECK(folded->to_inclusive_seq == 8U);
    CHECK(folded->records == *merged);
    CHECK(folded->encoded_bytes == encode_metadata_artifact(*folded)->size());
    // Folding the fold with a further run equals folding everything at once.
    auto const run_c = make(8, 9, metadata_artifact_kind::run, {image(8, 1, 7, 7), image(12, 0)});
    auto const refolded = std::array<metadata_artifact const*, 2> {&*folded, &run_c};
    auto const again = fold_metadata_chain(identity, 3, refolded);
    auto const all = std::array<metadata_artifact const*, 4> {&checkpoint, &run_a, &run_b, &run_c};
    auto const at_once = fold_metadata_chain(identity, 3, all);
    REQUIRE(again.has_value());
    REQUIRE(at_once.has_value());
    CHECK(again->records == at_once->records);
    CHECK(again->to_inclusive_seq == 9U);

    CHECK_FALSE(fold_metadata_chain(identity, 3, std::span<metadata_artifact const* const> {}).has_value());

    // Repeated edits to one game leave one record; editing every game bounds the
    // checkpoint at the document count.
    auto repeated = std::vector<metadata_artifact> {};
    constexpr auto epoch_count = std::uint64_t {12};
    constexpr auto repeated_document = 77U;
    for (auto epoch = std::uint64_t {0}; epoch < epoch_count; ++epoch) {
        repeated.push_back(
            make(epoch,
                 epoch + 1,
                 metadata_artifact_kind::run,
                 {image(repeated_document, static_cast<std::int8_t>(static_cast<int>(epoch % 3U) - 1), static_cast<std::int16_t>(epoch))}));
    }
    auto pointers = std::vector<metadata_artifact const*> {};
    for (auto const& artifact : repeated) {
        pointers.push_back(&artifact);
    }
    auto const one_game = fold_metadata_chain(identity, 0, pointers);
    REQUIRE(one_game.has_value());
    CHECK(one_game->records.size() == 1U);
    CHECK(one_game->records[0] == image(repeated_document, 1, 11));

    auto everything = std::vector<position_metadata_after_image> {};
    constexpr auto document_count = 500U;
    for (std::uint32_t document = 1; document <= document_count; ++document) {
        everything.push_back(image(document, static_cast<std::int8_t>(static_cast<int>(document % 3U) - 1)));
    }
    auto const all_games = make(0, 1, metadata_artifact_kind::run, everything);
    auto const all_games_again = make(1, 2, metadata_artifact_kind::run, everything);
    auto const two_epochs = std::array<metadata_artifact const*, 2> {&all_games, &all_games_again};
    CHECK(fold_metadata_chain(identity, 0, two_epochs)->records.size() == 500U);
}

// ── The postings reader's overlay API ────────────────────────────────────────

namespace
{

struct postings_fixture
{
    temp_dir dir {"overlay"};
    std::vector<motif_test::model_game> corpus;
    std::optional<position_postings> reader;

    explicit postings_fixture(std::uint64_t const seed)
        : corpus {motif_test::make_corpus(seed, fixture_corpus_size)}
    {
        auto manager = motif_test::build_bundle(dir.path / "db", corpus, /*with_tree=*/false);
        auto const* segment = manager.manifest().postings();
        REQUIRE(segment != nullptr);
        reader.emplace(dir.path / "db" / segment->artifact.filename);
        REQUIRE(reader->open().has_value());
    }
};

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("prepare/install replaces one document's metadata without touching postings", "[motif-db][pr6a][metadata][overlay]")
{
    static_assert(noexcept(std::declval<position_postings&>().install_metadata_update(std::declval<position_metadata_slot const&>())),
                  "install must be noexcept: it runs after the durable commit");
    constexpr auto update_seed = 301;
    postings_fixture fixture {update_seed};
    auto& reader = motif_test::require_value(fixture.reader);
    // A position every game reaches: the start.
    auto const root = zobrist_hash {motif_test::start_hash()};
    auto const before = reader.occurrences(root);
    REQUIRE(before.has_value());
    REQUIRE(before->size() >= fixture.corpus.size());
    auto const summary_before = reader.summary(root);

    auto const target = game_id {3};
    auto const baseline = reader.metadata_for(target);
    REQUIRE(baseline.has_value());
    CHECK(motif_test::require_value(baseline).result == encode_result_code(fixture.corpus[2].result));
    CHECK(motif_test::require_value(baseline).white_elo == std::optional<std::int16_t> {2300});

    // prepare validates and changes nothing.
    auto const slot = reader.prepare_metadata_update(image(3, -1, std::nullopt, -123));
    REQUIRE(slot.has_value());
    CHECK(reader.metadata_for(target) == baseline);
    CHECK(motif_test::same_matches(reader.occurrences(root).value(), *before));

    // Unknown document, unsupported revision, out-of-range result: rejected by prepare.
    CHECK_FALSE(reader.prepare_metadata_update(image(9'999, 0)).has_value());
    CHECK(reader.prepare_metadata_update(image(9'999, 0)).error() == error_code::not_found);
    auto stale_revision = image(3, 0);
    stale_revision.content_revision = 2;
    CHECK_FALSE(reader.prepare_metadata_update(stale_revision).has_value());
    CHECK_FALSE(reader.prepare_metadata_update(image(3, 2)).has_value());
    CHECK_FALSE(reader.prepare_metadata_update(image(0, 0)).has_value());

    // install assigns scalars: every match of that game now shows the after-image,
    // nothing else changes.
    reader.install_metadata_update(*slot);
    auto const after = reader.occurrences(root);
    REQUIRE(after.has_value());
    REQUIRE(after->size() == before->size());
    for (std::size_t index = 0; index < after->size(); ++index) {
        auto const& old = (*before)[index];
        auto const& now = (*after)[index];
        CHECK(old.game_id == now.game_id);
        CHECK(old.ply == now.ply);
        if (now.game_id == target) {
            CHECK(now.result == -1);
            CHECK_FALSE(now.white_elo.has_value());
            CHECK(now.black_elo == std::optional<std::int16_t> {-123});
        } else {
            CHECK(old.result == now.result);
            CHECK(old.white_elo == now.white_elo);
            CHECK(old.black_elo == now.black_elo);
        }
    }
    auto const summary_after = reader.summary(root);
    REQUIRE(summary_after.has_value());
    REQUIRE(summary_after->has_value());
    REQUIRE(summary_before.has_value());
    REQUIRE(summary_before->has_value());
    auto const& after_summary = motif_test::require_value(*summary_after);
    auto const& before_summary = motif_test::require_value(*summary_before);
    CHECK(after_summary.occurrence_count == before_summary.occurrence_count);  // membership unchanged
    CHECK(after_summary.distinct_game_count == before_summary.distinct_game_count);
    CHECK(after_summary.min_ply == before_summary.min_ply);
    CHECK(after_summary.max_ply == before_summary.max_ply);
    auto const first = reader.first_occurrences(root, std::array {target});
    REQUIRE(first.has_value());
    REQUIRE(first->size() == 1U);
    CHECK(first->front().result == -1);

    // A slot prepared earlier stays valid across later installs of other documents.
    auto const slot_a = reader.prepare_metadata_update(image(1, 1, 10, 20));
    auto const slot_b = reader.prepare_metadata_update(image(5, -1, -10, -20));
    REQUIRE(slot_a.has_value());
    REQUIRE(slot_b.has_value());
    reader.install_metadata_update(*slot_b);
    reader.install_metadata_update(*slot_a);
    CHECK(reader.metadata_for(game_id {1}) == std::optional<position_metadata_after_image> {image(1, 1, 10, 20)});
    CHECK(reader.metadata_for(game_id {5}) == std::optional<position_metadata_after_image> {image(5, -1, -10, -20)});
    CHECK_FALSE(reader.metadata_for(game_id {12'345}).has_value());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("apply_metadata_overlay materializes a chain all-or-nothing", "[motif-db][pr6a][metadata][overlay]")
{
    constexpr auto overlay_seed = 302;
    postings_fixture fixture {overlay_seed};
    auto& reader = motif_test::require_value(fixture.reader);
    auto const snapshot = [&]() -> std::vector<position_metadata_after_image>
    {
        auto all = std::vector<position_metadata_after_image> {};
        reader.for_each_metadata([&all](position_metadata_after_image const& item) -> void { all.push_back(item); });
        return all;
    };
    auto const baseline = snapshot();
    REQUIRE(baseline.size() == fixture.corpus.size());
    for (std::size_t index = 0; index + 1 < baseline.size(); ++index) {
        REQUIRE(baseline[index].id < baseline[index + 1].id);  // ascending ids
    }

    // A chain with an unknown document at the end applies nothing, not even its valid prefix.
    auto const bad = std::vector<position_metadata_after_image> {image(2, 1, 5, 6), image(4, -1), image(100'000, 0)};
    CHECK_FALSE(reader.apply_metadata_overlay(bad).has_value());
    CHECK(snapshot() == baseline);
    // Unsorted, duplicated, wrong-revision and out-of-range records are all refused.
    CHECK_FALSE(reader.apply_metadata_overlay(std::vector {image(4, 0), image(2, 0)}).has_value());
    CHECK_FALSE(reader.apply_metadata_overlay(std::vector {image(2, 0), image(2, 1)}).has_value());
    auto revision_two = image(2, 0);
    revision_two.content_revision = 2;
    CHECK_FALSE(reader.apply_metadata_overlay(std::vector {revision_two}).has_value());
    CHECK_FALSE(reader.apply_metadata_overlay(std::vector {image(2, 5)}).has_value());
    CHECK(snapshot() == baseline);

    // A valid sparse chain lands on exactly its documents.
    auto const good = std::vector<position_metadata_after_image> {image(2, 1, 5, 6), image(4, -1), image(9, 0, -32768, 32767)};
    REQUIRE(reader.apply_metadata_overlay(good).has_value());
    auto const after = snapshot();
    for (std::size_t index = 0; index < after.size(); ++index) {
        auto const found = std::ranges::find(good, after[index].id, &position_metadata_after_image::id);
        CHECK(after[index] == (found != good.end() ? *found : baseline[index]));
    }
    // An empty chain is a no-op.
    REQUIRE(reader.apply_metadata_overlay({}).has_value());
    CHECK(snapshot() == after);
    // A record that returns to its baseline value is accepted.
    REQUIRE(reader.apply_metadata_overlay(std::vector {baseline[1]}).has_value());
    CHECK(reader.metadata_for(baseline[1].id) == std::optional<position_metadata_after_image> {baseline[1]});
}
