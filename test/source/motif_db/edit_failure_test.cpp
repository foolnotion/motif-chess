// PR 6b atomicity: every row of the edit failure matrix (design section 4.3),
// ambiguous COMMIT reconciliation, forked-child crashes at the boundaries, the
// hard-cap fold, rebuild interplay, reimport behavior, reader exclusion and
// directory copies -- all against the production patch_game_metadata path.

#include <algorithm>
#include <array>
#include <atomic>
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
#include <set>
#include <string>
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
#include "motif/db/postings_metadata_patch.hpp"
#include "motif/db/types.hpp"

namespace
{

using namespace motif::db;  // NOLINT(google-build-using-namespace) -- the file exercises most of motif::db; qualifying every name buries
                            // the assertions.
using namespace motif_test;  // NOLINT(google-build-using-namespace) -- the shared harness helpers are used throughout.
namespace fault = motif::db::fault;
using fault::fault_action;
using fault::point;

// ── Fault plumbing (the hook is a plain function pointer) ────────────────────

struct fault_arm
{
    point at {};
    fault_action action {fault_action::fail_io};
    // The nth crossing of the boundary is the one that fires.
    std::size_t nth {1};
    std::size_t seen {0};
};

constexpr std::size_t max_trace = 1024;
// One slot per fault point (there are fewer than 64).
constexpr std::size_t sync_level_slots = 64;
// The standard corpus of the edit tests: this many random games added to the hand-built fixtures.
constexpr auto standard_games = std::size_t {10};
// Ratings just outside the signed 16-bit domain, and a game id no bundle holds.
constexpr auto rating_above_domain = 32768;
constexpr auto rating_below_domain = -32769;
constexpr auto absent_game = 9999U;
// The two ratings indexed_target() alternates between.
constexpr auto first_rating = 2222;
constexpr auto second_rating = 2223;
// The fault hook is a plain function pointer, so the arms and the trace it reads are process-global by necessity.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::array<fault_arm, 2> g_arms {};
std::size_t g_arm_count {0};
std::array<point, max_trace> g_trace {};
std::size_t g_trace_count {0};
// PRAGMA synchronous observed on the hook connection at selected boundaries.
std::array<int, sync_level_slots> g_sync_level {};
std::filesystem::path const* g_corrupt_bundle {nullptr};
std::set<std::string> const* g_corrupt_old_files {nullptr};
bool g_corrupted_checkpoint {false};

// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

auto corrupt_final_checkpoint(point const where, fault::artifact_kind /*kind*/, std::uint64_t /*generation*/) noexcept -> fault_action
{
    if (where != point::after_artifact_directory_fsync) {
        return fault_action::proceed;
    }
    try {
        for (auto const& entry : std::filesystem::directory_iterator {*g_corrupt_bundle}) {
            auto const name = entry.path().filename().string();
            if (name.starts_with(tree_artifact_prefix) && !g_corrupt_old_files->contains(name)) {
                auto file = std::fstream {entry.path(), std::ios::in | std::ios::out | std::ios::binary};
                file.put('\0');
                file.flush();
                g_corrupted_checkpoint = file.good();
                break;
            }
        }
    } catch (...) {
        g_corrupted_checkpoint = false;
    }
    return fault_action::proceed;
}

auto read_sync_level() -> int
{
    auto* const conn = fault::hook_connection();
    if (conn == nullptr) {
        return -1;
    }
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(conn, "PRAGMA synchronous;", -1, &raw, nullptr) != SQLITE_OK) {
        return -1;
    }
    auto level = -1;
    if (sqlite3_step(raw) == SQLITE_ROW) {
        level = sqlite3_column_int(raw, 0);
    }
    sqlite3_finalize(raw);
    return level;
}

auto trace_hook(point const where, fault::artifact_kind /*kind*/, std::uint64_t /*generation*/) noexcept -> fault_action
{
    if (g_trace_count < max_trace) {
        g_trace.at(g_trace_count++) = where;
    }
    if (where == point::before_snapshot_begin || where == point::before_sqlite_commit || where == point::before_synchronous_restore
        || where == point::after_sqlite_commit || where == point::before_synchronous_full)
    {
        g_sync_level.at(static_cast<std::size_t>(where)) = read_sync_level();
    }
    for (std::size_t index = 0; index < g_arm_count; ++index) {
        auto& arm = g_arms.at(index);
        if (arm.at == where && ++arm.seen == arm.nth) {
            return arm.action;
        }
    }
    return fault_action::proceed;
}

