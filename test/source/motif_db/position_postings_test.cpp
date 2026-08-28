#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "motif/db/position_postings.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chesslib/board/board.hpp>
#include <chesslib/board/move_codec.hpp>
#include <chesslib/util/san.hpp>

#include "motif/db/database_manager.hpp"
#include "motif/db/error.hpp"

namespace
{

class temporary_file
{
  public:
    temporary_file()
    {
        // Reserve a directory atomically instead of merely hoping a random
        // filename is unique: ctest runs many TEST_CASEs from this file as
        // separate concurrent processes under -j, and every one of them
        // constructs a temporary_file. std::filesystem::create_directory
        // is atomic (POSIX mkdir / Win32 CreateDirectory), so a collision
        // is detected and retried instead of silently overwriting another
        // process's file.
        std::filesystem::path dir;
        for (;;) {
            dir = std::filesystem::temp_directory_path() / ("motif-position-postings-test-" + std::to_string(std::random_device {}()));
            if (std::filesystem::create_directory(dir)) {
                break;
            }
        }
        dir_ = dir;
        path_ = dir / "postings.idx";
    }

    ~temporary_file() { std::filesystem::remove_all(dir_); }

    temporary_file(temporary_file const&) = delete;
    auto operator=(temporary_file const&) -> temporary_file& = delete;
    temporary_file(temporary_file&&) = delete;
    auto operator=(temporary_file&&) -> temporary_file& = delete;

    [[nodiscard]] auto path() const -> std::filesystem::path const& { return path_; }

  private:
    std::filesystem::path dir_;
    std::filesystem::path path_;
};

auto row(std::uint64_t hash, std::uint32_t game_key, std::uint16_t ply) -> motif::db::position_row
{
    return {.zobrist_hash = motif::db::zobrist_hash {hash},
            .game_id = motif::db::game_id {game_key},
            .ply = ply,
            .encoded_move = 0U,
            .result = 0,
            .white_elo = {},
            .black_elo = {}};
}

auto hash_after_sans(std::initializer_list<char const*> sans) -> motif::db::zobrist_hash
{
    auto board = chesslib::board {};
    for (auto const* san : sans) {
        auto move = chesslib::san::from_string(board, san);
        REQUIRE(move.has_value());
        chesslib::move_maker maker {board, *move};
        maker.make();
    }
    return motif::db::zobrist_hash {board.hash()};
}

auto encode_moves(std::initializer_list<char const*> sans) -> std::vector<std::uint16_t>
{
    auto board = chesslib::board {};
    auto moves = std::vector<std::uint16_t> {};
    moves.reserve(sans.size());
    for (auto const* san : sans) {
        auto move = chesslib::san::from_string(board, san);
        REQUIRE(move.has_value());
        moves.push_back(chesslib::codec::encode(*move));
        chesslib::move_maker maker {board, *move};
        maker.make();
    }
    return moves;
}

auto make_game(std::initializer_list<char const*> sans,
               std::string white_name,
               std::string black_name,
               std::optional<std::int32_t> white_elo = std::nullopt,
               std::optional<std::int32_t> black_elo = std::nullopt) -> motif::db::game
{
    return {.white = {.name = std::move(white_name), .elo = white_elo, .title = std::nullopt, .country = std::nullopt},
            .black = {.name = std::move(black_name), .elo = black_elo, .title = std::nullopt, .country = std::nullopt},
            .event_details = std::nullopt,
            .date = std::nullopt,
            .result = "1-0",
            .eco = std::nullopt,
            .moves = encode_moves(sans),
            .extra_tags = {},
            .provenance = {}};
}

auto read_whole_file(std::filesystem::path const& path) -> std::vector<char>
{
    std::ifstream input {path, std::ios::binary};
    return {std::istreambuf_iterator<char> {input}, std::istreambuf_iterator<char> {}};
}

void write_whole_file(std::filesystem::path const& path, std::span<char const> bytes)
{
    std::ofstream output {path, std::ios::binary | std::ios::trunc};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// Version-6 fixed-header byte offsets, mirroring the stable header layout in
// plans/compact-position-postings.md ("Header"). Used only to hand-corrupt a
// known-valid artifact for negative (corruption-rejection) tests.
constexpr auto header_offset_magic = std::size_t {0};
constexpr auto header_offset_version = std::size_t {8};
constexpr auto header_offset_header_size = std::size_t {12};
constexpr auto header_offset_metadata_offset = std::size_t {40};
constexpr auto header_offset_directory_offset = std::size_t {56};
constexpr auto header_size = std::size_t {88};

constexpr auto byte_bits = std::size_t {8};
constexpr auto byte_mask = std::uint64_t {0xff};

template<typename Integer>
void patch_little_endian(std::vector<char>& bytes, std::size_t const offset, Integer const value)
{
    auto raw_value = static_cast<std::uint64_t>(value);
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        bytes.at(offset + index) = static_cast<char>(raw_value & byte_mask);
        raw_value >>= byte_bits;
    }
}

template<typename Integer>
auto read_little_endian(std::span<char const> const bytes, std::size_t const offset) -> Integer
{
    auto value = std::uint64_t {0};
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset + index])) << (index * byte_bits);
    }
    return static_cast<Integer>(value);
}

// Builds a small, valid version-6 artifact (two games, a shared and a unique
// position) via the direct append()/finalize() path, used as the base for
// hand-corrupted negative tests.
auto build_small_valid_artifact(std::filesystem::path const& path) -> void
{
    auto postings = motif::db::position_postings {path};
    auto const rows = std::vector<motif::db::position_row> {
        row(0x1111, 1U, 0U),
        row(0x2222, 1U, 1U),
        row(0x1111, 2U, 0U),
    };
    REQUIRE(postings.append(rows).has_value());
    REQUIRE(postings.finalize().has_value());
}

}  // namespace

