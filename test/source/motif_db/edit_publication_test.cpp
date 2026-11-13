// PR 6b: database_manager::patch_game_metadata is ONE operation. A result or
// rating correction durably installs a metadata after-image run (and, with a
// tree, a signed tree-delta run -- or a folded checkpoint at the hard caps) and
// then commits the canonical UPDATE, the single sequence advance and the new
// snapshot row in one FULL transaction. These tests drive the production edit
// path (no hand-built runs) and compare every answer with a fresh rebuild of the
// edited canonical data.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>
#ifndef _WIN32
#    include <sys/wait.h>
#    include <unistd.h>
#endif

#include "composite_test_support.hpp"
#include "edit_test_support.hpp"
#include "equivalence_support.hpp"
#include "motif/db/database_manager.hpp"
#include "motif/db/fault_injection.hpp"
#include "motif/db/manifest.hpp"
#include "motif/db/opening_tree_composite.hpp"
#include "motif/db/opening_tree_patch.hpp"
#include "motif/db/postings_metadata_patch.hpp"
#include "motif/db/types.hpp"

namespace
{

using namespace motif::db;  // NOLINT(google-build-using-namespace) -- the file exercises most of motif::db; qualifying every name buries
                            // the assertions.
using namespace motif_test;  // NOLINT(google-build-using-namespace) -- the shared harness helpers are used throughout.

}  // namespace

// ── Equivalence ──────────────────────────────────────────────────────────────

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear equivalence sequence.
TEST_CASE("production edits keep every answer equal to a rebuild of the edited bundle (with a tree)", "[motif-db][pr6b][edit][equivalence]")
{
    constexpr auto random_games = std::size_t {14};
    constexpr auto edit_count = 16;
    constexpr auto reopen_seed_offset = 1000U;
    for (auto const seed : {701ULL, 702ULL}) {
        edit_bundle env {seed, random_games, /*tree=*/true};
        auto const checkpoint_name = env.manager->manifest().tree()->checkpoint.filename;
        auto const postings_name = env.manager->manifest().postings()->artifact.filename;
        auto const checkpoint_digest = file_digest(env.bundle / checkpoint_name);
        auto const postings_digest = file_digest(env.bundle / postings_name);
        auto rng = std::mt19937_64 {seed};
        for (auto step = 0; step < edit_count; ++step) {
            auto const index = std::uniform_int_distribution<std::size_t> {0, env.corpus.size() - 1}(rng);
            env.edit(index, random_edit_of(rng, env.corpus[index]));
            env.require_equivalent(seed + static_cast<std::uint64_t>(step));
        }
        // No ordinary edit touched a base artifact, and the chains stayed under the caps.
        CHECK(env.manager->manifest().tree()->checkpoint.filename == checkpoint_name);
        CHECK(env.manager->manifest().postings()->artifact.filename == postings_name);
        CHECK(file_digest(env.bundle / checkpoint_name) == checkpoint_digest);
        CHECK(file_digest(env.bundle / postings_name) == postings_digest);
        env.require_clean_directory();
        // Reopen: the chain, not a rebuild, restores the same state without replay.
        auto const generation = env.status().publication_generation;
        env.reopen();
        CHECK(env.status().publication_generation == generation);
        env.require_equivalent(seed + reopen_seed_offset);
        env.edit(0, random_edit_of(rng, env.corpus[0]));
        env.require_equivalent(seed + reopen_seed_offset + 1U);
    }
}

