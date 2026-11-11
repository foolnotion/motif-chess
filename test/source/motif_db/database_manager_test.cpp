#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "motif/db/database_manager.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chesslib/board/board.hpp>
#include <chesslib/board/move_codec.hpp>
#include <chesslib/core/types.hpp>
#include <fmt/format.h>
#include <sqlite3.h>
#ifndef _WIN32
#    include <sys/wait.h>
#    include <unistd.h>
#endif

#include "composite_test_support.hpp"
#include "motif/db/error.hpp"
#include "motif/db/fault_injection.hpp"
#include "motif/db/manifest.hpp"
#include "motif/db/schema.hpp"
#include "motif/db/types.hpp"

namespace
{

// RAII wrapper that removes the directory tree on destruction.
struct tmp_dir
{
    std::filesystem::path path;

    explicit tmp_dir(std::string const& suffix)
    {
        auto const base = std::filesystem::temp_directory_path();
        auto const tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = base / ("motif_dbmgr_test_" + suffix + "_" + std::to_string(tick));
    }

    ~tmp_dir() { std::filesystem::remove_all(path); }

    tmp_dir(tmp_dir const&) = delete;
    auto operator=(tmp_dir const&) -> tmp_dir& = delete;
    tmp_dir(tmp_dir&&) = delete;
    auto operator=(tmp_dir&&) -> tmp_dir& = delete;
};

auto make_one_move_game(  // NOLINT(llvm-prefer-static-over-anonymous-namespace)
    std::string white,
    std::string black) -> motif::db::game
{
    chesslib::move e2e4 {};
    e2e4.source_square = chesslib::square::e2;
    e2e4.target_square = chesslib::square::e4;
    e2e4.double_pawn = 1;
    return motif::db::game {
        .white = {.name = std::move(white), .elo = {}, .title = {}, .country = {}},
        .black = {.name = std::move(black), .elo = {}, .title = {}, .country = {}},
        .event_details = {},
        .date = {},
        .result = "1-0",
        .eco = {},
        .moves = {chesslib::codec::encode(e2e4)},
        .extra_tags = {},
        .provenance = {},
    };
}

}  // namespace

// ── AC1: create
// ───────────────────────────────────────────────────────────────

TEST_CASE("database_manager::create produces games.db and manifest.json", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"create"};

    auto res = motif::db::database_manager::create(tdir.path, "test-db");
    REQUIRE(res.has_value());

    CHECK(std::filesystem::exists(tdir.path / "games.db"));
    CHECK(std::filesystem::exists(tdir.path / "manifest.json"));
}

TEST_CASE("database_manager::create sets the snapshot identity, schema_version and an empty published generation",
          "[motif-db][database_manager]")
{
    tmp_dir const tdir {"create_mf"};

    auto res = motif::db::database_manager::create(tdir.path, "my-chess-db");
    REQUIRE(res.has_value());

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const& manifest = res->manifest();
    CHECK(manifest.name() == "my-chess-db");
    CHECK(manifest.schema_version == motif::db::schema::current_version);
    CHECK(manifest.canonical_game_count == 0);
    CHECK_FALSE(manifest.created_at().empty());
    CHECK(manifest.payload.bundle.id.size() == 32U);
    // Creation publishes the empty postings at sequence 0 as generation 1.
    CHECK(manifest.publication_generation == 1U);
    CHECK(manifest.covered_mutation_seq == std::optional<std::uint64_t> {0});

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const status = res->derived_index_status();
    REQUIRE(status.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(status->postings_usable);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK_FALSE(status->tree_usable);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(status->mutation_seq == 0U);
}

TEST_CASE("database_manager::create fails if bundle already exists", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"create_dup"};

    auto first = motif::db::database_manager::create(tdir.path, "dup-db");
    REQUIRE(first.has_value());
    first->close();

    auto second = motif::db::database_manager::create(tdir.path, "dup-db");
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error() == motif::db::error_code::io_failure);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("database_manager::discard_created_bundle keeps a pre-existing directory and unrelated files", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"discard_existing"};
    std::filesystem::create_directories(tdir.path / "keep_subdir");
    auto const sentinel = tdir.path / "sentinel.txt";
    {
        auto out = std::ofstream {sentinel};
        out << "user data";
    }
    auto const nested_sentinel = tdir.path / "keep_subdir" / "nested.txt";
    {
        auto out = std::ofstream {nested_sentinel};
        out << "nested user data";
    }

    auto created = motif::db::database_manager::create(tdir.path, "discard-existing");
    REQUIRE(created.has_value());
    REQUIRE(std::filesystem::exists(tdir.path / "games.db"));

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    created->discard_created_bundle();

    CHECK(std::filesystem::exists(tdir.path));
    CHECK(std::filesystem::exists(sentinel));
    CHECK(std::filesystem::exists(nested_sentinel));
    CHECK_FALSE(std::filesystem::exists(tdir.path / "games.db"));
    CHECK_FALSE(std::filesystem::exists(tdir.path / "manifest.json"));
    CHECK_FALSE(std::filesystem::exists(tdir.path / ".motif.lock"));
    auto leftovers = std::vector<std::string> {};
    for (auto const& entry : std::filesystem::directory_iterator {tdir.path}) {
        leftovers.push_back(entry.path().filename().string());
    }
    std::ranges::sort(leftovers);
    CHECK(leftovers == std::vector<std::string> {"keep_subdir", "sentinel.txt"});
}

TEST_CASE("database_manager::discard_created_bundle removes a directory that create() made", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"discard_fresh"};

    auto created = motif::db::database_manager::create(tdir.path, "discard-fresh");
    REQUIRE(created.has_value());
    REQUIRE(std::filesystem::exists(tdir.path));

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    created->discard_created_bundle();

    CHECK_FALSE(std::filesystem::exists(tdir.path));
}

TEST_CASE("database_manager::discard_created_bundle leaves an opened bundle on disk", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"discard_opened"};
    {
        auto created = motif::db::database_manager::create(tdir.path, "discard-opened");
        REQUIRE(created.has_value());
    }

    auto opened = motif::db::database_manager::open(tdir.path);
    REQUIRE(opened.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    opened->discard_created_bundle();

    CHECK(std::filesystem::exists(tdir.path / "games.db"));
    CHECK(std::filesystem::exists(tdir.path / "manifest.json"));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- fork/waitpid branching for the concurrent-writer check.
TEST_CASE("database_manager::open rejects a concurrent writer process for the same bundle", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"bundle_lock"};
    auto first = motif::db::database_manager::create(tdir.path, "lock-test");
    REQUIRE(first.has_value());

#ifdef _WIN32
    auto second = motif::db::database_manager::open(tdir.path);
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error() == motif::db::error_code::io_failure);
#else
    auto const child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        auto second = motif::db::database_manager::open(tdir.path);
        ::_exit(second.has_value() ? 1 : 0);
    }
    int status {};
    REQUIRE(::waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
#endif

    first->close();
    auto after_close = motif::db::database_manager::open(tdir.path);
    REQUIRE(after_close.has_value());
}

TEST_CASE("database_manager::open rejects a second open of the same bundle in one process", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"bundle_lock_same_process"};
    auto first = motif::db::database_manager::create(tdir.path, "lock-test");
    REQUIRE(first.has_value());

    auto second = motif::db::database_manager::open(tdir.path);
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error() == motif::db::error_code::io_failure);

    // A refused second open must not release the first holder's lock.
    auto third = motif::db::database_manager::open(tdir.path);
    CHECK_FALSE(third.has_value());

    first->close();
    auto after_close = motif::db::database_manager::open(tdir.path);
    REQUIRE(after_close.has_value());
}

TEST_CASE("database_manager holds a page-cache lease only while open", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"cache_lease"};
    auto& governor = motif::db::resource_governor::process();
    auto const baseline = governor.sqlite_cache_in_use();

    auto manager = motif::db::database_manager::create(tdir.path, "lease-test");
    REQUIRE(manager.has_value());
    CHECK(governor.sqlite_cache_in_use() > baseline);

    manager->close();
    CHECK(governor.sqlite_cache_in_use() == baseline);
}

TEST_CASE("database_manager::create initializes SQLite with correct schema version", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"schema_ver"};

    auto res = motif::db::database_manager::create(tdir.path, "ver-db");
    REQUIRE(res.has_value());
    res->close();

    // Re-open the raw SQLite file and verify user_version pragma.
    auto reopen = motif::db::database_manager::open(tdir.path);
    REQUIRE(reopen.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(reopen->manifest().schema_version == motif::db::schema::current_version);
}

// ── AC2: open
// ─────────────────────────────────────────────────────────────────

TEST_CASE("database_manager::open succeeds on an existing bundle", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"open"};

    {
        auto created = motif::db::database_manager::create(tdir.path, "open-db");
        REQUIRE(created.has_value());
    }  // closed here

    auto opened = motif::db::database_manager::open(tdir.path);
    REQUIRE(opened.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(opened->manifest().name() == "open-db");
}