TEST_CASE("position_postings preserves exact repeated occurrences across external merge", "[motif-db][position_postings]")
{
    temporary_file const file;
    constexpr auto target_hash = std::uint64_t {0x1234};
    auto postings = motif::db::position_postings {file.path(), 2U};
    auto const rows = std::vector<motif::db::position_row> {
        row(target_hash, 2U, 0U),
        row(0x5678, 4U, 1U),
        row(target_hash, 1U, 4U),
        row(target_hash, 1U, 0U),
        row(target_hash, 1U, 4U),
    };
    REQUIRE(postings.append(rows).has_value());
    REQUIRE(postings.finalize().has_value());

    auto reopened = motif::db::position_postings {file.path()};
    REQUIRE(reopened.open().has_value());
    auto matches = reopened.occurrences(motif::db::zobrist_hash {target_hash});
    REQUIRE(matches.has_value());
    REQUIRE(matches->size() == 3U);
    CHECK((*matches)[0].game_id == motif::db::game_id {1U});
    CHECK((*matches)[0].ply == 0U);
    CHECK((*matches)[1].game_id == motif::db::game_id {1U});
    CHECK((*matches)[1].ply == 4U);
    CHECK((*matches)[2].game_id == motif::db::game_id {2U});
    CHECK((*matches)[2].ply == 0U);
    auto const game_ids = reopened.distinct_game_ids(motif::db::zobrist_hash {target_hash});
    REQUIRE(game_ids.has_value());
    CHECK(*game_ids == std::vector<motif::db::game_id> {{1U}, {2U}});
    auto const summary = reopened.summary(motif::db::zobrist_hash {target_hash});
    REQUIRE(summary.has_value());
    REQUIRE(summary->has_value());
    auto const summary_value =
        summary.value_or(std::optional<motif::db::position_postings_summary> {}).value_or(motif::db::position_postings_summary {});
    CHECK(summary_value.distinct_game_count == 2U);
    CHECK(summary_value.min_ply == 0U);
    CHECK(summary_value.max_ply == 4U);
    auto const missing_summary = reopened.summary(motif::db::zobrist_hash {0x9999});
    REQUIRE(missing_summary.has_value());
    CHECK_FALSE(missing_summary->has_value());
    REQUIRE(reopened.occurrences(motif::db::zobrist_hash {0x9999}).has_value());
    CHECK(reopened.occurrences(motif::db::zobrist_hash {0x9999})->empty());
}