TEST_CASE("production edits on a postings-only bundle publish a metadata run and no tree work", "[motif-db][pr6b][edit][equivalence]")
{
    constexpr auto seed = 711U;
    constexpr auto random_games = std::size_t {12};
    constexpr auto edit_count = 10;
    constexpr auto reopen_check_seed = seed + 1000U;
    edit_bundle env {seed, random_games, /*tree=*/false};
    auto rng = std::mt19937_64 {seed};  // NOLINT(cert-msc51-cpp,cert-msc32-c) -- a fixed seed keeps the fixtures reproducible.
    for (auto step = 0; step < edit_count; ++step) {
        auto const index = std::uniform_int_distribution<std::size_t> {0, env.corpus.size() - 1}(rng);
        env.edit(index, random_edit_of(rng, env.corpus[index]));
        CHECK(env.manager->last_patch_report().replayed_games == 0U);
        CHECK(env.manager->manifest().tree() == nullptr);
        env.require_equivalent(seed + static_cast<std::uint64_t>(step));
    }
    env.require_clean_directory();
    env.reopen();
    env.require_equivalent(reopen_check_seed);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate these linear transition sweeps.
TEST_CASE("every result transition and every rating transition is a correct one-operation edit", "[motif-db][pr6b][edit][equivalence]")
{
    constexpr auto seed = std::uint64_t {721U};
    constexpr auto random_games = std::size_t {8};
    edit_bundle env {seed, random_games, /*tree=*/true};
    // All sixteen transitions between the four canonical results on one game.
    auto const game_index = std::size_t {5};
    for (auto const from : motif_test::result_strings) {
        for (auto const to_result : motif_test::result_strings) {
            auto reset = env.corpus[game_index];
            reset.result = std::string {from};
            env.edit(game_index, reset);
            auto target = env.corpus[game_index];
            target.result = std::string {to_result};
            auto const seq_before = env.status().mutation_seq;
            env.edit(game_index, target);
            // draw <-> '*' encodes identically but is a real canonical change: it advances
            // the sequence and publishes an (empty) certificate run, never a no-op.
            CHECK((env.status().mutation_seq == seq_before + (from == to_result ? 0U : 1U)));
            if (from != to_result && (from == "1/2-1/2" || from == "*") && (to_result == "1/2-1/2" || to_result == "*")) {
                CHECK(env.manager->last_patch_report().replayed_games == 0U);
                CHECK(env.manager->last_patch_report().replayed_plies == 0U);
                auto const& run = env.manager->manifest().tree()->patch_runs.back();
                auto decoded =
                    load_tree_patch_run(env.bundle / run.artifact.filename,
                                        run.artifact,
                                        tree_patch_expectation {.identity = {.bundle_id = bundle_identity_of(env.manager->manifest()),
                                                                             .index_semantics = index_semantics_version,
                                                                             .canonical_start_hash = motif_test::start_hash(),
                                                                             .max_root_ply = env.manager->manifest().tree()->max_root_ply},
                                                                .from_exclusive_seq = run.from_exclusive_seq,
                                                                .to_inclusive_seq = run.to_inclusive_seq});
                REQUIRE(decoded.has_value());
                CHECK(decoded->nodes.empty());
                CHECK(run.to_inclusive_seq == env.status().mutation_seq);
            }
        }
    }
    env.require_equivalent(seed + 1U);

    // Ratings: absent <-> present, present -> present, both sides, the domain edges.
    auto const ratings = std::array<std::optional<std::int32_t>, 7> {std::nullopt, -32768, -1, 0, 1, 2300, 32767};
    for (auto const white : ratings) {
        for (auto const black : ratings) {
            auto target = env.corpus[game_index];
            target.white_elo = white;
            target.black_elo = black;
            env.edit(game_index, target);
        }
    }
    env.require_equivalent(seed + 2U);
    env.require_clean_directory();
    env.reopen();
    env.require_equivalent(seed + 3U);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear no-op check.
TEST_CASE("an edit restating the stored values is a success that changes nothing", "[motif-db][pr6b][edit]")
{
    constexpr auto seed = 731U;
    constexpr auto random_games = std::size_t {6};
    edit_bundle env {seed, random_games, /*tree=*/true};
    auto const before_json = env.manager->manifest().payload_json;
    auto const before_status = env.status();
    auto const files = artifact_names(env.bundle);

    auto patch = game_patch {};
    patch.result = env.corpus[2].result;
    auto const white_elo = env.corpus[2].white_elo;
    if (!white_elo) {
        throw std::runtime_error {"the restated game must carry a white Elo for this no-op check"};
    }
    patch.white_elo = rating_patch::set(*white_elo);
    REQUIRE(env.manager->patch_game_metadata(edit_bundle::id_of(2), patch).has_value());
    CHECK(env.manager->last_patch_report().canonical_noop);
    CHECK_FALSE(env.manager->last_patch_report().published);
    CHECK(env.status().mutation_seq == before_status.mutation_seq);
    CHECK(env.status().publication_generation == before_status.publication_generation);
    CHECK(env.manager->manifest().payload_json == before_json);
    CHECK(artifact_names(env.bundle) == files);

    // An empty patch on an imported game is a no-op as well; a missing id is not found.
    REQUIRE(env.manager->patch_game_metadata(edit_bundle::id_of(1), game_patch {}).has_value());
    constexpr auto absent_game = 9999U;
    auto const missing = env.manager->patch_game_metadata(game_id {absent_game}, patch);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error() == error_code::not_found);
    CHECK(artifact_names(env.bundle) == files);
}

// ── Editability policy ───────────────────────────────────────────────────────

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear permission check.
TEST_CASE("imported games accept result and rating corrections only; manual games keep the wider permission",
          "[motif-db][pr6b][edit][policy]")
{
    constexpr auto seed = std::uint64_t {741U};
    constexpr auto random_games = std::size_t {6};
    edit_bundle env {seed, random_games, /*tree=*/true, /*manual_stride=*/0};
    auto const imported = edit_bundle::id_of(1);

    // A rating or result correction on an imported game works and keeps its provenance.
    auto correction = game_patch {};
    correction.result = env.corpus[1].result == "1-0" ? "0-1" : "1-0";
    constexpr auto corrected_rating = 2111;
    correction.white_elo = rating_patch::set(corrected_rating);
    REQUIRE(env.manager->patch_game_metadata(imported, correction).has_value());
    env.corpus[1].result = *correction.result;
    env.corpus[1].white_elo = corrected_rating;
    auto const corrected = env.manager->store().get(imported);
    REQUIRE(corrected.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(corrected->provenance.source_type == "imported");
    env.require_equivalent(seed + 1U);

    // Any other field rejects the whole patch atomically: the carried correction is NOT applied.
    auto const seq_before = env.status().mutation_seq;
    auto const generation_before = env.status().publication_generation;
    auto const files = artifact_names(env.bundle);
    for (auto const& forbidden : {"white_name", "black_name", "event", "site", "date", "eco", "source_label", "review_status", "notes"}) {
        auto mixed = game_patch {};
        mixed.result = env.corpus[1].result == "1-0" ? "0-1" : "1-0";
        constexpr auto mixed_rating = 1234;
        mixed.black_elo = rating_patch::set(mixed_rating);
        auto const field = std::string_view {forbidden};
        if (field == "white_name") {
            mixed.white_name = "Someone";
        } else if (field == "black_name") {
            mixed.black_name = "Someone";
        } else if (field == "event") {
            mixed.event = "Some event";
        } else if (field == "site") {
            mixed.site = "Some site";
        } else if (field == "date") {
            mixed.date = "2020.01.01";
        } else if (field == "eco") {
            mixed.eco = "B20";
        } else if (field == "source_label") {
            mixed.source_label = "label";
        } else if (field == "review_status") {
            mixed.review_status = "studied";
        } else {
            mixed.notes = "a note";
        }
        auto const rejected = env.manager->patch_game_metadata(imported, mixed);
        INFO(forbidden);
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error() == error_code::not_editable);
    }
    CHECK(env.status().mutation_seq == seq_before);
    CHECK(env.status().publication_generation == generation_before);
    CHECK(artifact_names(env.bundle) == files);
    auto const unchanged = env.manager->store().get(imported);
    REQUIRE(unchanged.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    CHECK(unchanged->result == env.corpus[1].result);
    CHECK(unchanged->black.elo == env.corpus[1].black_elo);
    // NOLINTEND(bugprone-unchecked-optional-access)

    // Imported deletion stays forbidden.
    auto const removed = env.manager->remove_user_game(imported);
    REQUIRE_FALSE(removed.has_value());
    CHECK(removed.error() == error_code::not_editable);

    // A manual game accepts the wide patch, indexed and non-indexed fields atomically in one commit.
    auto const manual_game = std::size_t {0};
    // The manual game is index 0: manual_stride 0 means none are manual, so flip one now.
    REQUIRE(env.manager->set_manual_game_provenance(edit_bundle::id_of(manual_game), std::nullopt, "new").has_value());
    auto wide = game_patch {};
    wide.white_name = "Renamed White";
    wide.event = "A new event";
    wide.notes = "careful";
    wide.result = env.corpus[manual_game].result == "1-0" ? "0-1" : "1-0";
    wide.black_elo = rating_patch::clear();
    auto const seq_wide = env.status().mutation_seq;
    REQUIRE(env.manager->patch_game_metadata(edit_bundle::id_of(manual_game), wide).has_value());
    CHECK(env.status().mutation_seq == seq_wide + 1U);
    CHECK(env.manager->last_patch_report().published);
    env.corpus[manual_game].result = *wide.result;
    env.corpus[manual_game].black_elo = std::nullopt;
    auto const stored = env.manager->store().get(edit_bundle::id_of(manual_game));
    REQUIRE(stored.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    CHECK(stored->white.name == "Renamed White");
    CHECK(stored->event_details.has_value());
    CHECK(stored->result == *wide.result);
    CHECK_FALSE(stored->black.elo.has_value());
    // NOLINTEND(bugprone-unchecked-optional-access)
    env.require_equivalent(seed + 2U);
}

TEST_CASE("fields the indexes do not embed keep the NORMAL canonical-only path", "[motif-db][pr6b][edit][policy]")
{
    constexpr auto seed = 751U;
    constexpr auto random_games = std::size_t {6};
    edit_bundle env {seed, random_games, /*tree=*/true, /*manual_stride=*/1};
    auto const files = artifact_names(env.bundle);
    auto const status_before = env.status();
    auto const json_before = env.manager->manifest().payload_json;

    auto patch = game_patch {};
    patch.white_name = "Quietly Renamed";
    patch.notes = "no index change";
    patch.review_status = "studied";
    REQUIRE(env.manager->patch_game_metadata(edit_bundle::id_of(1), patch).has_value());
    CHECK(env.manager->last_patch_report().canonical_only);
    CHECK_FALSE(env.manager->last_patch_report().published);
    CHECK(env.status().mutation_seq == status_before.mutation_seq);
    CHECK(env.status().publication_generation == status_before.publication_generation);
    CHECK(env.manager->manifest().payload_json == json_before);
    CHECK(artifact_names(env.bundle) == files);
    // The connection stayed at its normal durability: no FULL left behind.
    CHECK(env.status().postings_usable);
}

// ── Stale baseline ───────────────────────────────────────────────────────────

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear refusal check.
TEST_CASE("an index-relevant patch on a stale baseline is refused before any UPDATE", "[motif-db][pr6b][edit][stale]")
{
    constexpr auto seed = std::uint64_t {761U};
    constexpr auto random_games = std::size_t {6};
    edit_bundle env {seed, random_games, /*tree=*/true};
    // An insert the indexes do not cover yet makes the baseline stale.
    REQUIRE(env.manager->insert_game(motif_test::to_db_game(motif_test::fixture_games()[3], 999)).has_value());
    REQUIRE_FALSE(env.status().postings_usable);
    auto const seq = env.status().mutation_seq;
    auto const generation = env.status().publication_generation;
    auto const files = artifact_names(env.bundle);
    auto const before = env.manager->store().get(edit_bundle::id_of(1));
    REQUIRE(before.has_value());

    auto patch = game_patch {};
    patch.result = env.corpus[1].result == "1-0" ? "0-1" : "1-0";
    auto const refused = env.manager->patch_game_metadata(edit_bundle::id_of(1), patch);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == error_code::index_stale);
    CHECK(env.status().mutation_seq == seq);
    CHECK(env.status().publication_generation == generation);
    CHECK(artifact_names(env.bundle) == files);
    auto const after = env.manager->store().get(edit_bundle::id_of(1));
    REQUIRE(after.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(after->result == before->result);  // nothing was updated

    // Non-indexed patches still work on a stale bundle (they never touch the indexes) ...
    auto quiet = game_patch {};
    quiet.notes = "still editable";
    REQUIRE(env.manager->set_manual_game_provenance(edit_bundle::id_of(1), std::nullopt, "new").has_value());
    REQUIRE(env.manager->patch_game_metadata(edit_bundle::id_of(1), quiet).has_value());
    // ... and a rebuild restores the incremental path.
    REQUIRE(env.manager->rebuild_position_postings().has_value());
    REQUIRE(env.manager->rebuild_opening_tree_index().has_value());
    env.corpus.push_back(motif_test::fixture_games()[3]);
    REQUIRE(env.manager->patch_game_metadata(edit_bundle::id_of(1), patch).has_value());
    env.corpus[1].result = *patch.result;
    CHECK(env.manager->last_patch_report().published);
    env.require_equivalent(seed + 1U);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear refusal check.
TEST_CASE("an index-relevant patch cannot run inside an open canonical transaction", "[motif-db][pr6b][edit][stale]")
{
    constexpr auto seed = 762U;
    constexpr auto random_games = std::size_t {6};
    edit_bundle env {seed, random_games, /*tree=*/true};
    auto const seq = env.status().mutation_seq;
    auto const files = artifact_names(env.bundle);
    REQUIRE(env.manager->writer().begin_transaction().has_value());
    auto patch = game_patch {};
    patch.result = env.corpus[1].result == "1-0" ? "0-1" : "1-0";
    auto const refused = env.manager->patch_game_metadata(edit_bundle::id_of(1), patch);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == error_code::invalid_argument);
    env.manager->writer().rollback_transaction();
    CHECK(env.status().mutation_seq == seq);
    CHECK(artifact_names(env.bundle) == files);
    CHECK(env.manager->store().get(edit_bundle::id_of(1))->result == env.corpus[1].result);  // NOLINT(bugprone-unchecked-optional-access)
    // The refusal had no side effects: the same patch now works.
    REQUIRE(env.manager->patch_game_metadata(edit_bundle::id_of(1), patch).has_value());
}

// ── Identity ─────────────────────────────────────────────────────────────────

TEST_CASE("a result correction that would duplicate another game is refused as duplicate before any artifact exists",
          "[motif-db][pr6b][edit][identity]")
{
    temp_dir const dir {"duplicate"};
    auto const bundle = dir.path / "db";
    auto created = database_manager::create(bundle, "duplicate");
    REQUIRE(created.has_value());
    auto first = motif_test::to_db_game(motif_test::fixture_games()[4], 0);
    first.result = "1-0";
    auto second = first;
    second.result = "0-1";
    auto const first_id = created->insert_game(first);
    auto const second_id = created->insert_game(second);
    REQUIRE(first_id.has_value());
    REQUIRE(second_id.has_value());
    REQUIRE(created->rebuild_position_postings().has_value());
    REQUIRE(created->rebuild_opening_tree_index().has_value());

    auto const seq = created->derived_index_status()->mutation_seq;
    auto const generation = created->derived_index_status()->publication_generation;
    auto const files = artifact_names(bundle);
    auto patch = game_patch {};
    patch.result = "1-0";  // the first game's exact identity
    auto const refused = created->patch_game_metadata(*second_id, patch);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == error_code::duplicate);
    CHECK(created->derived_index_status()->mutation_seq == seq);
    CHECK(created->derived_index_status()->publication_generation == generation);
    CHECK(created->derived_index_status()->postings_usable);
    CHECK(artifact_names(bundle) == files);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(created->store().get(*second_id)->result == "0-1");
    // A change that does not collide still works.
    patch.result = "1/2-1/2";
    REQUIRE(created->patch_game_metadata(*second_id, patch).has_value());
}

TEST_CASE("a move-hash collision between distinct move lists takes the next collision slot instead of a false duplicate",
          "[motif-db][pr6b][edit][identity]")
{
    temp_dir const dir {"collision"};
    auto const bundle = dir.path / "db";
    {
        auto created = database_manager::create(bundle, "collision");
        REQUIRE(created.has_value());
        auto first = motif_test::to_db_game(motif_test::fixture_games()[4], 0);
        first.result = "1-0";
        first.moves = motif_test::uci_moves({"e2e4"});
        auto second = first;
        second.result = "0-1";
        second.moves = motif_test::uci_moves({"d2d4"});
        REQUIRE(created->insert_game(first).has_value());
        REQUIRE(created->insert_game(second).has_value());
        REQUIRE(created->rebuild_position_postings().has_value());
    }
    {
        // Model a hash collision: distinct move lists with equal identity fields and move_hash.
        auto const probe = probe_connection {bundle};
        probe.exec("UPDATE game SET move_hash = (SELECT move_hash FROM game WHERE id = 1) WHERE id = 2;");
    }
    auto reopened = database_manager::open(bundle);
    REQUIRE(reopened.has_value());
    REQUIRE(reopened->derived_index_status()->postings_usable);

    auto patch = game_patch {};
    patch.result = "1-0";
    REQUIRE(reopened->patch_game_metadata(game_id {2U}, patch).has_value());
    CHECK(reopened->last_patch_report().published);
    reopened->close();
    auto const probe = probe_connection {bundle};
    CHECK(probe.scalar("SELECT identity_collision FROM game WHERE id = 2") == 1);
    CHECK(probe.scalar("SELECT identity_collision FROM game WHERE id = 1") == 0);
    CHECK(probe.scalar("SELECT COUNT(*) FROM game WHERE result = '1-0'") == 2);
}
