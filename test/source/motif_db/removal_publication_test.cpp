// PR 7: database_manager::remove_game on a bundle whose indexes cover canonical
// state is ONE operation. The removed game's postings occurrences and tree
// contribution are retracted incrementally (no rebuild, no replay of the other
// games) and the canonical DELETE, the single sequence advance and the new
// snapshot row commit together. These tests drive the production removal path
// and compare every answer with a fresh rebuild of the remaining games (game ids
// are translated, because removal leaves gaps in the id space).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#ifndef _WIN32
#    include <sys/wait.h>
#    include <unistd.h>
#endif

#include "composite_test_support.hpp"
#include "edit_test_support.hpp"
#include "equivalence_support.hpp"
#include "motif/db/database_manager.hpp"
#include "motif/db/error.hpp"
#include "motif/db/fault_injection.hpp"
#include "motif/db/manifest.hpp"
#include "motif/db/patch_chain.hpp"
#include "motif/db/types.hpp"

namespace
{

using namespace motif::db;  // NOLINT(google-build-using-namespace) -- the file exercises most of motif::db; qualifying every name buries
                            // the assertions.
using namespace motif_test;  // NOLINT(google-build-using-namespace) -- the shared harness helpers are used throughout.

// A bundle whose canonical model is kept in step with every production removal, so it can be compared with a fresh rebuild of the
// games that remain. `ids` holds the live game id of each corpus game (ascending), `corpus` the games themselves.
struct removal_bundle
{
    temp_dir dir {"removal"};
    fs::path bundle;
    std::vector<model_game> corpus;
    std::vector<game_id> ids;
    // Null only between a deliberate reset() and the next reopen().
    std::unique_ptr<database_manager> manager;
    bool with_tree;

    removal_bundle(std::vector<model_game> games, bool const tree)
        : bundle {dir.path / "db"}
        , corpus {std::move(games)}
        , with_tree {tree}
    {
        manager = std::make_unique<database_manager>(build_bundle(bundle, corpus, tree));
        ids = contiguous_ids(corpus.size());
    }

    removal_bundle(std::uint64_t const seed, std::size_t const random_games, bool const tree)
        : removal_bundle {make_corpus(seed, random_games), tree}
    {
    }

    [[nodiscard]] auto status() const -> derived_index_report
    {
        auto report = manager->derived_index_status();
        REQUIRE(report.has_value());
        return *report;
    }

    void reopen()
    {
        manager.reset();
        auto reopened = database_manager::open(bundle);
        REQUIRE(reopened.has_value());
        manager = std::make_unique<database_manager>(std::move(*reopened));
    }

    // The removed game answers nowhere: not in the canonical store, not in the postings occurrences of the start position every game
    // passes through, and not in the metadata listing.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 macros inflate this linear absence check.
    void require_absent(game_id const target) const
    {
        auto const gone = manager->store().get(target);
        REQUIRE_FALSE(gone.has_value());
        CHECK(gone.error() == error_code::not_found);

        auto const start = zobrist_hash {start_hash()};
        auto const matches = manager->query_position_matches(start);
        REQUIRE(matches.has_value());
        CHECK(std::ranges::none_of(*matches, [target](position_match const& match) -> bool { return match.game_id == target; }));
        auto const listed = manager->position_game_ids(start);
        REQUIRE(listed.has_value());
        CHECK(std::ranges::find(*listed, target) == listed->end());

        auto const games = manager->find_games(search_filter {});
        REQUIRE(games.has_value());
        CHECK(std::cmp_equal(games->total_count, corpus.size()));
        CHECK(std::ranges::none_of(games->games, [target](auto const& row) -> bool { return row.id == target; }));
    }

    // One production removal of the live game at `position`; the model follows on success. The removal is exactly one publication: one
    // sequence advance, one generation, both indexes still usable, nothing rebuilt.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 macros inflate this linear remove-and-verify sequence.
    void remove(std::size_t const position)
    {
        auto const target = ids.at(position);
        auto const before = status();
        REQUIRE(before.postings_usable);
        auto const removed = manager->remove_game(target);
        INFO((removed.has_value() ? std::string {} : removed.error().message));
        REQUIRE(removed.has_value());
        corpus.erase(corpus.begin() + static_cast<std::ptrdiff_t>(position));
        ids.erase(ids.begin() + static_cast<std::ptrdiff_t>(position));

        auto const now = status();
        CHECK(now.mutation_seq == before.mutation_seq + 1U);
        CHECK(now.publication_generation == before.publication_generation + 1U);
        CHECK(now.covered_mutation_seq == std::optional<std::uint64_t> {now.mutation_seq});
        CHECK_FALSE(now.pending_canonical_change);
        CHECK(now.postings_usable);
        CHECK(now.tree_usable == with_tree);
        auto const& report = manager->last_removal_report();
        CHECK(report.published);
        CHECK_FALSE(report.canonical_only);
        CHECK_FALSE(report.canonical_noop);
        // Only a usable tree replays the one removed game; the other games are never replayed.
        CHECK(report.replayed_games <= (with_tree ? 1U : 0U));
        CHECK(report.tree_runs == (with_tree ? manager->manifest().tree()->patch_runs.size() : 0U));
        CHECK(report.metadata_runs == manager->manifest().payload.metadata.runs.size());
        auto const count = manager->store().count_games();
        REQUIRE(count.has_value());
        CHECK(std::cmp_equal(*count, corpus.size()));
        require_absent(target);
    }

    // The directory holds exactly the files the installed snapshot references.
    void require_clean_directory() const { REQUIRE(artifact_names(bundle) == referenced_names(manager->manifest())); }

    void require_equivalent(std::uint64_t const seed) const
    {
        REQUIRE_FALSE(corpus.empty());
        require_equivalent_to_fresh_ids(*manager, bundle, corpus, ids, with_tree, seed);
    }

    // A game added after the last build (an insert is not incremental: it leaves the indexes stale until a rebuild).
    auto insert(model_game const& model) -> game_id
    {
        auto const inserted = manager->insert_game(to_db_game(model, 900U + corpus.size()));
        REQUIRE(inserted.has_value());
        corpus.push_back(model);
        ids.push_back(*inserted);
        return *inserted;
    }

