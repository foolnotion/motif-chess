#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chesslib/board/board.hpp>
#include <chesslib/board/move_codec.hpp>
#include <chesslib/core/types.hpp>
#include <sqlite3.h>

#include "motif/db/database_manager.hpp"
#ifndef _WIN32
#    include <sys/wait.h>
#    include <unistd.h>
#endif

#include "motif/db/error.hpp"
#include "motif/db/fault_injection.hpp"
#include "motif/db/manifest.hpp"
#include "motif/db/schema.hpp"
#include "motif/db/types.hpp"

// The SQLite-first publication protocol: build -> fsync -> rename (no replace)
// -> fsync directory -> FULL-synchronous COMMIT -> install -> manifest mirror,
// recovery at every boundary, the cache's non-authority, and the coverage gate.

namespace
{

namespace fault = motif::db::fault;
using fault::point;
namespace fs = std::filesystem;
// Ratings of the sample game, and the Elo bucket width the position queries use.
constexpr auto white_rating = 2400;
constexpr auto black_rating = 2300;
constexpr auto bucket_width = 100;
// How far past the stored snapshot the forged "newer" manifest sits.
constexpr auto generations_ahead = std::uint64_t {5};
// Bytes kept of the manifest cache when a test truncates it mid-token.
constexpr auto truncated_cache_bytes = std::size_t {40};

struct tmp_dir
{
    fs::path path;

    explicit tmp_dir(std::string const& suffix)
    {
        auto const tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("motif_publication_test_" + suffix + "_" + std::to_string(tick));
    }

    ~tmp_dir()
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }

    tmp_dir(tmp_dir const&) = delete;
    auto operator=(tmp_dir const&) -> tmp_dir& = delete;
    tmp_dir(tmp_dir&&) = delete;
    auto operator=(tmp_dir&&) -> tmp_dir& = delete;
};

enum class first_move : std::uint8_t
{
    e4,
    d4,
};

auto make_game(std::string white, std::string black, std::string result = "1-0", first_move const opening = first_move::e4)
    -> motif::db::game
{
    chesslib::move move {};
    if (opening == first_move::e4) {
        move.source_square = chesslib::square::e2;
        move.target_square = chesslib::square::e4;
    } else {
        move.source_square = chesslib::square::d2;
        move.target_square = chesslib::square::d4;
    }
    move.double_pawn = 1;
    return motif::db::game {
        .white = {.name = std::move(white), .elo = white_rating, .title = {}, .country = {}},
        .black = {.name = std::move(black), .elo = black_rating, .title = {}, .country = {}},
        .event_details = {},
        .date = {},
        .result = std::move(result),
        .eco = {},
        .moves = {chesslib::codec::encode(move)},
        .extra_tags = {},
        .provenance = {},
    };
}

auto start_hash() -> motif::db::zobrist_hash
{
    return motif::db::zobrist_hash {chesslib::board {}.hash()};
}

auto read_bytes(fs::path const& path) -> std::string
{
    std::ifstream input {path, std::ios::binary};
    return {std::istreambuf_iterator<char> {input}, std::istreambuf_iterator<char> {}};
}

void write_bytes(fs::path const& path, std::string const& bytes)
{
    std::ofstream output {path, std::ios::binary | std::ios::trunc};
    output << bytes;
}

// Names of derived-artifact files (published or leftover) in a bundle.
auto artifact_files(fs::path const& dir) -> std::vector<std::string>
{
    std::vector<std::string> names;
    for (auto const& entry : fs::directory_iterator {dir}) {
        auto const name = entry.path().filename().string();
        if (name.starts_with("positions.postings.") || name.starts_with("opening_tree.idx.")) {
            names.push_back(name);
        }
    }
    std::ranges::sort(names);
    return names;
}

// One game, published postings, closed.
void make_bundle(fs::path const& dir, bool const with_tree = false)
{
    auto mgr = motif::db::database_manager::create(dir, "publication-test");
    REQUIRE(mgr.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    REQUIRE(mgr->insert_game(make_game("White", "Black")).has_value());
    REQUIRE(mgr->rebuild_position_postings().has_value());
    if (with_tree) {
        REQUIRE(mgr->rebuild_opening_tree_index().has_value());
    }
    mgr->close();
    // NOLINTEND(bugprone-unchecked-optional-access)
}

auto raw_exec(fs::path const& dir, char const* sql) -> int
{
    sqlite3* conn = nullptr;
    REQUIRE(sqlite3_open((dir / "games.db").c_str(), &conn) == SQLITE_OK);
    auto const status = sqlite3_exec(conn, sql, nullptr, nullptr, nullptr);
    sqlite3_close(conn);
    return status;
}

// ── Fault seam plumbing (the hook is a plain function pointer) ───────────────

// The hook is a plain function pointer, so what it records and arms is process-global by necessity.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
constexpr std::size_t max_recorded = 256;
std::array<point, max_recorded> g_recorded {};
std::size_t g_recorded_count {0};
point g_fail_at {};
bool g_fail_armed {false};
point g_crash_at {};

// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

auto recording_hook(point const where, fault::artifact_kind /*kind*/, std::uint64_t /*generation*/) noexcept -> fault::fault_action
{
    if (g_recorded_count < max_recorded) {
        g_recorded.at(g_recorded_count++) = where;
    }
    return g_fail_armed && where == g_fail_at ? fault::fault_action::fail_io : fault::fault_action::proceed;
}

// Records every boundary crossed and optionally fails one of them.
class fault_scope
{
  public:
    explicit fault_scope(std::optional<point> const fail_at = std::nullopt)
    {
        g_recorded_count = 0;
        g_fail_armed = fail_at.has_value();
        if (fail_at) {
            g_fail_at = *fail_at;
        }
        previous_ = fault::set_hook(recording_hook);
    }

    ~fault_scope()
    {
        fault::set_hook(previous_);
        g_fail_armed = false;
    }

    fault_scope(fault_scope const&) = delete;
    auto operator=(fault_scope const&) -> fault_scope& = delete;
    fault_scope(fault_scope&&) = delete;
    auto operator=(fault_scope&&) -> fault_scope& = delete;

  private:
    fault::hook_function previous_ {nullptr};
};

auto first_index_of(point const where) -> std::optional<std::size_t>
{
    for (std::size_t index = 0; index < g_recorded_count; ++index) {
        if (g_recorded.at(index) == where) {
            return index;
        }
    }
    return std::nullopt;
}

auto fail_any_build(point const where, fault::artifact_kind /*kind*/, std::uint64_t /*generation*/) noexcept -> fault::fault_action
{
    return where == point::before_build ? fault::fault_action::fail_io : fault::fault_action::proceed;
}

#ifndef _WIN32
// The status the crash hook's child exits with, so the parent can tell a crash at the boundary from any other exit.
constexpr auto crash_exit_code = 77;

auto crash_hook(point const where, fault::artifact_kind /*kind*/, std::uint64_t /*generation*/) noexcept -> fault::fault_action
{
    if (where == g_crash_at) {
        // Simulates the process dying right here: no destructor, no close(), no
        // cache write, no cleanup.
        _exit(crash_exit_code);
    }
    return fault::fault_action::proceed;
}

// Forks; the child runs body() with the crash hook armed at `at` and exits 0
// if it completed. Returns the child's exit code.
template<typename Body>
auto run_in_crashing_child(point const crash_point, Body body) -> int
{
    g_crash_at = crash_point;
    auto const pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        fault::set_hook(crash_hook);
        body();
        _exit(0);
    }
    int status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
#endif

// Counts the position-dependent APIs that refuse to answer.
auto failing_position_queries(motif::db::database_manager& mgr, motif::db::game_id const known_game) -> int
{
    auto failures = 0;
    auto const hash = start_hash();
    auto const ids = std::array<motif::db::game_id, 1> {known_game};
    auto const id_vector = std::vector<motif::db::game_id> {known_game};
    failures += mgr.query_position_matches(hash).has_value() ? 0 : 1;
    failures += mgr.query_position_first_matches(hash, ids).has_value() ? 0 : 1;
    failures += mgr.position_summary(hash).has_value() ? 0 : 1;
    failures += mgr.position_game_ids(hash).has_value() ? 0 : 1;
    failures += mgr.query_elo_distribution(hash, {}, bucket_width).has_value() ? 0 : 1;
    failures += mgr.query_unfiltered_opening_stats(hash).has_value() ? 0 : 1;
    failures += mgr.query_filtered_opening_stats(hash, id_vector).has_value() ? 0 : 1;
    failures += mgr.query_tree_slice(hash, 2).has_value() ? 0 : 1;
    auto filter = motif::db::search_filter {};
    filter.position = hash;
    failures += mgr.find_games(filter).has_value() ? 0 : 1;
    return failures;
}

constexpr auto position_query_families = 9;

}  // namespace