TEST_CASE("database_manager::open returns not_found for missing bundle", "[motif-db][database_manager]")
{
    auto const missing = std::filesystem::temp_directory_path() / "motif_dbmgr_missing_xyzzy";
    auto res = motif::db::database_manager::open(missing);
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error() == motif::db::error_code::not_found);
}

TEST_CASE("database_manager::open does not recreate tables (idempotent open)", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"idempotent_open"};

    // Insert a game, close, reopen, and verify the game is still there.
    motif::db::game const inserted_game {
        .white = {.name = "Carlsen", .elo = {}, .title = {}, .country = {}},
        .black = {.name = "Caruana", .elo = {}, .title = {}, .country = {}},
        .event_details = motif::db::event {.name = "WCC 2018", .site = {}, .date = {}},
        .date = {},
        .result = "1/2-1/2",
        .eco = {},
        .moves = {},
        .extra_tags = {},
        .provenance = {},
    };

    auto game_id = motif::db::game_id {};
    {
        auto mgr = motif::db::database_manager::create(tdir.path, "persist-db");
        REQUIRE(mgr.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        auto insert_res = mgr->insert_game(inserted_game);
        REQUIRE(insert_res.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        game_id = *insert_res;
    }

    auto reopened = motif::db::database_manager::open(tdir.path);
    REQUIRE(reopened.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto get_res = reopened->store().get(game_id);
    REQUIRE(get_res.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(get_res->white.name == "Carlsen");
}

// ── AC3: portability
// ──────────────────────────────────────────────────────────

TEST_CASE("database_manager: bundle copied to another directory opens successfully", "[motif-db][database_manager]")
{
    tmp_dir const src_dir {"portable_src"};
    tmp_dir const dst_dir {"portable_dst"};

    motif::db::game const test_game {
        .white = {.name = "Fischer", .elo = {}, .title = {}, .country = {}},
        .black = {.name = "Spassky", .elo = {}, .title = {}, .country = {}},
        .event_details = {},
        .date = {},
        .result = "1-0",
        .eco = {},
        .moves = {},
        .extra_tags = {},
        .provenance = {},
    };

    auto game_id = motif::db::game_id {};
    {
        auto mgr = motif::db::database_manager::create(src_dir.path, "portable-db");
        REQUIRE(mgr.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        auto insert_res = mgr->insert_game(test_game);
        REQUIRE(insert_res.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        game_id = *insert_res;
    }

    // Copy entire bundle directory to dst.
    std::filesystem::copy(
        src_dir.path, dst_dir.path, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);

    auto opened = motif::db::database_manager::open(dst_dir.path);
    REQUIRE(opened.has_value());

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto get_res = opened->store().get(game_id);
    REQUIRE(get_res.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(get_res->white.name == "Fischer");
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(get_res->result == "1-0");
}

// ── AC2 error path: schema mismatch ──────────────────────────────────────────

TEST_CASE("database_manager::open returns schema_mismatch when user_version differs", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"schema_mismatch"};

    // Create a valid bundle, then corrupt user_version via raw SQLite.
    {
        auto mgr = motif::db::database_manager::create(tdir.path, "corrupt-db");
        REQUIRE(mgr.has_value());
    }

    // Overwrite user_version to a bogus value.
    sqlite3* raw_conn = nullptr;
    sqlite3_open((tdir.path / "games.db").c_str(), &raw_conn);
    sqlite3_exec(raw_conn, "PRAGMA user_version = 999;", nullptr, nullptr, nullptr);
    sqlite3_close(raw_conn);

    auto res = motif::db::database_manager::open(tdir.path);
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error() == motif::db::error_code::schema_mismatch);
}

// ── Persistent bundles never require DuckDB
// ──────────────────────────────────

TEST_CASE("database_manager::create does not create positions.duckdb", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"create_no_duckdb"};

    auto res = motif::db::database_manager::create(tdir.path, "no-duckdb-db");
    REQUIRE(res.has_value());

    CHECK_FALSE(std::filesystem::exists(tdir.path / "positions.duckdb"));
    // A trivial empty (0-game) postings generation is published immediately
    // so the bundle never needs any legacy fallback just because nothing
    // has been imported yet -- see create()'s comment.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(res->manifest().postings() != nullptr);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(res->manifest().postings()->document_count == 0U);
}

TEST_CASE("database_manager::open ignores a leftover positions.duckdb file and leaves it untouched",
          "[motif-db][database_manager][migration]")
{
    tmp_dir const tdir {"dual_index"};

    // Build a persistent bundle with one game and a valid, covering postings
    // generation, entirely through the postings-only path.
    {
        auto mgr = motif::db::database_manager::create(tdir.path, "dual-db");
        REQUIRE(mgr.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->insert_game(make_one_move_game("White", "Black")).has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->rebuild_position_postings().has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->manifest().postings() != nullptr);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        mgr->close();
    }

    // Simulate a leftover positions.duckdb from an older release: the file
    // does not need to be a valid DuckDB database at all -- this codebase no
    // longer links or reads DuckDB in any form, so any leftover bytes at
    // this exact path must simply be ignored.
    {
        std::ofstream marker {tdir.path / "positions.duckdb", std::ios::binary};
        REQUIRE(marker.good());
        marker << "not a real duckdb file";
    }
    auto const duckdb_size_before = std::filesystem::file_size(tdir.path / "positions.duckdb");

    // The cache is not needed to open: drop it entirely to exercise the SQLite-
    // authoritative recovery decision, not just the ordinary open path.
    REQUIRE(std::filesystem::remove(tdir.path / "manifest.json"));

    auto reopened = motif::db::database_manager::open(tdir.path);
    REQUIRE(reopened.has_value());
    auto const start_hash = motif::db::zobrist_hash {chesslib::board {}.hash()};
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const matches = reopened->query_position_matches(start_hash);
    REQUIRE(matches.has_value());
    CHECK(matches->size() == 1U);
    CHECK(std::filesystem::exists(tdir.path / "positions.duckdb"));
    CHECK(std::filesystem::file_size(tdir.path / "positions.duckdb") == duckdb_size_before);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    reopened->close();
}

TEST_CASE("database_manager::rebuild_position_postings on empty DB indexes 0 games", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"postings_rebuild_empty"};

    auto mgr = motif::db::database_manager::create(tdir.path, "empty-db");
    REQUIRE(mgr.has_value());

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto rebuild_res = mgr->rebuild_position_postings();
    REQUIRE(rebuild_res.has_value());

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->manifest().postings() != nullptr);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(mgr->manifest().postings()->document_count == 0U);
}

TEST_CASE("database_manager::rebuild_position_postings after N-move game indexes N+1 occurrences", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"postings_rebuild_nmove"};

    // Encode e2-e4 (double pawn push) and e7-e5 (double pawn push)
    chesslib::move e2e4 {};
    e2e4.source_square = chesslib::square::e2;
    e2e4.target_square = chesslib::square::e4;
    e2e4.double_pawn = 1;

    chesslib::move e7e5 {};
    e7e5.source_square = chesslib::square::e7;
    e7e5.target_square = chesslib::square::e5;
    e7e5.double_pawn = 1;

    std::vector<std::uint16_t> const moves {
        chesslib::codec::encode(e2e4),
        chesslib::codec::encode(e7e5),
    };

    motif::db::game const test_game {
        .white = {.name = "White", .elo = {}, .title = {}, .country = {}},
        .black = {.name = "Black", .elo = {}, .title = {}, .country = {}},
        .event_details = {},
        .date = {},
        .result = "1-0",
        .eco = {},
        .moves = moves,
        .extra_tags = {},
        .provenance = {},
    };

    auto mgr = motif::db::database_manager::create(tdir.path, "nmove-db");
    REQUIRE(mgr.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->insert_game(test_game).has_value());

    auto metrics = motif::db::position_postings_build_metrics {};
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto rebuild_res = mgr->rebuild_position_postings(&metrics);
    REQUIRE(rebuild_res.has_value());

    // Every ply, including the starting position, is one occurrence.
    CHECK(metrics.occurrence_count == static_cast<std::uint64_t>(moves.size() + 1));
}