    void rebuild_all() const
    {
        REQUIRE(manager->rebuild_position_postings().has_value());
        if (with_tree) {
            REQUIRE(manager->rebuild_opening_tree_index().has_value());
        }
    }
};

auto read_file(fs::path const& path) -> std::string
{
    auto input = std::ifstream {path, std::ios::binary};
    return std::string {std::istreambuf_iterator<char> {input}, std::istreambuf_iterator<char> {}};
}

void write_file(fs::path const& path, std::string const& bytes)
{
    auto output = std::ofstream {path, std::ios::binary | std::ios::trunc};
    output << bytes;
    REQUIRE(output.good());
}

auto flipped_result(std::string const& current) -> std::string
{
    return current == "1-0" ? "0-1" : "1-0";
}

// The state a rejected or canonical-only operation must leave untouched.
struct state_snapshot
{
    derived_index_report status;
    std::string payload_json;
    std::set<std::string> files;
};

auto snapshot_of(removal_bundle const& env) -> state_snapshot
{
    return state_snapshot {
        .status = env.status(), .payload_json = env.manager->manifest().payload_json, .files = artifact_names(env.bundle)};
}

void require_unchanged(removal_bundle const& env, state_snapshot const& before)
{
    auto const now = env.status();
    CHECK(now.mutation_seq == before.status.mutation_seq);
    CHECK(now.publication_generation == before.status.publication_generation);
    CHECK(now.postings_usable == before.status.postings_usable);
    CHECK(now.tree_usable == before.status.tree_usable);
    CHECK(env.manager->manifest().payload_json == before.payload_json);
    CHECK(artifact_names(env.bundle) == before.files);
}

// Every answer a bundle with no games gives: valid, current, and empty.
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 macros inflate this linear check.
void require_empty_answers(removal_bundle const& env)
{
    auto const start = zobrist_hash {start_hash()};
    auto const status = env.status();
    CHECK(status.postings_usable);
    CHECK(status.tree_usable == env.with_tree);
    auto const count = env.manager->store().count_games();
    REQUIRE(count.has_value());
    CHECK(*count == 0);
    auto const summary = env.manager->position_summary(start);
    REQUIRE(summary.has_value());
    CHECK_FALSE(summary->has_value());
    auto const matches = env.manager->query_position_matches(start);
    REQUIRE(matches.has_value());
    CHECK(matches->empty());
    auto const listed = env.manager->position_game_ids(start);
    REQUIRE(listed.has_value());
    CHECK(listed->empty());
    auto const stats = env.manager->query_unfiltered_opening_stats(start);
    REQUIRE(stats.has_value());
    CHECK(stats->empty());
    auto const games = env.manager->find_games(search_filter {});
    REQUIRE(games.has_value());
    CHECK(games->total_count == 0);
    CHECK(games->games.empty());
}

// The chain caps the fold tests run against: a hard cap of `hard_runs` runs, and byte budgets far above anything the fixtures encode, so
// only the run count forces a fold.
auto fold_test_limits(std::size_t const hard_runs) -> patch_chain_limits
{
    constexpr auto soft_encoded = std::uint64_t {1} << 20U;
    constexpr auto hard_encoded = std::uint64_t {1} << 24U;
    constexpr auto hard_decoded = std::uint64_t {1} << 26U;
    return patch_chain_limits {.soft_runs = hard_runs - 1U,
                               .soft_encoded_bytes = soft_encoded,
                               .hard_runs = hard_runs,
                               .hard_encoded_bytes = hard_encoded,
                               .hard_decoded_bytes = hard_decoded};
}

}  // namespace

// ── Equivalence ──────────────────────────────────────────────────────────────

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear equivalence sequence.
TEST_CASE("removals keep every answer equal to a rebuild of the remaining games (with a tree)", "[motif-db][pr7][removal][equivalence]")
{
    constexpr auto random_games = std::size_t {14};
    constexpr auto removal_count = std::size_t {10};
    constexpr auto reopen_seed_offset = 1000U;
    for (auto const seed : {801ULL, 802ULL}) {
        removal_bundle env {seed, random_games, /*tree=*/true};
        auto const initial_games = env.corpus.size();
        auto const checkpoint_name = env.manager->manifest().tree()->checkpoint.filename;
        auto const postings_name = env.manager->manifest().postings()->artifact.filename;
        auto const checkpoint_digest = file_digest(env.bundle / checkpoint_name);
        auto const postings_digest = file_digest(env.bundle / postings_name);
        auto rng = std::mt19937_64 {seed};  // NOLINT(cert-msc51-cpp,cert-msc32-c) -- a fixed seed keeps the fixtures reproducible.
        for (std::size_t step = 0; step < removal_count; ++step) {
            auto const position = std::uniform_int_distribution<std::size_t> {0, env.corpus.size() - 1}(rng);
            env.remove(position);
            env.require_equivalent(seed + step);
        }
        CHECK(env.corpus.size() == initial_games - removal_count);
        // No removal rebuilt anything: both base artifacts are the very files the bundle started with, byte for byte.
        CHECK(env.manager->manifest().tree()->checkpoint.filename == checkpoint_name);
        CHECK(env.manager->manifest().postings()->artifact.filename == postings_name);
        CHECK(file_digest(env.bundle / checkpoint_name) == checkpoint_digest);
        CHECK(file_digest(env.bundle / postings_name) == postings_digest);
        env.require_clean_directory();

        // Reopen: the published chain, not a rebuild, restores the same state.
        auto const generation = env.status().publication_generation;
        auto const payload_json = env.manager->manifest().payload_json;
        env.reopen();
        CHECK(env.status().publication_generation == generation);
        CHECK(env.manager->manifest().payload_json == payload_json);
        env.require_equivalent(seed + reopen_seed_offset);
        // The reopened bundle keeps removing incrementally.
        env.remove(0);
        env.require_equivalent(seed + reopen_seed_offset + 1U);
        env.require_clean_directory();
    }
}

TEST_CASE("removals on a postings-only bundle keep exact postings queryable and equal to a rebuild",
          "[motif-db][pr7][removal][equivalence]")
{
    constexpr auto seed = std::uint64_t {811U};
    constexpr auto random_games = std::size_t {12};
    constexpr auto removal_count = std::size_t {8};
    constexpr auto reopen_check_seed = seed + 1000U;
    removal_bundle env {seed, random_games, /*tree=*/false};
    auto const postings_name = env.manager->manifest().postings()->artifact.filename;
    auto rng = std::mt19937_64 {seed};  // NOLINT(cert-msc51-cpp,cert-msc32-c) -- a fixed seed keeps the fixtures reproducible.
    for (std::size_t step = 0; step < removal_count; ++step) {
        auto const position = std::uniform_int_distribution<std::size_t> {0, env.corpus.size() - 1}(rng);
        env.remove(position);
        CHECK(env.manager->manifest().tree() == nullptr);
        env.require_equivalent(seed + step);
    }
    CHECK(env.manager->manifest().postings()->artifact.filename == postings_name);
    env.require_clean_directory();
    env.reopen();
    env.require_equivalent(reopen_check_seed);
}

// Removing the first, a middle and the last game of the id space, in an order that leaves gaps at both ends.
TEST_CASE("removing the first, a middle and the last game leaves a correct, gapped id space", "[motif-db][pr7][removal][equivalence]")
{
    constexpr auto seed = std::uint64_t {821U};
    constexpr auto random_games = std::size_t {10};
    for (auto const tree : {true, false}) {
        removal_bundle env {seed, random_games, tree};
        auto const last = env.corpus.size() - 1;
        auto const middle = env.corpus.size() / 2;
        env.remove(last);
        env.require_equivalent(seed);
        env.remove(middle);
        env.require_equivalent(seed + 1U);
        env.remove(0);
        env.require_equivalent(seed + 2U);
        env.require_clean_directory();
        env.reopen();
        env.require_equivalent(seed + 3U);
        // An explicit rebuild over the gapped id space answers the same (the retracted games are physically gone from the new
        // artifacts), and incremental removal continues from it.
        env.rebuild_all();
        env.require_equivalent(seed + 4U);
        env.require_clean_directory();
        env.remove(0);
        env.require_equivalent(seed + 5U);  // NOLINT(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers) -- step label
    }
}

// ── Shared positions and aggregates ──────────────────────────────────────────

namespace
{

auto stat_row_for(std::vector<opening_stat_agg_row> const& rows, std::uint16_t const move) -> opening_stat_agg_row const*
{
    auto const found =
        std::ranges::find_if(rows, [move](opening_stat_agg_row const& row) -> bool { return row.cont_encoded_move == move; });
    return found == rows.end() ? nullptr : &*found;
}

}  // namespace