TEST_CASE("position_postings rejects queries before open", "[motif-db][position_postings]")
{
    temporary_file const file;
    auto postings = motif::db::position_postings {file.path()};
    auto const matches = postings.occurrences(motif::db::zobrist_hash {0x1234});
    REQUIRE_FALSE(matches.has_value());
    CHECK(matches.error() == motif::db::error_code::invalid_argument);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertions inflate test setup complexity.
TEST_CASE("position_postings built from SQLite replay matches hand-computed occurrences", "[motif-db][position_postings][oracle]")
{
    // Game A: 1.Nf3 Nc6 2.Ng1 Nb8 3.Nf3 -- a fully reversible knight shuffle
    // that returns to the exact starting position at ply 4, then repeats
    // 1.Nf3 at ply 5. So "after 1.Nf3" is reached twice within this one
    // game: at ply 1 and at ply 5. Game B: 1.Nf3, reaching the same
    // position once, at ply 1. These expected occurrences are computed by
    // hand from the fixed fixture below, independent of any postings-reader
    // or DuckDB code path.
    temporary_file const file;
    auto manager = motif::db::database_manager::create_scratch();
    REQUIRE(manager.has_value());
    REQUIRE(manager->insert_game(make_game({"Nf3", "Nc6", "Ng1", "Nb8", "Nf3"}, "White One", "Black One", 2700, 2650)).has_value());
    REQUIRE(manager->insert_game(make_game({"Nf3"}, "White Two", "Black Two", std::nullopt, 2400)).has_value());

    REQUIRE(motif::db::position_postings::build(manager->store(), file.path(), 2U).has_value());
    auto postings = motif::db::position_postings {file.path()};
    REQUIRE(postings.open().has_value());

    auto const target_hash = hash_after_sans({"Nf3"});
    auto const posting_matches = postings.occurrences(target_hash);
    REQUIRE(posting_matches.has_value());
    REQUIRE(posting_matches->size() == 3U);

    CHECK((*posting_matches)[0].game_id == motif::db::game_id {1U});
    CHECK((*posting_matches)[0].ply == 1U);
    CHECK((*posting_matches)[0].result == 1);
    CHECK((*posting_matches)[0].white_elo == std::optional<std::int16_t> {2700});
    CHECK((*posting_matches)[0].black_elo == std::optional<std::int16_t> {2650});

    CHECK((*posting_matches)[1].game_id == motif::db::game_id {1U});
    CHECK((*posting_matches)[1].ply == 5U);
    CHECK((*posting_matches)[1].result == 1);
    CHECK((*posting_matches)[1].white_elo == std::optional<std::int16_t> {2700});
    CHECK((*posting_matches)[1].black_elo == std::optional<std::int16_t> {2650});

    CHECK((*posting_matches)[2].game_id == motif::db::game_id {2U});
    CHECK((*posting_matches)[2].ply == 1U);
    CHECK((*posting_matches)[2].result == 1);
    CHECK_FALSE((*posting_matches)[2].white_elo.has_value());
    CHECK((*posting_matches)[2].black_elo == std::optional<std::int16_t> {2400});
}

TEST_CASE("database_manager uses persisted position postings for exact position matches", "[motif-db][position_postings]")
{
    constexpr auto patched_white_elo = std::int32_t {2600};
    constexpr auto elo_bucket_width = 100;
    temporary_file const file;
    auto const bundle_dir = file.path().parent_path() / "motif-position-postings-bundle";
    std::filesystem::remove_all(bundle_dir);
    auto manager = motif::db::database_manager::create(bundle_dir, "postings-search");
    REQUIRE(manager.has_value());
    REQUIRE(manager->insert_game(make_game({"Nf3", "Nc6", "Ng1", "Nb8", "Nf3"}, "White One", "Black One", 2700, 2650)).has_value());
    REQUIRE(manager->insert_game(make_game({"Nf3"}, "White Two", "Black Two", std::nullopt, 2400)).has_value());
    REQUIRE(manager->rebuild_position_postings().has_value());
    REQUIRE(manager->manifest().position_postings.has_value());
    CHECK(std::filesystem::exists(bundle_dir
                                  / manager->manifest().position_postings.value_or(motif::db::derived_index_manifest_entry {}).filename));
    CHECK(manager->manifest().position_postings.value_or(motif::db::derived_index_manifest_entry {}).source_generation
          == manager->manifest().source_generation);

    auto const target_hash = hash_after_sans({"Nf3"});
    auto const matches = manager->query_position_matches(target_hash);
    REQUIRE(matches.has_value());
    REQUIRE(matches->size() == 3U);
    CHECK((*matches)[0].game_id == motif::db::game_id {1U});
    CHECK((*matches)[0].ply == 1U);
    CHECK((*matches)[0].result == 1);
    CHECK((*matches)[0].white_elo == std::optional<std::int16_t> {2700});
    CHECK((*matches)[0].black_elo == std::optional<std::int16_t> {2650});
    CHECK((*matches)[1].game_id == motif::db::game_id {1U});
    CHECK((*matches)[1].ply == 5U);
    CHECK((*matches)[2].game_id == motif::db::game_id {2U});
    CHECK((*matches)[2].ply == 1U);
    CHECK_FALSE((*matches)[2].white_elo.has_value());
    CHECK((*matches)[2].black_elo == std::optional<std::int16_t> {2400});
    auto const requested_game_ids = std::array<motif::db::game_id, 2> {motif::db::game_id {1U}, motif::db::game_id {2U}};
    auto const first_matches = manager->query_position_first_matches(target_hash, requested_game_ids);
    REQUIRE(first_matches.has_value());
    REQUIRE(first_matches->size() == requested_game_ids.size());
    CHECK((*first_matches)[0].game_id == motif::db::game_id {1U});
    CHECK((*first_matches)[0].ply == 1U);
    CHECK((*first_matches)[1].game_id == motif::db::game_id {2U});
    CHECK((*first_matches)[1].ply == 1U);

    auto const no_requested_game_ids = std::array<motif::db::game_id, 0> {};
    auto const no_first_matches = manager->query_position_first_matches(target_hash, no_requested_game_ids);
    REQUIRE(no_first_matches.has_value());
    CHECK(no_first_matches->empty());
    {
        auto const generation_lock = manager->lock_generation();
        REQUIRE(manager->writer().begin_transaction().has_value());
        REQUIRE(manager->writer().insert(make_game({"d4"}, "White Three", "Black Three")).has_value());
        REQUIRE(manager->writer().commit_transaction().has_value());
    }
    auto const mismatched_first_matches = manager->query_position_first_matches(target_hash, requested_game_ids);
    CHECK_FALSE(mismatched_first_matches.has_value());
    REQUIRE(manager->rebuild_position_postings().has_value());

    REQUIRE(manager->set_manual_game_provenance(motif::db::game_id {1U}, std::nullopt, "new").has_value());
    auto patch = motif::db::game_patch {};
    patch.result = "0-1";
    patch.white_elo = patched_white_elo;
    REQUIRE(manager->patch_game_metadata(motif::db::game_id {1U}, patch).has_value());
    CHECK_FALSE(manager->manifest().position_postings.has_value());
    CHECK_FALSE(manager->manifest().opening_tree_index.has_value());
    auto const patched_game = manager->store().get(motif::db::game_id {1U});
    REQUIRE(patched_game.has_value());
    CHECK(patched_game->white.elo == std::optional<std::int32_t> {patched_white_elo});
    auto const stale_matches = manager->query_position_matches(target_hash);
    REQUIRE_FALSE(stale_matches.has_value());
    auto const stale_first_matches = manager->query_position_first_matches(target_hash, requested_game_ids);
    CHECK_FALSE(stale_first_matches.has_value());
    REQUIRE(manager->rebuild_position_postings().has_value());
    auto const patched_matches = manager->query_position_matches(target_hash);
    REQUIRE(patched_matches.has_value());
    CHECK((*patched_matches)[0].result == -1);
    CHECK((*patched_matches)[0].white_elo == std::optional<std::int16_t> {static_cast<std::int16_t>(patched_white_elo)});

    auto const paged = manager->query_position_matches(target_hash, 1U, 1U);
    REQUIRE(paged.has_value());
    REQUIRE(paged->size() == 1U);
    CHECK(paged->front().game_id == motif::db::game_id {1U});
    CHECK(paged->front().ply == 5U);

    manager->close();
    auto reopened = motif::db::database_manager::open(bundle_dir);
    REQUIRE(reopened.has_value());
    auto const reopened_matches = reopened->query_position_matches(target_hash);
    REQUIRE(reopened_matches.has_value());
    REQUIRE(reopened_matches->size() == 3U);
    CHECK((*reopened_matches)[0].game_id == motif::db::game_id {1U});
    CHECK((*reopened_matches)[0].ply == 1U);
    CHECK((*reopened_matches)[0].white_elo == std::optional<std::int16_t> {static_cast<std::int16_t>(patched_white_elo)});
    CHECK((*reopened_matches)[0].black_elo == std::optional<std::int16_t> {2650});
    auto const elo_distribution =
        reopened->query_elo_distribution(motif::db::zobrist_hash {chesslib::board {}.hash()}, {}, elo_bucket_width);
    REQUIRE(elo_distribution.has_value());
    REQUIRE(elo_distribution->size() == 1U);
    CHECK(elo_distribution->front().encoded_move == encode_moves({"Nf3"}).front());
    CHECK(elo_distribution->front().elo_bucket_floor == patched_white_elo);
    CHECK(elo_distribution->front().black_wins == 1U);
    CHECK(elo_distribution->front().game_count == 1U);
    auto filter = motif::db::search_filter {};
    filter.position = target_hash;
    filter.limit = motif::db::default_search_limit;
    auto const filtered_games = reopened->find_games(filter);
    REQUIRE(filtered_games.has_value());
    REQUIRE(filtered_games->games.size() == 2U);
    CHECK(filtered_games->games[0].id == motif::db::game_id {1U});
    CHECK(filtered_games->games[1].id == motif::db::game_id {2U});
    reopened->close();
    std::filesystem::remove_all(bundle_dir);
}

TEST_CASE("database_manager::query_position_matches does not overflow with a SIZE_MAX limit", "[motif-db][position_postings]")
{
    temporary_file const file;
    auto const bundle_dir = file.path().parent_path() / "motif-position-postings-overflow-bundle";
    std::filesystem::remove_all(bundle_dir);
    auto manager = motif::db::database_manager::create(bundle_dir, "overflow-search");
    REQUIRE(manager.has_value());
    REQUIRE(manager->insert_game(make_game({"Nf3", "Nc6", "Ng1", "Nb8", "Nf3"}, "White One", "Black One", 2700, 2650)).has_value());
    REQUIRE(manager->insert_game(make_game({"Nf3"}, "White Two", "Black Two", std::nullopt, 2400)).has_value());
    REQUIRE(manager->rebuild_position_postings().has_value());

    auto const target_hash = hash_after_sans({"Nf3"});
    // begin (1) + SIZE_MAX overflows before a std::min clamp if computed via
    // plain addition; the pagination window must still resolve correctly.
    auto const matches = manager->query_position_matches(target_hash, std::numeric_limits<std::size_t>::max(), 1U);
    REQUIRE(matches.has_value());
    REQUIRE(matches->size() == 2U);
    CHECK((*matches)[0].game_id == motif::db::game_id {1U});
    CHECK((*matches)[0].ply == 5U);
    CHECK((*matches)[1].game_id == motif::db::game_id {2U});
    CHECK((*matches)[1].ply == 1U);

    manager->close();
    std::filesystem::remove_all(bundle_dir);
}

TEST_CASE("database_manager::position_summary reports valid, stale, and absent postings", "[motif-db][position_postings]")
{
    temporary_file const file;
    auto const bundle_dir = file.path().parent_path() / "motif-position-summary-bundle";
    std::filesystem::remove_all(bundle_dir);
    auto manager = motif::db::database_manager::create(bundle_dir, "summary-search");
    REQUIRE(manager.has_value());

    auto const target_hash = hash_after_sans({"Nf3"});

    // No postings have ever been built: not an error, just no data.
    auto const before_build = manager->position_summary(target_hash);
    REQUIRE(before_build.has_value());
    CHECK_FALSE(before_build->has_value());

    REQUIRE(manager->insert_game(make_game({"Nf3", "Nc6", "Ng1", "Nb8", "Nf3"}, "White One", "Black One", 2700, 2650)).has_value());
    REQUIRE(manager->insert_game(make_game({"Nf3"}, "White Two", "Black Two", std::nullopt, 2400)).has_value());
    REQUIRE(manager->rebuild_position_postings().has_value());
    REQUIRE(manager->manifest().position_postings.has_value());

    // Valid postings, hash present.
    auto const valid = manager->position_summary(target_hash);
    REQUIRE(valid.has_value());
    REQUIRE(valid->has_value());
    CHECK(valid->value_or(motif::db::position_postings_summary {}).distinct_game_count == 2U);

    // Valid postings, hash absent from the index.
    auto const absent_hash = motif::db::zobrist_hash {0xDEAD'BEEFU};
    auto const absent = manager->position_summary(absent_hash);
    REQUIRE(absent.has_value());
    CHECK_FALSE(absent->has_value());

    // Postings now stale: another game lands without a postings rebuild.
    REQUIRE(manager->insert_game(make_game({"Nf3"}, "White Three", "Black Three", std::nullopt, std::nullopt)).has_value());
    auto const stale = manager->position_summary(target_hash);
    REQUIRE(stale.has_value());
    CHECK_FALSE(stale->has_value());

    manager->close();
    std::filesystem::remove_all(bundle_dir);
}

// NOLINTBEGIN(readability-function-cognitive-complexity) -- Catch2 assertions inflate failure-injection setup complexity.
TEST_CASE("database_manager::rebuild_position_postings leaves the prior generation intact but stale when publish fails",
          "[motif-db][position_postings]")
{
    temporary_file const file;
    auto const bundle_dir = file.path().parent_path() / "motif-position-postings-publish-fail-bundle";
    std::filesystem::remove_all(bundle_dir);
    auto manager = motif::db::database_manager::create(bundle_dir, "publish-fail-search");
    REQUIRE(manager.has_value());

    REQUIRE(manager->insert_game(make_game({"Nf3", "Nc6", "Ng1", "Nb8", "Nf3"}, "White One", "Black One", 2700, 2650)).has_value());
    REQUIRE(manager->rebuild_position_postings().has_value());
    REQUIRE(manager->manifest().position_postings.has_value());
    auto const original_filename = manager->manifest().position_postings.value_or(motif::db::derived_index_manifest_entry {}).filename;
    auto const original_path = bundle_dir / original_filename;
    REQUIRE(std::filesystem::exists(original_path));

    auto const target_hash = hash_after_sans({"Nf3"});
    auto const before = manager->query_position_matches(target_hash);
    REQUIRE(before.has_value());
    REQUIRE(before->size() == 2U);

    // Block the next publication without changing canonical SQLite. The
    // replacement bytes may be identical, but the generated filename differs,
    // so the manifest and orphan checks still distinguish success from failure.
    auto const manifest_tmp_path = bundle_dir / "manifest.json.tmp";
    std::filesystem::create_directory(manifest_tmp_path);

    auto const rebuild_res = manager->rebuild_position_postings();
    REQUIRE_FALSE(rebuild_res.has_value());

    std::filesystem::remove(manifest_tmp_path);

    // The manifest entry, the file it names, and the live in-memory reader
    // must all still be the original (pre-failure) generation.
    REQUIRE(manager->manifest().position_postings.has_value());
    CHECK(manager->manifest().position_postings.value_or(motif::db::derived_index_manifest_entry {}).filename == original_filename);
    CHECK(std::filesystem::exists(original_path));

    // No orphaned unpublished-generation file left behind.
    for (auto const& entry : std::filesystem::directory_iterator {bundle_dir}) {
        auto const name = entry.path().filename().string();
        if (name.starts_with("positions.postings.") && name != original_filename) {
            FAIL("unexpected leftover postings file: " << name);
        }
    }

    // The old file remains queryable because canonical SQLite did not change.
    auto const after_failed_publish = manager->query_position_matches(target_hash);
    REQUIRE(after_failed_publish.has_value());
    CHECK(after_failed_publish->size() == 2U);

    // A retry (build_seq untouched by the failed publish, since manifest_
    // -- the only thing that advances it -- was never assigned) succeeds and
    // supersedes the original generation.
    REQUIRE(manager->rebuild_position_postings().has_value());
    REQUIRE(manager->manifest().position_postings.has_value());
    CHECK(manager->manifest().position_postings.value_or(motif::db::derived_index_manifest_entry {}).filename != original_filename);
    CHECK_FALSE(std::filesystem::exists(original_path));
    auto const after_retry = manager->query_position_matches(target_hash);
    REQUIRE(after_retry.has_value());
    CHECK(after_retry->size() == 2U);

    manager->close();
    std::filesystem::remove_all(bundle_dir);
}

// NOLINTEND(readability-function-cognitive-complexity)

TEST_CASE("database_manager::open skips rebuilding when exact postings cover canonical games", "[motif-db][position_postings]")
{
    temporary_file const file;
    auto const bundle_dir = file.path().parent_path() / "motif-postings-skip-rebuild-bundle";
    std::filesystem::remove_all(bundle_dir);
    {
        auto manager = motif::db::database_manager::create(bundle_dir, "postings-skip-rebuild");
        REQUIRE(manager.has_value());
        REQUIRE(manager->insert_game(make_game({"Nf3"}, "White One", "Black One", 2700, 2650)).has_value());
        REQUIRE(manager->rebuild_position_postings().has_value());
        auto const build_seq_before = manager->manifest().derived_index_build_seq;
        REQUIRE(manager->manifest().position_postings.has_value());
        manager->close();

        // Simulate an unclean shutdown: force the in-use dirty flag back on.
        auto manifest = motif::db::read_manifest(bundle_dir / "manifest.json");
        REQUIRE(manifest.has_value());
        manifest->position_index_dirty = true;  // NOLINT(bugprone-unchecked-optional-access)
        REQUIRE(motif::db::write_manifest(bundle_dir / "manifest.json", *manifest).has_value());

        auto reopened = motif::db::database_manager::open(bundle_dir);
        REQUIRE(reopened.has_value());
        // Dirty recovery recognized the covering postings generation and did
        // not rebuild: the build sequence counter is unchanged from before
        // the dirty reopen.
        CHECK(reopened->manifest().derived_index_build_seq == build_seq_before);
        REQUIRE(reopened->manifest().position_postings.has_value());
        auto const target_hash = hash_after_sans({"Nf3"});
        auto const matches = reopened->query_position_matches(target_hash);
        REQUIRE(matches.has_value());
        REQUIRE(matches->size() == 1U);
        CHECK((*matches)[0].game_id == motif::db::game_id {1U});

        reopened->close();
    }
    std::filesystem::remove_all(bundle_dir);
}

// ── Directory-block boundaries, pagination, and the sorted summary stream ──

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- exercises every directory-block-boundary hash count in one case.
TEST_CASE("position_postings directory block boundaries at 255, 256, and 257 hashes", "[motif-db][position_postings]")
{
    constexpr auto hash_base = std::uint64_t {0x1'0000'0000};
    constexpr auto ply_modulus = std::uint32_t {1000};
    constexpr auto block_size = std::uint32_t {256};
    constexpr auto last_index_in_first_block = block_size - 1U;
    for (auto const hash_count : {block_size - 1U, block_size, block_size + 1U}) {
        temporary_file const file;
        auto postings = motif::db::position_postings {file.path()};
        std::vector<motif::db::position_row> rows;
        rows.reserve(hash_count);
        for (std::uint32_t index = 0; index < hash_count; ++index) {
            rows.push_back(row(hash_base + index, 1U, static_cast<std::uint16_t>(index % ply_modulus)));
        }
        REQUIRE(postings.append(rows).has_value());
        REQUIRE(postings.finalize().has_value());

        auto reopened = motif::db::position_postings {file.path()};
        REQUIRE(reopened.open().has_value());

        // First hash, the hash straddling the 256-entry block boundary (when
        // present), and the last hash must all resolve correctly regardless
        // of which compressed directory block holds them.
        for (auto const index : {std::uint32_t {0}, last_index_in_first_block, block_size, hash_count - 1U}) {
            if (index >= hash_count) {
                continue;
            }
            auto const hash = motif::db::zobrist_hash {hash_base + index};
            auto const matches = reopened.occurrences(hash);
            REQUIRE(matches.has_value());
            REQUIRE(matches->size() == 1U);
            CHECK((*matches)[0].ply == index % ply_modulus);
            auto const summary = reopened.summary(hash);
            REQUIRE(summary.has_value());
            REQUIRE(summary->has_value());
            CHECK(summary->value_or(motif::db::position_postings_summary {}).distinct_game_count == 1U);
        }
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- exercises every pagination boundary in one case.
TEST_CASE("position_postings occurrences pagination boundaries", "[motif-db][position_postings]")
{
    temporary_file const file;
    constexpr auto target_hash = std::uint64_t {0xabcd};
    constexpr auto game_count = std::uint32_t {5};
    auto postings = motif::db::position_postings {file.path()};
    std::vector<motif::db::position_row> rows;
    rows.reserve(game_count);
    for (std::uint32_t index = 0; index < game_count; ++index) {
        rows.push_back(row(target_hash, index + 1U, 0U));
    }
    REQUIRE(postings.append(rows).has_value());
    REQUIRE(postings.finalize().has_value());

    auto reopened = motif::db::position_postings {file.path()};
    REQUIRE(reopened.open().has_value());
    auto const hash = motif::db::zobrist_hash {target_hash};

    // Zero limit is unbounded.
    auto const all = reopened.occurrences(hash, 0U, 0U);
    REQUIRE(all.has_value());
    CHECK(all->size() == 5U);

    // limit == 1.
    auto const one = reopened.occurrences(hash, 1U, 0U);
    REQUIRE(one.has_value());
    REQUIRE(one->size() == 1U);
    CHECK(one->front().game_id == motif::db::game_id {1U});

    // limit == exact remaining count.
    auto const exact = reopened.occurrences(hash, 5U, 0U);
    REQUIRE(exact.has_value());
    CHECK(exact->size() == 5U);

    // limit beyond the count clamps to what remains.
    auto const beyond = reopened.occurrences(hash, 100U, 3U);
    REQUIRE(beyond.has_value());
    REQUIRE(beyond->size() == 2U);
    CHECK(beyond->front().game_id == motif::db::game_id {4U});

    // offset at exactly the count returns empty, not an error.
    auto const at_count = reopened.occurrences(hash, 0U, 5U);
    REQUIRE(at_count.has_value());
    CHECK(at_count->empty());

    // offset == SIZE_MAX must not overflow the begin/remaining computation.
    auto const max_offset = reopened.occurrences(hash, 0U, std::numeric_limits<std::size_t>::max());
    REQUIRE(max_offset.has_value());
    CHECK(max_offset->empty());
}

TEST_CASE("position_postings first_occurrences returns only requested games at their earliest ply", "[motif-db][position_postings]")
{
    temporary_file const file;
    constexpr auto target_hash = std::uint64_t {0xabcd};
    auto postings = motif::db::position_postings {file.path()};
    auto const rows = std::vector<motif::db::position_row> {
        row(target_hash, 1U, 1U),
        row(target_hash, 1U, 5U),
        row(target_hash, 2U, 3U),
        row(target_hash, 3U, 2U),
    };
    REQUIRE(postings.append(rows).has_value());
    REQUIRE(postings.finalize().has_value());

    auto reopened = motif::db::position_postings {file.path()};
    REQUIRE(reopened.open().has_value());
    auto const requested = std::vector<motif::db::game_id> {motif::db::game_id {3U}, motif::db::game_id {1U}};
    auto const matches = reopened.first_occurrences(motif::db::zobrist_hash {target_hash}, requested);

    REQUIRE(matches.has_value());
    REQUIRE(matches->size() == 2U);
    CHECK((*matches)[0].game_id == motif::db::game_id {1U});
    CHECK((*matches)[0].ply == 1U);
    CHECK((*matches)[1].game_id == motif::db::game_id {3U});
    CHECK((*matches)[1].ply == 2U);
}

TEST_CASE("position_postings first_occurrences handles empty and missing requested IDs", "[motif-db][position_postings]")
{
    temporary_file const file;
    constexpr auto target_hash = std::uint64_t {0xabcd};
    auto postings = motif::db::position_postings {file.path()};
    auto const rows = std::vector<motif::db::position_row> {row(target_hash, 1U, 2U)};
    REQUIRE(postings.append(rows).has_value());
    REQUIRE(postings.finalize().has_value());

    auto reopened = motif::db::position_postings {file.path()};
    REQUIRE(reopened.open().has_value());
    CHECK(
        reopened.first_occurrences(motif::db::zobrist_hash {target_hash}, {}).value_or(std::vector<motif::db::position_match> {}).empty());

    auto const requested = std::vector<motif::db::game_id> {motif::db::game_id {1U}, motif::db::game_id {99U}, motif::db::game_id {99U}};
    auto const matches = reopened.first_occurrences(motif::db::zobrist_hash {target_hash}, requested);
    REQUIRE(matches.has_value());
    REQUIRE(matches->size() == 1U);
    CHECK(matches->front().game_id == motif::db::game_id {1U});
    CHECK(matches->front().ply == 2U);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- checks the streamed order plus each summary field per hash.
TEST_CASE("position_postings for_each_summary streams every hash ascending and matches point summary()", "[motif-db][position_postings]")
{
    temporary_file const file;
    constexpr auto hash_low = std::uint64_t {0x100};
    constexpr auto hash_mid = std::uint64_t {0x200};
    constexpr auto hash_high = std::uint64_t {0x300};
    auto postings = motif::db::position_postings {file.path()};
    std::vector<motif::db::position_row> rows {
        row(hash_high, 1U, 0U),
        row(hash_low, 1U, 1U),
        row(hash_low, 2U, 0U),
        row(hash_mid, 3U, 0U),
    };
    REQUIRE(postings.append(rows).has_value());
    REQUIRE(postings.finalize().has_value());

    auto reopened = motif::db::position_postings {file.path()};
    REQUIRE(reopened.open().has_value());

    std::vector<std::pair<std::uint64_t, motif::db::position_postings_summary>> streamed;
    auto const summary_visitor = [&streamed](motif::db::zobrist_hash const hash,
                                             motif::db::position_postings_summary const& summary) -> motif::db::result<void>
    {
        streamed.emplace_back(hash.value, summary);
        return {};
    };
    auto const stream_result = reopened.for_each_summary(summary_visitor);
    REQUIRE(stream_result.has_value());

    REQUIRE(streamed.size() == 3U);
    CHECK(streamed[0].first == hash_low);
    CHECK(streamed[1].first == hash_mid);
    CHECK(streamed[2].first == hash_high);
    for (auto const& [hash_value, streamed_summary] : streamed) {
        auto const point_summary = reopened.summary(motif::db::zobrist_hash {hash_value});
        REQUIRE(point_summary.has_value());
        REQUIRE(point_summary->has_value());
        auto const expected = point_summary->value_or(motif::db::position_postings_summary {});
        CHECK(streamed_summary.occurrence_count == expected.occurrence_count);
        CHECK(streamed_summary.distinct_game_count == expected.distinct_game_count);
        CHECK(streamed_summary.min_ply == expected.min_ply);
        CHECK(streamed_summary.max_ply == expected.max_ply);
    }
}

TEST_CASE("position_postings directory preserves wide hash deltas and repeated-game occurrence counts", "[motif-db][position_postings]")
{
    temporary_file const file;
    constexpr auto hash_low = std::uint64_t {0x100};
    constexpr auto hash_high = (std::uint64_t {1} << 56U) + hash_low;
    auto postings = motif::db::position_postings {file.path()};
    auto const rows = std::array {
        row(hash_low, 1U, 0U),
        row(hash_high, 1U, 1U),
        row(hash_high, 1U, 3U),
        row(hash_high, 2U, 2U),
    };
    REQUIRE(postings.append(rows).has_value());
    REQUIRE(postings.finalize().has_value());

    auto reopened = motif::db::position_postings {file.path()};
    REQUIRE(reopened.open().has_value());
    auto const summary = reopened.summary(motif::db::zobrist_hash {hash_high});
    REQUIRE(summary.has_value());
    REQUIRE(summary->has_value());
    auto const value = summary->value_or(motif::db::position_postings_summary {});
    CHECK(value.occurrence_count == 3U);
    CHECK(value.distinct_game_count == 2U);
    CHECK(value.min_ply == 1U);
    CHECK(value.max_ply == 3U);

    auto const occurrences = reopened.occurrences(motif::db::zobrist_hash {hash_high});
    REQUIRE(occurrences.has_value());
    REQUIRE(occurrences->size() == 3U);
    CHECK((*occurrences)[0].game_id == motif::db::game_id {1U});
    CHECK((*occurrences)[0].ply == 1U);
    CHECK((*occurrences)[1].game_id == motif::db::game_id {1U});
    CHECK((*occurrences)[1].ply == 3U);
    CHECK((*occurrences)[2].game_id == motif::db::game_id {2U});
    CHECK((*occurrences)[2].ply == 2U);
}

TEST_CASE("position_postings directory reopens a single-entry block", "[motif-db][position_postings]")
{
    temporary_file const file;
    constexpr auto only_hash = std::uint64_t {0x123456789abcdef0ULL};
    auto postings = motif::db::position_postings {file.path()};
    auto const rows = std::array {row(only_hash, 1U, 7U)};
    REQUIRE(postings.append(rows).has_value());
    REQUIRE(postings.finalize().has_value());

    auto reopened = motif::db::position_postings {file.path()};
    REQUIRE(reopened.open().has_value());
    auto const summary = reopened.summary(motif::db::zobrist_hash {only_hash});
    REQUIRE(summary.has_value());
    REQUIRE(summary->has_value());
    CHECK(summary->value_or(motif::db::position_postings_summary {}).occurrence_count == 1U);
}

// ── Lifecycle ──

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- checks every query API against the empty-index case.
TEST_CASE("position_postings builds and reopens an empty index", "[motif-db][position_postings]")
{
    temporary_file const file;
    auto postings = motif::db::position_postings {file.path()};
    REQUIRE(postings.finalize().has_value());

    auto reopened = motif::db::position_postings {file.path()};
    REQUIRE(reopened.open().has_value());
    CHECK(reopened.indexed_game_count() == 0U);
    auto const matches = reopened.occurrences(motif::db::zobrist_hash {0x1234});
    REQUIRE(matches.has_value());
    CHECK(matches->empty());
    auto const summary = reopened.summary(motif::db::zobrist_hash {0x1234});
    REQUIRE(summary.has_value());
    CHECK_FALSE(summary->has_value());
    std::size_t streamed_count = 0;
    auto const empty_index_visitor = [&streamed_count](motif::db::zobrist_hash,
                                                       motif::db::position_postings_summary const&) -> motif::db::result<void>
    {
        ++streamed_count;
        return {};
    };
    auto const stream_result = reopened.for_each_summary(empty_index_visitor);
    REQUIRE(stream_result.has_value());
    CHECK(streamed_count == 0U);
}

TEST_CASE("position_postings leaves no .spill*/.dirspool/.tmp temporaries after a successful build", "[motif-db][position_postings]")
{
    temporary_file const file;
    constexpr auto row_count = std::uint32_t {20};
    constexpr auto hash_base = std::uint64_t {0x9000};
    // A tiny spill threshold forces several spill runs and a multi-block
    // directory spool, so the cleanup path has real temporaries to remove.
    auto postings = motif::db::position_postings {file.path(), 2U};
    std::vector<motif::db::position_row> rows;
    rows.reserve(row_count);
    for (std::uint32_t index = 0; index < row_count; ++index) {
        rows.push_back(row(hash_base + index, 1U, static_cast<std::uint16_t>(index)));
    }
    REQUIRE(postings.append(rows).has_value());
    REQUIRE(postings.finalize().has_value());

    auto const own_filename = file.path().filename().string();
    for (auto const& entry : std::filesystem::directory_iterator {file.path().parent_path()}) {
        auto const name = entry.path().filename().string();
        if (!name.starts_with(own_filename) || name.size() <= own_filename.size()) {
            continue;
        }
        auto const suffix = name.substr(own_filename.size());
        if (suffix == ".tmp" || suffix == ".dirspool" || suffix.starts_with(".spill")) {
            FAIL("unexpected leftover temporary: " << name);
        }
    }
}

// ── Codec and corruption ──

TEST_CASE("position_postings open() rejects a corrupted magic", "[motif-db][position_postings]")
{
    temporary_file const file;
    build_small_valid_artifact(file.path());
    auto bytes = read_whole_file(file.path());
    bytes.at(header_offset_magic) = 'X';
    write_whole_file(file.path(), bytes);

    auto postings = motif::db::position_postings {file.path()};
    auto const opened = postings.open();
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error() == motif::db::error_code::schema_mismatch);
}

TEST_CASE("position_postings open() rejects nonzero directory bitmap padding", "[motif-db][position_postings]")
{
    temporary_file const file;
    build_small_valid_artifact(file.path());
    auto bytes = read_whole_file(file.path());
    auto const directory_offset = read_little_endian<std::uint64_t>(bytes, header_offset_directory_offset);
    constexpr auto directory_block_header_size = std::size_t {18};
    constexpr auto unused_padding_bit = std::uint8_t {0x80};
    auto& bitmap_byte = bytes.at(static_cast<std::size_t>(directory_offset) + directory_block_header_size);
    bitmap_byte = static_cast<char>(static_cast<std::uint8_t>(bitmap_byte) | unused_padding_bit);
    write_whole_file(file.path(), bytes);

    auto postings = motif::db::position_postings {file.path()};
    auto const opened = postings.open();
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error() == motif::db::error_code::schema_mismatch);
}

TEST_CASE("position_postings open() rejects an unknown version", "[motif-db][position_postings]")
{
    temporary_file const file;
    build_small_valid_artifact(file.path());
    auto bytes = read_whole_file(file.path());
    constexpr auto unknown_version = std::uint32_t {99};
    patch_little_endian(bytes, header_offset_version, unknown_version);
    write_whole_file(file.path(), bytes);

    auto postings = motif::db::position_postings {file.path()};
    auto const opened = postings.open();
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error() == motif::db::error_code::schema_mismatch);
}

TEST_CASE("position_postings direct construction records indexed game count", "[motif-db][position_postings]")
{
    temporary_file const file;
    auto postings = motif::db::position_postings {file.path()};
    auto const rows = std::array {row(0x100U, 3U, 0U), row(0x200U, 9U, 0U)};
    REQUIRE(postings.append(rows).has_value());
    REQUIRE(postings.finalize().has_value());

    auto reopened = motif::db::position_postings {file.path()};
    REQUIRE(reopened.open().has_value());
    CHECK(reopened.indexed_game_count() == 2U);
}

TEST_CASE("position_postings rejects invalid result values", "[motif-db][position_postings]")
{
    temporary_file const file;
    constexpr auto target_hash = std::uint64_t {0x100};
    auto postings = motif::db::position_postings {file.path()};
    auto invalid = row(target_hash, 1U, 0U);
    invalid.result = 2;
    auto const appended = postings.append(std::span {&invalid, 1U});
    REQUIRE_FALSE(appended.has_value());
    CHECK(appended.error() == motif::db::error_code::invalid_argument);
}

TEST_CASE("position_postings for_each_summary propagates visitor failure", "[motif-db][position_postings]")
{
    temporary_file const file;
    auto postings = motif::db::position_postings {file.path()};
    auto const rows = std::array {row(0x100U, 1U, 0U)};
    REQUIRE(postings.append(rows).has_value());
    REQUIRE(postings.finalize().has_value());

    auto reopened = motif::db::position_postings {file.path()};
    REQUIRE(reopened.open().has_value());
    auto const streamed =
        reopened.for_each_summary([](motif::db::zobrist_hash, motif::db::position_postings_summary const&) -> motif::db::result<void>
                                  { return tl::unexpected {motif::db::error_code::io_failure}; });
    REQUIRE_FALSE(streamed.has_value());
    CHECK(streamed.error() == motif::db::error_code::io_failure);
}

TEST_CASE("position_postings open() rejects a version-4 artifact rather than misreading it", "[motif-db][position_postings]")
{
    temporary_file const file;
    build_small_valid_artifact(file.path());
    auto bytes = read_whole_file(file.path());
    patch_little_endian(bytes, header_offset_version, std::uint32_t {4});
    write_whole_file(file.path(), bytes);

    auto postings = motif::db::position_postings {file.path()};
    auto const opened = postings.open();
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error() == motif::db::error_code::schema_mismatch);
}

TEST_CASE("position_postings open() rejects a mismatched header_size", "[motif-db][position_postings]")
{
    temporary_file const file;
    build_small_valid_artifact(file.path());
    auto bytes = read_whole_file(file.path());
    constexpr auto header_size_delta = std::uint32_t {8};
    patch_little_endian(bytes, header_offset_header_size, static_cast<std::uint32_t>(header_size) + header_size_delta);
    write_whole_file(file.path(), bytes);

    auto postings = motif::db::position_postings {file.path()};
    auto const opened = postings.open();
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error() == motif::db::error_code::schema_mismatch);
}