TEST_CASE("database_manager::rebuild_position_postings is idempotent", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"postings_rebuild_idem"};

    chesslib::move e2e4 {};
    e2e4.source_square = chesslib::square::e2;
    e2e4.target_square = chesslib::square::e4;
    e2e4.double_pawn = 1;

    motif::db::game const test_game {
        .white = {.name = "White", .elo = {}, .title = {}, .country = {}},
        .black = {.name = "Black", .elo = {}, .title = {}, .country = {}},
        .event_details = {},
        .date = {},
        .result = "1/2-1/2",
        .eco = {},
        .moves = {chesslib::codec::encode(e2e4)},
        .extra_tags = {},
        .provenance = {},
    };

    auto mgr = motif::db::database_manager::create(tdir.path, "idem-db");
    REQUIRE(mgr.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->insert_game(test_game).has_value());

    auto first_metrics = motif::db::position_postings_build_metrics {};
    auto second_metrics = motif::db::position_postings_build_metrics {};
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->rebuild_position_postings(&first_metrics).has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->rebuild_position_postings(&second_metrics).has_value());

    CHECK(first_metrics.occurrence_count == 2U);
    CHECK(second_metrics.occurrence_count == 2U);
}

TEST_CASE("database_manager::rebuild_position_postings rejects out-of-range elo", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"postings_rebuild_elo_range"};

    chesslib::move e2e4 {};
    e2e4.source_square = chesslib::square::e2;
    e2e4.target_square = chesslib::square::e4;
    e2e4.double_pawn = 1;

    motif::db::game const test_game {
        .white = {.name = "White", .elo = 40000, .title = {}, .country = {}},
        .black = {.name = "Black", .elo = {}, .title = {}, .country = {}},
        .event_details = {},
        .date = {},
        .result = "1-0",
        .eco = {},
        .moves = {chesslib::codec::encode(e2e4)},
        .extra_tags = {},
        .provenance = {},
    };

    auto mgr = motif::db::database_manager::create(tdir.path, "elo-range-db");
    REQUIRE(mgr.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->insert_game(test_game).has_value());

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto rebuild_res = mgr->rebuild_position_postings();
    REQUIRE_FALSE(rebuild_res.has_value());
    CHECK(rebuild_res.error() == motif::db::error_code::io_failure);
}

TEST_CASE("database_manager::rebuild_position_postings indexes every distinct hash exactly once", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"postings_rebuild_distinct_hashes"};

    chesslib::move e2e4 {};
    e2e4.source_square = chesslib::square::e2;
    e2e4.target_square = chesslib::square::e4;
    e2e4.double_pawn = 1;

    chesslib::move d2d4 {};
    d2d4.source_square = chesslib::square::d2;
    d2d4.target_square = chesslib::square::d4;
    d2d4.double_pawn = 1;

    motif::db::game const test_game_a {
        .white = {.name = "White", .elo = 2800, .title = {}, .country = {}},
        .black = {.name = "Black", .elo = 2700, .title = {}, .country = {}},
        .event_details = {},
        .date = {},
        .result = "1-0",
        .eco = {},
        .moves = {chesslib::codec::encode(e2e4)},
        .extra_tags = {},
        .provenance = {},
    };

    motif::db::game const test_game_b {
        .white = {.name = "White 2", .elo = 2500, .title = {}, .country = {}},
        .black = {.name = "Black 2", .elo = 2400, .title = {}, .country = {}},
        .event_details = {},
        .date = {},
        .result = "0-1",
        .eco = {},
        .moves = {chesslib::codec::encode(d2d4)},
        .extra_tags = {},
        .provenance = {},
    };

    auto mgr = motif::db::database_manager::create(tdir.path, "distinct-hashes-db");
    REQUIRE(mgr.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->insert_game(test_game_a).has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->insert_game(test_game_b).has_value());

    auto metrics = motif::db::position_postings_build_metrics {};
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto rebuild_res = mgr->rebuild_position_postings(&metrics);
    REQUIRE(rebuild_res.has_value());

    // Both games share the starting position (1 distinct hash) and each
    // reaches its own distinct post-move hash: 3 distinct hashes across 4
    // total occurrences.
    CHECK(metrics.occurrence_count == 4U);
    CHECK(metrics.distinct_hash_count == 3U);

    auto const start_hash = motif::db::zobrist_hash {chesslib::board {}.hash()};
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const start_summary = mgr->position_summary(start_hash);
    REQUIRE(start_summary.has_value());
    REQUIRE(start_summary->has_value());
    CHECK(start_summary->value_or(motif::db::position_postings_summary {}).distinct_game_count == 2U);
}

// ── remove_game
// ───────────────────────────────────────────────────────────────

TEST_CASE("database_manager::remove_game deletes the SQLite row and marks postings stale", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"remove_game"};

    auto mgr = motif::db::database_manager::create(tdir.path, "remove-db");
    REQUIRE(mgr.has_value());

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const gid = mgr->insert_game(make_one_move_game("White", "Black"));
    REQUIRE(gid.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->rebuild_position_postings().has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->manifest().postings() != nullptr);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const published_file = tdir.path / mgr->manifest().postings()->artifact.filename;

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->remove_game(*gid).has_value());

    // Game gone from SQLite.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const get_res = mgr->store().get(*gid);
    REQUIRE_FALSE(get_res.has_value());
    CHECK(get_res.error() == motif::db::error_code::not_found);

    // remove_game() advanced the mutation sequence in its own transaction: the
    // last published snapshot stays referenced (and its file on disk) but no
    // longer covers canonical state, so every position query fails closed.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const stale = mgr->derived_index_status();
    REQUIRE(stale.has_value());
    CHECK_FALSE(stale->postings_usable);
    CHECK(stale->mutation_seq > stale->covered_mutation_seq.value_or(0));
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(mgr->manifest().postings() != nullptr);
    CHECK(std::filesystem::exists(published_file));
    CHECK_FALSE(mgr->query_position_matches(motif::db::zobrist_hash {chesslib::board {}.hash()}).has_value());

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->rebuild_position_postings().has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(mgr->manifest().postings()->document_count == 0U);
    // The superseded generation is deleted once the new one is committed.
    CHECK_FALSE(std::filesystem::exists(published_file));
}

TEST_CASE("database_manager::remove_game returns not_found for absent id", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"remove_game_nf"};

    auto mgr = motif::db::database_manager::create(tdir.path, "remove-nf-db");
    REQUIRE(mgr.has_value());

    auto const absent_id = motif::db::game_id {99999U};
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const res = mgr->remove_game(absent_id);
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error() == motif::db::error_code::not_found);
}

TEST_CASE("database_manager::remove_user_game rejects imported games without changing postings", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"remove_imported_game"};

    auto mgr = motif::db::database_manager::create(tdir.path, "remove-imported-db");
    REQUIRE(mgr.has_value());

    auto const gid = mgr->insert_game(make_one_move_game("White", "Black"));
    REQUIRE(gid.has_value());
    REQUIRE(mgr->rebuild_position_postings().has_value());
    auto const generation_before = mgr->manifest().publication_generation;

    auto const remove_result = mgr->remove_user_game(*gid);
    REQUIRE_FALSE(remove_result.has_value());
    CHECK(remove_result.error() == motif::db::error_code::not_editable);
    CHECK(mgr->store().get(*gid).has_value());
    // A rejected removal advances nothing: the published snapshot still covers
    // the bundle and queries keep working.
    CHECK(mgr->manifest().publication_generation == generation_before);
    auto const status = mgr->derived_index_status();
    REQUIRE(status.has_value());
    CHECK(status->postings_usable);
    CHECK(mgr->query_position_matches(motif::db::zobrist_hash {chesslib::board {}.hash()}).has_value());
}

TEST_CASE("database_manager::remove_user_game deletes a manual game and marks postings stale", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"remove_manual_game"};

    auto mgr = motif::db::database_manager::create(tdir.path, "remove-manual-db");
    REQUIRE(mgr.has_value());

    auto const gid = mgr->insert_game(make_one_move_game("White", "Black"));
    REQUIRE(gid.has_value());
    REQUIRE(mgr->set_manual_game_provenance(*gid, std::nullopt, "pending").has_value());
    REQUIRE(mgr->rebuild_position_postings().has_value());
    REQUIRE(mgr->remove_user_game(*gid).has_value());

    auto const game = mgr->store().get(*gid);
    REQUIRE_FALSE(game.has_value());
    CHECK(game.error() == motif::db::error_code::not_found);
    // The removal advanced the sequence; the last snapshot is retained but stale.
    auto const stale = mgr->derived_index_status();
    REQUIRE(stale.has_value());
    CHECK_FALSE(stale->postings_usable);
    CHECK(mgr->query_position_matches(motif::db::zobrist_hash {chesslib::board {}.hash()}).error() == motif::db::error_code::io_failure);

    REQUIRE(mgr->rebuild_position_postings().has_value());
    REQUIRE(mgr->manifest().postings() != nullptr);
    CHECK(mgr->manifest().postings()->document_count == 0U);
    CHECK(mgr->derived_index_status()->postings_usable);
}

