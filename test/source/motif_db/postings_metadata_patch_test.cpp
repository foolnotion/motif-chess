// PR 6a: the postings metadata artifact (run / sparse checkpoint) and the
// postings reader's overlay / prepare-install API.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <random>
#include <set>
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

// ── Retired documents (removed games) ────────────────────────────────────────

namespace
{

auto retired_image(std::uint32_t const document) -> position_metadata_after_image
{
    auto retired = image(document, 0);
    retired.retired = true;
    return retired;
}

// The positions of one game by ply, from the model: hashes[p] is the position after p moves.
auto position_hashes(motif_test::model_game const& game) -> std::vector<std::uint64_t>
{
    auto hashes = std::vector<std::uint64_t> {};
    for (std::size_t ply = 0; ply <= game.moves.size(); ++ply) {
        hashes.push_back(motif_test::hash_after(game.moves, ply));
    }
    return hashes;
}

// What the postings of the games that are not retired hold, computed from the model alone.
struct live_model
{
    std::vector<std::vector<std::uint64_t>> hashes;  // by document - 1
    std::set<std::uint32_t> retired;

    explicit live_model(std::vector<motif_test::model_game> const& corpus)
    {
        for (auto const& game : corpus) {
            hashes.push_back(position_hashes(game));
        }
    }

    // Every (document, ply) occurrence of `hash` in a live game, ascending.
    [[nodiscard]] auto occurrences(std::uint64_t const hash) const -> std::vector<std::pair<std::uint32_t, std::uint16_t>>
    {
        auto found = std::vector<std::pair<std::uint32_t, std::uint16_t>> {};
        for (std::size_t index = 0; index < hashes.size(); ++index) {
            auto const document = static_cast<std::uint32_t>(index + 1U);
            if (retired.contains(document)) {
                continue;
            }
            for (std::size_t ply = 0; ply < hashes[index].size(); ++ply) {
                if (hashes[index][ply] == hash) {
                    found.emplace_back(document, static_cast<std::uint16_t>(ply));
                }
            }
        }
        return found;
    }