// ── Coverage gate ────────────────────────────────────────────────────────────

TEST_CASE("publication: delete plus reinsert keeps the game count but every position query fails until rebuild",
          "[motif-db][publication][coverage]")
{
    tmp_dir const tdir {"equal_count"};
    auto mgr = motif::db::database_manager::create(tdir.path, "equal-count");
    REQUIRE(mgr.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    auto const first = mgr->insert_game(make_game("White", "Black", "1-0", first_move::e4));
    REQUIRE(first.has_value());
    REQUIRE(mgr->rebuild_position_postings().has_value());
    REQUIRE(failing_position_queries(*mgr, *first) == 0);

    REQUIRE(mgr->remove_game(*first).has_value());
    auto const second = mgr->insert_game(make_game("White", "Black", "0-1", first_move::d4));
    REQUIRE(second.has_value());
    auto const count = mgr->store().count_games();
    REQUIRE(count.has_value());
    // The count the old snapshot indexed is back, which is exactly what the
    // former count-equality gate accepted.
    CHECK(std::cmp_equal(*count, mgr->manifest().postings()->document_count));

    CHECK(failing_position_queries(*mgr, *second) == position_query_families);
    auto const status = mgr->derived_index_status();
    REQUIRE(status.has_value());
    CHECK_FALSE(status->postings_usable);
    CHECK(status->mutation_seq == 3U);  // insert, remove, insert
    CHECK(status->covered_mutation_seq == std::optional<std::uint64_t> {1});

    // Ordinary canonical reads stay available while the index is stale.
    CHECK(mgr->find_games(motif::db::search_filter {}).has_value());
    CHECK(mgr->store().get(*second).has_value());

    REQUIRE(mgr->rebuild_position_postings().has_value());
    CHECK(failing_position_queries(*mgr, *second) == 0);
    auto const ids = mgr->position_game_ids(start_hash());
    REQUIRE(ids.has_value());
    REQUIRE(ids->size() == 1U);
    CHECK(ids->front() == *second);
    CHECK(mgr->derived_index_status()->covered_mutation_seq == std::optional<std::uint64_t> {3});
    // NOLINTEND(bugprone-unchecked-optional-access)
}

TEST_CASE("publication: metadata the indexes do not embed publishes nothing; indexed fields publish incrementally",
          "[motif-db][publication][coverage]")
{
    tmp_dir const tdir {"metadata_class"};
    auto mgr = motif::db::database_manager::create(tdir.path, "metadata-class");
    REQUIRE(mgr.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    auto const game = mgr->insert_game(make_game("White", "Black"));
    REQUIRE(game.has_value());
    REQUIRE(mgr->set_manual_game_provenance(*game, std::nullopt, "new").has_value());
    REQUIRE(mgr->rebuild_position_postings().has_value());
    auto const generation = mgr->manifest().publication_generation;
    auto const sequence = mgr->derived_index_status()->mutation_seq;

    auto patch = motif::db::game_patch {};
    patch.event = "Some Event";
    patch.site = "Some Site";
    patch.date = "2024.01.01";
    patch.eco = "C20";
    patch.notes = "note";
    patch.white_name = "Renamed";
    REQUIRE(mgr->patch_game_metadata(*game, patch).has_value());
    REQUIRE(mgr->set_manual_game_provenance(*game, "label", "reviewed").has_value());
    CHECK(mgr->derived_index_status()->postings_usable);
    CHECK(failing_position_queries(*mgr, *game) == 0);
    CHECK(mgr->manifest().publication_generation == generation);
    CHECK(mgr->derived_index_status()->mutation_seq == sequence);

    // A rating (or result) correction is index-relevant, but it is one atomic operation: the
    // sequence advances once, one new snapshot covers it, and every query family stays available
    // and reports the corrected value.
    auto indexed = motif::db::game_patch {};
    constexpr auto corrected_rating = 2500;
    indexed.white_elo = motif::db::rating_patch::set(corrected_rating);
    REQUIRE(mgr->patch_game_metadata(*game, indexed).has_value());
    CHECK(mgr->derived_index_status()->postings_usable);
    CHECK(mgr->derived_index_status()->mutation_seq == sequence + 1U);
    CHECK(mgr->derived_index_status()->covered_mutation_seq == std::optional<std::uint64_t> {sequence + 1U});
    CHECK(mgr->manifest().publication_generation == generation + 1U);
    CHECK(failing_position_queries(*mgr, *game) == 0);
    auto const matches = mgr->query_position_matches(start_hash());
    REQUIRE(matches.has_value());
    REQUIRE(matches->size() == 1U);
    CHECK(matches->front().white_elo == std::optional<std::int16_t> {2500});
    CHECK(matches->front().black_elo == std::optional<std::int16_t> {2300});
    // NOLINTEND(bugprone-unchecked-optional-access)
}

TEST_CASE("publication: a pending canonical batch closes position queries and refuses publication", "[motif-db][publication][coverage]")
{
    tmp_dir const tdir {"pending"};
    auto mgr = motif::db::database_manager::create(tdir.path, "pending");
    REQUIRE(mgr.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    auto const game = mgr->insert_game(make_game("White", "Black"));
    REQUIRE(game.has_value());
    REQUIRE(mgr->rebuild_position_postings().has_value());
    REQUIRE(mgr->derived_index_status()->postings_usable);

    auto const scope = mgr->lock_generation();
    REQUIRE(mgr->writer().begin_transaction().has_value());
    // An open batch with no change yet leaves the snapshot valid...
    CHECK(mgr->derived_index_status()->postings_usable);
    REQUIRE(mgr->writer().insert(make_game("Other", "Player", "0-1", first_move::d4)).has_value());
    // ...but a reentrant query on this connection would see new rows under the
    // old sequence, so pending state alone must close the gate.
    auto const status = mgr->derived_index_status();
    REQUIRE(status.has_value());
    CHECK(status->pending_canonical_change);
    CHECK_FALSE(status->postings_usable);
    CHECK(failing_position_queries(*mgr, *game) == position_query_families);

    auto const refused = mgr->rebuild_position_postings();
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == motif::db::error_code::invalid_argument);

    mgr->writer().rollback_transaction();
    CHECK(mgr->derived_index_status()->postings_usable);
    CHECK(failing_position_queries(*mgr, *game) == 0);
    // NOLINTEND(bugprone-unchecked-optional-access)
}

TEST_CASE("publication: generation advances on every publication, the mutation sequence only on canonical change",
          "[motif-db][publication][coverage]")
{
    tmp_dir const tdir {"generations"};
    auto mgr = motif::db::database_manager::create(tdir.path, "generations");
    REQUIRE(mgr.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    CHECK(mgr->manifest().publication_generation == 1U);
    CHECK(mgr->derived_index_status()->mutation_seq == 0U);

    REQUIRE(mgr->insert_game(make_game("White", "Black")).has_value());
    CHECK(mgr->manifest().publication_generation == 1U);
    CHECK(mgr->derived_index_status()->mutation_seq == 1U);

    REQUIRE(mgr->rebuild_position_postings().has_value());
    CHECK(mgr->manifest().publication_generation == 2U);
    CHECK(mgr->derived_index_status()->mutation_seq == 1U);

    REQUIRE(mgr->rebuild_opening_tree_index().has_value());
    CHECK(mgr->manifest().publication_generation == 3U);
    CHECK(mgr->derived_index_status()->mutation_seq == 1U);
    CHECK(mgr->derived_index_status()->tree_usable);

    // Rebuilding the postings at the same sequence keeps a tree that is still valid.
    REQUIRE(mgr->rebuild_position_postings().has_value());
    CHECK(mgr->manifest().publication_generation == 4U);
    CHECK(mgr->manifest().tree() != nullptr);
    CHECK(mgr->derived_index_status()->tree_usable);
    // NOLINTEND(bugprone-unchecked-optional-access)
}

// ── Publication order and failure boundaries ─────────────────────────────────

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("publication: boundaries fire in order, files and directory sync before COMMIT, cache after", "[motif-db][publication][order]")
{
    tmp_dir const tdir {"order"};
    make_bundle(tdir.path);
    auto mgr = motif::db::database_manager::open(tdir.path);
    REQUIRE(mgr.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    REQUIRE(mgr->insert_game(make_game("Other", "Player", "0-1", first_move::d4)).has_value());

    {
        fault_scope const scope;
        REQUIRE(mgr->rebuild_position_postings().has_value());
    }
    constexpr auto expected_order = std::array {point::before_build,
                                                point::after_build,
                                                point::before_artifact_validation,
                                                point::after_artifact_validation,
                                                point::before_artifact_fsync,
                                                point::after_artifact_fsync,
                                                point::before_artifact_rename,
                                                point::after_artifact_rename,
                                                point::before_artifact_directory_fsync,
                                                point::after_artifact_directory_fsync,
                                                point::before_synchronous_full,
                                                point::before_snapshot_begin,
                                                point::after_snapshot_row_write,
                                                point::before_sqlite_commit,
                                                point::before_synchronous_restore,
                                                point::after_sqlite_commit,
                                                point::before_snapshot_install,
                                                point::after_snapshot_install,
                                                point::before_manifest_write,
                                                point::after_manifest_temp_write,
                                                point::after_manifest_temp_fsync,
                                                point::before_manifest_rename,
                                                point::after_manifest_rename,
                                                point::before_manifest_directory_fsync,
                                                point::after_manifest_write,
                                                point::before_cleanup,
                                                point::after_cleanup_unlink};
    std::optional<std::size_t> previous;
    for (auto const where : expected_order) {
        auto const index = first_index_of(where);
        INFO("boundary " << static_cast<int>(where));
        REQUIRE(index.has_value());
        if (previous) {
            CHECK(*index > *previous);
        }
        previous = index;
    }
    // The three durability steps all precede the single publication point.
    CHECK(*first_index_of(point::after_artifact_fsync) < *first_index_of(point::before_sqlite_commit));
    CHECK(*first_index_of(point::after_artifact_directory_fsync) < *first_index_of(point::before_sqlite_commit));
    CHECK(*first_index_of(point::before_manifest_write) > *first_index_of(point::after_sqlite_commit));
    // NOLINTEND(bugprone-unchecked-optional-access)
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("publication: a failure before the COMMIT leaves the previous snapshot authoritative and no orphan behind",
          "[motif-db][publication][failure]")
{
    auto const failing_point = GENERATE(point::before_build,
                                        point::after_build,
                                        point::before_artifact_validation,
                                        point::after_artifact_validation,
                                        point::before_artifact_fsync,
                                        point::after_artifact_fsync,
                                        point::before_artifact_rename,
                                        point::after_artifact_rename,
                                        point::before_artifact_directory_fsync,
                                        point::after_artifact_directory_fsync,
                                        point::before_synchronous_full,
                                        point::before_snapshot_begin,
                                        point::after_snapshot_row_write,
                                        point::before_sqlite_commit);
    INFO("failing boundary " << static_cast<int>(failing_point));

    tmp_dir const tdir {"precommit_failure"};
    make_bundle(tdir.path);
    auto mgr = motif::db::database_manager::open(tdir.path);
    REQUIRE(mgr.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    auto const published = mgr->manifest().postings()->artifact.filename;
    auto const generation = mgr->manifest().publication_generation;
    auto const first_game = motif::db::game_id {1U};

    SECTION("an already-current rebuild keeps serving the old files")
    {
        {
            fault_scope const scope {failing_point};
            REQUIRE_FALSE(mgr->rebuild_position_postings().has_value());
        }
        CHECK(mgr->manifest().publication_generation == generation);
        CHECK(mgr->manifest().postings()->artifact.filename == published);
        CHECK(failing_position_queries(*mgr, first_game) == 0);
        CHECK(artifact_files(tdir.path) == std::vector<std::string> {published});
        // The connection is back in autocommit and the next publication works.
        REQUIRE(mgr->rebuild_position_postings().has_value());
        CHECK(mgr->manifest().publication_generation == generation + 1U);
    }

    SECTION("after a committed mutation the mutation persists and the old snapshot stays stale")
    {
        REQUIRE(mgr->insert_game(make_game("Other", "Player", "0-1", first_move::d4)).has_value());
        {
            fault_scope const scope {failing_point};
            REQUIRE_FALSE(mgr->rebuild_position_postings().has_value());
        }
        // The canonical mutation was a separate, already-committed transaction:
        // a failed publication must never be read as rolling it back.
        CHECK(mgr->store().count_games().value_or(0) == 2);
        CHECK(mgr->manifest().publication_generation == generation);
        CHECK(mgr->manifest().postings()->artifact.filename == published);
        CHECK(failing_position_queries(*mgr, first_game) == position_query_families);
        CHECK(artifact_files(tdir.path) == std::vector<std::string> {published});

        // Reopen rebuilds from current SQLite.
        mgr->close();
        auto reopened = motif::db::database_manager::open(tdir.path);
        REQUIRE(reopened.has_value());
        CHECK(reopened->manifest().publication_generation == generation + 1U);
        CHECK(reopened->derived_index_status()->postings_usable);
        CHECK(reopened->position_game_ids(start_hash()).value_or(std::vector<motif::db::game_id> {}).size() == 2U);
    }
    // NOLINTEND(bugprone-unchecked-optional-access)
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("publication: a failure after the COMMIT is still a successful publication", "[motif-db][publication][failure]")
{
    auto const failing_point = GENERATE(point::after_sqlite_commit,
                                        point::before_synchronous_restore,
                                        point::before_snapshot_install,
                                        point::after_snapshot_install,
                                        point::before_manifest_write,
                                        point::after_manifest_temp_write,
                                        point::after_manifest_temp_fsync,
                                        point::before_manifest_rename,
                                        point::after_manifest_rename,
                                        point::before_manifest_directory_fsync,
                                        point::after_manifest_write,
                                        point::before_cleanup);
    INFO("failing boundary " << static_cast<int>(failing_point));

    tmp_dir const tdir {"postcommit_failure"};
    make_bundle(tdir.path);
    auto mgr = motif::db::database_manager::open(tdir.path);
    REQUIRE(mgr.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    auto const old_generation = mgr->manifest().publication_generation;
    REQUIRE(mgr->insert_game(make_game("Other", "Player", "0-1", first_move::d4)).has_value());
    {
        fault_scope const scope {failing_point};
        // The COMMIT happened; no later failure can turn it into an error.
        REQUIRE(mgr->rebuild_position_postings().has_value());
    }
    CHECK(mgr->manifest().publication_generation == old_generation + 1U);
    CHECK(mgr->derived_index_status()->postings_usable);
    auto const ids = mgr->position_game_ids(start_hash());
    REQUIRE(ids.has_value());
    CHECK(ids->size() == 2U);

    // A failing cache write left the cache stale or absent; the next open
    // repairs it from SQLite without replaying anything.
    mgr->close();
    {
        auto const hook = fault::scoped_hook {fail_any_build};
        auto reopened = motif::db::database_manager::open(tdir.path);
        REQUIRE(reopened.has_value());
        CHECK(reopened->manifest().publication_generation == old_generation + 1U);
        CHECK(motif::db::manifest_cache_current(tdir.path / "manifest.json", reopened->manifest()));
        CHECK(reopened->position_game_ids(start_hash()).value_or(std::vector<motif::db::game_id> {}).size() == 2U);
    }
    // Whatever the failure point left behind, exactly the published artifact remains.
    CHECK(artifact_files(tdir.path).size() == 1U);
    // NOLINTEND(bugprone-unchecked-optional-access)
}

TEST_CASE("publication: a failing cache write is retried on a clean close", "[motif-db][publication][failure]")
{
    tmp_dir const tdir {"cache_retry"};
    make_bundle(tdir.path);
    auto mgr = motif::db::database_manager::open(tdir.path);
    REQUIRE(mgr.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    REQUIRE(mgr->insert_game(make_game("Other", "Player", "0-1", first_move::d4)).has_value());
    {
        fault_scope const scope {point::before_manifest_rename};
        REQUIRE(mgr->rebuild_position_postings().has_value());
    }
    CHECK_FALSE(motif::db::manifest_cache_current(tdir.path / "manifest.json", mgr->manifest()));
    auto const new_manifest = mgr->manifest();
    mgr->close();
    CHECK(motif::db::manifest_cache_current(tdir.path / "manifest.json", new_manifest));
    // NOLINTEND(bugprone-unchecked-optional-access)
}

// ── Crash recovery (forked child dies at the boundary; parent reopens) ───────

#ifndef _WIN32
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("publication: a crash before the COMMIT recovers on the previous snapshot without adopting the orphan",
          "[motif-db][publication][crash]")
{
    auto const crash_point = GENERATE(point::before_build,
                                      point::after_build,
                                      point::before_artifact_validation,
                                      point::after_artifact_validation,
                                      point::before_artifact_fsync,
                                      point::after_artifact_fsync,
                                      point::before_artifact_rename,
                                      point::after_artifact_rename,
                                      point::before_artifact_directory_fsync,
                                      point::after_artifact_directory_fsync,
                                      point::before_synchronous_full,
                                      point::before_snapshot_begin,
                                      point::after_snapshot_row_write,
                                      point::before_sqlite_commit);
    INFO("crash boundary " << static_cast<int>(crash_point));

    tmp_dir const tdir {"precommit_crash"};
    make_bundle(tdir.path);
    std::string published;
    std::uint64_t generation = 0;
    {
        auto mgr = motif::db::database_manager::open(tdir.path);
        REQUIRE(mgr.has_value());
        // NOLINTBEGIN(bugprone-unchecked-optional-access)
        published = mgr->manifest().postings()->artifact.filename;
        generation = mgr->manifest().publication_generation;
        mgr->close();
        // NOLINTEND(bugprone-unchecked-optional-access)
    }

    auto const exit_code = run_in_crashing_child(crash_point,
                                                 [&tdir]() -> void
                                                 {
                                                     auto child = motif::db::database_manager::open(tdir.path);
                                                     if (!child) {
                                                         _exit(3);
                                                     }
                                                     (void)child->rebuild_position_postings();
                                                 });
    REQUIRE(exit_code == crash_exit_code);

    // The candidate is on disk (at least its reserved staging name) but nothing
    // references it.
    CHECK(artifact_files(tdir.path).size() >= 2U);

    // Reopen: no replay (a build would fail the open), the old snapshot serves,
    // and the orphan is collected, never adopted.
    auto const hook = fault::scoped_hook {fail_any_build};
    auto reopened = motif::db::database_manager::open(tdir.path);
    REQUIRE(reopened.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    CHECK(reopened->manifest().publication_generation == generation);
    CHECK(reopened->manifest().postings()->artifact.filename == published);
    CHECK(failing_position_queries(*reopened, motif::db::game_id {1U}) == 0);
    CHECK(artifact_files(tdir.path) == std::vector<std::string> {published});
    // NOLINTEND(bugprone-unchecked-optional-access)
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("publication: a crash after the COMMIT reopens on the new snapshot without replay and repairs the cache",
          "[motif-db][publication][crash]")
{
    auto const crash_point = GENERATE(point::after_sqlite_commit,
                                      point::before_synchronous_restore,
                                      point::before_snapshot_install,
                                      point::after_snapshot_install,
                                      point::before_manifest_write,
                                      point::after_manifest_temp_write,
                                      point::after_manifest_temp_fsync,
                                      point::before_manifest_rename,
                                      point::after_manifest_rename,
                                      point::before_manifest_directory_fsync,
                                      point::after_manifest_write,
                                      point::before_cleanup);
    INFO("crash boundary " << static_cast<int>(crash_point));

    tmp_dir const tdir {"postcommit_crash"};
    make_bundle(tdir.path);
    std::uint64_t generation = 0;
    {
        auto mgr = motif::db::database_manager::open(tdir.path);
        REQUIRE(mgr.has_value());
        // NOLINTBEGIN(bugprone-unchecked-optional-access)
        generation = mgr->manifest().publication_generation;
        // A committed canonical mutation: the next open must publish.
        REQUIRE(mgr->insert_game(make_game("Other", "Player", "0-1", first_move::d4)).has_value());
        mgr->close();
        // NOLINTEND(bugprone-unchecked-optional-access)
    }

    auto const exit_code = run_in_crashing_child(crash_point,
                                                 [&tdir]() -> void
                                                 {
                                                     // open() sees a stale snapshot and rebuilds; the crash lands
                                                     // inside that publication.
                                                     auto child = motif::db::database_manager::open(tdir.path);
                                                     (void)child;
                                                 });
    REQUIRE(exit_code == crash_exit_code);

    auto const hook = fault::scoped_hook {fail_any_build};
    auto reopened = motif::db::database_manager::open(tdir.path);
    REQUIRE(reopened.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    CHECK(reopened->manifest().publication_generation == generation + 1U);
    CHECK(reopened->derived_index_status()->postings_usable);
    CHECK(reopened->position_game_ids(start_hash()).value_or(std::vector<motif::db::game_id> {}).size() == 2U);
    CHECK(motif::db::manifest_cache_current(tdir.path / "manifest.json", reopened->manifest()));
    CHECK(artifact_files(tdir.path).size() == 1U);
    // NOLINTEND(bugprone-unchecked-optional-access)
}

TEST_CASE("publication: a crash in the middle of a canonical batch commits nothing and leaves the snapshot usable",
          "[motif-db][publication][crash]")
{
    tmp_dir const tdir {"batch_crash"};
    make_bundle(tdir.path);
    auto const exit_code = run_in_crashing_child(point::before_canonical_commit,
                                                 [&tdir]() -> void
                                                 {
                                                     auto child = motif::db::database_manager::open(tdir.path);
                                                     if (!child) {
                                                         _exit(3);
                                                     }
                                                     auto const scope = child->lock_generation();
                                                     if (!child->writer().begin_transaction()) {
                                                         _exit(4);
                                                     }
                                                     (void)child->writer().insert(make_game("Lost", "Batch", "0-1", first_move::d4));
                                                     (void)child->writer().commit_transaction();
                                                 });
    REQUIRE(exit_code == crash_exit_code);

    auto const hook = fault::scoped_hook {fail_any_build};
    auto reopened = motif::db::database_manager::open(tdir.path);
    REQUIRE(reopened.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    // Rows, allocator and sequence share one fate: all rolled back together.
    CHECK(reopened->store().count_games().value_or(0) == 1);
    CHECK(reopened->derived_index_status()->mutation_seq == 1U);
    CHECK(reopened->derived_index_status()->postings_usable);
    // NOLINTEND(bugprone-unchecked-optional-access)
}

TEST_CASE("publication: a crash right after a batch COMMIT keeps the rows and the advanced sequence, and the snapshot is stale",
          "[motif-db][publication][crash]")
{
    tmp_dir const tdir {"batch_crash_after"};
    make_bundle(tdir.path);
    auto const exit_code = run_in_crashing_child(point::after_canonical_commit,
                                                 [&tdir]() -> void
                                                 {
                                                     auto child = motif::db::database_manager::open(tdir.path);
                                                     if (!child) {
                                                         _exit(3);
                                                     }
                                                     auto const scope = child->lock_generation();
                                                     if (!child->writer().begin_transaction()) {
                                                         _exit(4);
                                                     }
                                                     (void)child->writer().insert(make_game("Kept", "Batch", "0-1", first_move::d4));
                                                     (void)child->writer().commit_transaction();
                                                 });
    REQUIRE(exit_code == crash_exit_code);

    auto reopened = motif::db::database_manager::open(tdir.path);
    REQUIRE(reopened.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    CHECK(reopened->store().count_games().value_or(0) == 2);
    // open() found the snapshot behind the advanced sequence and rebuilt.
    CHECK(reopened->derived_index_status()->postings_usable);
    CHECK(reopened->derived_index_status()->covered_mutation_seq
          == std::optional<std::uint64_t> {reopened->derived_index_status()->mutation_seq});
    // NOLINTEND(bugprone-unchecked-optional-access)
}
#endif

// ── The cache is never authoritative ─────────────────────────────────────────

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("publication: a missing, damaged, stale, ahead, foreign or forged cache is ignored and repaired from SQLite",
          "[motif-db][publication][cache]")
{
    tmp_dir const tdir {"cache_variants"};
    tmp_dir const other {"cache_variants_other"};
    make_bundle(tdir.path);
    make_bundle(other.path);
    auto const cache_path = tdir.path / "manifest.json";

    // A cache from an older publication: capture it, publish again, restore it.
    auto const generation_one_cache = read_bytes(cache_path);
    std::uint64_t generation = 0;
    std::optional<motif::db::db_manifest> truth;
    {
        auto mgr = motif::db::database_manager::open(tdir.path);
        REQUIRE(mgr.has_value());
        // NOLINTBEGIN(bugprone-unchecked-optional-access)
        REQUIRE(mgr->rebuild_position_postings().has_value());
        generation = mgr->manifest().publication_generation;
        truth = mgr->manifest();
        mgr->close();
        // NOLINTEND(bugprone-unchecked-optional-access)
    }
    REQUIRE(truth.has_value());

    constexpr int foreign_bundle_cache = 5;
    auto const variant = GENERATE(0, 1, 2, 3, 4, 5, 6);
    INFO("cache variant " << variant);
    switch (variant) {
        case 0:  // absent
            REQUIRE(fs::remove(cache_path));
            break;
        case 1:  // truncated mid-token
            write_bytes(cache_path, read_bytes(cache_path).substr(0, truncated_cache_bytes));
            break;
        case 2:  // garbage
            write_bytes(cache_path, "this is not a manifest\n");
            break;
        case 3:  // an older publication's cache (a stale cache is not "ahead")
            write_bytes(cache_path, generation_one_cache);
            break;
        case 4:  // claims a much higher generation than SQLite holds
        {
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
            auto ahead = motif::db::build_manifest(truth->publication_generation + generations_ahead,
                                                   truth->covered_mutation_seq,
                                                   truth->canonical_game_count,
                                                   truth->payload);
            REQUIRE(ahead.has_value());
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
            REQUIRE(motif::db::write_manifest(cache_path, *ahead).has_value());
            break;
        }
        case foreign_bundle_cache:  // another bundle's cache
            fs::copy_file(other.path / "manifest.json", cache_path, fs::copy_options::overwrite_existing);
            break;
        default:  // claims a schema this build does not know
        {
            auto forged = *truth;
            forged.schema_version = motif::db::schema::current_version + 3U;
            REQUIRE(motif::db::write_manifest(cache_path, forged).has_value());
            break;
        }
    }

    auto const hook = fault::scoped_hook {fail_any_build};  // proof: nothing is rebuilt
    auto reopened = motif::db::database_manager::open(tdir.path);
    REQUIRE(reopened.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    CHECK(reopened->manifest().publication_generation == generation);
    CHECK(reopened->manifest().payload_checksum == truth->payload_checksum);
    CHECK(reopened->manifest().name() == "publication-test");
    CHECK(reopened->derived_index_status()->postings_usable);
    CHECK(motif::db::manifest_cache_current(cache_path, reopened->manifest()));
    // NOLINTEND(bugprone-unchecked-optional-access)
}

TEST_CASE("publication: a v7 database never reads the legacy manifest fields at all", "[motif-db][publication][cache]")
{
    tmp_dir const tdir {"cache_legacy_shape"};
    make_bundle(tdir.path);
    // A v6-shaped manifest next to a v7 SQLite: if it were trusted, its entries
    // would point at nonexistent artifacts and its dirty flag would matter.
    write_bytes(
        tdir.path / "manifest.json",
        R"({"name":"liar","schema_version":6,"game_count":99,"created_at":"2001-01-01T00:00:00Z",)" R"("position_index_dirty":true,"source_generation":7,"derived_index_build_seq":8,)" R"("position_postings":{"filename":"positions.postings.1","source_generation":7,"game_count":99,"file_size":1,"checksum":1},)" R"("opening_tree_index":null})");
    auto const hook = fault::scoped_hook {fail_any_build};
    auto reopened = motif::db::database_manager::open(tdir.path);
    REQUIRE(reopened.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    CHECK(reopened->manifest().name() == "publication-test");
    CHECK(reopened->derived_index_status()->postings_usable);
    CHECK(motif::db::manifest_cache_current(tdir.path / "manifest.json", reopened->manifest()));
    // NOLINTEND(bugprone-unchecked-optional-access)
}

// ── Corrupt snapshot row fails closed ────────────────────────────────────────

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("publication: a corrupt or unsupported snapshot row fails closed and preserves the files", "[motif-db][publication][corruption]")
{
    tmp_dir const tdir {"corrupt_row"};
    make_bundle(tdir.path);
    auto const files_before = artifact_files(tdir.path);
    REQUIRE(files_before.size() == 1U);
    auto const cache_before = read_bytes(tdir.path / "manifest.json");

    struct scenario
    {
        char const* name;
        char const* sql;
        motif::db::error_code expected;
    };

    auto const bad = GENERATE(
        scenario {"payload bytes changed under the checksum",
                  "UPDATE published_snapshot SET payload_json = payload_json || ' '",
                  motif::db::error_code::io_failure},
        scenario {"payload not json",
                  "UPDATE published_snapshot SET payload_json = 'nope', payload_checksum = '0000000000000000'",
                  motif::db::error_code::io_failure},
        scenario {"future payload version", "UPDATE published_snapshot SET payload_version = 3", motif::db::error_code::schema_mismatch},
        scenario {"snapshot row missing", "DELETE FROM published_snapshot", motif::db::error_code::io_failure},
        scenario {"sequence singleton missing", "DELETE FROM mutation_state", motif::db::error_code::io_failure},
        scenario {
            "snapshot ahead of the canonical sequence", "UPDATE mutation_state SET mutation_seq = 0", motif::db::error_code::io_failure},
        scenario {"coverage inconsistent with the payload",
                  "UPDATE published_snapshot SET covered_mutation_seq = 0",
                  motif::db::error_code::io_failure});
    INFO(bad.name);
    REQUIRE(raw_exec(tdir.path, bad.sql) == SQLITE_OK);

    // Even with a perfectly good cache beside it, the row decides.
    auto const hook = fault::scoped_hook {fail_any_build};
    auto const opened = motif::db::database_manager::open(tdir.path);
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error() == bad.expected);
    // Nothing was repaired, rebuilt or garbage-collected from a bad inventory.
    CHECK(artifact_files(tdir.path) == files_before);
    CHECK(read_bytes(tdir.path / "manifest.json") == cache_before);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("publication: missing or corrupt SQLite is never replaced by a derived salvage", "[motif-db][publication][corruption]")
{
    tmp_dir const tdir {"corrupt_sqlite"};
    make_bundle(tdir.path);
    auto const files_before = artifact_files(tdir.path);

    SECTION("missing games.db is not created")
    {
        for (auto const& suffix : {"", "-wal", "-shm"}) {
            fs::remove(tdir.path / (std::string {"games.db"} + suffix));
        }
        auto const opened = motif::db::database_manager::open(tdir.path);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error() == motif::db::error_code::not_found);
        CHECK_FALSE(fs::exists(tdir.path / "games.db"));
        CHECK(artifact_files(tdir.path) == files_before);
    }
    SECTION("a corrupt games.db fails the open")
    {
        for (auto const& suffix : {"-wal", "-shm"}) {
            fs::remove(tdir.path / (std::string {"games.db"} + suffix));
        }
        constexpr auto garbage_bytes = std::size_t {8192};
        write_bytes(tdir.path / "games.db", std::string(garbage_bytes, 'x'));
        auto const opened = motif::db::database_manager::open(tdir.path);
        REQUIRE_FALSE(opened.has_value());
        CHECK(artifact_files(tdir.path) == files_before);
    }
}

// ── Artifacts: missing, corrupt, orphaned, unrelated ─────────────────────────

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("publication: a corrupt required postings file triggers one rebuild; a corrupt tree does not",
          "[motif-db][publication][artifacts]")
{
    tmp_dir const tdir {"corrupt_artifacts"};
    make_bundle(tdir.path, /*with_tree=*/true);

    std::string postings_name;
    std::string tree_name;
    std::uint64_t generation = 0;
    {
        auto mgr = motif::db::database_manager::open(tdir.path);
        REQUIRE(mgr.has_value());
        // NOLINTBEGIN(bugprone-unchecked-optional-access)
        postings_name = mgr->manifest().postings()->artifact.filename;
        tree_name = mgr->manifest().tree()->checkpoint.filename;
        generation = mgr->manifest().publication_generation;
        CHECK(mgr->derived_index_status()->tree_usable);
        mgr->close();
        // NOLINTEND(bugprone-unchecked-optional-access)
    }

    auto flip_a_byte = [&](std::string const& name) -> void
    {
        auto bytes = read_bytes(tdir.path / name);
        REQUIRE_FALSE(bytes.empty());
        constexpr auto flip_mask = 0x5AU;
        bytes[bytes.size() / 2] = static_cast<char>(static_cast<unsigned char>(bytes[bytes.size() / 2]) ^ flip_mask);
        write_bytes(tdir.path / name, bytes);
    };

    SECTION("optional tree: corrupt")
    {
        flip_a_byte(tree_name);
        // Postings stay valid, so nothing is replayed.
        auto const hook = fault::scoped_hook {fail_any_build};
        auto reopened = motif::db::database_manager::open(tdir.path);
        REQUIRE(reopened.has_value());
        // NOLINTBEGIN(bugprone-unchecked-optional-access)
        CHECK(reopened->derived_index_status()->postings_usable);
        CHECK_FALSE(reopened->derived_index_status()->tree_usable);
        CHECK(reopened->manifest().tree() == nullptr);
        // A repair publication cleared the descriptor: generation moved, the
        // mutation sequence did not.
        CHECK(reopened->manifest().publication_generation == generation + 1U);
        CHECK(reopened->derived_index_status()->mutation_seq == 1U);
        CHECK(reopened->query_unfiltered_opening_stats(start_hash()).has_value());
        CHECK(artifact_files(tdir.path) == std::vector<std::string> {postings_name});
        // NOLINTEND(bugprone-unchecked-optional-access)
    }
    SECTION("optional tree: missing")
    {
        REQUIRE(fs::remove(tdir.path / tree_name));
        auto const hook = fault::scoped_hook {fail_any_build};
        auto reopened = motif::db::database_manager::open(tdir.path);
        REQUIRE(reopened.has_value());
        // NOLINTBEGIN(bugprone-unchecked-optional-access)
        CHECK(reopened->derived_index_status()->postings_usable);
        CHECK(reopened->manifest().tree() == nullptr);
        CHECK(reopened->manifest().publication_generation == generation + 1U);
        // NOLINTEND(bugprone-unchecked-optional-access)
    }
    SECTION("required postings: corrupt")
    {
        flip_a_byte(postings_name);
        auto reopened = motif::db::database_manager::open(tdir.path);
        REQUIRE(reopened.has_value());
        // NOLINTBEGIN(bugprone-unchecked-optional-access)
        CHECK(reopened->derived_index_status()->postings_usable);
        CHECK(reopened->manifest().postings()->artifact.filename != postings_name);
        CHECK(reopened->manifest().publication_generation == generation + 1U);
        // The rebuilt snapshot is at the same sequence; the old tree is not
        // trusted blindly and is rebuilt only on request.
        CHECK(reopened->query_position_matches(start_hash()).has_value());
        CHECK_FALSE(fs::exists(tdir.path / postings_name));
        // NOLINTEND(bugprone-unchecked-optional-access)
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("publication: only well-identified unreferenced artifacts are collected; everything else survives",
          "[motif-db][publication][artifacts]")
{
    tmp_dir const tdir {"gc_scope"};
    make_bundle(tdir.path);

    auto const artifact_id = std::string {"0123456789abcdef0123456789abcdef"};
    auto const postings = std::string {"positions.postings."};
    auto const tree = std::string {"opening_tree.idx."};
    auto const metadata = std::string {"positions.metadata."};
    auto const patch = std::string {"opening_tree.patch."};

    // Names this subsystem never generates for the bundle: shaped like artifacts
    // but outside the exact grammar (decimal generation or exactly 32 lower-case
    // hex digits, then the suffixes that family's builder produces). They must
    // survive every collection.
    auto const look_alikes = std::vector<std::string> {
        "positions.duckdb",
        "notes.txt",
        postings + "keepme",
        postings + "deadbeef.building",  // short hex token: not an artifact id
        postings + "deadbeef.tmp",
        tree + "00ff",
        tree + "bak",
        postings + "1234.bak",
        postings + "1234.building",  // a decimal generation is never staged
        postings + artifact_id + ".keep",
        postings + artifact_id + ".spill",  // spill runs are numbered
        postings + artifact_id + ".building.spill00",
        postings + artifact_id + ".building.spill18446744073709551616",
        postings + artifact_id + ".dirspool",  // scratch only hangs off the staging name
        postings + artifact_id + ".tmp",
        postings + artifact_id + ".building.roots",  // tree scratch on a postings file
        postings + artifact_id + ".building.tmp.tmp",
        tree + artifact_id + ".building.dirspool",  // postings scratch on a tree file
        tree + artifact_id + ".building.spill0",
        tree + artifact_id + ".building.childfreq.spill",
        tree + artifact_id + ".building.build.roots.",
        tree + artifact_id + ".building.build.roots.01",
        postings + std::string {"0123456789ABCDEF0123456789ABCDEF"},  // upper-case hex
        postings + artifact_id.substr(1),  // 31 digits
        postings + artifact_id + "0",  // 33 digits
        postings + std::string(21, '7'),  // too long to be a generation counter
        postings + "0007",  // not a canonical decimal generation
        postings + "18446744073709551616",  // exceeds uint64
        metadata + "7",  // runs are v7-only: no decimal generation
        metadata + artifact_id + ".tmp",  // runs are written directly: no scratch
        patch + artifact_id + ".building.tmp",
        patch + "7",
        patch + "keepme",
    };
    for (auto const& name : look_alikes) {
        write_bytes(tdir.path / name, "not ours");
    }
    fs::create_directory(tdir.path / (postings + "deadbeef"));
    write_bytes(tdir.path / (postings + "deadbeef") / "inside", "a directory with an artifact-like name");

    // Genuinely owned, unreferenced files: pre-v7 numbered artifacts and their
    // builder scratch, v7 artifacts, staging files and every scratch file a
    // builder leaves hanging off a staging name.
    auto const orphans = std::vector<std::string> {
        postings + "7",
        postings + "8.tmp",
        postings + "8.dirspool",
        postings + "8.spill0",
        tree + "5",
        tree + "5.tmp",
        tree + "5.childcounts.tmp",
        postings + artifact_id,
        postings + artifact_id + ".building",
        postings + artifact_id + ".building.spill0",
        postings + artifact_id + ".building.spill12",
        postings + artifact_id + ".building.dirspool",
        postings + artifact_id + ".building.tmp",
        tree + artifact_id,
        tree + artifact_id + ".building",
        tree + artifact_id + ".building.tmp",
        tree + artifact_id + ".building.roots",
        tree + artifact_id + ".building.edges",
        tree + artifact_id + ".building.childcounts",
        tree + artifact_id + ".building.childcounts.tmp",
        tree + artifact_id + ".building.childfreq.spill0",
        tree + artifact_id + ".building.build.roots.1",
        tree + artifact_id + ".building.build.edges.2",
        metadata + artifact_id,
        metadata + artifact_id + ".building",
        patch + artifact_id,
        patch + artifact_id + ".building",
    };
    for (auto const& name : orphans) {
        write_bytes(tdir.path / name, "orphan");
    }

    auto mgr = motif::db::database_manager::open(tdir.path);
    REQUIRE(mgr.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    auto const published = mgr->manifest().postings()->artifact.filename;
    // open() collects orphans after establishing a valid snapshot.
    for (auto const& name : orphans) {
        INFO(name);
        CHECK_FALSE(fs::exists(tdir.path / name));
    }
    for (auto const& name : look_alikes) {
        INFO(name);
        CHECK(fs::exists(tdir.path / name));
    }
    CHECK(fs::exists(tdir.path / published));
    CHECK(fs::exists(tdir.path / (postings + "deadbeef") / "inside"));
    CHECK(read_bytes(tdir.path / "positions.duckdb") == "not ours");

    // A fresh publication supersedes the old file but touches nothing else.
    REQUIRE(mgr->rebuild_position_postings().has_value());
    CHECK_FALSE(fs::exists(tdir.path / published));
    for (auto const& name : look_alikes) {
        INFO(name);
        CHECK(fs::exists(tdir.path / name));
    }
    CHECK(fs::exists(tdir.path / (postings + "deadbeef") / "inside"));
    // NOLINTEND(bugprone-unchecked-optional-access)
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("publication: the artifact install never replaces an existing file and a retry picks a new name",
          "[motif-db][publication][artifacts]")
{
    tmp_dir const tdir {"no_replace"};
    make_bundle(tdir.path);
    auto mgr = motif::db::database_manager::open(tdir.path);
    REQUIRE(mgr.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    auto const published = mgr->manifest().postings()->artifact.filename;
    auto const generation = mgr->manifest().publication_generation;

    // Right before the rename, a file appears at the exact final name the
    // attempt is about to install (the staging name is its final name plus
    // ".building"): the install must refuse rather than overwrite it.
    static fs::path squatted;
    static std::string squatter_name;
    squatted = tdir.path;
    squatter_name.clear();
    auto const squat = +[](point const where, fault::artifact_kind, std::uint64_t) noexcept -> fault::fault_action
    {
        if (where != point::before_artifact_rename) {
            return fault::fault_action::proceed;
        }
        for (auto const& entry : fs::directory_iterator {squatted}) {
            auto const name = entry.path().filename().string();
            if (name.ends_with(".building")) {
                squatter_name = name.substr(0, name.size() - std::string_view {".building"}.size());
                std::ofstream squatter {squatted / squatter_name, std::ios::binary};
                squatter << "squatter";
            }
        }
        return fault::fault_action::proceed;
    };
    {
        auto const hook = fault::scoped_hook {squat};
        REQUIRE_FALSE(mgr->rebuild_position_postings().has_value());
    }
    REQUIRE_FALSE(squatter_name.empty());
    CHECK(read_bytes(tdir.path / squatter_name) == "squatter");
    CHECK(mgr->manifest().publication_generation == generation);
    CHECK(mgr->manifest().postings()->artifact.filename == published);
    CHECK(failing_position_queries(*mgr, motif::db::game_id {1U}) == 0);

    REQUIRE(mgr->rebuild_position_postings().has_value());
    CHECK(mgr->manifest().postings()->artifact.filename != squatter_name);
    CHECK(mgr->manifest().postings()->artifact.filename != published);
    // NOLINTEND(bugprone-unchecked-optional-access)
}

// ── Opening, orphans from crashed attempts, raw-import repair ───────────────

TEST_CASE("publication: open repairs an interrupted raw import before accepting coverage", "[motif-db][publication][import]")
{
    tmp_dir const tdir {"raw_repair"};
    {
        auto mgr = motif::db::database_manager::create(tdir.path, "raw-repair");
        REQUIRE(mgr.has_value());
        // NOLINTBEGIN(bugprone-unchecked-optional-access)
        REQUIRE(mgr->writer().drop_identity_index().has_value());
        REQUIRE(mgr->writer().insert_raw(make_game("White", "Black")).has_value());
        REQUIRE(mgr->writer().insert_raw(make_game("White", "Black")).has_value());  // exact duplicate
        REQUIRE(mgr->writer().insert_raw(make_game("Other", "Player", "0-1", first_move::d4)).has_value());
        // NOLINTEND(bugprone-unchecked-optional-access)
    }
    auto reopened = motif::db::database_manager::open(tdir.path);
    REQUIRE(reopened.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    // The duplicate was removed (sequence-aware) and coverage was built against
    // the reconciled rows.
    CHECK(reopened->store().count_games().value_or(0) == 2);
    CHECK(reopened->writer().identity_index_exists().value_or(false));
    auto const status = reopened->derived_index_status();
    REQUIRE(status.has_value());
    CHECK(status->postings_usable);
    CHECK(status->covered_mutation_seq == std::optional<std::uint64_t> {status->mutation_seq});
    CHECK(reopened->manifest().postings()->document_count == 2U);
    // NOLINTEND(bugprone-unchecked-optional-access)
}

TEST_CASE("publication: opening a bundle never needs positions.duckdb and mutation_state survives a copy", "[motif-db][publication]")
{
    tmp_dir const src {"copy_src"};
    tmp_dir const dst {"copy_dst"};
    make_bundle(src.path, /*with_tree=*/true);
    fs::copy(src.path, dst.path, fs::copy_options::recursive);
    fs::remove(dst.path / ".motif.lock");
    auto const hook = fault::scoped_hook {fail_any_build};
    auto copied = motif::db::database_manager::open(dst.path);
    REQUIRE(copied.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    CHECK(copied->derived_index_status()->tree_usable);
    // The bundle identity travels with the SQLite file.
    auto original = motif::db::database_manager::open(src.path);
    REQUIRE(original.has_value());
    CHECK(copied->manifest().payload.bundle.id == original->manifest().payload.bundle.id);
    // NOLINTEND(bugprone-unchecked-optional-access)
}

// ── Synchronous level, WAL enforcement, ambiguous COMMIT, create ownership ──

namespace
{

constexpr std::size_t point_slots = 64;
constexpr auto level_unset = -100;
constexpr auto level_normal = 1;
constexpr auto level_full = 2;

// What a hook observed on the connection at each boundary. The first sample per
// point wins, so a test can tell the open-time migration from a later publication.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables) -- process-global state read by the plain-function hook.
std::array<int, point_slots> g_sync_level {};
std::array<int, point_slots> g_journal_is_wal {};
point g_level_fail_at {};
bool g_level_fail_armed {false};

// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

auto slot_of(point const where) -> std::size_t
{
    return static_cast<std::size_t>(where) % point_slots;
}

auto read_connection_int(char const* sql) noexcept -> int
{
    auto* const conn = fault::hook_connection();
    if (conn == nullptr) {
        return -1;
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return -2;
    }
    auto value = -3;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        value = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return value;
}

auto journal_is_wal_on_hook_connection() noexcept -> int
{
    auto* const conn = fault::hook_connection();
    if (conn == nullptr) {
        return -1;
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(conn, "PRAGMA journal_mode;", -1, &stmt, nullptr) != SQLITE_OK) {
        return -2;
    }
    auto value = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        auto const* text = reinterpret_cast<char const*>(sqlite3_column_text(stmt, 0));
        value = text != nullptr && std::string_view {text} == "wal" ? 1 : 0;
    }
    sqlite3_finalize(stmt);
    return value;
}

auto level_hook(point const where, fault::artifact_kind /*kind*/, std::uint64_t /*generation*/) noexcept -> fault::fault_action
{
    auto const slot = slot_of(where);
    if (g_sync_level.at(slot) == level_unset) {
        g_sync_level.at(slot) = read_connection_int("PRAGMA synchronous;");
        g_journal_is_wal.at(slot) = journal_is_wal_on_hook_connection();
    }
    return g_level_fail_armed && where == g_level_fail_at ? fault::fault_action::fail_io : fault::fault_action::proceed;
}

class level_scope
{
  public:
    explicit level_scope(std::optional<point> const fail_at = std::nullopt)
    {
        g_sync_level.fill(level_unset);
        g_journal_is_wal.fill(level_unset);
        g_level_fail_armed = fail_at.has_value();
        if (fail_at) {
            g_level_fail_at = *fail_at;
        }
        previous_ = fault::set_hook(level_hook);
    }

    ~level_scope()
    {
        fault::set_hook(previous_);
        g_level_fail_armed = false;
    }

    level_scope(level_scope const&) = delete;
    auto operator=(level_scope const&) -> level_scope& = delete;
    level_scope(level_scope&&) = delete;
    auto operator=(level_scope&&) -> level_scope& = delete;

  private:
    fault::hook_function previous_ {nullptr};
};

[[nodiscard]] auto level_at(point const where) -> int
{
    return g_sync_level.at(slot_of(where));
}

// Commits one canonical batch so the connection's level can be sampled at
// before_canonical_commit, and leaves the snapshot stale.
void commit_one_batch(motif::db::database_manager& mgr, std::string const& white)
{
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    REQUIRE(mgr.writer().begin_transaction().has_value());
    REQUIRE(mgr.writer().insert(make_game(white, "Batch Black")).has_value());
    REQUIRE(mgr.writer().commit_transaction().has_value());
    // NOLINTEND(bugprone-unchecked-optional-access)
}

auto journal_mode_of(fs::path const& dir) -> std::string
{
    sqlite3* conn = nullptr;
    REQUIRE(sqlite3_open_v2((dir / "games.db").c_str(), &conn, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(conn, "PRAGMA journal_mode;", -1, &stmt, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto const* text = reinterpret_cast<char const*>(sqlite3_column_text(stmt, 0));
    auto const mode = std::string {text != nullptr ? text : ""};
    sqlite3_finalize(stmt);
    sqlite3_close(conn);
    return mode;
}

}  // namespace

TEST_CASE("publication: the snapshot commit runs at synchronous=FULL and the previous level is restored afterwards",
          "[motif-db][publication][durability]")
{
    tmp_dir const tdir {"sync_levels"};
    make_bundle(tdir.path);
    auto mgr = motif::db::database_manager::open(tdir.path);
    REQUIRE(mgr.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    commit_one_batch(*mgr, "Stale White");  // the snapshot is stale: a rebuild publishes

    {
        level_scope const scope;
        REQUIRE(mgr->rebuild_position_postings().has_value());
        // Raised OUTSIDE the transaction (before BEGIN): SQLite fixes the commit's
        // sync level when the transaction starts.
        CHECK(level_at(point::before_synchronous_full) == level_normal);
        CHECK(level_at(point::before_snapshot_begin) == level_full);
        CHECK(level_at(point::before_sqlite_commit) == level_full);
        CHECK(level_at(point::before_synchronous_restore) == level_full);
        CHECK(level_at(point::after_sqlite_commit) == level_normal);
    }

    SECTION("a failed publication restores the previous level")
    {
        commit_one_batch(*mgr, "Second Stale White");
        level_scope const scope {point::before_sqlite_commit};
        REQUIRE_FALSE(mgr->rebuild_position_postings().has_value());
        CHECK(level_at(point::before_sqlite_commit) == level_full);
        commit_one_batch(*mgr, "Third Stale White");
        CHECK(level_at(point::before_canonical_commit) == level_normal);
    }

    SECTION("a failed restore leaves the connection at FULL, never below, and the publication stands")
    {
        commit_one_batch(*mgr, "Second Stale White");
        level_scope const scope {point::before_synchronous_restore};
        REQUIRE(mgr->rebuild_position_postings().has_value());
        REQUIRE(mgr->derived_index_status()->postings_usable);
        commit_one_batch(*mgr, "Third Stale White");
        CHECK(level_at(point::before_canonical_commit) == level_full);
    }
    // NOLINTEND(bugprone-unchecked-optional-access)
}

// Drives an ambiguous COMMIT: SQLite reports an error although its transaction
// is gone. The seam applies or discards the commit for real, so the manager's
// reconciliation reads true state.
namespace
{
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables) -- process-global state read by the plain-function hook.
fault::fault_action g_commit_action {fault::fault_action::proceed};
bool g_reconcile_read_fails {false};

// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

auto ambiguous_commit_hook(point const where, fault::artifact_kind /*kind*/, std::uint64_t /*generation*/) noexcept -> fault::fault_action
{
    if (where == point::before_sqlite_commit) {
        return g_commit_action;
    }
    if (where == point::before_commit_reconcile && g_reconcile_read_fails) {
        return fault::fault_action::fail_io;
    }
    return fault::fault_action::proceed;
}
}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("publication: an ambiguous COMMIT is reconciled against SQLite, never decided from the error code",
          "[motif-db][publication][failure][ambiguous]")
{
    tmp_dir const tdir {"ambiguous_commit"};
    make_bundle(tdir.path);
    auto opened = motif::db::database_manager::open(tdir.path);
    REQUIRE(opened.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    auto const old_generation = opened->manifest().publication_generation;
    commit_one_batch(*opened, "Stale White");  // the snapshot is stale: the next rebuild has something to publish
    REQUIRE_FALSE(opened->derived_index_status()->postings_usable);
    auto const run_with = [&](fault::fault_action const action, bool const reconcile_fails) -> motif::db::result<void>
    {
        g_commit_action = action;
        g_reconcile_read_fails = reconcile_fails;
        auto const hook = fault::scoped_hook {ambiguous_commit_hook};
        return opened->rebuild_position_postings();
    };

    SECTION("the row landed after all: the publication succeeds")
    {
        REQUIRE(run_with(fault::fault_action::commit_error_after_commit, false).has_value());
        auto const status = opened->derived_index_status();
        REQUIRE(status.has_value());
        CHECK(status->postings_usable);
        CHECK(opened->manifest().publication_generation == old_generation + 1);
        CHECK(artifact_files(tdir.path).size() == 1);  // the new artifact; the superseded one was collected
    }

    SECTION("the row was lost: the publication fails, the old snapshot stays and the candidate is removed")
    {
        auto const failed = run_with(fault::fault_action::commit_error_after_rollback, false);
        REQUIRE_FALSE(failed.has_value());
        CHECK(opened->manifest().publication_generation == old_generation);
        CHECK_FALSE(opened->derived_index_status()->postings_usable);
        CHECK(artifact_files(tdir.path).size() == 1);  // only the previous artifact
    }

    SECTION("outcome unknown, row landed: the candidate is kept, and a reopen adopts it without a rebuild")
    {
        REQUIRE_FALSE(run_with(fault::fault_action::commit_error_after_commit, true).has_value());
        CHECK(artifact_files(tdir.path).size() == 2);  // never delete what may be referenced
        opened->close();
        fault_scope const scope;
        auto reopened = motif::db::database_manager::open(tdir.path);
        REQUIRE(reopened.has_value());
        CHECK_FALSE(first_index_of(point::before_build).has_value());
        CHECK(reopened->derived_index_status()->postings_usable);
        CHECK(reopened->manifest().publication_generation == old_generation + 1);
    }

    SECTION("outcome unknown, row lost: the orphan is collected by the repair that follows")
    {
        REQUIRE_FALSE(run_with(fault::fault_action::commit_error_after_rollback, true).has_value());
        CHECK(artifact_files(tdir.path).size() == 2);
        opened->close();
        auto reopened = motif::db::database_manager::open(tdir.path);
        REQUIRE(reopened.has_value());
        CHECK(reopened->derived_index_status()->postings_usable);
        CHECK(reopened->manifest().publication_generation == old_generation + 1);
        CHECK(artifact_files(tdir.path).size() == 1);
    }
    // NOLINTEND(bugprone-unchecked-optional-access)
}

TEST_CASE("publication: a rollback-journal v7 bundle is moved to WAL on open and can then publish", "[motif-db][publication][wal]")
{
    tmp_dir const tdir {"nonwal_v7"};
    make_bundle(tdir.path);
    REQUIRE(raw_exec(tdir.path, "PRAGMA journal_mode = DELETE;") == SQLITE_OK);
    REQUIRE(journal_mode_of(tdir.path) == "delete");

    auto mgr = motif::db::database_manager::open(tdir.path);
    REQUIRE(mgr.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    CHECK(journal_mode_of(tdir.path) == "wal");
    REQUIRE(mgr->insert_game(make_game("Later White", "Later Black", "0-1", first_move::d4)).has_value());
    REQUIRE(mgr->rebuild_position_postings().has_value());
    CHECK(mgr->derived_index_status()->postings_usable);
    // NOLINTEND(bugprone-unchecked-optional-access)
}

TEST_CASE("publication: a rollback-journal v6 bundle is moved to WAL before it migrates, and migrates under FULL",
          "[motif-db][publication][wal][migration]")
{
    tmp_dir const tdir {"nonwal_v6"};
    make_bundle(tdir.path);
    REQUIRE(raw_exec(tdir.path, "PRAGMA journal_mode = DELETE;") == SQLITE_OK);
    REQUIRE(raw_exec(tdir.path, "DROP TABLE mutation_state; DROP TABLE published_snapshot; PRAGMA user_version = 6;") == SQLITE_OK);
    REQUIRE(journal_mode_of(tdir.path) == "delete");

    level_scope const scope;
    auto mgr = motif::db::database_manager::open(tdir.path);
    REQUIRE(mgr.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    CHECK(journal_mode_of(tdir.path) == "wal");
    // The first FULL window is the migration's own (later ones are publications):
    // entered at NORMAL, in WAL, and still FULL right before the restore.
    CHECK(level_at(point::before_synchronous_full) == level_normal);
    CHECK(g_journal_is_wal.at(slot_of(point::before_synchronous_full)) == 1);
    CHECK(level_at(point::before_synchronous_restore) == level_full);
    // The migrated bundle serves its published postings and can publish again.
    CHECK(mgr->derived_index_status()->postings_usable);
    CHECK(mgr->query_position_matches(start_hash()).value_or(std::vector<motif::db::position_match> {}).size() == 1);
    REQUIRE(mgr->insert_game(make_game("Later White", "Later Black", "0-1", first_move::d4)).has_value());
    REQUIRE(mgr->rebuild_position_postings().has_value());
    // NOLINTEND(bugprone-unchecked-optional-access)
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("publication: a failed create never deletes pre-existing files, even with look-alike names",
          "[motif-db][publication][create][ownership]")
{
    tmp_dir const tdir {"create_ownership"};
    fs::create_directories(tdir.path);
    auto const user_files = std::vector<std::pair<std::string, std::string>> {
        {"positions.postings.keepme", "user data 1"},
        {"opening_tree.idx.backup", "user data 2"},
        {"positions.postings.0123456789abcdef.bak", "user data 3"},
        {"manifest.json.bak", "user data 4"},
        {"games.db.orig", "user data 5"},
    };
    for (auto const& [name, content] : user_files) {
        write_bytes(tdir.path / name, content);
    }

    {
        // The first rebuild fails: create() must roll its own files back.
        auto const hook = fault::scoped_hook {fail_any_build};
        auto failed = motif::db::database_manager::create(tdir.path, "ownership");
        REQUIRE_FALSE(failed.has_value());
    }

    REQUIRE(fs::exists(tdir.path));  // the directory predates create()
    for (auto const& [name, content] : user_files) {
        INFO(name);
        REQUIRE(fs::exists(tdir.path / name));
        CHECK(read_bytes(tdir.path / name) == content);
    }
    // What create() made is gone: nothing but the user's files remains.
    auto remaining = std::vector<std::string> {};
    for (auto const& entry : fs::directory_iterator {tdir.path}) {
        remaining.push_back(entry.path().filename().string());
    }
    CHECK(remaining.size() == user_files.size());
    CHECK_FALSE(fs::exists(tdir.path / "games.db"));
    CHECK_FALSE(fs::exists(tdir.path / ".motif.lock"));

    // And a directory create() itself made disappears entirely on failure.
    tmp_dir const fresh {"create_ownership_fresh"};
    {
        auto const hook = fault::scoped_hook {fail_any_build};
        REQUIRE_FALSE(motif::db::database_manager::create(fresh.path, "ownership").has_value());
    }
    CHECK_FALSE(fs::exists(fresh.path));
}

TEST_CASE("publication: discarding a created bundle removes its own artifacts and nothing look-alike",
          "[motif-db][publication][create][ownership]")
{
    tmp_dir const tdir {"discard_ownership"};
    fs::create_directories(tdir.path);
    write_bytes(tdir.path / "positions.postings.keepme", "user data");
    write_bytes(tdir.path / "opening_tree.idx.backup", "user data");

    auto created = motif::db::database_manager::create(tdir.path, "ownership");
    REQUIRE(created.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    REQUIRE(created->insert_game(make_game("White", "Black")).has_value());
    REQUIRE(created->rebuild_position_postings().has_value());
    REQUIRE(created->rebuild_opening_tree_index().has_value());
    created->discard_created_bundle();
    // NOLINTEND(bugprone-unchecked-optional-access)

    CHECK(artifact_files(tdir.path) == std::vector<std::string> {"opening_tree.idx.backup", "positions.postings.keepme"});
    CHECK_FALSE(fs::exists(tdir.path / "games.db"));
    CHECK_FALSE(fs::exists(tdir.path / "manifest.json"));
}