TEST_CASE("database_manager::find_games intersects position and metadata filters", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"find_games_position"};

    auto mgr = motif::db::database_manager::create(tdir.path, "find-games-position-db");
    REQUIRE(mgr.has_value());

    chesslib::move e2e4 {};
    e2e4.source_square = chesslib::square::e2;
    e2e4.target_square = chesslib::square::e4;
    e2e4.double_pawn = 1;

    chesslib::move d2d4 {};
    d2d4.source_square = chesslib::square::d2;
    d2d4.target_square = chesslib::square::d4;
    d2d4.double_pawn = 1;

    constexpr std::int32_t carlsen_elo {2830};
    constexpr std::int32_t nepomniachtchi_elo {2795};
    constexpr std::int32_t caruana_elo {2780};
    constexpr std::int32_t ding_elo {2760};
    constexpr std::int32_t nakamura_elo {2800};

    auto matching_game = motif::db::game {
        .white = {.name = "Magnus Carlsen", .elo = carlsen_elo, .title = {}, .country = {}},
        .black = {.name = "Ian Nepomniachtchi", .elo = nepomniachtchi_elo, .title = {}, .country = {}},
        .event_details = {},
        .date = "2024.01.01",
        .result = "1-0",
        .eco = "B90",
        .moves = {chesslib::codec::encode(e2e4)},
        .extra_tags = {},
        .provenance = {},
    };
    auto wrong_player = motif::db::game {
        .white = {.name = "Fabiano Caruana", .elo = caruana_elo, .title = {}, .country = {}},
        .black = {.name = "Ding Liren", .elo = ding_elo, .title = {}, .country = {}},
        .event_details = {},
        .date = "2024.01.02",
        .result = "1-0",
        .eco = "B90",
        .moves = {chesslib::codec::encode(e2e4)},
        .extra_tags = {},
        .provenance = {},
    };
    auto wrong_position = motif::db::game {
        .white = {.name = "Magnus Carlsen", .elo = carlsen_elo, .title = {}, .country = {}},
        .black = {.name = "Hikaru Nakamura", .elo = nakamura_elo, .title = {}, .country = {}},
        .event_details = {},
        .date = "2024.01.03",
        .result = "1-0",
        .eco = "B90",
        .moves = {chesslib::codec::encode(d2d4)},
        .extra_tags = {},
        .provenance = {},
    };

    auto const matching_id = mgr->insert_game(matching_game);
    REQUIRE(matching_id.has_value());
    REQUIRE(mgr->insert_game(wrong_player).has_value());
    REQUIRE(mgr->insert_game(wrong_position).has_value());
    REQUIRE(mgr->rebuild_position_postings().has_value());

    auto board = chesslib::board {};
    chesslib::move_maker {board, e2e4}.make();

    auto games = mgr->find_games(motif::db::search_filter {
        .player_name = "Carlsen",
        .player_color = motif::db::player_color::either,
        .min_elo = {},
        .max_elo = {},
        .result = {},
        .eco_prefix = {},
        .position = motif::db::zobrist_hash {board.hash()},
    });
    REQUIRE(games.has_value());
    REQUIRE(games->games.size() == 1);
    CHECK(games->total_count == 1);
    CHECK(games->games.front().id == *matching_id);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("database_manager::find_games with a position filter respects a non-default sort", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"find_games_position_sort"};

    auto mgr = motif::db::database_manager::create(tdir.path, "find-games-position-sort-db");
    REQUIRE(mgr.has_value());

    chesslib::move e2e4 {};
    e2e4.source_square = chesslib::square::e2;
    e2e4.target_square = chesslib::square::e4;
    e2e4.double_pawn = 1;

    // Insert in an order where ascending game-id order does NOT match
    // ascending white-name order: id 1 = Charlie, id 2 = Alice, id 3 = Bob.
    // The position-postings fast path (no metadata filter) must not
    // paginate by game-id order and only re-sort the resulting page --
    // that silently drops Bob (id 3) from a white-ascending, limit-2 page.
    for (auto const& name : {"Charlie", "Alice", "Bob"}) {
        auto game = motif::db::game {
            .white = {.name = name, .elo = {}, .title = {}, .country = {}},
            .black = {.name = "Opponent", .elo = {}, .title = {}, .country = {}},
            .event_details = {},
            .date = "2024.01.01",
            .result = "1-0",
            .eco = "B90",
            .moves = {chesslib::codec::encode(e2e4)},
            .extra_tags = {},
            .provenance = {},
        };
        REQUIRE(mgr->insert_game(game).has_value());
    }
    REQUIRE(mgr->rebuild_position_postings().has_value());

    auto board = chesslib::board {};
    chesslib::move_maker {board, e2e4}.make();

    auto const first_page = mgr->find_games(motif::db::search_filter {
        .player_name = {},
        .player_color = motif::db::player_color::either,
        .min_elo = {},
        .max_elo = {},
        .result = {},
        .eco_prefix = {},
        .position = motif::db::zobrist_hash {board.hash()},
        .offset = 0,
        .limit = 2,
        .sort_column = motif::db::game_sort_column::white,
        .sort_ascending = true,
    });
    REQUIRE(first_page.has_value());
    CHECK(first_page->total_count == 3);
    REQUIRE(first_page->games.size() == 2);
    CHECK(first_page->games[0].white == "Alice");
    CHECK(first_page->games[1].white == "Bob");
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("database_manager::find_games handles >999 position IDs via batched IN clause", "[motif-db][database_manager]")
{
    // AC6: batching at 999 must be exercised. Insert 1001 games all starting
    // with 1.e4 so that the position store returns >999 matching IDs.
    tmp_dir const tdir {"find_games_batch"};
    auto mgr = motif::db::database_manager::create(tdir.path, "find-games-batch-db");
    REQUIRE(mgr.has_value());

    chesslib::move e2e4 {};
    e2e4.source_square = chesslib::square::e2;
    e2e4.target_square = chesslib::square::e4;
    e2e4.double_pawn = 1;

    constexpr std::size_t game_count {1001};
    constexpr std::size_t expected_page {500};

    auto const insert_game = [&](std::size_t index) -> void
    {
        auto game_rec = motif::db::game {
            .white = {.name = "W" + std::to_string(index), .elo = {}, .title = {}, .country = {}},
            .black = {.name = "B" + std::to_string(index), .elo = {}, .title = {}, .country = {}},
            .event_details = {},
            .date = {},
            .result = "1-0",
            .eco = {},
            .moves = {chesslib::codec::encode(e2e4)},
            .extra_tags = {},
            .provenance = {},
        };
        REQUIRE(mgr->insert_game(game_rec).has_value());
    };

    for (std::size_t index = 0; index < game_count; ++index) {
        insert_game(index);
    }
    REQUIRE(mgr->rebuild_position_postings().has_value());

    auto board = chesslib::board {};
    chesslib::move_maker {board, e2e4}.make();

    auto const games = mgr->find_games(motif::db::search_filter {
        .player_name = {},
        .player_color = motif::db::player_color::either,
        .min_elo = {},
        .max_elo = {},
        .result = {},
        .eco_prefix = {},
        .position = motif::db::zobrist_hash {board.hash()},
        .offset = 0,
        .limit = expected_page,
    });
    REQUIRE(games.has_value());
    // 1001 games match the position; limit=500 so we get 500 rows but total_count must reflect all.
    CHECK(games->games.size() == expected_page);
    CHECK(games->total_count == static_cast<std::int64_t>(game_count));  // NOLINT(modernize-use-integer-sign-comparison)
}

// ──────────────────────────────────────

TEST_CASE("database_manager::close keeps the published baseline and does not refresh it to the live count", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"manifest_count"};

    {
        auto mgr = motif::db::database_manager::create(tdir.path, "count-db");
        REQUIRE(mgr.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->insert_game(make_one_move_game("A", "B")).has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->insert_game(make_one_move_game("C", "D")).has_value());
    }  // close() called here

    // The cache mirrors SQLite's snapshot row: still the generation published at
    // creation, covering sequence 0 with the 0 games it indexed. Current counts
    // come from the game store, never from this baseline.
    auto const cache = motif::db::read_manifest(tdir.path / "manifest.json");
    REQUIRE(cache.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(cache->canonical_game_count == 0U);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(cache->covered_mutation_seq == std::optional<std::uint64_t> {0});
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(cache->publication_generation == 1U);
}

namespace
{

// Hook state for tests that fail one fault point; the seam is a plain function
// pointer, so the target lives at namespace scope.
motif::db::fault::point g_fail_point {};  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables) -- read by the plain-function hook.
std::uint64_t g_hook_calls {};  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables) -- see g_fail_point.

auto fail_at_selected_point(motif::db::fault::point const where,
                            motif::db::fault::artifact_kind /*kind*/,
                            std::uint64_t /*generation*/) noexcept -> motif::db::fault::fault_action
{
    if (where == g_fail_point) {
        ++g_hook_calls;
        return motif::db::fault::fault_action::fail_io;
    }
    return motif::db::fault::fault_action::proceed;
}