    // The extrema of the edge root -> child over live games other than `excluded`.
    [[nodiscard]] auto edge(std::uint64_t const root,
                            // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- a root and a child hash; every call site names both.
                            std::uint64_t const child,
                            std::uint32_t const excluded,
                            std::uint16_t const horizon,
                            bool const any_ply) const -> std::optional<position_edge_extrema>
    {
        auto found = std::optional<position_edge_extrema> {};
        for (std::size_t index = 0; index < hashes.size(); ++index) {
            auto const document = static_cast<std::uint32_t>(index + 1U);
            if (retired.contains(document) || document == excluded) {
                continue;
            }
            auto best = std::optional<std::uint16_t> {};
            for (std::size_t ply = 0; ply + 1U < hashes[index].size(); ++ply) {
                if (hashes[index][ply] == root && hashes[index][ply + 1U] == child && (any_ply || ply <= horizon)) {
                    best = static_cast<std::uint16_t>(ply);
                    break;
                }
            }
            if (!best) {
                continue;
            }
            if (!found) {
                found = position_edge_extrema {.min_root_ply = *best, .sample_min = game_id {document}, .sample_max = game_id {document}};
            } else {
                found->min_root_ply = std::min(found->min_root_ply, *best);
                found->sample_max = game_id {document};
            }
        }
        return found;
    }
};

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear comparison.
void require_summary_matches(position_postings const& reader, live_model const& model, std::uint64_t const hash)
{
    INFO("hash " << hash);
    auto const expected = model.occurrences(hash);
    auto const summary = reader.summary(zobrist_hash {hash});
    REQUIRE(summary.has_value());
    if (expected.empty()) {
        CHECK_FALSE(summary->has_value());
        return;
    }
    REQUIRE(summary->has_value());
    auto distinct = std::set<std::uint32_t> {};
    auto lowest = expected.front().second;
    auto highest = expected.front().second;
    for (auto const& [document, ply] : expected) {
        distinct.insert(document);
        lowest = std::min(lowest, ply);
        highest = std::max(highest, ply);
    }
    // NOLINTBEGIN(bugprone-unchecked-optional-access) -- REQUIREd just above
    CHECK((*summary)->occurrence_count == expected.size());
    CHECK((*summary)->distinct_game_count == distinct.size());
    CHECK((*summary)->min_ply == lowest);
    CHECK((*summary)->max_ply == highest);
    // NOLINTEND(bugprone-unchecked-optional-access)
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("retired documents vanish from every reader, pages stay bounded and summaries follow the retired set",
          "[motif-db][pr7][metadata][retired]")
{
    constexpr auto retired_seed = 303;
    postings_fixture fixture {retired_seed};
    auto& reader = motif_test::require_value(fixture.reader);
    auto model = live_model {fixture.corpus};
    auto const start = motif_test::start_hash();
    auto const all_hashes = [&]() -> std::vector<std::uint64_t>
    {
        auto distinct = std::set<std::uint64_t> {};
        for (auto const& game_hashes : model.hashes) {
            distinct.insert(game_hashes.begin(), game_hashes.end());
        }
        return {distinct.begin(), distinct.end()};
    }();

    // Nothing retired yet: the directory answers.
    CHECK(reader.retired_game_count() == 0U);
    CHECK(reader.indexed_game_count() == fixture.corpus.size());
    for (auto const hash : all_hashes) {
        require_summary_matches(reader, model, hash);
    }

    // Each retirement changes the summaries the reader already gave once (a remembered answer must not survive it).
    for (auto const document : {5U, 2U, 9U}) {
        auto const slot = reader.prepare_metadata_update(retired_image(document));
        REQUIRE(slot.has_value());
        reader.install_metadata_update(*slot);
        model.retired.insert(document);
        CHECK(reader.retired_game_count() == model.retired.size());
        CHECK(reader.indexed_game_count() == fixture.corpus.size() - model.retired.size());
        CHECK_FALSE(reader.metadata_for(game_id {document}).has_value());
        for (auto const hash : all_hashes) {
            require_summary_matches(reader, model, hash);
            require_summary_matches(reader, model, hash);  // the remembered answer is the same one
        }
        // A retired document can be neither edited nor retired again.
        CHECK_FALSE(reader.prepare_metadata_update(image(document, 1)).has_value());
        CHECK_FALSE(reader.prepare_metadata_update(retired_image(document)).has_value());
    }

    // Every page of live occurrences equals the same window of the live model, however the window is cut.
    for (auto const hash : {start, all_hashes[all_hashes.size() / 2], all_hashes.back()}) {
        auto const expected = model.occurrences(hash);
        for (auto const limit : {std::size_t {0}, std::size_t {1}, std::size_t {2}, std::size_t {5}, std::size_t {1'000}}) {
            for (auto const offset : {std::size_t {0}, std::size_t {1}, std::size_t {3}, std::size_t {7}, std::size_t {10'000}}) {
                INFO("hash " << hash << " limit " << limit << " offset " << offset);
                auto const page = reader.occurrences(zobrist_hash {hash}, limit, offset);
                REQUIRE(page.has_value());
                auto const begin = std::min(offset, expected.size());
                auto const count = limit == 0U ? expected.size() - begin : std::min(limit, expected.size() - begin);
                REQUIRE(page->size() == count);
                for (std::size_t index = 0; index < count; ++index) {
                    CHECK((*page)[index].game_id.value == expected[begin + index].first);
                    CHECK((*page)[index].ply == expected[begin + index].second);
                }
                // A bounded page holds a bounded allocation, however large the block.
                if (limit != 0U) {
                    CHECK(page->capacity() <= limit);
                }
            }
        }
    }

    // The distinct games, the first occurrences and the metadata listing agree on who is live.
    auto const ids = reader.distinct_game_ids(zobrist_hash {start});
    REQUIRE(ids.has_value());
    CHECK(ids->size() == fixture.corpus.size() - model.retired.size());
    for (auto const live_id : *ids) {
        CHECK_FALSE(model.retired.contains(live_id.value));
    }
    auto const requested = std::vector<game_id> {game_id {1}, game_id {2}, game_id {5}, game_id {9}, game_id {10}};
    auto const first = reader.first_occurrences(zobrist_hash {start}, requested);
    REQUIRE(first.has_value());
    CHECK(first->size() == 2U);  // documents 1 and 10: 2, 5 and 9 are retired
    auto listed = std::size_t {0};
    reader.for_each_metadata([&listed](position_metadata_after_image const&) -> void { ++listed; });
    CHECK(listed == fixture.corpus.size() - model.retired.size());

    // The streamed summaries are the same live summaries.
    auto streamed = std::size_t {0};
    REQUIRE(reader
                .for_each_summary(
                    [&](zobrist_hash const hash, position_postings_summary const& summary) -> result<void>
                    {
                        ++streamed;
                        auto const expected = model.occurrences(hash.value);
                        CHECK(summary.occurrence_count == expected.size());
                        return {};
                    })
                .has_value());
    CHECK(streamed <= all_hashes.size());
    CHECK(streamed > 0U);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("an edge scan finds the extrema of the live games that reach the edge, and stops as soon as the wanted ones are settled",
          "[motif-db][pr7][metadata][retired][edge]")
{
    constexpr auto scan_seed = 304;
    constexpr auto horizon = std::uint16_t {20};
    postings_fixture fixture {scan_seed};
    auto& reader = motif_test::require_value(fixture.reader);
    auto model = live_model {fixture.corpus};
    // Retire two documents through the overlay (the open-time path), so the scan has to skip them.
    REQUIRE(reader.apply_metadata_overlay(std::vector {retired_image(3), retired_image(7)}).has_value());
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers) -- arbitrary retired document ids
    model.retired = {3U, 7U};

    auto checked = std::size_t {0};
    auto found_edges = std::size_t {0};
    for (std::size_t index = 0; index < fixture.corpus.size(); ++index) {
        auto const& hashes = model.hashes[index];
        for (std::size_t ply = 0; ply + 1U < hashes.size(); ++ply) {
            auto const root = hashes[ply];
            auto const child = hashes[ply + 1U];
            for (auto const excluded : {0U, static_cast<std::uint32_t>(index + 1U)}) {
                for (auto const any_ply : {false, true}) {
                    auto const expected = model.edge(root, child, excluded, horizon, any_ply);
                    auto scan = position_edge_scan {.root = zobrist_hash {root},
                                                    .child = zobrist_hash {child},
                                                    .excluded = game_id {excluded},
                                                    .max_root_ply = horizon,
                                                    .any_root_ply = any_ply,
                                                    .want_min_ply = true,
                                                    .min_ply_floor = 0,
                                                    .want_sample_min = true,
                                                    .want_sample_max = true};
                    auto const all = reader.edge_extrema(scan);
                    REQUIRE(all.has_value());
                    REQUIRE(all->has_value() == expected.has_value());
                    ++checked;
                    if (!expected) {
                        continue;
                    }
                    ++found_edges;
                    // NOLINTBEGIN(bugprone-unchecked-optional-access) -- REQUIREd just above each access
                    CHECK((*all)->min_root_ply == expected->min_root_ply);
                    CHECK((*all)->sample_min == expected->sample_min);
                    CHECK((*all)->sample_max == expected->sample_max);

                    // Each early exit returns exactly the field it was asked for. The floor and the ceiling are what a caller
                    // knows from the corpus WITH the excluded game: a lower bound of the minimum and an upper bound of the ids.
                    auto const with_all = model.edge(root, child, 0U, horizon, any_ply);
                    REQUIRE(with_all.has_value());
                    auto only_min = scan;
                    only_min.want_sample_min = false;
                    only_min.want_sample_max = false;
                    only_min.min_ply_floor = with_all->min_root_ply;
                    auto const min_result = reader.edge_extrema(only_min);
                    REQUIRE(min_result.has_value());
                    REQUIRE(min_result->has_value());
                    CHECK((*min_result)->min_root_ply == expected->min_root_ply);
                    auto only_low = scan;
                    only_low.want_min_ply = false;
                    only_low.want_sample_max = false;
                    auto const low_result = reader.edge_extrema(only_low);
                    REQUIRE(low_result.has_value());
                    REQUIRE(low_result->has_value());
                    CHECK((*low_result)->sample_min == expected->sample_min);
                    auto only_high = scan;
                    only_high.want_min_ply = false;
                    only_high.want_sample_min = false;
                    only_high.sample_ceiling = with_all->sample_max;
                    auto const high_result = reader.edge_extrema(only_high);
                    REQUIRE(high_result.has_value());
                    REQUIRE(high_result->has_value());
                    CHECK((*high_result)->sample_max == expected->sample_max);
                    // NOLINTEND(bugprone-unchecked-optional-access)
                }
            }
        }
    }
    CHECK(checked > 0U);
    CHECK(found_edges > 0U);

    // An unknown root or child reaches no edge; so does a pair no game plays.
    auto const unknown = reader.edge_extrema(position_edge_scan {.root = zobrist_hash {1}, .child = zobrist_hash {2}});
    REQUIRE(unknown.has_value());
    CHECK_FALSE(unknown->has_value());
    auto const never = reader.edge_extrema(
        position_edge_scan {.root = zobrist_hash {model.hashes[1][1]}, .child = zobrist_hash {model.hashes[1][0]}, .any_root_ply = true});
    REQUIRE(never.has_value());
    CHECK_FALSE(never->has_value());
}

// ── Live windows over hand-built postings, and corruption an early exit must not hide ──

namespace
{

struct placement
{
    std::uint64_t hash;
    std::uint32_t game;
    std::uint16_t ply;
};

auto write_artifact(std::filesystem::path const& path, std::vector<placement> const& placements) -> void
{
    auto rows = std::vector<position_row> {};
    for (auto const& item : placements) {
        rows.push_back(position_row {.zobrist_hash = zobrist_hash {item.hash},
                                     .game_id = game_id {item.game},
                                     .ply = item.ply,
                                     .encoded_move = 0,
                                     .result = 0,
                                     .white_elo = std::nullopt,
                                     .black_elo = std::nullopt});
    }
    auto builder = position_postings {path};
    REQUIRE(builder.append(rows).has_value());
    REQUIRE(builder.finalize().has_value());
}

// Opens the artifact and retires `retired` (ascending ids) the way an open-time overlay does.
auto open_retiring(std::filesystem::path const& path, std::set<std::uint32_t> const& retired) -> position_postings
{
    auto reader = position_postings {path};
    REQUIRE(reader.open().has_value());
    auto images = std::vector<position_metadata_after_image> {};
    for (auto const document : retired) {
        images.push_back(retired_image(document));
    }
    REQUIRE(reader.apply_metadata_overlay(images).has_value());
    return reader;
}

auto live_placements(std::vector<placement> const& placements, std::uint64_t const hash, std::set<std::uint32_t> const& retired)
    -> std::vector<std::pair<std::uint32_t, std::uint16_t>>
{
    auto live = std::vector<std::pair<std::uint32_t, std::uint16_t>> {};
    for (auto const& item : placements) {
        if (item.hash == hash && !retired.contains(item.game)) {
            live.emplace_back(item.game, item.ply);
        }
    }
    std::ranges::sort(live);
    return live;
}

auto distinct_games(std::vector<std::pair<std::uint32_t, std::uint16_t>> const& occurrences) -> std::vector<std::uint32_t>
{
    auto games = std::vector<std::uint32_t> {};
    for (auto const& occurrence : occurrences) {
        if (games.empty() || games.back() != occurrence.first) {
            games.push_back(occurrence.first);
        }
    }
    return games;
}

template<typename T>
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- a page size and an offset; every call site names both.
auto window_of(std::vector<T> const& all, std::size_t const limit, std::size_t const offset) -> std::vector<T>
{
    auto const begin = std::min(offset, all.size());
    auto const count = limit == 0U ? all.size() - begin : std::min(limit, all.size() - begin);
    return {all.begin() + static_cast<std::ptrdiff_t>(begin), all.begin() + static_cast<std::ptrdiff_t>(begin + count)};
}

auto as_pairs(std::vector<position_match> const& matches) -> std::vector<std::pair<std::uint32_t, std::uint16_t>>
{
    auto pairs = std::vector<std::pair<std::uint32_t, std::uint16_t>> {};
    for (auto const& match : matches) {
        pairs.emplace_back(match.game_id.value, match.ply);
    }
    return pairs;
}

auto as_numbers(std::vector<game_id> const& ids) -> std::vector<std::uint32_t>
{
    auto numbers = std::vector<std::uint32_t> {};
    for (auto const identifier : ids) {
        numbers.push_back(identifier.value);
    }
    return numbers;
}

constexpr auto crowded_hash = std::uint64_t {0x77};

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this exhaustive scenario.
TEST_CASE("live windows count only live occurrences and live games, for every limit and offset",
          "[motif-db][pr7][metadata][retired][pagination]")
{
    // Repeated positions inside a game (games 2, 4, 5), a single-occurrence game between them (3), games at both ends (1, 6).
    auto const placements = std::vector<placement> {{.hash = crowded_hash, .game = 1, .ply = 0},
                                                    {.hash = crowded_hash, .game = 2, .ply = 0},
                                                    {.hash = crowded_hash, .game = 2, .ply = 4},
                                                    {.hash = crowded_hash, .game = 2, .ply = 8},
                                                    {.hash = crowded_hash, .game = 3, .ply = 2},
                                                    {.hash = crowded_hash, .game = 4, .ply = 0},
                                                    {.hash = crowded_hash, .game = 4, .ply = 4},
                                                    {.hash = crowded_hash, .game = 5, .ply = 1},
                                                    {.hash = crowded_hash, .game = 5, .ply = 3},
                                                    {.hash = crowded_hash, .game = 5, .ply = 5},
                                                    {.hash = crowded_hash, .game = 6, .ply = 7},
                                                    {.hash = 0x88, .game = 1, .ply = 1},
                                                    {.hash = 0x88, .game = 2, .ply = 1},
                                                    {.hash = 0x88, .game = 3, .ply = 1},
                                                    {.hash = 0x88, .game = 4, .ply = 1},
                                                    {.hash = 0x88, .game = 5, .ply = 1},
                                                    {.hash = 0x88, .game = 6, .ply = 1}};
    temp_dir const dir {"live_windows"};
    auto const path = dir.path / "postings.idx";
    write_artifact(path, placements);

    auto const retired_sets =
        std::vector<std::set<std::uint32_t>> {{}, {1}, {2}, {6}, {1, 3}, {2, 4}, {1, 2}, {3, 4, 5}, {1, 2, 3, 4, 5}, {1, 2, 3, 4, 5, 6}};
    for (auto const& retired : retired_sets) {
        auto const reader = open_retiring(path, retired);
        auto const expected = live_placements(placements, crowded_hash, retired);
        auto const expected_games = distinct_games(expected);
        INFO("retired " << retired.size() << " documents, first " << (retired.empty() ? 0U : *retired.begin()));
        // Past the end of the live list on both axes, so every boundary (empty page, exact fit, overshoot) is met.
        for (std::size_t limit = 0; limit <= expected.size() + 2U; ++limit) {
            for (std::size_t offset = 0; offset <= expected.size() + 2U; ++offset) {
                INFO("limit " << limit << " offset " << offset);
                auto const page = reader.occurrences(zobrist_hash {crowded_hash}, limit, offset);
                REQUIRE(page.has_value());
                CHECK(as_pairs(*page) == window_of(expected, limit, offset));
                auto const ids = reader.distinct_game_ids(zobrist_hash {crowded_hash}, limit, offset);
                REQUIRE(ids.has_value());
                CHECK(as_numbers(*ids) == window_of(expected_games, limit, offset));
            }
        }
        auto const everything = reader.distinct_game_ids(zobrist_hash {crowded_hash});
        REQUIRE(everything.has_value());
        CHECK(as_numbers(*everything) == expected_games);
    }

    // The shape a reviewer might fear: a retired document first, then a live one. Offset 1 skips the one live
    // occurrence, so it lists nothing; the retired document does not push the live one into the page.
    auto const reader = open_retiring(path, {1});
    auto const only_live_first = reader.distinct_game_ids(zobrist_hash {crowded_hash}, 1, 0);
    REQUIRE(only_live_first.has_value());
    CHECK(as_numbers(*only_live_first) == std::vector<std::uint32_t> {2});
    auto const skipping_it = reader.occurrences(zobrist_hash {crowded_hash}, 2, 1);
    REQUIRE(skipping_it.has_value());
    CHECK(as_pairs(*skipping_it) == std::vector<std::pair<std::uint32_t, std::uint16_t>> {{2, 4}, {2, 8}});
    auto const after_retired_pair = open_retiring(path, {1, 2}).occurrences(zobrist_hash {crowded_hash}, 0, 1);
    REQUIRE(after_retired_pair.has_value());
    CHECK(as_pairs(*after_retired_pair).front() == std::pair<std::uint32_t, std::uint16_t> {4, 0});

    // Every size_t value is a valid limit and offset.
    constexpr auto largest = std::numeric_limits<std::size_t>::max();
    auto const huge = reader.distinct_game_ids(zobrist_hash {crowded_hash}, largest, 1);
    REQUIRE(huge.has_value());
    CHECK(as_numbers(*huge) == window_of(distinct_games(live_placements(placements, crowded_hash, {1})), 0, 1));
    auto const beyond = reader.distinct_game_ids(zobrist_hash {crowded_hash}, 1, largest);
    REQUIRE(beyond.has_value());
    CHECK(beyond->empty());
    // A hash no document reaches has no pages.
    auto const absent = reader.distinct_game_ids(zobrist_hash {0x1234}, 5, 0);
    REQUIRE(absent.has_value());
    CHECK(absent->empty());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("pages of a large block stitch back into the live list", "[motif-db][pr7][metadata][retired][pagination]")
{
    constexpr auto games = std::uint32_t {2'000};
    auto placements = std::vector<placement> {};
    for (auto document = std::uint32_t {1}; document <= games; ++document) {
        placements.push_back({.hash = crowded_hash, .game = document, .ply = 0});
        if (document % 3U == 0U) {
            placements.push_back({.hash = crowded_hash, .game = document, .ply = 2});
            placements.push_back({.hash = crowded_hash, .game = document, .ply = 4});
        }
    }
    constexpr auto retired_stride = std::uint32_t {7};
    auto retired = std::set<std::uint32_t> {1, games};
    for (auto document = retired_stride; document <= games; document += retired_stride) {
        retired.insert(document);
    }
    temp_dir const dir {"stitched_pages"};
    auto const path = dir.path / "postings.idx";
    write_artifact(path, placements);
    auto const reader = open_retiring(path, retired);
    auto const expected = live_placements(placements, crowded_hash, retired);
    auto const expected_games = distinct_games(expected);

    auto stitched_matches = std::vector<std::pair<std::uint32_t, std::uint16_t>> {};
    constexpr auto match_page = std::size_t {64};
    for (auto offset = std::size_t {0};; offset += match_page) {
        auto const page = reader.occurrences(zobrist_hash {crowded_hash}, match_page, offset);
        REQUIRE(page.has_value());
        REQUIRE(page->size() <= match_page);
        auto const rows = as_pairs(*page);
        stitched_matches.insert(stitched_matches.end(), rows.begin(), rows.end());
        if (page->size() < match_page) {
            break;
        }
    }
    CHECK(stitched_matches == expected);

    auto stitched_games = std::vector<std::uint32_t> {};
    constexpr auto game_page = std::size_t {50};
    for (auto offset = std::size_t {0};; offset += game_page) {
        auto const page = reader.distinct_game_ids(zobrist_hash {crowded_hash}, game_page, offset);
        REQUIRE(page.has_value());
        REQUIRE(page->size() <= game_page);
        auto const ids = as_numbers(*page);
        stitched_games.insert(stitched_games.end(), ids.begin(), ids.end());
        if (page->size() < game_page) {
            break;
        }
    }
    CHECK(stitched_games == expected_games);
    auto const summary = reader.summary(zobrist_hash {crowded_hash});
    REQUIRE(summary.has_value());
    REQUIRE(summary->has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access) -- REQUIREd just above
    CHECK((*summary)->distinct_game_count == expected_games.size());
    CHECK((*summary)->occurrence_count == expected.size());
    // NOLINTEND(bugprone-unchecked-optional-access)
}

namespace
{

constexpr auto edge_root = std::uint64_t {0xA};
constexpr auto edge_child = std::uint64_t {0xB};
constexpr auto posting_start = std::size_t {88};  // the posting blocks follow the fixed header, ascending by hash

auto read_bytes(std::filesystem::path const& path) -> std::vector<char>
{
    auto input = std::ifstream {path, std::ios::binary};
    return {std::istreambuf_iterator<char> {input}, std::istreambuf_iterator<char> {}};
}

void write_bytes(std::filesystem::path const& path, std::vector<char> const& bytes)
{
    auto output = std::ofstream {path, std::ios::binary | std::ios::trunc};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// The scan of root -> child that ends in the middle of the root block (the first match settles the smallest id)...
auto settles_early() -> position_edge_scan
{
    return position_edge_scan {
        .root = zobrist_hash {edge_root}, .child = zobrist_hash {edge_child}, .any_root_ply = true, .want_sample_min = true};
}

// ...and the scan that runs until the child block is exhausted, leaving the tail of the root block unread.
auto runs_until_child_ends() -> position_edge_scan
{
    return position_edge_scan {
        .root = zobrist_hash {edge_root}, .child = zobrist_hash {edge_child}, .any_root_ply = true, .want_sample_max = true};
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this table-driven scenario.
TEST_CASE("an edge scan that ends early still rejects a corrupt posting block, in the part it never read",
          "[motif-db][pr7][metadata][retired][edge][corruption]")
{
    // Root at ply 0 in games 1-4, child at ply 1 in games 1-2 only: the join has settled or run out of child games
    // long before it reaches games 3 and 4 of the root block.
    auto placements = std::vector<placement> {};
    for (auto document = std::uint32_t {1}; document <= 4U; ++document) {
        placements.push_back({.hash = edge_root, .game = document, .ply = 0});
    }
    placements.push_back({.hash = edge_child, .game = 1, .ply = 1});
    placements.push_back({.hash = edge_child, .game = 2, .ply = 1});
    temp_dir const dir {"edge_corruption"};
    auto const path = dir.path / "postings.idx";
    write_artifact(path, placements);
    auto const pristine = read_bytes(path);

    // Block layout: per game (id delta, ply count, first ply), root block first (12 bytes), then the child block (6 bytes).
    auto const expected_layout = std::vector<unsigned char> {1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1};
    REQUIRE(pristine.size() > posting_start + expected_layout.size());
    auto const same_byte = [](unsigned char const want, char const got) -> bool { return want == static_cast<unsigned char>(got); };
    REQUIRE(std::equal(
        expected_layout.begin(), expected_layout.end(), pristine.begin() + static_cast<std::ptrdiff_t>(posting_start), same_byte));

    using scan_result = result<std::optional<position_edge_extrema>>;
    auto const scan_all = [&](std::vector<char> const& bytes) -> std::pair<scan_result, scan_result>
    {
        write_bytes(path, bytes);
        auto reader = position_postings {path};
        REQUIRE(reader.open().has_value());  // the directory is intact: only a posting block is damaged
        return {reader.edge_extrema(settles_early()), reader.edge_extrema(runs_until_child_ends())};
    };

    SECTION("an intact artifact answers both scans")
    {
        // NOLINTBEGIN(bugprone-unchecked-optional-access) -- REQUIREd just above each access
        auto const [early, until_end] = scan_all(pristine);
        REQUIRE(early.has_value());
        REQUIRE(early->has_value());
        CHECK((*early)->sample_min == game_id {1});
        REQUIRE(until_end.has_value());
        REQUIRE(until_end->has_value());
        CHECK((*until_end)->sample_max == game_id {2});
        // NOLINTEND(bugprone-unchecked-optional-access)
    }

    struct damage
    {
        char const* name;
        std::size_t offset;  // from the first posting byte
        char value;
    };

    // Each damage sits in a group neither scan decodes before it stops.
    auto const damages = std::vector<damage> {
        {.name = "a ply outside the entry's envelope in the last root group", .offset = 11, .value = 5},
        {.name = "a ply count that runs past the end of the root block", .offset = 10, .value = 2},
        {.name = "a repeated game id in a middle root group", .offset = 6, .value = 0},
        {.name = "a ply outside the entry's envelope in the last child group", .offset = 17, .value = 2},
    };
    for (auto const& item : damages) {
        SECTION(item.name)
        {
            auto damaged = pristine;
            damaged.at(posting_start + item.offset) = item.value;
            auto const [early, until_end] = scan_all(damaged);
            REQUIRE_FALSE(early.has_value());
            CHECK(early.error() == error_code::schema_mismatch);
            REQUIRE_FALSE(until_end.has_value());
            CHECK(until_end.error() == error_code::schema_mismatch);
        }
    }
}