// Installs the recording hook, arms up to two boundaries, restores the previous
// hook afterwards.
class armed_faults
{
  public:
    armed_faults(std::initializer_list<fault_arm> arms)
        : previous_ {fault::set_hook(trace_hook)}
    {
        g_trace_count = 0;
        g_arm_count = 0;
        g_sync_level.fill(-2);
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

auto first_crossing(point const where) -> std::optional<std::size_t>
{
    for (std::size_t index = 0; index < g_trace_count; ++index) {
        if (g_trace.at(index) == where) {
            return index;
        }
    }
    return std::nullopt;
}

// ── Observing "the tuple": canonical fields, sequence, payload, metadata, tree ──

struct observed_state
{
    std::uint64_t sequence {};
    std::uint64_t generation {};
    std::string payload;
    bool postings_usable {false};
    bool tree_usable {false};
    std::string result;
    std::optional<std::int32_t> white_elo;
    std::optional<std::int32_t> black_elo;
    std::string white_name;
    bool has_event {false};
    std::size_t tag_count {};
    std::optional<position_match> metadata;
    std::vector<opening_stat_agg_row> start_rows;
    std::int64_t player_rows {};
};

auto observe(edit_bundle& env, std::size_t const index) -> observed_state
{
    auto state = observed_state {};
    auto const report = env.status();
    state.sequence = report.mutation_seq;
    state.generation = report.publication_generation;
    state.payload = env.manager->manifest().payload_json;
    state.postings_usable = report.postings_usable;
    state.tree_usable = report.tree_usable;
    auto const stored = env.manager->store().get(edit_bundle::id_of(index));
    REQUIRE(stored.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    state.result = stored->result;
    state.white_elo = stored->white.elo;
    state.black_elo = stored->black.elo;
    state.white_name = stored->white.name;
    state.has_event = stored->event_details.has_value();
    state.tag_count = stored->extra_tags.size();
    // NOLINTEND(bugprone-unchecked-optional-access)
    if (report.postings_usable) {
        auto const ids = std::vector<game_id> {edit_bundle::id_of(index)};
        auto const matches = env.manager->query_position_first_matches(zobrist_hash {start_hash()}, ids);
        REQUIRE(matches.has_value());
        if (!matches->empty()) {
            state.metadata = matches->front();
        }
        auto rows = env.manager->query_unfiltered_opening_stats(zobrist_hash {start_hash()});
        REQUIRE(rows.has_value());
        state.start_rows = std::move(*rows);
    }
    return state;
}

auto same_state(observed_state const& left, observed_state const& right) -> bool
{
    auto const same_metadata = left.metadata.has_value() == right.metadata.has_value()
        && (!left.metadata
            || (left.metadata->result == right.metadata->result && left.metadata->white_elo == right.metadata->white_elo
                && left.metadata->black_elo == right.metadata->black_elo));
    return left.sequence == right.sequence && left.generation == right.generation && left.payload == right.payload
        && left.postings_usable == right.postings_usable && left.tree_usable == right.tree_usable && left.result == right.result
        && left.white_elo == right.white_elo && left.black_elo == right.black_elo && left.white_name == right.white_name
        && left.has_event == right.has_event && left.tag_count == right.tag_count && same_metadata
        && rows_equal(left.start_rows, right.start_rows);
}

auto distinct_result(std::string const& current) -> std::string
{
    return current == "1-0" ? "0-1" : "1-0";
}

// The edit most tests apply: a result flip plus a rating change on one game.
auto indexed_target(model_game const& base) -> model_game
{
    auto target = base;
    target.result = distinct_result(base.result);
    target.white_elo = base.white_elo == std::optional<std::int32_t> {first_rating} ? std::optional<std::int32_t> {second_rating}
                                                                                    : std::optional<std::int32_t> {first_rating};
    return target;
}

// A manual game's wide patch: indexed fields plus a new player, an event and notes.
auto wide_patch(model_game const& base) -> game_patch
{
    auto patch = patch_for(base, indexed_target(base));
    patch.white_name = "Brand New Player";
    patch.event = "A Brand New Event";
    patch.notes = "wide patch";
    return patch;
}

}  // namespace

// ── The failure matrix, rows 2-7: nothing reached COMMIT ─────────────────────

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear boundary matrix.
TEST_CASE("a failure at any boundary before COMMIT leaves the entire old tuple and removes the attempt's files",
          "[motif-db][pr6b][failure]")
{
    struct boundary
    {
        point at;
        std::size_t nth;
    };

    auto const boundaries = std::vector<boundary> {
        {point::before_build, 1},
        {point::after_build, 1},
        {point::before_disk_admission, 1},
        {point::before_artifact_validation, 1},
        {point::after_artifact_validation, 1},
        {point::before_artifact_fsync, 1},
        {point::before_artifact_fsync, 2},  // the first candidate is already staged
        {point::after_artifact_fsync, 1},
        {point::after_artifact_fsync, 2},
        {point::before_artifact_rename, 1},
        {point::after_artifact_rename, 1},
        {point::before_artifact_directory_fsync, 1},
        {point::after_artifact_directory_fsync, 1},
        {point::before_synchronous_full, 1},
        {point::before_snapshot_begin, 1},
        {point::after_canonical_update, 1},
        {point::after_sequence_bump, 1},
        {point::after_snapshot_row_write, 1},
        {point::before_sqlite_commit, 1},
    };
    for (auto const mixed_manual : {false, true}) {
        for (auto const& boundary : boundaries) {
            INFO("boundary " << static_cast<int>(boundary.at) << " crossing " << boundary.nth
                             << (mixed_manual ? " (mixed manual patch)" : ""));
            constexpr auto seed = std::uint64_t {801U};
            edit_bundle env {seed, standard_games, /*tree=*/true};
            auto const index = mixed_manual ? std::size_t {3} : std::size_t {2};  // 3 is manual, 2 imported
            auto const patch =
                mixed_manual ? wide_patch(env.corpus[index]) : patch_for(env.corpus[index], indexed_target(env.corpus[index]));
            auto const target = indexed_target(env.corpus[index]);
            auto const old_state = observe(env, index);
            auto const old_files = artifact_names(env.bundle);
            auto const old_revision = env.manager->data_revision();
            auto const old_players = env.manager->store().count_games();
            REQUIRE(old_players.has_value());

            auto outcome = result<void> {};
            {
                auto const faults = armed_faults {{.at = boundary.at, .action = fault_action::fail_io, .nth = boundary.nth}};
                outcome = env.manager->patch_game_metadata(edit_bundle::id_of(index), patch);
                REQUIRE(crossings_of(boundary.at) >= boundary.nth);  // the boundary really was crossed
            }
            REQUIRE_FALSE(outcome.has_value());

            // The whole old tuple: canonical fields (including names/event/notes of a mixed
            // patch), sequence, snapshot payload, metadata and tree answers.
            auto const after_failure = observe(env, index);
            REQUIRE(same_state(old_state, after_failure));
            REQUIRE(artifact_names(env.bundle) == old_files);
            REQUIRE(env.manager->data_revision() == old_revision);
            env.require_equivalent(seed);
            {
                auto const probe = probe_connection {env.bundle};
                CHECK(probe.scalar("SELECT COUNT(*) FROM player WHERE name = 'Brand New Player'") == 0);
                CHECK(probe.scalar("SELECT COUNT(*) FROM event WHERE name = 'A Brand New Event'") == 0);
            }

            // The manager is healthy afterwards: the same patch now succeeds, caches included.
            REQUIRE(env.manager->patch_game_metadata(edit_bundle::id_of(index), patch).has_value());
            env.corpus[index] = target;
            CHECK(env.manager->data_revision() > old_revision);
            env.require_equivalent(seed + 1U);
            env.require_clean_directory();
            if (mixed_manual) {
                auto const stored = env.manager->store().get(edit_bundle::id_of(index));
                REQUIRE(stored.has_value());
                // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
                CHECK(stored->white.name == "Brand New Player");
            }
        }
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear crossing trace.
TEST_CASE("an edit crosses COMMIT exactly once, at FULL, and the previous synchronous level comes back",
          "[motif-db][pr6b][failure][durability]")
{
    constexpr auto seed = std::uint64_t {802U};
    edit_bundle env {seed, standard_games, /*tree=*/true};
    auto const index = std::size_t {2};
    auto const target = indexed_target(env.corpus[index]);
    {
        auto const faults = armed_faults {};
        REQUIRE(env.manager->patch_game_metadata(edit_bundle::id_of(index), patch_for(env.corpus[index], target)).has_value());
        // One snapshot transaction carries the canonical UPDATE, the single sequence
        // advance and the snapshot row; there is no second (writer) commit.
        CHECK(crossings_of(point::before_snapshot_begin) == 1U);
        CHECK(crossings_of(point::before_sqlite_commit) == 1U);
        CHECK(crossings_of(point::before_canonical_commit) == 0U);
        CHECK(crossings_of(point::after_canonical_commit) == 0U);
        // The documented order of the protocol.
        auto const order = std::array {point::before_build,
                                       point::after_build,
                                       point::after_artifact_fsync,
                                       point::after_artifact_directory_fsync,
                                       point::before_synchronous_full,
                                       point::before_snapshot_begin,
                                       point::after_canonical_update,
                                       point::after_sequence_bump,
                                       point::after_snapshot_row_write,
                                       point::before_sqlite_commit,
                                       point::after_sqlite_commit,
                                       point::before_snapshot_install,
                                       point::after_snapshot_install,
                                       point::after_manifest_write};
        auto previous = std::optional<std::size_t> {};
        for (auto const step : order) {
            auto const where = first_crossing(step);
            REQUIRE(where.has_value());
            if (previous.has_value() && where.has_value()) {
                CHECK(*where > *previous);
            }
            previous = where;
        }
        constexpr auto normal = 1;
        constexpr auto full = 2;
        CHECK(g_sync_level[static_cast<std::size_t>(point::before_synchronous_full)] == normal);
        CHECK(g_sync_level[static_cast<std::size_t>(point::before_snapshot_begin)] == full);
        CHECK(g_sync_level[static_cast<std::size_t>(point::before_sqlite_commit)] == full);
        CHECK(g_sync_level[static_cast<std::size_t>(point::after_sqlite_commit)] == normal);
    }
    env.corpus[index] = target;
    env.require_equivalent(seed);

    // A non-indexed patch keeps the NORMAL path: no snapshot transaction at all.
    {
        auto const faults = armed_faults {};
        auto quiet = game_patch {};
        quiet.notes = "quiet";
        REQUIRE(env.manager->set_manual_game_provenance(edit_bundle::id_of(1), std::nullopt, "new").has_value());
        REQUIRE(env.manager->patch_game_metadata(edit_bundle::id_of(1), quiet).has_value());
        CHECK(crossings_of(point::before_snapshot_begin) == 0U);
        CHECK(crossings_of(point::before_synchronous_full) == 0U);
    }
}

// ── Row 1: requests that are refused outright ────────────────────────────────

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this table of refusals.
TEST_CASE("a missing id, an invalid value or a forbidden field changes nothing, stages nothing and starts no artifact work",
          "[motif-db][pr6b][failure][validation]")
{
    constexpr auto seed = std::uint64_t {805U};
    edit_bundle env {seed, standard_games, /*tree=*/true};
    auto const old_state = observe(env, 2);
    auto const old_files = artifact_names(env.bundle);
    auto const old_revision = env.manager->data_revision();

    struct refusal
    {
        game_id id;
        game_patch patch;
        error_code expected;
    };

    auto refusals = std::vector<refusal> {};
    refusals.push_back({game_id {absent_game},
                        []() -> game_patch
                        {
                            auto request = game_patch {};
                            request.result = "0-1";
                            return request;
                        }(),
                        error_code::not_found});
    refusals.push_back({edit_bundle::id_of(2),
                        []() -> game_patch
                        {
                            auto request = game_patch {};
                            request.result = "draw";
                            return request;
                        }(),
                        error_code::invalid_argument});
    refusals.push_back({edit_bundle::id_of(2),
                        []() -> game_patch
                        {
                            auto request = game_patch {};
                            request.result = "";
                            return request;
                        }(),
                        error_code::invalid_argument});
    refusals.push_back({edit_bundle::id_of(2),
                        []() -> game_patch
                        {
                            auto request = game_patch {};
                            request.white_elo = rating_patch::set(rating_above_domain);
                            return request;
                        }(),
                        error_code::invalid_argument});
    refusals.push_back({edit_bundle::id_of(2),
                        []() -> game_patch
                        {
                            auto request = game_patch {};
                            request.black_elo = rating_patch::set(rating_below_domain);
                            return request;
                        }(),
                        error_code::invalid_argument});
    // Imported game (index 2): a forbidden field next to a legal correction.
    refusals.push_back({edit_bundle::id_of(2),
                        []() -> game_patch
                        {
                            auto request = game_patch {};
                            request.result = "0-1";
                            request.eco = "A00";
                            return request;
                        }(),
                        error_code::not_editable});
    for (auto const& refused : refusals) {
        auto const armed = armed_faults {};
        auto const outcome = env.manager->patch_game_metadata(refused.id, refused.patch);
        REQUIRE_FALSE(outcome.has_value());
        CHECK(outcome.error() == refused.expected);
        CHECK(crossings_of(point::before_build) == 0U);
        CHECK(crossings_of(point::before_snapshot_begin) == 0U);
    }
    REQUIRE(same_state(old_state, observe(env, 2)));
    CHECK(artifact_names(env.bundle) == old_files);
    CHECK(env.manager->data_revision() == old_revision);
    env.require_equivalent(seed + 1U);
}

// ── Row 8: a COMMIT SQLite reports as failed although its transaction is gone ──

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate these four linear sections.
TEST_CASE("an ambiguous COMMIT reconciles to the true outcome and never serves old metadata with new canonical rows",
          "[motif-db][pr6b][failure][ambiguous]")
{
    SECTION("known new: the row landed after all")
    {
        constexpr auto seed = std::uint64_t {811U};
        edit_bundle env {seed, standard_games, /*tree=*/true};
        auto const index = std::size_t {2};
        auto const target = indexed_target(env.corpus[index]);
        auto const seq = env.status().mutation_seq;
        {
            auto const faults = armed_faults {{.at = point::before_sqlite_commit, .action = fault_action::commit_error_after_commit}};
            REQUIRE(env.manager->patch_game_metadata(edit_bundle::id_of(index), patch_for(env.corpus[index], target)).has_value());
        }
        env.corpus[index] = target;
        CHECK(env.status().mutation_seq == seq + 1U);
        CHECK(env.manager->last_patch_report().published);
        env.require_equivalent(seed + 1U);
        env.require_clean_directory();
        env.reopen();
        env.require_equivalent(seed + 2U);
    }
    SECTION("known old: the transaction was discarded")
    {
        constexpr auto seed = std::uint64_t {812U};
        edit_bundle env {seed, standard_games, /*tree=*/true};
        auto const index = std::size_t {2};
        auto const target = indexed_target(env.corpus[index]);
        auto const old_state = observe(env, index);
        auto const old_files = artifact_names(env.bundle);
        {
            auto const faults = armed_faults {{.at = point::before_sqlite_commit, .action = fault_action::commit_error_after_rollback}};
            REQUIRE_FALSE(env.manager->patch_game_metadata(edit_bundle::id_of(index), patch_for(env.corpus[index], target)).has_value());
        }
        REQUIRE(same_state(old_state, observe(env, index)));
        CHECK(artifact_names(env.bundle) == old_files);
        env.require_equivalent(seed + 1U);
        REQUIRE(env.manager->patch_game_metadata(edit_bundle::id_of(index), patch_for(env.corpus[index], target)).has_value());
        env.corpus[index] = target;
        env.require_equivalent(seed + 2U);
    }
    SECTION("unknown, but the row landed: fail closed, keep every candidate, recover on reopen")
    {
        constexpr auto seed = std::uint64_t {813U};
        edit_bundle env {seed, standard_games, /*tree=*/true};
        auto const index = std::size_t {2};
        auto const target = indexed_target(env.corpus[index]);
        auto const files_before = artifact_names(env.bundle);
        {
            auto const faults = armed_faults {{.at = point::before_sqlite_commit, .action = fault_action::commit_error_after_commit},
                                              {.at = point::before_commit_reconcile, .action = fault_action::fail_io}};
            auto const failed = env.manager->patch_game_metadata(edit_bundle::id_of(index), patch_for(env.corpus[index], target));
            REQUIRE_FALSE(failed.has_value());
            // The edit itself failed (unknown COMMIT outcome): the indexes it leaves unusable do not
            // turn that into the stale-baseline refusal, which is reserved for a gate that ran first.
            CHECK(failed.error() == error_code::io_failure);
        }
        // Canonical rows are new and the in-memory snapshot is old: no query may combine them.
        CHECK_FALSE(env.status().postings_usable);
        CHECK_FALSE(env.manager->query_position_matches(zobrist_hash {start_hash()}).has_value());
        CHECK_FALSE(env.manager->query_unfiltered_opening_stats(zobrist_hash {start_hash()}).has_value());
        // The candidate files are potentially referenced: none may be deleted.
        auto const files_now = artifact_names(env.bundle);
        CHECK(files_now.size() > files_before.size());
        for (auto const& name : files_before) {
            CHECK(files_now.contains(name));
        }
        env.corpus[index] = target;
        env.reopen();  // the row proves ownership: the new chain loads without a rebuild
        env.require_equivalent(seed + 1U);
        env.require_clean_directory();
    }
    SECTION("unknown, but the transaction was discarded: old state keeps serving, candidates wait for GC")
    {
        constexpr auto seed = std::uint64_t {814U};
        edit_bundle env {seed, standard_games, /*tree=*/true};
        auto const index = std::size_t {2};
        auto const target = indexed_target(env.corpus[index]);
        auto const old_state = observe(env, index);
        auto const files_before = artifact_names(env.bundle);
        {
            auto const faults = armed_faults {{.at = point::before_sqlite_commit, .action = fault_action::commit_error_after_rollback},
                                              {.at = point::before_commit_reconcile, .action = fault_action::fail_io}};
            REQUIRE_FALSE(env.manager->patch_game_metadata(edit_bundle::id_of(index), patch_for(env.corpus[index], target)).has_value());
        }
        REQUIRE(same_state(old_state, observe(env, index)));  // sequence still equal: the old state is genuine
        CHECK(artifact_names(env.bundle).size() > files_before.size());  // candidates preserved, never guessed away
        env.require_equivalent(seed + 1U);
        // The next successful publication collects them.
        REQUIRE(env.manager->patch_game_metadata(edit_bundle::id_of(index), patch_for(env.corpus[index], target)).has_value());
        env.corpus[index] = target;
        env.require_equivalent(seed + 2U);
        env.require_clean_directory();
    }
}

// ── Rows 9-12: after COMMIT nothing can fail the edit ────────────────────────

TEST_CASE("failures after COMMIT (install, synchronous restore, manifest mirror) never undo a committed edit",
          "[motif-db][pr6b][failure][postcommit]")
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
    for (auto const failing : points) {
        INFO("boundary " << static_cast<int>(failing));
        constexpr auto seed = std::uint64_t {821U};
        edit_bundle env {seed, standard_games, /*tree=*/true};
        auto const index = std::size_t {2};
        auto const target = indexed_target(env.corpus[index]);
        auto const seq = env.status().mutation_seq;
        {
            auto const faults = armed_faults {{.at = failing, .action = fault_action::fail_io}};
            auto const outcome = env.manager->patch_game_metadata(edit_bundle::id_of(index), patch_for(env.corpus[index], target));
            REQUIRE(outcome.has_value());
            CHECK(crossings_of(failing) >= 1U);
        }
        env.corpus[index] = target;
        CHECK(env.status().mutation_seq == seq + 1U);
        env.require_equivalent(seed);
        // The mirror is a cache: whatever state it was left in, reopen repairs it from SQLite.
        env.reopen();
        CHECK(manifest_cache_current(env.bundle / "manifest.json", env.manager->manifest()));
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

// Opens the bundle in a child, arms the crash and runs the patch. Returns the exit code.
auto run_patch_in_crashing_child(fs::path const& bundle, point const crash_point, game_id const target, game_patch const& patch) -> int
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
        auto const outcome = manager->patch_game_metadata(target, patch);
        _exit(outcome.has_value() ? 0 : 3);
    }
    auto status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear crash matrix.
TEST_CASE("a process dying at any boundary leaves either the entire old tuple or the entire new tuple", "[motif-db][pr6b][failure][crash]")
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
    for (auto const mixed_manual : {false, true}) {
        for (auto const& crash : cases) {
            INFO("crash at " << static_cast<int>(crash.at) << (mixed_manual ? " (mixed manual patch)" : ""));
            constexpr auto seed = std::uint64_t {831U};
            edit_bundle env {seed, standard_games, /*tree=*/true};
            auto const index = mixed_manual ? std::size_t {3} : std::size_t {2};
            auto const patch =
                mixed_manual ? wide_patch(env.corpus[index]) : patch_for(env.corpus[index], indexed_target(env.corpus[index]));
            auto const old_state = observe(env, index);
            auto const old_corpus = env.corpus;
            env.manager.reset();  // the child owns the bundle

            REQUIRE(run_patch_in_crashing_child(env.bundle, crash.at, edit_bundle::id_of(index), patch) == crash_exit_code);

            env.reopen();
            auto const recovered = observe(env, index);
            REQUIRE(recovered.postings_usable);
            REQUIRE(recovered.tree_usable);
            if (crash.expect_new) {
                env.corpus[index] = indexed_target(old_corpus[index]);
                CHECK(recovered.sequence == old_state.sequence + 1U);
                CHECK(recovered.generation == old_state.generation + 1U);
                CHECK(recovered.result == env.corpus[index].result);
                CHECK(recovered.white_elo == env.corpus[index].white_elo);
                if (mixed_manual) {
                    CHECK(recovered.white_name == "Brand New Player");
                    CHECK(recovered.has_event);
                    CHECK(recovered.tag_count == old_state.tag_count + 1U);
                }
            } else {
                // Not one field of the tuple moved.
                REQUIRE(same_state(old_state, recovered));
            }
            env.require_equivalent(seed);
            // No corpus replay was needed to recover, and the next edit works.
            auto const next = indexed_target(env.corpus[index]);
            env.edit(index, next);
            env.require_equivalent(seed + 1U);
        }
    }
}
#endif

// ── Hard caps: fold instead of exceeding them ────────────────────────────────

namespace
{

auto small_caps(std::size_t const hard_runs) -> patch_chain_limits
{
    constexpr auto soft_encoded = std::uint64_t {1} << 20U;
    constexpr auto hard_encoded = std::uint64_t {1} << 24U;
    constexpr auto hard_decoded = std::uint64_t {1} << 26U;
    return patch_chain_limits {.soft_runs = hard_runs > 1 ? hard_runs - 1 : 1,
                               .soft_encoded_bytes = soft_encoded,
                               .hard_runs = hard_runs,
                               .hard_encoded_bytes = hard_encoded,
                               .hard_decoded_bytes = hard_decoded};
}

// A result that certainly differs from the stored one, cycling through all four.
auto next_result(std::string const& current) -> std::string
{
    static constexpr auto cycle = std::array<std::string_view, 4> {"1-0", "1/2-1/2", "0-1", "*"};
    for (std::size_t index = 0; index < cycle.size(); ++index) {
        if (cycle.at(index) == current) {
            return std::string {cycle.at((index + 1U) % cycle.size())};
        }
    }
    return std::string {cycle[0]};
}

auto metadata_checkpoint_records(edit_bundle& env) -> std::size_t
{
    auto const& chain = env.manager->manifest().payload.metadata;
    REQUIRE(chain.checkpoint.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const& member = *chain.checkpoint;
    auto artifact = load_metadata_artifact(
        env.bundle / member.artifact.filename,
        member.artifact,
        metadata_artifact_expectation {
            .identity = {.bundle_id = bundle_identity_of(env.manager->manifest()), .index_semantics = index_semantics_version},
            .kind = metadata_artifact_kind::checkpoint,
            .from_exclusive_seq = member.from_exclusive_seq,
            .to_inclusive_seq = member.to_inclusive_seq});
    REQUIRE(artifact.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return artifact->records.size();
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear edit-and-verify sequence.
TEST_CASE("the hard cap folds the chains into new checkpoints instead of publishing an oversized chain", "[motif-db][pr6b][cap]")
{
    constexpr auto seed = std::uint64_t {841U};
    constexpr auto edit_count = 14;
    constexpr auto reopen_check_seed = seed + 100U;
    edit_bundle env {seed, standard_games, /*tree=*/true};
    env.manager->set_chain_limits(small_caps(3));
    auto const original_checkpoint = env.manager->manifest().tree()->checkpoint.filename;
    auto rng = std::mt19937_64 {seed};  // NOLINT(cert-msc51-cpp,cert-msc32-c) -- a fixed seed keeps the fixtures reproducible.
    auto folds = 0;
    for (auto step = 1; step <= edit_count; ++step) {
        auto const index = std::uniform_int_distribution<std::size_t> {0, env.corpus.size() - 1}(rng);
        auto target = env.corpus[index];
        target.result = next_result(target.result);
        env.edit(index, target);
        auto const& report = env.manager->last_patch_report();
        REQUIRE(report.published);
        // Never over the cap; the chains reset exactly when the fourth run would have been published.
        CHECK(report.tree_runs <= 3U);
        CHECK(report.metadata_runs <= 3U);
        CHECK(report.replayed_games == 1U);  // a fold replays no corpus
        if (report.tree_folded) {
            ++folds;
            CHECK(report.tree_runs == 0U);
            CHECK(report.metadata_folded);
            CHECK(env.manager->manifest().tree()->patch_runs.empty());
            CHECK(env.manager->manifest().tree()->base_mutation_seq == env.manager->manifest().tree()->covered_mutation_seq);
            CHECK(env.manager->manifest().tree()->checkpoint.filename != original_checkpoint);
            REQUIRE(env.manager->manifest().payload.metadata.checkpoint.has_value());
            CHECK(env.manager->manifest().payload.metadata.runs.empty());
        }
        env.require_clean_directory();  // superseded checkpoints and runs were collected exactly once
        env.require_equivalent(seed + static_cast<std::uint64_t>(step));
    }
    CHECK(folds == 3);  // 14 edits against a cap of 3 runs: folds at edits 4, 8 and 12
    CHECK_FALSE(artifact_names(env.bundle).contains(original_checkpoint));
    env.reopen();
    env.require_equivalent(reopen_check_seed);
    env.edit(1,
             [&]() -> model_game
             {
                 auto flipped = env.corpus[1];
                 flipped.result = next_result(flipped.result);
                 return flipped;
             }());
    env.require_equivalent(reopen_check_seed + 1U);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear check sequence.
TEST_CASE("chain limits are clamped to what a reopen accepts, so a huge limit never publishes an unopenable chain", "[motif-db][pr6b][cap]")
{
    constexpr auto seed = std::uint64_t {848U};
    constexpr auto random_games = std::size_t {6};
    edit_bundle env {seed, random_games, /*tree=*/true};
    auto const unbounded = std::numeric_limits<std::uint64_t>::max();
    env.manager->set_chain_limits(patch_chain_limits {.soft_runs = std::numeric_limits<std::size_t>::max(),
                                                      .soft_encoded_bytes = unbounded,
                                                      .hard_runs = std::numeric_limits<std::size_t>::max(),
                                                      .hard_encoded_bytes = unbounded,
                                                      .hard_decoded_bytes = unbounded});
    CHECK(env.manager->chain_limits().hard_runs == max_patch_chain_runs);
    CHECK(env.manager->chain_limits().hard_decoded_bytes == max_chain_resident_bytes);

    // The manifest decoder refuses a chain longer than max_patch_chain_runs, so the
    // edit that would append run number max_patch_chain_runs + 1 must fold instead.
    for (std::size_t step = 1; step <= max_patch_chain_runs; ++step) {
        auto target = env.corpus[3];
        target.result = next_result(target.result);
        env.edit(3, target);
        auto const& report = env.manager->last_patch_report();
        REQUIRE_FALSE(report.tree_folded);
        REQUIRE_FALSE(report.metadata_folded);
        REQUIRE(report.tree_runs == step);
        REQUIRE(report.metadata_runs == step);
    }
    env.reopen();  // the longest admitted chain still opens as a chain
    CHECK(env.status().postings_usable);
    CHECK(env.status().tree_usable);
    CHECK(env.manager->manifest().tree()->patch_runs.size() == max_patch_chain_runs);

    auto target = env.corpus[3];
    target.result = next_result(target.result);
    env.edit(3, target);
    CHECK(env.manager->last_patch_report().tree_folded);
    CHECK(env.manager->last_patch_report().metadata_folded);
    CHECK(env.manager->last_patch_report().tree_runs == 0U);
    env.reopen();
    CHECK(env.status().postings_usable);
    CHECK(env.status().tree_usable);
    env.require_equivalent(seed);
}

TEST_CASE("repeated edits to one game keep a single metadata checkpoint entry; editing every game bounds it by the game count",
          "[motif-db][pr6b][cap]")
{
    {
        constexpr auto seed = std::uint64_t {842U};
        constexpr auto edit_count = 9;
        edit_bundle env {seed, standard_games, /*tree=*/true};
        env.manager->set_chain_limits(small_caps(2));
        for (auto step = 0; step < edit_count; ++step) {
            auto target = env.corpus[4];
            target.result = next_result(target.result);
            env.edit(4, target);
        }
        // The last fold happened at the 9th edit (cap 2: runs 1, 2, then fold) and kept one record per game.
        REQUIRE(env.manager->manifest().payload.metadata.checkpoint.has_value());
        CHECK(metadata_checkpoint_records(env) == 1U);
        env.require_equivalent(seed + 1U);
    }
    {
        constexpr auto seed = std::uint64_t {843U};
        constexpr auto random_games = std::size_t {8};
        edit_bundle env {seed, random_games, /*tree=*/false};
        env.manager->set_chain_limits(small_caps(2));
        for (std::size_t index = 0; index < env.corpus.size(); ++index) {
            auto target = env.corpus[index];
            target.result = next_result(target.result);
            env.edit(index, target);
        }
        // One more edit forces a fold if the last one did not.
        auto target = env.corpus[0];
        target.result = next_result(target.result);
        env.edit(0, target);
        target = env.corpus[1];
        target.result = next_result(target.result);
        env.edit(1, target);
        REQUIRE(env.manager->manifest().payload.metadata.checkpoint.has_value());
        CHECK(metadata_checkpoint_records(env) <= env.corpus.size());
        env.require_equivalent(seed + 1U);
        env.reopen();
        env.require_equivalent(seed + 2U);
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear fold check.
TEST_CASE("a candidate too large for the byte or resident budgets is folded, never published as an oversized chain",
          "[motif-db][pr6b][cap]")
{
    for (auto const which : {0, 1}) {
        constexpr auto seed = std::uint64_t {844U};
        constexpr auto generous_runs = std::size_t {16};
        constexpr auto tiny_decoded_cap = std::uint64_t {16};
        edit_bundle env {seed, standard_games, /*tree=*/true};
        auto limits = small_caps(generous_runs);
        if (which == 0) {
            limits.hard_decoded_bytes = tiny_decoded_cap;  // smaller than any single decoded run
        } else {
            limits.hard_encoded_bytes = 1;  // smaller than any single encoded run
        }
        env.manager->set_chain_limits(limits);
        for (auto step = 0; step < 3; ++step) {
            auto target = env.corpus[2 + static_cast<std::size_t>(step)];
            target.result = next_result(target.result);
            env.edit(2 + static_cast<std::size_t>(step), target);
            CHECK(env.manager->last_patch_report().tree_folded);
            CHECK(env.manager->last_patch_report().metadata_folded);
            CHECK(env.manager->last_patch_report().tree_runs == 0U);
            CHECK(env.manager->last_patch_report().metadata_runs == 0U);
            env.require_clean_directory();
        }
        env.require_equivalent(seed + 1U);
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear fold-and-retry check.
TEST_CASE("a failed or unaffordable fold leaves the old chain serving and a retry folds", "[motif-db][pr6b][cap][failure]")
{
    for (auto const failing :
         {point::before_disk_admission, point::after_artifact_validation, point::before_artifact_fsync, point::before_sqlite_commit})
    {
        INFO("boundary " << static_cast<int>(failing));
        constexpr auto seed = std::uint64_t {845U};
        edit_bundle env {seed, standard_games, /*tree=*/true};
        env.manager->set_chain_limits(small_caps(2));
        for (auto step = 0; step < 2; ++step) {
            auto target = env.corpus[2];
            target.result = next_result(target.result);
            env.edit(2, target);
        }
        REQUIRE(env.manager->manifest().tree()->patch_runs.size() == 2U);  // the next edit must fold
        auto const old_state = observe(env, 2);
        auto const old_files = artifact_names(env.bundle);
        auto target = env.corpus[2];
        target.result = next_result(target.result);
        {
            auto const faults = armed_faults {{.at = failing, .action = fault_action::fail_io}};
            REQUIRE_FALSE(env.manager->patch_game_metadata(edit_bundle::id_of(2), patch_for(env.corpus[2], target)).has_value());
            CHECK(crossings_of(failing) >= 1U);
        }
        REQUIRE(same_state(old_state, observe(env, 2)));
        CHECK(artifact_names(env.bundle) == old_files);
        env.require_equivalent(seed + 1U);
        env.edit(2, target);
        CHECK(env.manager->last_patch_report().tree_folded);
        env.require_equivalent(seed + 2U);
    }
}

TEST_CASE("a corrupt final folded checkpoint is rejected before commit", "[motif-db][pr6b][cap][failure]")
{
    constexpr auto seed = std::uint64_t {847U};
    edit_bundle env {seed, standard_games, /*tree=*/true};
    env.manager->set_chain_limits(small_caps(2));
    for (auto step = 0; step < 2; ++step) {
        auto target = env.corpus[2];
        target.result = next_result(target.result);
        env.edit(2, target);
    }
    auto const old_state = observe(env, 2);
    auto const old_files = artifact_names(env.bundle);
    auto target = env.corpus[2];
    target.result = next_result(target.result);
    g_corrupt_bundle = &env.bundle;
    g_corrupt_old_files = &old_files;
    g_corrupted_checkpoint = false;
    auto const previous = fault::set_hook(corrupt_final_checkpoint);
    auto const result = env.manager->patch_game_metadata(edit_bundle::id_of(2), patch_for(env.corpus[2], target));
    fault::set_hook(previous);
    g_corrupt_bundle = nullptr;
    g_corrupt_old_files = nullptr;
    REQUIRE(g_corrupted_checkpoint);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(same_state(old_state, observe(env, 2)));
    CHECK(artifact_names(env.bundle) == old_files);
    env.edit(2, target);
    CHECK(env.manager->last_patch_report().tree_folded);
    env.require_equivalent(seed + 1U);
}

TEST_CASE("an interrupted garbage collection after a fold leaks files but never a live one", "[motif-db][pr6b][cap][failure]")
{
    constexpr auto seed = std::uint64_t {846U};
    edit_bundle env {seed, standard_games, /*tree=*/true};
    env.manager->set_chain_limits(small_caps(2));
    for (auto step = 0; step < 2; ++step) {
        auto target = env.corpus[2];
        target.result = next_result(target.result);
        env.edit(2, target);
    }
    auto target = env.corpus[2];
    target.result = next_result(target.result);
    {
        auto const faults = armed_faults {{.at = point::before_cleanup, .action = fault_action::fail_io}};
        REQUIRE(env.manager->patch_game_metadata(edit_bundle::id_of(2), patch_for(env.corpus[2], target)).has_value());
        CHECK(crossings_of(point::before_cleanup) >= 1U);
    }
    env.corpus[2] = target;
    CHECK(env.manager->last_patch_report().tree_folded);
    // The superseded chain leaked, every live file is still there.
    auto const live = referenced_names(env.manager->manifest());
    auto const present = artifact_names(env.bundle);
    for (auto const& name : live) {
        CHECK(present.contains(name));
    }
    CHECK(present.size() > live.size());
    env.require_equivalent(seed + 1U);
    env.reopen();
    env.require_equivalent(seed + 2U);
    // The next publication collects the leftovers.
    auto again = env.corpus[2];
    again.result = next_result(again.result);
    env.edit(2, again);
    env.require_clean_directory();
}

// ── Rebuild interplay ────────────────────────────────────────────────────────

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear rebuild sequence.
TEST_CASE("rebuild after edits and edits after a rebuild stay equal to a fresh rebuild", "[motif-db][pr6b][rebuild]")
{
    constexpr auto seed = std::uint64_t {851U};
    constexpr auto random_games = std::size_t {12};
    constexpr auto edit_count = 5;
    constexpr auto last_check_seed = seed + 5U;
    edit_bundle env {seed, random_games, /*tree=*/true};
    auto rng = std::mt19937_64 {seed};  // NOLINT(cert-msc51-cpp,cert-msc32-c) -- a fixed seed keeps the fixtures reproducible.
    for (auto step = 0; step < edit_count; ++step) {
        auto const index = std::uniform_int_distribution<std::size_t> {0, env.corpus.size() - 1}(rng);
        auto target = env.corpus[index];
        target.result = next_result(target.result);
        target.white_elo = motif_test::pick_elo(rng);
        env.edit(index, target);
    }
    REQUIRE_FALSE(env.manager->manifest().payload.metadata.empty());
    auto const tree_runs = env.manager->manifest().tree()->patch_runs.size();
    REQUIRE(tree_runs > 0U);

    // A postings rebuild embeds the current canonical metadata in a new segment, clears the
    // metadata chain, and keeps the healthy tree together with its runs.
    REQUIRE(env.manager->rebuild_position_postings().has_value());
    CHECK(env.manager->manifest().payload.metadata.empty());
    CHECK(env.manager->manifest().postings()->source_mutation_seq == env.status().mutation_seq);
    CHECK(env.manager->manifest().tree()->patch_runs.size() == tree_runs);
    env.require_equivalent(seed + 1U);
    env.edit(1,
             [&]() -> model_game
             {
                 auto flipped = env.corpus[1];
                 flipped.result = next_result(flipped.result);
                 return flipped;
             }());
    env.require_equivalent(seed + 2U);

    // A tree rebuild writes a v6 checkpoint at the current sequence and clears only the tree runs.
    auto const metadata_runs = env.manager->manifest().payload.metadata.runs.size();
    REQUIRE(env.manager->rebuild_opening_tree_index().has_value());
    CHECK(env.manager->manifest().tree()->patch_runs.empty());
    CHECK(env.manager->manifest().payload.metadata.runs.size() == metadata_runs);
    env.require_equivalent(seed + 3U);
    env.edit(2,
             [&]() -> model_game
             {
                 auto flipped = env.corpus[2];
                 flipped.result = next_result(flipped.result);
                 return flipped;
             }());
    env.require_equivalent(seed + 4U);
    env.reopen();
    env.require_equivalent(last_check_seed);
}

// ── Reimport: identity excludes ratings but not the result ───────────────────

TEST_CASE("reimporting a rating-corrected game is skipped as a duplicate and keeps the correction; the original result adds a second game",
          "[motif-db][pr6b][reimport]")
{
    constexpr auto seed = 861U;
    edit_bundle env {seed, standard_games, /*tree=*/true, /*manual_stride=*/0};
    auto const index = std::size_t {2};
    auto const original = env.corpus[index];
    auto const as_imported = to_db_game(original, index);

    // A rating correction: reimporting the original (old ratings) is a duplicate.
    auto corrected = original;
    constexpr auto corrected_rating = 1999;
    corrected.white_elo = corrected_rating;
    corrected.black_elo = std::nullopt;
    env.edit(index, corrected);
    auto const games_before = env.manager->store().count_games();
    REQUIRE(games_before.has_value());
    auto const duplicate = env.manager->insert_game(as_imported);
    REQUIRE_FALSE(duplicate.has_value());
    CHECK(duplicate.error() == error_code::duplicate);
    CHECK(env.manager->store().count_games().value_or(-1) == *games_before);  // NOLINT(bugprone-unchecked-optional-access)
    auto const kept = env.manager->store().get(edit_bundle::id_of(index));
    REQUIRE(kept.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    CHECK(kept->white.elo == 1999);
    CHECK_FALSE(kept->black.elo.has_value());
    // NOLINTEND(bugprone-unchecked-optional-access)
    CHECK(env.status().postings_usable);  // a skipped duplicate changed nothing

    // A result correction changes the identity: the original-result record is a different game.
    auto flipped = corrected;
    flipped.result = distinct_result(original.result);
    env.edit(index, flipped);
    auto const second = env.manager->insert_game(as_imported);
    REQUIRE(second.has_value());
    CHECK(env.manager->store().count_games().value_or(-1) == *games_before + 1);  // NOLINT(bugprone-unchecked-optional-access)
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(second->value != edit_bundle::id_of(index).value);
    CHECK_FALSE(env.status().postings_usable);  // the insert is a PR 5 mutation: the baseline is stale until rebuilt
}

// ── Reader exclusion, copies, caches ─────────────────────────────────────────

TEST_CASE("a read scope held across an edit blocks it, and a reader after the edit sees new canonical and new derived state",
          "[motif-db][pr6b][lock]")
{
    constexpr auto seed = 871U;
    constexpr auto reader_hold = std::chrono::milliseconds {300};
    edit_bundle env {seed, standard_games, /*tree=*/true};
    auto const index = std::size_t {2};
    auto const target = indexed_target(env.corpus[index]);
    auto const before_rows = env.manager->query_unfiltered_opening_stats(zobrist_hash {start_hash()});
    REQUIRE(before_rows.has_value());

    auto finished = std::atomic<bool> {false};
    auto edited = result<void> {};
    {
        auto scope = env.manager->lock_generation();
        auto editor =
            std::thread {[&]() -> void
                         {
                             edited = env.manager->patch_game_metadata(edit_bundle::id_of(index), patch_for(env.corpus[index], target));
                             finished = true;
                         }};
        std::this_thread::sleep_for(reader_hold);
        // The reader still holds the generation: the edit has not started to publish.
        CHECK_FALSE(finished.load());
        auto const unchanged = env.manager->query_unfiltered_opening_stats(zobrist_hash {start_hash()});
        REQUIRE(unchanged.has_value());
        CHECK(rows_equal(*unchanged, *before_rows));
        scope.unlock();
        editor.join();
    }
    REQUIRE(edited.has_value());
    CHECK(finished.load());
    env.corpus[index] = target;
    env.require_equivalent(seed);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear copy-and-reopen check.
TEST_CASE("a bundle directory copied after edits opens with the chains intact and no rebuild", "[motif-db][pr6b][copy]")
{
    constexpr auto seed = std::uint64_t {872U};
    edit_bundle env {seed, standard_games, /*tree=*/true};
    for (auto step = 0; step < 4; ++step) {
        auto target = env.corpus[static_cast<std::size_t>(step) + 1U];
        target.result = next_result(target.result);
        env.edit(static_cast<std::size_t>(step) + 1U, target);
    }
    auto const generation = env.status().publication_generation;
    auto const sequence = env.status().mutation_seq;
    auto const corpus = env.corpus;
    env.manager.reset();

    temp_dir const copy_dir {"copy"};
    auto const copy = copy_dir.path / "copy";
    fs::create_directories(copy);
    for (auto const& entry : fs::directory_iterator {env.bundle}) {
        if (entry.is_regular_file() && entry.path().filename() != ".motif.lock") {
            fs::copy_file(entry.path(), copy / entry.path().filename());
        }
    }
    auto opened = database_manager::open(copy);
    REQUIRE(opened.has_value());
    CHECK(opened->derived_index_status()->publication_generation == generation);
    CHECK(opened->derived_index_status()->mutation_seq == sequence);
    CHECK(opened->derived_index_status()->postings_usable);
    CHECK(opened->derived_index_status()->tree_usable);
    CHECK(artifact_names(copy) == referenced_names(opened->manifest()));
    motif_test::require_equivalent_to_fresh(*opened, copy, corpus, /*with_tree=*/true, seed + 1U);
}

TEST_CASE("a rolled-back mixed manual patch leaves no stale player, event or tag ids behind", "[motif-db][pr6b][cache]")
{
    constexpr auto seed = 873U;
    edit_bundle env {seed, standard_games, /*tree=*/true};
    auto const index = std::size_t {3};  // manual
    auto const patch = wide_patch(env.corpus[index]);
    {
        auto const faults = armed_faults {{.at = point::after_sequence_bump, .action = fault_action::fail_io}};
        REQUIRE_FALSE(env.manager->patch_game_metadata(edit_bundle::id_of(index), patch).has_value());
    }
    // The player/event/tag rows the apply created were rolled back with the transaction. A
    // later insert naming them must create fresh rows (a stale cached id would break its FK).
    constexpr auto new_game_number = std::size_t {77};
    auto stranger = to_db_game(env.corpus[index], new_game_number);
    stranger.white.name = "Brand New Player";
    stranger.event_details = event {.name = "A Brand New Event", .site = std::nullopt, .date = std::nullopt};
    stranger.extra_tags = {{"_notes", "wide patch"}};
    REQUIRE(env.manager->insert_game(stranger).has_value());
}