auto read_all_bytes(std::filesystem::path const& path) -> std::string
{
    std::ifstream input {path, std::ios::binary};
    return {std::istreambuf_iterator<char> {input}, std::istreambuf_iterator<char> {}};
}

struct legacy_bundle
{
    std::filesystem::path postings_file;
    std::optional<std::filesystem::path> tree_file;
};

// Hand-builds a schema-v6 bundle with real artifacts: a bundle built by this
// code, then rewritten into exactly what a v6 binary left on disk -- no
// mutation_state or published_snapshot, user_version 6, generation-numbered
// artifact names, and a legacy manifest.json that lists them.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto make_legacy_v6_bundle(std::filesystem::path const& dir, std::string const& name, bool const with_tree) -> legacy_bundle
{
    std::filesystem::path postings_name;
    std::filesystem::path tree_name;
    {
        auto mgr = motif::db::database_manager::create(dir, name);
        REQUIRE(mgr.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->insert_game(make_one_move_game("White", "Black")).has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->rebuild_position_postings().has_value());
        if (with_tree) {
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
            REQUIRE(mgr->rebuild_opening_tree_index().has_value());
        }
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->manifest().postings() != nullptr);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        postings_name = mgr->manifest().postings()->artifact.filename;
        if (with_tree) {
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
            tree_name = mgr->manifest().tree()->checkpoint.filename;
        }
    }

    sqlite3* raw_conn = nullptr;
    REQUIRE(sqlite3_open((dir / "games.db").c_str(), &raw_conn) == SQLITE_OK);
    REQUIRE(sqlite3_exec(
                raw_conn, "DROP TABLE mutation_state; DROP TABLE published_snapshot; PRAGMA user_version = 6;", nullptr, nullptr, nullptr)
            == SQLITE_OK);
    sqlite3_close(raw_conn);

    auto fixture = legacy_bundle {};
    fixture.postings_file = dir / "positions.postings.4";
    std::filesystem::rename(dir / postings_name, fixture.postings_file);
    auto const entry_json = [](std::filesystem::path const& file) -> std::string
    {
        auto const bytes = read_all_bytes(file);
        return fmt::format(R"({{"filename":"{}","source_generation":2,"game_count":1,"file_size":{},"checksum":{}}})",
                           file.filename().string(),
                           bytes.size(),
                           motif::db::fnv1a64(bytes));
    };
    auto tree_entry = std::string {"null"};
    if (with_tree) {
        fixture.tree_file = dir / "opening_tree.idx.5";
        std::filesystem::rename(dir / tree_name, *fixture.tree_file);
        tree_entry = entry_json(*fixture.tree_file);
    }
    std::ofstream manifest {dir / "manifest.json", std::ios::trunc};
    manifest << fmt::format(
        R"({{"name":"{}","schema_version":6,"game_count":1,"created_at":"2026-03-01T00:00:00Z",)" R"("position_index_dirty":false,"source_generation":2,"derived_index_build_seq":6,)" R"("position_postings":{},"opening_tree_index":{}}})",
        name,
        entry_json(fixture.postings_file),
        tree_entry);
    return fixture;
}

}  // namespace