// Three of five games share the e4 e5 Nf3 prefix and two of them the whole line: a removal must retract exactly the removed game's
// occurrences and contribution, never the shared position itself.
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear sequence of position checks.
TEST_CASE("removing a game leaves positions shared with other games, and every aggregate over them, correct",
          "[motif-db][pr7][removal][shared]")
{
    constexpr auto seed = std::uint64_t {831U};
    constexpr auto elo_high = 2400;
    constexpr auto elo_mid = 2200;
    for (auto const tree : {true, false}) {
        auto const line_a = uci_moves({"e2e4", "e7e5", "g1f3", "b8c6"});
        auto const line_b = uci_moves({"e2e4", "e7e5", "g1f3", "g8f6"});
        auto const sicilian = uci_moves({"e2e4", "c7c5"});
        auto const queens_pawn = uci_moves({"d2d4", "d7d5"});
        // Ids 1..5: A, B, C, D and E (a duplicate of A's moves with another result and ratings).
        auto games = std::vector<model_game> {
            {.moves = line_a, .result = "1-0", .white_elo = elo_high, .black_elo = elo_mid},
            {.moves = line_b, .result = "0-1", .white_elo = elo_mid, .black_elo = elo_high},
            {.moves = sicilian, .result = "1/2-1/2", .white_elo = elo_mid, .black_elo = std::nullopt},
            {.moves = queens_pawn, .result = "1-0", .white_elo = std::nullopt, .black_elo = std::nullopt},
            {.moves = line_a, .result = "0-1", .white_elo = elo_high, .black_elo = elo_high},
        };
        removal_bundle env {std::move(games), tree};
        auto const start = start_hash();
        auto const after_e4 = hash_after(line_a, 1);
        auto const after_nf3 = hash_after(line_a, 3);
        auto const after_nc6 = hash_after(line_a, 4);
        auto const after_nf6 = hash_after(line_b, 4);
        auto const after_c5 = hash_after(sicilian, 2);
        auto const e4_move = line_a.front();
        auto const d4_move = queens_pawn.front();

        auto const distinct_at = [&env](std::uint64_t const hash) -> std::uint32_t
        {
            auto const summary = env.manager->position_summary(zobrist_hash {hash});
            REQUIRE(summary.has_value());
            return summary->has_value() ? summary->value().distinct_game_count : 0U;
        };
        auto const games_at = [&env](std::uint64_t const hash) -> std::vector<game_id>
        {
            auto listed = env.manager->position_game_ids(zobrist_hash {hash});
            REQUIRE(listed.has_value());
            return *listed;
        };

        REQUIRE(distinct_at(start) == 5U);
        REQUIRE(distinct_at(after_e4) == 4U);
        REQUIRE(distinct_at(after_nf3) == 3U);
        REQUIRE(distinct_at(after_nc6) == 2U);
        REQUIRE(distinct_at(after_nf6) == 1U);

        // Remove A: B and E still reach Nf3, E alone still reaches Nc6.
        env.remove(0);
        CHECK(distinct_at(start) == 4U);
        CHECK(distinct_at(after_e4) == 3U);
        CHECK(distinct_at(after_nf3) == 2U);
        CHECK(games_at(after_nf3) == std::vector<game_id> {game_id {2U}, game_id {5U}});
        CHECK(distinct_at(after_nc6) == 1U);
        CHECK(games_at(after_nc6) == std::vector<game_id> {game_id {5U}});
        CHECK(distinct_at(after_nf6) == 1U);
        {
            auto const stats = env.manager->query_unfiltered_opening_stats(zobrist_hash {start});
            REQUIRE(stats.has_value());
            auto const* e4_row = stat_row_for(*stats, e4_move);
            REQUIRE(e4_row != nullptr);
            // B (0-1), C (draw) and E (0-1) remain; A's win is gone.
            CHECK(e4_row->frequency == 3);
            CHECK(e4_row->white_wins == 0);
            CHECK(e4_row->draws == 1);
            CHECK(e4_row->black_wins == 2);
            auto const* d4_row = stat_row_for(*stats, d4_move);
            REQUIRE(d4_row != nullptr);
            CHECK(d4_row->frequency == 1);
        }
        env.require_equivalent(seed);

        // Remove E (now the last game): the Nc6 position, reached by nobody else, disappears; the shared Nf3 position survives via B.
        env.remove(env.corpus.size() - 1);
        CHECK(distinct_at(after_nc6) == 0U);
        CHECK(games_at(after_nc6).empty());
        CHECK(distinct_at(after_nf3) == 1U);
        CHECK(games_at(after_nf3) == std::vector<game_id> {game_id {2U}});
        env.require_equivalent(seed + 1U);

        // Remove B: the whole e4 e5 branch goes; the Sicilian and the queen's pawn games remain.
        env.remove(0);
        CHECK(distinct_at(after_nf3) == 0U);
        CHECK(distinct_at(after_nf6) == 0U);
        CHECK(distinct_at(after_e4) == 1U);
        CHECK(distinct_at(after_c5) == 1U);
        CHECK(distinct_at(start) == 2U);
        {
            auto const stats = env.manager->query_unfiltered_opening_stats(zobrist_hash {start});
            REQUIRE(stats.has_value());
            CHECK(stats->size() == 2U);
            auto const* e4_row = stat_row_for(*stats, e4_move);
            REQUIRE(e4_row != nullptr);
            CHECK(e4_row->frequency == 1);
            CHECK(e4_row->draws == 1);
            CHECK(stat_row_for(*stats, d4_move) != nullptr);
        }
        env.require_equivalent(seed + 2U);
        env.reopen();
        CHECK(distinct_at(start) == 2U);
        CHECK(distinct_at(after_nf3) == 0U);
        env.require_equivalent(seed + 3U);
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertions inflate this linear failure check.
TEST_CASE("opening stats report a damaged postings file after removal", "[motif-db][pr7][removal][corruption]")
{
    auto const line = uci_moves({"e2e4", "e7e5"});
    auto const other = uci_moves({"d2d4", "d7d5"});
    auto games = std::vector<model_game> {
        {.moves = line, .result = "1-0"},
        {.moves = other, .result = "0-1"},
    };
    removal_bundle env {std::move(games), /*tree=*/true};
    auto const position = zobrist_hash {hash_after(line, 1)};
    REQUIRE(env.manager->remove_game(env.ids.back()).has_value());
    auto const* postings = env.manager->manifest().postings();
    REQUIRE(postings != nullptr);
    fs::resize_file(env.bundle / postings->artifact.filename, 0);

    auto const summary = env.manager->position_summary(position);
    REQUIRE_FALSE(summary.has_value());
    auto const stats = env.manager->query_unfiltered_opening_stats(position);
    REQUIRE_FALSE(stats.has_value());
    CHECK(stats.error().code == summary.error().code);
}

// ── Edge cases ───────────────────────────────────────────────────────────────

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear sweep.
TEST_CASE("every hand-built fixture game removes correctly: zero moves, a mate, a beyond-horizon return, repeated edges, rating extremes",
          "[motif-db][pr7][removal][fixtures]")
{
    constexpr auto seed = std::uint64_t {841U};
    constexpr auto fixture_count = std::size_t {5};
    for (auto const tree : {true, false}) {
        for (std::size_t first = 0; first < fixture_count; ++first) {
            removal_bundle env {seed + first, /*random_games=*/0, tree};
            REQUIRE(env.corpus.size() == fixture_count);
            env.remove(first);
            env.require_equivalent(seed + first);
            // Then the rest, front to back, until one game remains.
            while (env.corpus.size() > 1) {
                env.remove(0);
                // Keep phase-two labels clear of phase one.
                // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
                env.require_equivalent(seed + first + 1000U + env.corpus.size());
            }
            env.require_clean_directory();
        }
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear sequence.
TEST_CASE("removing every game leaves valid, empty, current indexes that survive a reopen and accept new games after a rebuild",
          "[motif-db][pr7][removal][empty]")
{
    constexpr auto seed = std::uint64_t {851U};
    for (auto const tree : {true, false}) {
        removal_bundle env {seed, /*random_games=*/0, tree};
        while (env.corpus.size() > 1) {
            env.remove(env.corpus.size() - 1);
            env.require_equivalent(seed + env.corpus.size());
        }
        env.remove(0);
        REQUIRE(env.corpus.empty());
        require_empty_answers(env);
        env.require_clean_directory();

        env.reopen();
        require_empty_answers(env);

        // A later insert is not incremental: it leaves the indexes stale until a rebuild, which then serves the new game alone.
        auto const added = env.insert(fixture_games()[4]);
        CHECK_FALSE(env.status().postings_usable);
        CHECK(added.value > 5U);  // removal never recycles an id
        env.rebuild_all();
        env.require_equivalent(seed + 99U);  // NOLINT(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers) -- step label
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear sequence.
TEST_CASE("removing an absent or already-removed game is not_found and changes nothing", "[motif-db][pr7][removal][absent]")
{
    constexpr auto seed = std::uint64_t {861U};
    constexpr auto random_games = std::size_t {6};
    constexpr auto absent_game = 99'999U;
    for (auto const tree : {true, false}) {
        removal_bundle env {seed, random_games, tree};
        auto before = snapshot_of(env);
        auto const absent = env.manager->remove_game(game_id {absent_game});
        REQUIRE_FALSE(absent.has_value());
        CHECK(absent.error() == error_code::not_found);
        require_unchanged(env, before);
        CHECK_FALSE(env.manager->last_removal_report().published);

        auto const victim = env.ids[1];
        env.remove(1);
        before = snapshot_of(env);
        auto const again = env.manager->remove_game(victim);
        REQUIRE_FALSE(again.has_value());
        CHECK(again.error() == error_code::not_found);
        require_unchanged(env, before);
        CHECK_FALSE(env.manager->last_removal_report().published);
        env.require_equivalent(seed);
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear sequence.
TEST_CASE("remove_user_game removes a manual game incrementally and still refuses an imported one", "[motif-db][pr7][removal][policy]")
{
    constexpr auto seed = std::uint64_t {871U};
    constexpr auto random_games = std::size_t {8};
    removal_bundle env {seed, random_games, /*tree=*/true};
    REQUIRE(env.manager->set_manual_game_provenance(env.ids[0], std::nullopt, "new").has_value());

    auto const before = snapshot_of(env);
    auto const imported = env.manager->remove_user_game(env.ids[3]);
    REQUIRE_FALSE(imported.has_value());
    CHECK(imported.error() == error_code::not_editable);
    require_unchanged(env, before);
    CHECK(env.manager->store().get(env.ids[3]).has_value());

    auto const manual = env.ids[0];
    auto const removed = env.manager->remove_user_game(manual);
    REQUIRE(removed.has_value());
    CHECK(env.status().mutation_seq == before.status.mutation_seq + 1U);
    CHECK(env.status().publication_generation == before.status.publication_generation + 1U);
    CHECK(env.status().postings_usable);
    CHECK(env.status().tree_usable);
    env.corpus.erase(env.corpus.begin());
    env.ids.erase(env.ids.begin());
    env.require_absent(manual);
    env.require_equivalent(seed);
    env.require_clean_directory();
}

// ── Interaction with edits ───────────────────────────────────────────────────

// A removal must retract the game's CURRENT indexed values, so the contribution a prior metadata correction installed is the one that
// leaves, and an edit after a removal still finds consistent indexes.
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear sequence.
TEST_CASE("removal retracts the corrected values of an edited game, and edits after a removal stay correct",
          "[motif-db][pr7][removal][edit]")
{
    constexpr auto seed = std::uint64_t {881U};
    constexpr auto random_games = std::size_t {10};
    constexpr auto corrected_white = 2222;
    constexpr auto corrected_black = 1111;
    for (auto const tree : {true, false}) {
        removal_bundle env {seed, random_games, tree};
        auto const edit = [&env, corrected_white, corrected_black](std::size_t const position, bool const change_ratings) -> void
        {
            auto after = env.corpus.at(position);
            after.result = flipped_result(after.result);
            if (change_ratings) {
                after.white_elo = corrected_white;
                after.black_elo = corrected_black;
            }
            auto const patched = env.manager->patch_game_metadata(env.ids.at(position), patch_for(env.corpus.at(position), after));
            REQUIRE(patched.has_value());
            env.corpus.at(position) = after;
        };

        edit(3, true);
        // NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers) -- arbitrary corpus positions
        edit(8, false);
        edit(8, true);
        env.require_equivalent(seed);
        env.remove(8);  // an edited game
        // NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
        env.require_equivalent(seed + 1U);
        env.remove(3);  // another edited game
        env.require_equivalent(seed + 2U);
        edit(3, true);  // a different game now sits at this position
        env.require_equivalent(seed + 3U);
        env.remove(0);
        edit(0, false);
        env.require_equivalent(seed + 4U);
        env.require_clean_directory();
        env.reopen();
        env.require_equivalent(seed + 5U);  // NOLINT(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers) -- step label
    }
}

// ── Stale baseline and inserts ───────────────────────────────────────────────

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear sequence.
TEST_CASE("a removal on indexes that do not cover the bundle stays canonical-only and a rebuild restores incremental removal",
          "[motif-db][pr7][removal][stale]")
{
    constexpr auto seed = std::uint64_t {891U};
    constexpr auto random_games = std::size_t {8};
    for (auto const tree : {true, false}) {
        removal_bundle env {seed, random_games, tree};
        // An insert the indexes do not cover yet makes the baseline stale.
        env.insert(fixture_games()[3]);
        REQUIRE_FALSE(env.status().postings_usable);
        auto const before = snapshot_of(env);

        auto const victim = env.ids[2];
        REQUIRE(env.manager->remove_game(victim).has_value());
        env.corpus.erase(env.corpus.begin() + 2);
        env.ids.erase(env.ids.begin() + 2);
        // The row went, the sequence advanced, and no snapshot was published: the indexes stay exactly as stale as they were.
        CHECK(env.manager->store().get(victim).error() == error_code::not_found);
        CHECK(env.status().mutation_seq == before.status.mutation_seq + 1U);
        CHECK(env.status().publication_generation == before.status.publication_generation);
        CHECK_FALSE(env.status().postings_usable);
        CHECK_FALSE(env.status().tree_usable);
        CHECK(env.manager->manifest().payload_json == before.payload_json);
        CHECK(artifact_names(env.bundle) == before.files);
        CHECK_FALSE(env.manager->query_position_matches(zobrist_hash {start_hash()}).has_value());
        CHECK(env.manager->last_removal_report().canonical_only);
        CHECK_FALSE(env.manager->last_removal_report().published);

        env.rebuild_all();
        env.require_equivalent(seed);
        // Incremental removal is back.
        env.remove(0);
        env.require_equivalent(seed + 1U);
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear sequence.
TEST_CASE("a removal followed by an insert that restores the game count still fails closed until a rebuild",
          "[motif-db][pr7][removal][stale]")
{
    constexpr auto seed = std::uint64_t {901U};
    constexpr auto random_games = std::size_t {6};
    for (auto const tree : {true, false}) {
        removal_bundle env {seed, random_games, tree};
        env.remove(2);  // incremental: the indexes now describe one game fewer
        env.require_equivalent(seed);
        auto const count_after_removal = env.corpus.size();

        env.insert(fixture_games()[4]);  // the count is back to what it was before the removal
        REQUIRE(env.corpus.size() == count_after_removal + 1U);
        CHECK_FALSE(env.status().postings_usable);
        CHECK_FALSE(env.status().tree_usable);
        auto const summary = env.manager->position_summary(zobrist_hash {start_hash()});
        REQUIRE_FALSE(summary.has_value());
        CHECK(summary.error() == error_code::io_failure);
        CHECK_FALSE(env.manager->query_position_matches(zobrist_hash {start_hash()}).has_value());
        CHECK_FALSE(env.manager->query_unfiltered_opening_stats(zobrist_hash {start_hash()}).has_value());
        // Ordinary canonical reads stay available.
        CHECK(env.manager->find_games(search_filter {}).has_value());

        env.rebuild_all();
        env.require_equivalent(seed + 1U);
    }
}

// ── Chain caps ───────────────────────────────────────────────────────────────

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear sequence.
TEST_CASE("removals fold the chains at the hard cap instead of publishing an oversized chain", "[motif-db][pr7][removal][cap]")
{
    constexpr auto seed = std::uint64_t {911U};
    constexpr auto random_games = std::size_t {14};
    constexpr auto removal_count = std::size_t {12};
    constexpr auto hard_runs = std::size_t {3};
    removal_bundle env {seed, random_games, /*tree=*/true};
    env.manager->set_chain_limits(fold_test_limits(hard_runs));
    auto const original_checkpoint = env.manager->manifest().tree()->checkpoint.filename;
    auto rng = std::mt19937_64 {seed};  // NOLINT(cert-msc51-cpp,cert-msc32-c) -- a fixed seed keeps the fixtures reproducible.
    auto folds = 0;
    for (std::size_t step = 0; step < removal_count; ++step) {
        auto const position = std::uniform_int_distribution<std::size_t> {0, env.corpus.size() - 1}(rng);
        env.remove(position);
        // Never over the cap, whatever the removal published or folded.
        CHECK(env.manager->manifest().tree()->patch_runs.size() <= hard_runs);
        CHECK(env.manager->manifest().payload.metadata.runs.size() <= hard_runs);
        if (env.manager->last_removal_report().tree_folded) {
            ++folds;
            CHECK(env.manager->last_removal_report().tree_runs == 0U);
        }
        env.require_clean_directory();  // superseded checkpoints and runs were collected
        env.require_equivalent(seed + step);
    }
    // Twelve removals against a cap of three runs must have folded the tree at least once.
    CHECK(env.manager->manifest().tree()->checkpoint.filename != original_checkpoint);
    CHECK(folds >= 1);
    CHECK_FALSE(artifact_names(env.bundle).contains(original_checkpoint));
    env.reopen();
    // NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers) -- seed + n labels one equivalence check per step
    env.require_equivalent(seed + 100U);
    env.remove(0);
    env.require_equivalent(seed + 101U);
    // NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
}

// ── Durability: SQLite is authoritative, manifest.json is only a cache ───────

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear sequence.
TEST_CASE("a published removal survives a reopen even when manifest.json is stale or missing", "[motif-db][pr7][removal][durable]")
{
    constexpr auto seed = std::uint64_t {921U};
    constexpr auto random_games = std::size_t {10};
    removal_bundle env {seed, random_games, /*tree=*/true};
    auto const stale_mirror = read_file(env.bundle / "manifest.json");
    REQUIRE_FALSE(stale_mirror.empty());
    auto const first_victim = env.ids[1];
    auto const second_victim = env.ids[6];
    env.remove(6);  // NOLINT(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers) -- an arbitrary corpus position
    env.remove(1);
    auto const generation = env.status().publication_generation;
    auto const seq = env.status().mutation_seq;
    auto const payload_json = env.manager->manifest().payload_json;
    env.manager.reset();

    // The mirror from before the removals: SQLite's published_snapshot row wins, nothing is rebuilt and the removed games stay removed.
    write_file(env.bundle / "manifest.json", stale_mirror);
    env.reopen();
    CHECK(env.status().publication_generation == generation);
    CHECK(env.status().mutation_seq == seq);
    CHECK(env.manager->manifest().payload_json == payload_json);
    CHECK(env.status().postings_usable);
    CHECK(env.status().tree_usable);
    env.require_absent(first_victim);
    env.require_absent(second_victim);
    env.require_equivalent(seed);

    // No mirror at all.
    env.manager.reset();
    fs::remove(env.bundle / "manifest.json");
    env.reopen();
    CHECK(env.status().publication_generation == generation);
    CHECK(env.manager->manifest().payload_json == payload_json);
    env.require_equivalent(seed + 1U);
    env.require_clean_directory();
}

// The removal's DELETE is durable canonical state: an independent SQLite connection on the bundle already sees it.
TEST_CASE("a second connection sees the removal as committed canonical state", "[motif-db][pr7][removal][durable]")
{
    constexpr auto seed = std::uint64_t {931U};
    constexpr auto random_games = std::size_t {6};
    removal_bundle env {seed, random_games, /*tree=*/false};
    auto const victim = env.ids[2];
    auto const total = env.corpus.size();
    env.remove(2);
    probe_connection const probe {env.bundle};
    CHECK(probe.scalar("SELECT COUNT(*) FROM game") == static_cast<std::int64_t>(total - 1U));
    CHECK(probe.scalar("SELECT COUNT(*) FROM game WHERE id = " + std::to_string(victim.value)) == 0);
}

// ── Failure atomicity: the removal is one transaction or nothing ─────────────

namespace
{

namespace fault = motif::db::fault;
using fault::fault_action;
using fault::point;

// The fault hook is a plain function pointer, so the arms and the trace it reads are process-global by necessity.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
struct fault_arm
{
    point at {};
    fault_action action {fault_action::fail_io};
    // The nth crossing of the boundary is the one that fires.
    std::size_t nth {1};
    std::size_t seen {0};
};

constexpr auto max_trace = std::size_t {1024};
std::array<fault_arm, 2> g_arms {};
std::size_t g_arm_count {0};
std::array<point, max_trace> g_trace {};
std::size_t g_trace_count {0};

// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

auto trace_hook(point const where, fault::artifact_kind /*kind*/, std::uint64_t /*generation*/) noexcept -> fault_action
{
    if (g_trace_count < max_trace) {
        g_trace.at(g_trace_count++) = where;
    }
    for (std::size_t index = 0; index < g_arm_count; ++index) {
        auto& arm = g_arms.at(index);
        if (arm.at == where && ++arm.seen == arm.nth) {
            return arm.action;
        }
    }
    return fault_action::proceed;
}

// Installs the recording hook, arms up to two boundaries, restores the previous hook afterwards.
class armed_faults
{
  public:
    armed_faults(std::initializer_list<fault_arm> arms)
        : previous_ {fault::set_hook(trace_hook)}
    {
        g_trace_count = 0;
        g_arm_count = 0;
        for (auto const& arm : arms) {
            g_arms.at(g_arm_count++) = arm;
        }
    }

    ~armed_faults()
    {
        fault::set_hook(previous_);
        g_arm_count = 0;
    }

    armed_faults(armed_faults const&) = delete;
    auto operator=(armed_faults const&) -> armed_faults& = delete;
    armed_faults(armed_faults&&) = delete;
    auto operator=(armed_faults&&) -> armed_faults& = delete;

  private:
    fault::hook_function previous_ {nullptr};
};

auto crossings_of(point const where) -> std::size_t
{
    return static_cast<std::size_t>(
        std::count(g_trace.begin(), std::next(g_trace.begin(), static_cast<std::ptrdiff_t>(g_trace_count)), where));
}

// A removal that failed left the game, the sequence, the snapshot, the files and every answer as they were.
void require_removal_rolled_back(removal_bundle const& env,
                                 state_snapshot const& before,
                                 game_id const victim,
                                 std::uint64_t const revision)
{
    require_unchanged(env, before);
    CHECK(env.manager->store().get(victim).has_value());
    CHECK(env.manager->data_revision() == revision);
    CHECK_FALSE(env.manager->last_removal_report().published);
}

struct boundary
{
    point at;
    std::size_t nth;
};

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear boundary matrix.
TEST_CASE("a failure at any boundary before COMMIT leaves a removal's entire old tuple and removes the attempt's files",
          "[motif-db][pr7][removal][failure]")
{
    auto const boundaries = std::vector<boundary> {
        {point::before_build, 1},
        {point::after_build, 1},
        {point::before_disk_admission, 1},
        {point::before_artifact_validation, 1},
        {point::after_artifact_validation, 1},
        {point::before_artifact_fsync, 1},
        {point::before_artifact_fsync, 2},  // the tombstone run is staged; the tree run follows
        {point::after_artifact_fsync, 1},
        {point::after_artifact_fsync, 2},
        {point::before_artifact_rename, 1},
        {point::after_artifact_rename, 1},
        {point::before_artifact_directory_fsync, 1},
        {point::after_artifact_directory_fsync, 1},
        {point::before_synchronous_full, 1},
        {point::before_snapshot_begin, 1},
        {point::after_canonical_update, 1},  // the DELETE ran inside the transaction
        {point::after_sequence_bump, 1},
        {point::after_snapshot_row_write, 1},
        {point::before_sqlite_commit, 1},
    };
    constexpr auto seed = std::uint64_t {941U};
    constexpr auto random_games = std::size_t {8};
    constexpr auto victim_position = std::size_t {3};
    for (auto const tree : {true, false}) {
        for (auto const& boundary : boundaries) {
            // Only a tree stages a second candidate (its retraction run) and validates a candidate tree reader; a postings-only removal
            // stages just the tombstone run.
            auto const validates = boundary.at == point::before_artifact_validation || boundary.at == point::after_artifact_validation;
            if (!tree && (boundary.nth > 1U || validates)) {
                continue;
            }
            INFO("boundary " << static_cast<int>(boundary.at) << " crossing " << boundary.nth << (tree ? " (tree)" : " (postings only)"));
            removal_bundle env {seed, random_games, tree};
            auto const victim = env.ids.at(victim_position);
            auto const before = snapshot_of(env);
            auto const revision = env.manager->data_revision();

            auto outcome = result<void> {};
            {
                auto const faults = armed_faults {{.at = boundary.at, .action = fault_action::fail_io, .nth = boundary.nth}};
                outcome = env.manager->remove_game(victim);
                REQUIRE(crossings_of(boundary.at) >= boundary.nth);  // the boundary really was crossed
            }
            REQUIRE_FALSE(outcome.has_value());
            require_removal_rolled_back(env, before, victim, revision);
            env.require_equivalent(seed);  // every answer still describes the whole corpus
            auto const count = env.manager->store().count_games();
            REQUIRE(count.has_value());
            CHECK(std::cmp_equal(*count, env.corpus.size()));

            // The manager is healthy afterwards: the same removal now succeeds and the game is gone everywhere.
            env.remove(victim_position);
            CHECK(env.manager->data_revision() > revision);
            env.require_equivalent(seed + 1U);
            env.require_clean_directory();
        }
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear crossing trace.
TEST_CASE("a removal crosses COMMIT exactly once, with the DELETE inside the same transaction", "[motif-db][pr7][removal][failure]")
{
    constexpr auto seed = std::uint64_t {951U};
    constexpr auto random_games = std::size_t {6};
    for (auto const tree : {true, false}) {
        removal_bundle env {seed, random_games, tree};
        {
            auto const faults = armed_faults {};
            env.remove(2);
            CHECK(crossings_of(point::before_sqlite_commit) == 1U);
            CHECK(crossings_of(point::after_sqlite_commit) == 1U);
            CHECK(crossings_of(point::after_canonical_update) == 1U);
            CHECK(crossings_of(point::after_sequence_bump) == 1U);
            CHECK(crossings_of(point::before_canonical_commit) == 0U);  // no separate canonical transaction
            auto const first = [](point const where) -> std::size_t
            {
                auto* const end = std::next(g_trace.begin(), static_cast<std::ptrdiff_t>(g_trace_count));
                return static_cast<std::size_t>(std::find(g_trace.begin(), end, where) - g_trace.begin());
            };
            // The DELETE, then the one sequence advance, then the snapshot row, then COMMIT -- in that order.
            CHECK(first(point::after_canonical_update) < first(point::after_sequence_bump));
            CHECK(first(point::after_sequence_bump) < first(point::after_snapshot_row_write));
            CHECK(first(point::after_snapshot_row_write) < first(point::before_sqlite_commit));
            CHECK(first(point::before_sqlite_commit) < first(point::after_sqlite_commit));
        }
        env.require_equivalent(seed);
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear outcome matrix.
TEST_CASE("an ambiguous COMMIT of a removal reconciles to the true outcome and never serves the game against its tombstone",
          "[motif-db][pr7][removal][failure][ambiguous]")
{
    constexpr auto random_games = std::size_t {8};
    constexpr auto victim_position = std::size_t {2};
    SECTION("known new: the row landed after all")
    {
        constexpr auto seed = std::uint64_t {961U};
        removal_bundle env {seed, random_games, /*tree=*/true};
        {
            auto const faults = armed_faults {{.at = point::before_sqlite_commit, .action = fault_action::commit_error_after_commit}};
            env.remove(victim_position);  // succeeds: the stored row proves the commit
        }
        env.require_equivalent(seed + 1U);
        env.require_clean_directory();
        env.reopen();
        env.require_equivalent(seed + 2U);
    }
    SECTION("known old: the transaction was discarded")
    {
        constexpr auto seed = std::uint64_t {962U};
        removal_bundle env {seed, random_games, /*tree=*/true};
        auto const victim = env.ids.at(victim_position);
        auto const before = snapshot_of(env);
        auto const revision = env.manager->data_revision();
        {
            auto const faults = armed_faults {{.at = point::before_sqlite_commit, .action = fault_action::commit_error_after_rollback}};
            REQUIRE_FALSE(env.manager->remove_game(victim).has_value());
        }
        require_removal_rolled_back(env, before, victim, revision);
        env.require_equivalent(seed + 1U);
        env.remove(victim_position);
        env.require_equivalent(seed + 2U);
    }
    SECTION("unknown, but the row landed: fail closed, keep every candidate, recover on reopen")
    {
        constexpr auto seed = std::uint64_t {963U};
        removal_bundle env {seed, random_games, /*tree=*/true};
        auto const victim = env.ids.at(victim_position);
        auto const files_before = artifact_names(env.bundle);
        {
            auto const faults = armed_faults {{.at = point::before_sqlite_commit, .action = fault_action::commit_error_after_commit},
                                              {.at = point::before_commit_reconcile, .action = fault_action::fail_io}};
            auto const failed = env.manager->remove_game(victim);
            REQUIRE_FALSE(failed.has_value());
            CHECK(failed.error() == error_code::io_failure);
        }
        // The game is gone from SQLite but the in-memory snapshot is the old one: no query may combine the two.
        CHECK(env.manager->store().get(victim).error() == error_code::not_found);
        CHECK_FALSE(env.status().postings_usable);
        CHECK_FALSE(env.manager->query_position_matches(zobrist_hash {start_hash()}).has_value());
        CHECK_FALSE(env.manager->query_unfiltered_opening_stats(zobrist_hash {start_hash()}).has_value());
        // The candidate files may be referenced by the landed row: none may be deleted.
        auto const files_now = artifact_names(env.bundle);
        CHECK(files_now.size() > files_before.size());
        for (auto const& name : files_before) {
            CHECK(files_now.contains(name));
        }
        env.corpus.erase(env.corpus.begin() + static_cast<std::ptrdiff_t>(victim_position));
        env.ids.erase(env.ids.begin() + static_cast<std::ptrdiff_t>(victim_position));
        env.reopen();  // the row proves ownership: the chain with the tombstone loads without a rebuild
        env.require_absent(victim);
        env.require_equivalent(seed + 1U);
        env.require_clean_directory();
    }
    SECTION("unknown, but the transaction was discarded: the old state keeps serving, candidates wait for collection")
    {
        constexpr auto seed = std::uint64_t {964U};
        removal_bundle env {seed, random_games, /*tree=*/true};
        auto const victim = env.ids.at(victim_position);
        auto const before = snapshot_of(env);
        {
            auto const faults = armed_faults {{.at = point::before_sqlite_commit, .action = fault_action::commit_error_after_rollback},
                                              {.at = point::before_commit_reconcile, .action = fault_action::fail_io}};
            REQUIRE_FALSE(env.manager->remove_game(victim).has_value());
        }
        CHECK(env.manager->store().get(victim).has_value());
        CHECK(env.status().mutation_seq == before.status.mutation_seq);
        CHECK(artifact_names(env.bundle).size() > before.files.size());  // candidates preserved, never guessed away
        env.require_equivalent(seed + 1U);
        // The next successful publication collects them.
        env.remove(victim_position);
        env.require_equivalent(seed + 2U);
        env.require_clean_directory();
    }
}

TEST_CASE("failures after COMMIT never undo a committed removal", "[motif-db][pr7][removal][failure][postcommit]")
{
    auto const points = {point::after_sqlite_commit,
                         point::before_synchronous_restore,
                         point::before_snapshot_install,
                         point::after_snapshot_install,
                         point::before_manifest_write,
                         point::after_manifest_temp_write,
                         point::after_manifest_temp_fsync,
                         point::before_manifest_rename,
                         point::after_manifest_rename,
                         point::before_manifest_directory_fsync,
                         point::after_manifest_write};
    constexpr auto seed = std::uint64_t {971U};
    constexpr auto random_games = std::size_t {8};
    constexpr auto victim_position = std::size_t {4};
    for (auto const failing : points) {
        INFO("boundary " << static_cast<int>(failing));
        removal_bundle env {seed, random_games, /*tree=*/true};
        auto const victim = env.ids.at(victim_position);
        {
            auto const faults = armed_faults {{.at = failing, .action = fault_action::fail_io}};
            env.remove(victim_position);  // REQUIREs success and one more sequence, generation and snapshot
            CHECK(crossings_of(failing) >= 1U);
        }
        env.require_absent(victim);
        env.require_equivalent(seed);
        // The mirror is a cache: whatever state it was left in, reopen repairs it from SQLite.
        env.reopen();
        CHECK(manifest_cache_current(env.bundle / "manifest.json", env.manager->manifest()));
        env.require_absent(victim);
        env.require_equivalent(seed + 1U);
    }
}

// ── Crash tests: a forked child dies at the boundary, the parent reopens ─────

#ifndef _WIN32
namespace
{

// The status the crash hook's child exits with, so the parent can tell a crash at the boundary from any other exit.
constexpr auto crash_exit_code = 77;

point g_crash_at {};  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables) -- read by the plain-function crash hook.

auto crash_hook(point const where, fault::artifact_kind /*kind*/, std::uint64_t /*generation*/) noexcept -> fault_action
{
    if (where == g_crash_at) {
        // The process dies right here: no destructor, no close(), no cache write, no cleanup.
        _exit(crash_exit_code);
    }
    return fault_action::proceed;
}

// Opens the bundle in a child, arms the crash and removes the game. Returns the child's exit code.
auto remove_in_crashing_child(fs::path const& bundle, point const crash_point, game_id const victim) -> int
{
    g_crash_at = crash_point;
    auto const pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        auto manager = database_manager::open(bundle);
        if (!manager) {
            _exit(2);
        }
        fault::set_hook(crash_hook);
        auto const outcome = manager->remove_game(victim);
        _exit(outcome.has_value() ? 0 : 3);
    }
    auto status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear crash matrix.
TEST_CASE("a process dying at any boundary of a removal leaves either the entire old tuple or the entire new tuple",
          "[motif-db][pr7][removal][failure][crash]")
{
    struct crash_case
    {
        point at;
        bool expect_new;
    };

    auto const cases = std::vector<crash_case> {
        {point::before_build, false},
        {point::after_build, false},
        {point::after_artifact_fsync, false},
        {point::after_artifact_rename, false},
        {point::after_artifact_directory_fsync, false},
        {point::before_snapshot_begin, false},
        {point::after_canonical_update, false},
        {point::after_sequence_bump, false},
        {point::after_snapshot_row_write, false},
        {point::before_sqlite_commit, false},
        {point::after_sqlite_commit, true},
        {point::before_synchronous_restore, true},
        {point::before_snapshot_install, true},
        {point::after_snapshot_install, true},
        {point::before_manifest_write, true},
        {point::after_manifest_write, true},
    };
    constexpr auto seed = std::uint64_t {981U};
    constexpr auto random_games = std::size_t {8};
    constexpr auto victim_position = std::size_t {3};
    for (auto const tree : {true, false}) {
        for (auto const& crash : cases) {
            INFO("crash at " << static_cast<int>(crash.at) << (tree ? " (tree)" : " (postings only)"));
            removal_bundle env {seed, random_games, tree};
            auto const victim = env.ids.at(victim_position);
            auto const old_seq = env.status().mutation_seq;
            auto const old_generation = env.status().publication_generation;
            env.manager.reset();  // the child owns the bundle

            REQUIRE(remove_in_crashing_child(env.bundle, crash.at, victim) == crash_exit_code);

            env.reopen();
            auto const recovered = env.status();
            REQUIRE(recovered.postings_usable);
            CHECK(recovered.tree_usable == tree);
            if (crash.expect_new) {
                env.corpus.erase(env.corpus.begin() + static_cast<std::ptrdiff_t>(victim_position));
                env.ids.erase(env.ids.begin() + static_cast<std::ptrdiff_t>(victim_position));
                CHECK(recovered.mutation_seq == old_seq + 1U);
                CHECK(recovered.publication_generation == old_generation + 1U);
                env.require_absent(victim);
            } else {
                // Not one field of the tuple moved: the game is still there, in SQLite and in every answer.
                CHECK(recovered.mutation_seq == old_seq);
                CHECK(recovered.publication_generation == old_generation);
                CHECK(env.manager->store().get(victim).has_value());
            }
            env.require_equivalent(seed);
            // Recovery needed no rebuild, and the next removal is incremental again.
            env.remove(0);
            env.require_equivalent(seed + 1U);
        }
    }
}
#endif

// ── Repair: a lost or damaged chain is rebuilt from canonical SQLite ─────────

namespace
{

void flip_last_bytes(fs::path const& path)
{
    auto bytes = read_file(path);
    REQUIRE(bytes.size() > 8U);
    constexpr auto from_end = std::size_t {5};
    constexpr auto flip_mask = 0x55U;
    auto const position = bytes.size() - from_end;
    bytes[position] = static_cast<char>(static_cast<unsigned char>(bytes[position]) ^ flip_mask);
    write_file(path, bytes);
}

// Three removals, so the bundle holds three tombstone runs and three retraction runs.
void remove_three(removal_bundle& env, std::vector<game_id>& removed)
{
    for (auto const position : {std::size_t {6}, std::size_t {1}, std::size_t {4}}) {
        removed.push_back(env.ids.at(position));
        env.remove(position);
    }
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear repair matrix.
TEST_CASE("a damaged or missing tombstone run is repaired from canonical SQLite and the removed games stay removed",
          "[motif-db][pr7][removal][repair]")
{
    constexpr auto seed = std::uint64_t {991U};
    constexpr auto random_games = std::size_t {10};
    for (auto const variant : {0, 1, 2}) {
        INFO("variant " << (variant == 0 ? "flipped byte" : variant == 1 ? "truncated" : "deleted"));
        removal_bundle env {seed, random_games, /*tree=*/true};
        auto removed = std::vector<game_id> {};
        remove_three(env, removed);
        auto const generation = env.status().publication_generation;
        auto const sequence = env.status().mutation_seq;
        auto const tree_before = *env.manager->manifest().tree();
        REQUIRE(env.manager->manifest().payload.metadata.runs.size() == 3U);
        auto const victim = env.bundle / env.manager->manifest().payload.metadata.runs[1].artifact.filename;
        env.manager.reset();
        if (variant == 0) {
            flip_last_bytes(victim);
        } else if (variant == 1) {
            fs::resize_file(victim, fs::file_size(victim) - 1U);
        } else {
            fs::remove(victim);
        }
        env.reopen();

        // One repair publication, from SQLite alone: no rebuild, and the healthy tree chain is untouched.
        auto const status = env.status();
        CHECK(status.postings_usable);
        CHECK(status.tree_usable);
        CHECK(status.mutation_seq == sequence);
        CHECK(status.publication_generation == generation + 1U);
        auto const& chain = env.manager->manifest().payload.metadata;
        CHECK(chain.checkpoint.has_value());  // replaced by one checkpoint that holds the tombstones
        CHECK(chain.runs.empty());
        CHECK(*env.manager->manifest().tree() == tree_before);
        for (auto const game : removed) {
            env.require_absent(game);
        }
        env.require_equivalent(seed + 1U);

        // The repaired state is durable, and removal stays incremental from it.
        auto const payload_json = env.manager->manifest().payload_json;
        env.reopen();
        CHECK(env.manager->manifest().payload_json == payload_json);
        CHECK(env.status().publication_generation == generation + 1U);
        env.require_equivalent(seed + 2U);
        env.remove(0);
        env.require_equivalent(seed + 3U);
        env.require_clean_directory();
    }
}

TEST_CASE("a damaged metadata checkpoint folded from tombstones is repaired from canonical SQLite", "[motif-db][pr7][removal][repair]")
{
    constexpr auto seed = std::uint64_t {1001U};
    constexpr auto random_games = std::size_t {12};
    constexpr auto hard_runs = std::size_t {3};
    removal_bundle env {seed, random_games, /*tree=*/true};
    env.manager->set_chain_limits(fold_test_limits(hard_runs));
    auto removed = std::vector<game_id> {};
    for (auto const position : {std::size_t {2}, std::size_t {5}, std::size_t {0}, std::size_t {3}, std::size_t {1}}) {
        removed.push_back(env.ids.at(position));
        env.remove(position);
        // Every removal that follows a fold keeps the position the model saw, so recompute from the live list.
    }
    auto const& chain = env.manager->manifest().payload.metadata;
    REQUIRE(chain.checkpoint.has_value());  // the chain folded under the cap of three runs
    auto const generation = env.status().publication_generation;
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const checkpoint_path = env.bundle / chain.checkpoint->artifact.filename;
    env.manager.reset();
    flip_last_bytes(checkpoint_path);
    env.reopen();

    CHECK(env.status().postings_usable);
    CHECK(env.status().tree_usable);
    CHECK(env.status().publication_generation == generation + 1U);
    for (auto const game : removed) {
        env.require_absent(game);
    }
    env.require_equivalent(seed + 1U);
    env.remove(0);
    env.require_equivalent(seed + 2U);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear repair sequence.
TEST_CASE("a damaged retraction run disables only the tree: exact postings keep the removed games removed until the tree is rebuilt",
          "[motif-db][pr7][removal][repair]")
{
    constexpr auto seed = std::uint64_t {1011U};
    constexpr auto random_games = std::size_t {10};
    removal_bundle env {seed, random_games, /*tree=*/true};
    auto removed = std::vector<game_id> {};
    remove_three(env, removed);
    auto const generation = env.status().publication_generation;
    auto const sequence = env.status().mutation_seq;
    auto const victim = env.bundle / env.manager->manifest().tree()->patch_runs[1].artifact.filename;
    env.manager.reset();
    flip_last_bytes(victim);
    env.reopen();

    auto const status = env.status();
    CHECK(status.postings_usable);
    CHECK_FALSE(status.tree_usable);
    CHECK(status.mutation_seq == sequence);
    CHECK(env.manager->manifest().tree() == nullptr);  // the descriptor was cleared, not repaired
    CHECK(status.publication_generation == generation + 1U);
    for (auto const game : removed) {
        env.require_absent(game);
    }
    // Postings alone answer exactly what a rebuild of the remaining games answers.
    require_equivalent_to_fresh_ids(*env.manager, env.bundle, env.corpus, env.ids, /*with_tree=*/false, seed + 1U);

    // A rebuilt tree restores the whole service, and removal is incremental again.
    REQUIRE(env.manager->rebuild_opening_tree_index().has_value());
    env.require_equivalent(seed + 2U);
    env.remove(0);
    env.require_equivalent(seed + 3U);
}

TEST_CASE("losing the postings segment after removals rebuilds from canonical state without the removed games",
          "[motif-db][pr7][removal][repair]")
{
    constexpr auto seed = std::uint64_t {1021U};
    constexpr auto random_games = std::size_t {10};
    removal_bundle env {seed, random_games, /*tree=*/false};
    auto removed = std::vector<game_id> {};
    remove_three(env, removed);
    auto const segment = env.bundle / env.manager->manifest().postings()->artifact.filename;
    env.manager.reset();
    fs::remove(segment);
    env.reopen();

    CHECK(env.status().postings_usable);
    // The new segment is built from the games that remain, so it needs no tombstones.
    CHECK(env.manager->manifest().payload.metadata.empty());
    for (auto const game : removed) {
        env.require_absent(game);
    }
    env.require_equivalent(seed + 1U);
    env.remove(0);
    env.require_equivalent(seed + 2U);
    env.require_clean_directory();
}