TEST_CASE("position_postings open() rejects a truncated header", "[motif-db][position_postings]")
{
    temporary_file const file;
    build_small_valid_artifact(file.path());
    auto bytes = read_whole_file(file.path());
    bytes.resize(header_size - 1U);
    write_whole_file(file.path(), bytes);

    auto postings = motif::db::position_postings {file.path()};
    auto const opened = postings.open();
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error() == motif::db::error_code::schema_mismatch);
}

TEST_CASE("position_postings open() rejects trailing bytes after the sparse directory", "[motif-db][position_postings]")
{
    temporary_file const file;
    build_small_valid_artifact(file.path());
    auto bytes = read_whole_file(file.path());
    bytes.push_back('\0');
    write_whole_file(file.path(), bytes);

    auto postings = motif::db::position_postings {file.path()};
    auto const opened = postings.open();
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error() == motif::db::error_code::schema_mismatch);
}

TEST_CASE("position_postings open() rejects a metadata_offset that overlaps the fixed header", "[motif-db][position_postings]")
{
    temporary_file const file;
    build_small_valid_artifact(file.path());
    auto bytes = read_whole_file(file.path());
    // The fixed header claims metadata starts inside itself, which can never
    // be a valid layout regardless of what the rest of the file contains.
    patch_little_endian(bytes, header_offset_metadata_offset, std::uint64_t {header_size - 1U});
    write_whole_file(file.path(), bytes);

    auto postings = motif::db::position_postings {file.path()};
    auto const opened = postings.open();
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error() == motif::db::error_code::schema_mismatch);
}

TEST_CASE("position_postings open() rejects a nonexistent file", "[motif-db][position_postings]")
{
    temporary_file const file;
    auto postings = motif::db::position_postings {file.path()};
    auto const opened = postings.open();
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error() == motif::db::error_code::io_failure);
}