TEST_CASE("database_manager::open of an unchanged bundle neither replays nor rewrites the cache", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"unchanged_reopen"};
    {
        auto mgr = motif::db::database_manager::create(tdir.path, "unchanged-db");
        REQUIRE(mgr.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->insert_game(make_one_move_game("A", "B")).has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->rebuild_position_postings().has_value());
    }
    auto const cache_before = read_all_bytes(tdir.path / "manifest.json");
    auto const modified_before = std::filesystem::last_write_time(tdir.path / "manifest.json");

    // Any attempt to build an artifact on this reopen fails the open: proof that
    // a crash-free session with no mutation never triggers a corpus replay.
    g_fail_point = motif::db::fault::point::before_build;
    g_hook_calls = 0;
    {
        motif::db::fault::scoped_hook const hook {fail_at_selected_point};
        auto reopened = motif::db::database_manager::open(tdir.path);
        REQUIRE(reopened.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        CHECK(reopened->derived_index_status()->postings_usable);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        CHECK(reopened->query_position_matches(motif::db::zobrist_hash {chesslib::board {}.hash()}).has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        reopened->close();
    }
    CHECK(g_hook_calls == 0U);
    CHECK(read_all_bytes(tdir.path / "manifest.json") == cache_before);
    auto const cache_was_not_rewritten = std::filesystem::last_write_time(tdir.path / "manifest.json") == modified_before;
    CHECK(cache_was_not_rewritten);
}

TEST_CASE("database_manager::open rebuilds postings automatically when the snapshot does not cover the canonical sequence",
          "[motif-db][database_manager]")
{
    tmp_dir const tdir {"manifest_rebuild_dirty"};

    {
        auto mgr = motif::db::database_manager::create(tdir.path, "rebuild-dirty-db");
        REQUIRE(mgr.has_value());
        // Insert without rebuilding: the bundle now has 1 game but its
        // published postings generation still covers sequence 0 and 0 games.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->insert_game(make_one_move_game("W", "B")).has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        mgr->close();
    }

    {
        auto const cache = motif::db::read_manifest(tdir.path / "manifest.json");
        REQUIRE(cache.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        CHECK(cache->covered_mutation_seq == std::optional<std::uint64_t> {0});
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        CHECK(cache->canonical_game_count == 0U);
    }

    auto reopened = motif::db::database_manager::open(tdir.path);
    REQUIRE(reopened.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(reopened->manifest().postings() != nullptr);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(reopened->manifest().postings()->document_count == 1U);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(reopened->manifest().covered_mutation_seq == std::optional<std::uint64_t> {1});
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(reopened->manifest().publication_generation == 2U);

    auto const start_hash = motif::db::zobrist_hash {chesslib::board {}.hash()};
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const matches = reopened->query_position_matches(start_hash);
    REQUIRE(matches.has_value());
    CHECK(matches->size() == 1U);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    reopened->close();
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("database_manager::open migrates a v6 bundle with real artifacts by one full rebuild", "[motif-db][database_manager][migration]")
{
    tmp_dir const tdir {"legacy_migrate"};
    auto const legacy = make_legacy_v6_bundle(tdir.path, "legacy-db", /*with_tree=*/true);
    REQUIRE(legacy.tree_file.has_value());

    // A leftover positions.duckdb from an older release. It does not need to be
    // a valid DuckDB database -- this codebase never reads it.
    {
        std::ofstream marker {tdir.path / "positions.duckdb", std::ios::binary};
        REQUIRE(marker.good());
        marker << "not a real duckdb file";
    }
    auto const duckdb_size_before = std::filesystem::file_size(tdir.path / "positions.duckdb");

    // The legacy artifacts are not adopted: exactly one build happens, and
    // only for postings.
    g_fail_point = motif::db::fault::point::before_build;
    g_hook_calls = 0;
    auto const counting_hook =
        +[](motif::db::fault::point const where, motif::db::fault::artifact_kind, std::uint64_t) noexcept -> motif::db::fault::fault_action
    {
        if (where == motif::db::fault::point::before_build) {
            ++g_hook_calls;
        }
        return motif::db::fault::fault_action::proceed;
    };
    std::uint64_t first_generation {};
    {
        motif::db::fault::scoped_hook const hook {counting_hook};
        auto reopened = motif::db::database_manager::open(tdir.path);
        REQUIRE(reopened.has_value());
        CHECK(g_hook_calls == 1U);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        CHECK(reopened->manifest().name() == "legacy-db");
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        CHECK(reopened->manifest().created_at() == "2026-03-01T00:00:00Z");
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(reopened->manifest().postings() != nullptr);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        CHECK(reopened->manifest().covered_mutation_seq == std::optional<std::uint64_t> {0});
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        CHECK(reopened->manifest().postings()->document_count == 1U);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        CHECK(reopened->manifest().publication_generation == 1U);

        auto const start_hash = motif::db::zobrist_hash {chesslib::board {}.hash()};
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        auto const summary = reopened->position_summary(start_hash);
        REQUIRE(summary.has_value());
        REQUIRE(summary->has_value());
        CHECK(summary->value_or(motif::db::position_postings_summary {}).distinct_game_count == 1U);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        first_generation = reopened->manifest().publication_generation;
        // The tree is not carried over: it is rebuilt on request.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        CHECK(reopened->manifest().tree() == nullptr);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        reopened->close();
    }

    // The old generation-numbered artifacts are gone once the v7 snapshot
    // committed; the leftover positions.duckdb is never read or deleted.
    CHECK_FALSE(std::filesystem::exists(legacy.postings_file));
    CHECK_FALSE(std::filesystem::exists(motif_test::require_value(legacy.tree_file)));
    CHECK(std::filesystem::exists(tdir.path / "positions.duckdb"));
    CHECK(std::filesystem::file_size(tdir.path / "positions.duckdb") == duckdb_size_before);

    // The cache was rewritten from SQLite as a v7 envelope.
    auto const cache = motif::db::read_manifest(tdir.path / "manifest.json");
    REQUIRE(cache.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(cache->schema_version == motif::db::schema::current_version);

    // A later open() of the now-migrated bundle needs no further rebuild.
    g_hook_calls = 0;
    {
        motif::db::fault::scoped_hook const hook {counting_hook};
        auto reopened_again = motif::db::database_manager::open(tdir.path);
        REQUIRE(reopened_again.has_value());
        CHECK(g_hook_calls == 0U);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        CHECK(reopened_again->manifest().publication_generation == first_generation);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        reopened_again->close();
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("database_manager::open fails closed and is retryable when the post-migration rebuild cannot publish",
          "[motif-db][database_manager][migration]")
{
    tmp_dir const tdir {"legacy_migrate_fail"};
    auto const legacy = make_legacy_v6_bundle(tdir.path, "legacy-fail-db", /*with_tree=*/false);
    {
        std::ofstream marker {tdir.path / "positions.duckdb", std::ios::binary};
        REQUIRE(marker.good());
        marker << "not a real duckdb file";
    }
    auto const duckdb_size_before = std::filesystem::file_size(tdir.path / "positions.duckdb");

    g_fail_point = motif::db::fault::point::before_artifact_rename;
    g_hook_calls = 0;
    {
        motif::db::fault::scoped_hook const hook {fail_at_selected_point};
        auto reopened = motif::db::database_manager::open(tdir.path);
        REQUIRE_FALSE(reopened.has_value());
    }
    CHECK(g_hook_calls == 1U);
    CHECK(std::filesystem::file_size(tdir.path / "positions.duckdb") == duckdb_size_before);
    // Nothing was published and nothing legacy was thrown away: the old
    // artifact is still there for the retry, and no orphan candidate remains.
    CHECK(std::filesystem::exists(legacy.postings_file));
    for (auto const& entry : std::filesystem::directory_iterator {tdir.path}) {
        CHECK_FALSE(entry.path().filename().string().ends_with(".building"));
    }

    // The migration itself had committed (v7, unbuilt snapshot); the retry only
    // has to build.
    auto retried = motif::db::database_manager::open(tdir.path);
    REQUIRE(retried.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(retried->manifest().postings() != nullptr);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(retried->derived_index_status()->postings_usable);
    CHECK_FALSE(std::filesystem::exists(legacy.postings_file));
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    retried->close();
}

TEST_CASE("database_manager::open fails closed when a missing postings artifact cannot be rebuilt",
          "[motif-db][database_manager][position_postings]")
{
    tmp_dir const tdir {"postings_repair_fail"};

    std::filesystem::path published_file;
    std::uint64_t generation_before {};
    {
        auto mgr = motif::db::database_manager::create(tdir.path, "repair-fail-db");
        REQUIRE(mgr.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->insert_game(make_one_move_game("White", "Black")).has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->rebuild_position_postings().has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        published_file = tdir.path / mgr->manifest().postings()->artifact.filename;
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        generation_before = mgr->manifest().publication_generation;
    }
    REQUIRE(std::filesystem::remove(published_file));

    g_fail_point = motif::db::fault::point::before_build;
    {
        motif::db::fault::scoped_hook const hook {fail_at_selected_point};
        auto reopened = motif::db::database_manager::open(tdir.path);
        REQUIRE_FALSE(reopened.has_value());
    }
    CHECK_FALSE(std::filesystem::exists(tdir.path / "positions.duckdb"));

    // Retryable once the failure clears: the rebuild replaces the lost file.
    auto retried = motif::db::database_manager::open(tdir.path);
    REQUIRE(retried.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(retried->manifest().publication_generation == generation_before + 1U);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(retried->query_position_matches(motif::db::zobrist_hash {chesslib::board {}.hash()}).has_value());
}

// ── patch_game_metadata
// ──────────────────────────────────────────────────────────

TEST_CASE("database_manager::patch_game_metadata keeps postings usable and reports the new elo without a rebuild",
          "[motif-db][database_manager]")
{
    tmp_dir const tdir {"patch_elo_sync"};

    auto mgr = motif::db::database_manager::create(tdir.path, "patch-elo-db");
    REQUIRE(mgr.has_value());

    constexpr auto initial_white_elo = std::int32_t {2400};
    constexpr auto initial_black_elo = std::int32_t {2300};
    constexpr auto patched_white_elo = std::int32_t {2600};
    constexpr auto patched_black_elo = std::int32_t {2500};

    chesslib::move e2e4 {};
    e2e4.source_square = chesslib::square::e2;
    e2e4.target_square = chesslib::square::e4;
    e2e4.double_pawn = 1;

    motif::db::game const game {
        .white = {.name = "White Player", .elo = initial_white_elo, .title = {}, .country = {}},
        .black = {.name = "Black Player", .elo = initial_black_elo, .title = {}, .country = {}},
        .event_details = {},
        .date = {},
        .result = "1-0",
        .eco = {},
        .moves = {chesslib::codec::encode(e2e4)},
        .extra_tags = {},
        .provenance = {},
    };

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const game_id = mgr->insert_game(game);
    REQUIRE(game_id.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->set_manual_game_provenance(*game_id, {}, "new").has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->rebuild_position_postings().has_value());

    auto const start_hash = motif::db::zobrist_hash {chesslib::board {}.hash()};

    // Verify initial elos are in the published postings.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto rows_before = mgr->query_position_matches(start_hash);
    REQUIRE(rows_before.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(rows_before->size() == 1);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(rows_before->front().white_elo == std::optional<std::int16_t> {static_cast<std::int16_t>(initial_white_elo)});
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(rows_before->front().black_elo == std::optional<std::int16_t> {static_cast<std::int16_t>(initial_black_elo)});

    // Patch both elos.
    auto patch = motif::db::game_patch {};
    patch.white_elo = motif::db::rating_patch::set(patched_white_elo);
    patch.black_elo = motif::db::rating_patch::set(patched_black_elo);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->patch_game_metadata(*game_id, patch).has_value());

    // The patch is one atomic operation: its canonical UPDATE, the sequence advance and the
    // new snapshot commit together, so queries stay available and already report the
    // corrected ratings. No rebuild is involved.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const patched_status = mgr->derived_index_status();
    REQUIRE(patched_status.has_value());
    CHECK(patched_status->postings_usable);
    CHECK(patched_status->covered_mutation_seq == std::optional<std::uint64_t> {patched_status->mutation_seq});
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(mgr->manifest().postings() != nullptr);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto rows_patched = mgr->query_position_matches(start_hash);
    REQUIRE(rows_patched.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(rows_patched->size() == 1);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(rows_patched->front().white_elo == std::optional<std::int16_t> {static_cast<std::int16_t>(patched_white_elo)});
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(rows_patched->front().black_elo == std::optional<std::int16_t> {static_cast<std::int16_t>(patched_black_elo)});

    // A full rebuild from canonical SQLite answers identically.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->rebuild_position_postings().has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto rows_after = mgr->query_position_matches(start_hash);
    REQUIRE(rows_after.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(rows_after->size() == 1);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(rows_after->front().white_elo == std::optional<std::int16_t> {static_cast<std::int16_t>(patched_white_elo)});
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(rows_after->front().black_elo == std::optional<std::int16_t> {static_cast<std::int16_t>(patched_black_elo)});
}

TEST_CASE("database_manager::patch_game_metadata keeps postings usable and reports the new result without a rebuild",
          "[motif-db][database_manager]")
{
    tmp_dir const tdir {"patch_result_sync"};

    auto mgr = motif::db::database_manager::create(tdir.path, "patch-result-db");
    REQUIRE(mgr.has_value());

    chesslib::move e2e4 {};
    e2e4.source_square = chesslib::square::e2;
    e2e4.target_square = chesslib::square::e4;
    e2e4.double_pawn = 1;

    motif::db::game const game {
        .white = {.name = "White Player", .elo = {}, .title = {}, .country = {}},
        .black = {.name = "Black Player", .elo = {}, .title = {}, .country = {}},
        .event_details = {},
        .date = {},
        .result = "1-0",
        .eco = {},
        .moves = {chesslib::codec::encode(e2e4)},
        .extra_tags = {},
        .provenance = {},
    };

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const game_id = mgr->insert_game(game);
    REQUIRE(game_id.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->set_manual_game_provenance(*game_id, {}, "new").has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->rebuild_position_postings().has_value());

    auto const start_hash = motif::db::zobrist_hash {chesslib::board {}.hash()};

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto rows_before = mgr->query_position_matches(start_hash);
    REQUIRE(rows_before.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(rows_before->size() == 1);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(rows_before->front().result == std::int8_t {1});

    auto patch = motif::db::game_patch {};
    patch.result = "0-1";
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->patch_game_metadata(*game_id, patch).has_value());

    // The patch is one atomic operation (canonical row, sequence advance and a new snapshot in a
    // single commit): the position queries stay available and already report the new result.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const patched_status = mgr->derived_index_status();
    REQUIRE(patched_status.has_value());
    CHECK(patched_status->postings_usable);
    CHECK(patched_status->covered_mutation_seq == std::optional<std::uint64_t> {patched_status->mutation_seq});
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(mgr->manifest().postings() != nullptr);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto rows_patched = mgr->query_position_matches(start_hash);
    REQUIRE(rows_patched.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(rows_patched->size() == 1);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(rows_patched->front().result == std::int8_t {-1});

    // A full rebuild from canonical SQLite answers identically.
    REQUIRE(mgr->rebuild_position_postings().has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto rows_rebuilt = mgr->query_position_matches(start_hash);
    REQUIRE(rows_rebuilt.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(rows_rebuilt->size() == 1);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(rows_rebuilt->front().result == std::int8_t {-1});
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear scenario.
TEST_CASE("database_manager::patch_game_metadata partial elo patch updates one rating incrementally and leaves the other unchanged",
          "[motif-db][database_manager]")
{
    tmp_dir const tdir {"patch_elo_partial"};

    auto mgr = motif::db::database_manager::create(tdir.path, "patch-elo-partial-db");
    REQUIRE(mgr.has_value());

    constexpr auto initial_white_elo = std::int32_t {2000};
    constexpr auto initial_black_elo = std::int32_t {1900};
    constexpr auto patched_white_elo = std::int32_t {2100};

    chesslib::move e2e4 {};
    e2e4.source_square = chesslib::square::e2;
    e2e4.target_square = chesslib::square::e4;
    e2e4.double_pawn = 1;

    motif::db::game const game {
        .white = {.name = "White Partial", .elo = initial_white_elo, .title = {}, .country = {}},
        .black = {.name = "Black Partial", .elo = initial_black_elo, .title = {}, .country = {}},
        .event_details = {},
        .date = {},
        .result = "1/2-1/2",
        .eco = {},
        .moves = {chesslib::codec::encode(e2e4)},
        .extra_tags = {},
        .provenance = {},
    };

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const game_id = mgr->insert_game(game);
    REQUIRE(game_id.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->set_manual_game_provenance(*game_id, {}, "new").has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->rebuild_position_postings().has_value());

    // Patch only white elo.
    auto patch = motif::db::game_patch {};
    patch.white_elo = motif::db::rating_patch::set(patched_white_elo);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->patch_game_metadata(*game_id, patch).has_value());

    // Incremental: no rebuild is needed and none happens before the check.
    CHECK(mgr->derived_index_status()->postings_usable);
    auto const start_hash = motif::db::zobrist_hash {chesslib::board {}.hash()};
    auto const check_rows = [&]() -> void
    {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        auto rows = mgr->query_position_matches(start_hash);
        REQUIRE(rows.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(rows->size() == 1);
        // White elo updated; black elo unchanged.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        CHECK(rows->front().white_elo == std::optional<std::int16_t> {static_cast<std::int16_t>(patched_white_elo)});
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        CHECK(rows->front().black_elo == std::optional<std::int16_t> {static_cast<std::int16_t>(initial_black_elo)});
    };
    check_rows();
    // A full rebuild of the edited bundle answers identically.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->rebuild_position_postings().has_value());
    check_rows();
}

// ── Durable game-ID allocator and v5 -> v6 migration ─────────────────────────

TEST_CASE("database_manager: removing the highest game never lets a later insert reuse its id, even across reopen",
          "[motif-db][database_manager][allocator]")
{
    tmp_dir const tdir {"id_no_reuse"};

    auto removed_id = motif::db::game_id {};
    auto third_id = motif::db::game_id {};
    {
        auto mgr = motif::db::database_manager::create(tdir.path, "no-reuse");
        REQUIRE(mgr.has_value());
        // NOLINTBEGIN(bugprone-unchecked-optional-access)
        auto const first = mgr->insert_game(make_one_move_game("Alpha", "Beta"));
        auto const second = mgr->insert_game(make_one_move_game("Gamma", "Delta"));
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        removed_id = *second;
        REQUIRE(mgr->remove_game(removed_id).has_value());

        auto const third = mgr->insert_game(make_one_move_game("Epsilon", "Zeta"));
        REQUIRE(third.has_value());
        CHECK(third->value > removed_id.value);
        third_id = *third;
        // NOLINTEND(bugprone-unchecked-optional-access)
    }

    auto reopened = motif::db::database_manager::open(tdir.path);
    REQUIRE(reopened.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    REQUIRE(reopened->remove_game(third_id).has_value());
    auto const fourth = reopened->insert_game(make_one_move_game("Eta", "Theta"));
    REQUIRE(fourth.has_value());
    CHECK(fourth->value > third_id.value);
    // NOLINTEND(bugprone-unchecked-optional-access)
}

TEST_CASE("database_manager::open migrates a v5 bundle in place", "[motif-db][database_manager][migration]")
{
    tmp_dir const tdir {"open_v5"};

    auto last_id = motif::db::game_id {};
    {
        auto mgr = motif::db::database_manager::create(tdir.path, "v5-db");
        REQUIRE(mgr.has_value());
        // NOLINTBEGIN(bugprone-unchecked-optional-access)
        REQUIRE(mgr->insert_game(make_one_move_game("Alpha", "Beta")).has_value());
        auto const second = mgr->insert_game(make_one_move_game("Gamma", "Delta"));
        REQUIRE(second.has_value());
        last_id = *second;
        // NOLINTEND(bugprone-unchecked-optional-access)
    }

    // Turn the bundle back into a v5 one: no allocator table, no
    // content_revision column, user_version 5.
    {
        sqlite3* raw_conn = nullptr;
        REQUIRE(sqlite3_open((tdir.path / "games.db").c_str(), &raw_conn) == SQLITE_OK);
        REQUIRE(
            sqlite3_exec(
                raw_conn,
                "DROP TABLE mutation_state;" "DROP TABLE published_snapshot;" "DROP TABLE bundle_meta;" "ALTER TABLE game " "DROP " "COLUMN" " " "co" "n" "ten" "t_" "revision;" "PRA" "GMA" " " "user_version " "= 5;",
                nullptr,
                nullptr,
                nullptr)
            == SQLITE_OK);
        sqlite3_close(raw_conn);
    }

    auto opened = motif::db::database_manager::open(tdir.path);
    REQUIRE(opened.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    CHECK(opened->manifest().schema_version == motif::db::schema::current_version);
    auto const count = opened->store().count_games();
    REQUIRE(count.has_value());
    CHECK(*count == 2);

    auto const next = opened->insert_game(make_one_move_game("Epsilon", "Zeta"));
    REQUIRE(next.has_value());
    CHECK(next->value > last_id.value);
    // NOLINTEND(bugprone-unchecked-optional-access)
    opened->close();

    sqlite3* check_conn = nullptr;
    REQUIRE(sqlite3_open((tdir.path / "games.db").c_str(), &check_conn) == SQLITE_OK);
    auto const ver = motif::db::schema::version(check_conn);
    sqlite3_close(check_conn);
    REQUIRE(ver.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(*ver == motif::db::schema::current_version);
}

// ── One allocator per connection; migration/manifest ordering ───────────────

namespace
{

// Rewrites a freshly created bundle's SQLite file into the v5 shape: no
// allocator table, no content_revision column, user_version 5. The manifest is
// left alone.
void downgrade_bundle_to_v5(std::filesystem::path const& dir)
{
    sqlite3* raw_conn = nullptr;
    REQUIRE(sqlite3_open((dir / "games.db").c_str(), &raw_conn) == SQLITE_OK);
    REQUIRE(
        sqlite3_exec(
            raw_conn,
            "DROP TABLE mutation_state;" "DROP TABLE published_snapshot;" "DROP TABLE bundle_meta;" "ALTER TABLE game DROP " "COLUMN " "con" "ten" "t_" "revision;" "PRA" "GMA" " " "user_version = 5;",
            nullptr,
            nullptr,
            nullptr)
        == SQLITE_OK);
    sqlite3_close(raw_conn);
}

[[nodiscard]] auto raw_scalar(std::filesystem::path const& dir, char const* sql) -> std::int64_t
{
    sqlite3* raw_conn = nullptr;
    REQUIRE(sqlite3_open((dir / "games.db").c_str(), &raw_conn) == SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(raw_conn, sql, -1, &stmt, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    auto const value = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    sqlite3_close(raw_conn);
    return value;
}

void set_manifest_schema_version(std::filesystem::path const& dir, std::uint32_t const version)
{
    auto manifest = motif::db::read_manifest(dir / "manifest.json");
    REQUIRE(manifest.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    manifest->schema_version = version;
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(motif::db::write_manifest(dir / "manifest.json", *manifest).has_value());
}

}  // namespace

TEST_CASE("database_manager: an insert_game made inside the writer's open batch shares its allocator",
          "[motif-db][database_manager][allocator]")
{
    tmp_dir const tdir {"shared_allocator"};

    auto mgr = motif::db::database_manager::create(tdir.path, "shared-allocator");
    REQUIRE(mgr.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    auto& writer = mgr->writer();

    REQUIRE(writer.begin_transaction().has_value());
    auto const batch_first = writer.insert(make_one_move_game("Alpha", "Beta"));
    // A second route to the same connection, while the batch is open. With a
    // private writer behind insert_game() this reloaded the not-yet-persisted
    // counter, handed out the batch's id again, and failed on the primary key.
    auto const via_manager = mgr->insert_game(make_one_move_game("Gamma", "Delta"));
    auto const batch_second = writer.insert(make_one_move_game("Epsilon", "Zeta"));
    REQUIRE(batch_first.has_value());
    REQUIRE(via_manager.has_value());
    REQUIRE(batch_second.has_value());
    CHECK(via_manager->value == batch_first->value + 1U);
    CHECK(batch_second->value == via_manager->value + 1U);

    // The manager's insert joined the batch instead of committing it or
    // writing the counter on its own: a second connection sees neither rows
    // nor a moved counter, and rolling back discards all three games.
    CHECK(raw_scalar(tdir.path, "SELECT COUNT(*) FROM game") == 0);
    CHECK(raw_scalar(tdir.path, "SELECT next_game_id FROM bundle_meta WHERE id = 1") == 1);
    writer.rollback_transaction();
    auto const count = mgr->store().count_games();
    REQUIRE(count.has_value());
    CHECK(*count == 0);

    // Both routes keep working afterwards, without colliding.
    auto const after_via_manager = mgr->insert_game(make_one_move_game("Eta", "Theta"));
    auto const after_via_writer = writer.insert(make_one_move_game("Iota", "Kappa"));
    REQUIRE(after_via_manager.has_value());
    REQUIRE(after_via_writer.has_value());
    CHECK(after_via_writer->value == after_via_manager->value + 1U);
    // NOLINTEND(bugprone-unchecked-optional-access)
}

TEST_CASE("database_manager: a batch with a mid-batch manager insert persists one counter covering every row on commit",
          "[motif-db][database_manager][allocator]")
{
    tmp_dir const tdir {"shared_allocator_commit"};

    std::uint32_t highest = 0;
    {
        auto mgr = motif::db::database_manager::create(tdir.path, "shared-allocator-commit");
        REQUIRE(mgr.has_value());
        // NOLINTBEGIN(bugprone-unchecked-optional-access)
        auto& writer = mgr->writer();
        REQUIRE(writer.begin_transaction().has_value());
        REQUIRE(writer.insert(make_one_move_game("Alpha", "Beta")).has_value());
        auto const via_manager = mgr->insert_game(make_one_move_game("Gamma", "Delta"));
        REQUIRE(via_manager.has_value());
        auto const last = writer.insert(make_one_move_game("Epsilon", "Zeta"));
        REQUIRE(last.has_value());
        highest = last->value;
        REQUIRE(writer.commit_transaction().has_value());

        // Standalone inserts after the batch continue above it.
        auto const after = mgr->insert_game(make_one_move_game("Eta", "Theta"));
        REQUIRE(after.has_value());
        CHECK(after->value == highest + 1U);
        highest = after->value;
        // NOLINTEND(bugprone-unchecked-optional-access)
    }

    CHECK(raw_scalar(tdir.path, "SELECT COUNT(*) FROM game") == 4);
    CHECK(raw_scalar(tdir.path, "SELECT COUNT(DISTINCT id) FROM game") == 4);
    CHECK(raw_scalar(tdir.path, "SELECT next_game_id FROM bundle_meta WHERE id = 1") == static_cast<std::int64_t>(highest) + 1);
}

TEST_CASE("database_manager::open rejects a manifest newer than this build before migrating SQLite",
          "[motif-db][database_manager][migration]")
{
    tmp_dir const tdir {"future_manifest"};

    {
        auto mgr = motif::db::database_manager::create(tdir.path, "future-manifest");
        REQUIRE(mgr.has_value());
    }
    downgrade_bundle_to_v5(tdir.path);
    set_manifest_schema_version(tdir.path, motif::db::schema::current_version + 1U);

    auto const opened = motif::db::database_manager::open(tdir.path);
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error() == motif::db::error_code::schema_mismatch);

    // SQLite was not migrated: still v5, no allocator table, no new column.
    CHECK(raw_scalar(tdir.path, "PRAGMA user_version") == 5);
    CHECK(raw_scalar(tdir.path, "SELECT COUNT(*) FROM sqlite_master WHERE name = 'bundle_meta'") == 0);
    CHECK(raw_scalar(tdir.path, "SELECT COUNT(*) FROM pragma_table_info('game') WHERE name = 'content_revision'") == 0);

    // And the manifest is exactly as the newer build left it. (read_manifest()
    // would refuse it as newer than supported, so use the lenient legacy reader.)
    auto const manifest = motif::db::read_legacy_manifest(tdir.path / "manifest.json");
    REQUIRE(manifest.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(manifest->schema_version == motif::db::schema::current_version + 1U);
}

TEST_CASE("database_manager::open raises a lower manifest schema_version only after a successful migration",
          "[motif-db][database_manager][migration]")
{
    tmp_dir const tdir {"lower_manifest"};

    {
        auto mgr = motif::db::database_manager::create(tdir.path, "lower-manifest");
        REQUIRE(mgr.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->insert_game(make_one_move_game("Alpha", "Beta")).has_value());
    }
    constexpr auto downgraded_version = 5U;
    downgrade_bundle_to_v5(tdir.path);
    set_manifest_schema_version(tdir.path, downgraded_version);

    {
        auto opened = motif::db::database_manager::open(tdir.path);
        REQUIRE(opened.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        CHECK(opened->manifest().schema_version == motif::db::schema::current_version);
    }
    CHECK(raw_scalar(tdir.path, "PRAGMA user_version") == motif::db::schema::current_version);
    auto const manifest = motif::db::read_manifest(tdir.path / "manifest.json");
    REQUIRE(manifest.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(manifest->schema_version == motif::db::schema::current_version);

    // A failed migration leaves the lower manifest version alone as well.
    tmp_dir const bad_dir {"lower_manifest_bad_id"};
    {
        auto mgr = motif::db::database_manager::create(bad_dir.path, "lower-manifest-bad");
        REQUIRE(mgr.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->insert_game(make_one_move_game("Alpha", "Beta")).has_value());
    }
    downgrade_bundle_to_v5(bad_dir.path);
    set_manifest_schema_version(bad_dir.path, downgraded_version);
    {
        sqlite3* raw_conn = nullptr;
        REQUIRE(sqlite3_open((bad_dir.path / "games.db").c_str(), &raw_conn) == SQLITE_OK);
        REQUIRE(sqlite3_exec(raw_conn, "UPDATE game SET id = 0;", nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(raw_conn);
    }
    auto const rejected = motif::db::database_manager::open(bad_dir.path);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error() == motif::db::error_code::schema_mismatch);
    CHECK(raw_scalar(bad_dir.path, "PRAGMA user_version") == 5);
    auto const bad_manifest = motif::db::read_manifest(bad_dir.path / "manifest.json");
    REQUIRE(bad_manifest.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(bad_manifest->schema_version == 5U);
}

TEST_CASE("database_manager::open of a migrated bundle ends with foreign_keys enforced", "[motif-db][database_manager][migration]")
{
    tmp_dir const tdir {"migrated_fk"};

    {
        auto mgr = motif::db::database_manager::create(tdir.path, "migrated-fk");
        REQUIRE(mgr.has_value());
    }
    downgrade_bundle_to_v5(tdir.path);

    {
        auto opened = motif::db::database_manager::open(tdir.path);
        REQUIRE(opened.has_value());
        // NOLINTBEGIN(bugprone-unchecked-optional-access)
        auto tagged = make_one_move_game("Alpha", "Beta");
        tagged.extra_tags = {{"Round", "7"}};
        auto const inserted = opened->insert_game(tagged);
        REQUIRE(inserted.has_value());
        REQUIRE(opened->remove_game(*inserted).has_value());
        // NOLINTEND(bugprone-unchecked-optional-access)
    }

    // game_tag rows are removed by ON DELETE CASCADE, which SQLite applies only
    // while foreign_keys is ON for the deleting connection.
    CHECK(raw_scalar(tdir.path, "SELECT COUNT(*) FROM game") == 0);
    CHECK(raw_scalar(tdir.path, "SELECT COUNT(*) FROM game_tag") == 0);
}
