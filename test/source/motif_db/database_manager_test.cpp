#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

#include "motif/db/error.hpp"
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

TEST_CASE("database_manager::create sets manifest name and schema_version", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"create_mf"};

    auto res = motif::db::database_manager::create(tdir.path, "my-chess-db");
    REQUIRE(res.has_value());

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const& manifest = res->manifest();
    CHECK(manifest.name == "my-chess-db");
    CHECK(manifest.schema_version == motif::db::schema::current_version);
    CHECK(manifest.game_count == 0);
    CHECK_FALSE(manifest.created_at.empty());
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
    CHECK(opened->manifest().name == "open-db");
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
        auto insert_res = mgr->store().insert(inserted_game);
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
        auto insert_res = mgr->store().insert(test_game);
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
    REQUIRE(res->manifest().position_postings.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(res->manifest().position_postings->game_count == 0U);
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
        REQUIRE(mgr->store().insert(make_one_move_game("White", "Black")).has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->rebuild_position_postings().has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->manifest().position_postings.has_value());
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

    // Force a dirty reopen (simulated unclean shutdown) to exercise the
    // recovery decision, not just the ordinary clean-close path.
    {
        auto manifest = motif::db::read_manifest(tdir.path / "manifest.json");
        REQUIRE(manifest.has_value());
        manifest->position_index_dirty = true;  // NOLINT(bugprone-unchecked-optional-access)
        REQUIRE(motif::db::write_manifest(tdir.path / "manifest.json", *manifest).has_value());
    }

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
    REQUIRE(mgr->manifest().position_postings.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(mgr->manifest().position_postings->game_count == 0U);
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
    REQUIRE(mgr->store().insert(test_game).has_value());

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
    REQUIRE(mgr->store().insert(test_game).has_value());

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
    REQUIRE(mgr->store().insert(test_game).has_value());

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
    REQUIRE(mgr->store().insert(test_game_a).has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->store().insert(test_game_b).has_value());

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
    auto const gid = mgr->store().insert(make_one_move_game("White", "Black"));
    REQUIRE(gid.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->rebuild_position_postings().has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->manifest().position_postings.has_value());

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->remove_game(*gid).has_value());

    // Game gone from SQLite.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const get_res = mgr->store().get(*gid);
    REQUIRE_FALSE(get_res.has_value());
    CHECK(get_res.error() == motif::db::error_code::not_found);

    // remove_game() marks postings stale immediately.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK_FALSE(mgr->manifest().position_postings.has_value());

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->rebuild_position_postings().has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(mgr->manifest().position_postings->game_count == 0U);
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

    auto const gid = mgr->store().insert(make_one_move_game("White", "Black"));
    REQUIRE(gid.has_value());
    REQUIRE(mgr->rebuild_position_postings().has_value());
    auto const postings_before = mgr->manifest().position_postings;
    REQUIRE(postings_before.has_value());

    auto const remove_result = mgr->remove_user_game(*gid);
    REQUIRE_FALSE(remove_result.has_value());
    CHECK(remove_result.error() == motif::db::error_code::not_editable);
    CHECK(mgr->store().get(*gid).has_value());
    auto const postings_after = mgr->manifest().position_postings;
    REQUIRE(postings_after.has_value());
    CHECK(postings_after.value_or(motif::db::derived_index_manifest_entry {}).filename
          == postings_before.value_or(motif::db::derived_index_manifest_entry {}).filename);
}

TEST_CASE("database_manager::remove_user_game deletes a manual game and marks postings stale", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"remove_manual_game"};

    auto mgr = motif::db::database_manager::create(tdir.path, "remove-manual-db");
    REQUIRE(mgr.has_value());

    auto const gid = mgr->store().insert(make_one_move_game("White", "Black"));
    REQUIRE(gid.has_value());
    REQUIRE(mgr->store().set_manual_provenance(*gid, std::nullopt, "pending").has_value());
    REQUIRE(mgr->rebuild_position_postings().has_value());
    REQUIRE(mgr->remove_user_game(*gid).has_value());

    auto const game = mgr->store().get(*gid);
    REQUIRE_FALSE(game.has_value());
    CHECK(game.error() == motif::db::error_code::not_found);
    CHECK_FALSE(mgr->manifest().position_postings.has_value());

    REQUIRE(mgr->rebuild_position_postings().has_value());
    CHECK(mgr->manifest().position_postings.value_or(motif::db::derived_index_manifest_entry {}).game_count == 0U);
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

    auto const matching_id = mgr->store().insert(matching_game);
    REQUIRE(matching_id.has_value());
    REQUIRE(mgr->store().insert(wrong_player).has_value());
    REQUIRE(mgr->store().insert(wrong_position).has_value());
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
        REQUIRE(mgr->store().insert(game_rec).has_value());
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

TEST_CASE("database_manager::close persists game_count in manifest", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"manifest_count"};

    {
        auto mgr = motif::db::database_manager::create(tdir.path, "count-db");
        REQUIRE(mgr.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->store().insert(make_one_move_game("A", "B")).has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->store().insert(make_one_move_game("C", "D")).has_value());
    }  // close() called here

    auto const manifest_after = motif::db::read_manifest(tdir.path / "manifest.json");
    REQUIRE(manifest_after.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(manifest_after->game_count == 2U);
    // Direct store writes bypass derived-index rebuilding, so close persists
    // the game count but keeps recovery dirty until postings are repaired.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(manifest_after->position_index_dirty);
}

TEST_CASE("database_manager::open marks manifest dirty and close clears it", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"manifest_dirty"};

    {
        auto mgr = motif::db::database_manager::create(tdir.path, "dirty-db");
        REQUIRE(mgr.has_value());
    }

    {
        auto mgr = motif::db::database_manager::open(tdir.path);
        REQUIRE(mgr.has_value());
        // Manifest on disk must be dirty while a session is open.
        auto const mf_open = motif::db::read_manifest(tdir.path / "manifest.json");
        REQUIRE(mf_open.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        CHECK(mf_open->position_index_dirty);
    }  // close() called here

    // After clean close the dirty flag is cleared.
    auto const mf_closed = motif::db::read_manifest(tdir.path / "manifest.json");
    REQUIRE(mf_closed.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK_FALSE(mf_closed->position_index_dirty);
}

TEST_CASE("database_manager::open rebuilds postings automatically when they do not cover canonical games", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"manifest_rebuild_dirty"};

    {
        auto mgr = motif::db::database_manager::create(tdir.path, "rebuild-dirty-db");
        REQUIRE(mgr.has_value());
        // Insert directly through the SQLite store, bypassing
        // rebuild_position_postings(): the bundle now has 1 game but its
        // published postings generation still describes 0.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->store().insert(make_one_move_game("W", "B")).has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        mgr->close();
    }

    // A clean close cannot mark the index clean when postings do not cover
    // canonical games -- confirm the crash-recovery marker really is set,
    // then that the next open() repairs it without any manual nudge.
    {
        auto const manifest_before_reopen = motif::db::read_manifest(tdir.path / "manifest.json");
        REQUIRE(manifest_before_reopen.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        CHECK(manifest_before_reopen->position_index_dirty);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        CHECK(manifest_before_reopen->position_postings.value_or(motif::db::derived_index_manifest_entry {}).game_count == 0U);
    }

    auto reopened = motif::db::database_manager::open(tdir.path);
    REQUIRE(reopened.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(reopened->manifest().position_postings.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(reopened->manifest().position_postings->game_count == 1U);

    auto const start_hash = motif::db::zobrist_hash {chesslib::board {}.hash()};
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const matches = reopened->query_position_matches(start_hash);
    REQUIRE(matches.has_value());
    CHECK(matches->size() == 1U);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    reopened->close();
}

TEST_CASE("database_manager::open migrates a legacy bundle with a leftover positions.duckdb by rebuilding postings from SQLite",
          "[motif-db][database_manager][migration]")
{
    tmp_dir const tdir {"legacy_migrate"};

    {
        auto mgr = motif::db::database_manager::create(tdir.path, "legacy-db");
        REQUIRE(mgr.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->store().insert(make_one_move_game("White", "Black")).has_value());
        // Shape a bundle "from before postings existed": no postings entry
        // in the manifest at all, matching a genuinely legacy bundle.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->prepare_canonical_mutation().has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        CHECK_FALSE(mgr->manifest().position_postings.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        mgr->close();
    }

    // Leave a leftover positions.duckdb from an older release. It does not
    // need to be a valid DuckDB database -- this codebase never reads it.
    {
        std::ofstream marker {tdir.path / "positions.duckdb", std::ios::binary};
        REQUIRE(marker.good());
        marker << "not a real duckdb file";
    }
    auto const duckdb_size_before = std::filesystem::file_size(tdir.path / "positions.duckdb");

    auto reopened = motif::db::database_manager::open(tdir.path);
    REQUIRE(reopened.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(reopened->manifest().position_postings.has_value());
    // The leftover file is never read or deleted by the migration attempt.
    CHECK(std::filesystem::exists(tdir.path / "positions.duckdb"));
    CHECK(std::filesystem::file_size(tdir.path / "positions.duckdb") == duckdb_size_before);

    auto const start_hash = motif::db::zobrist_hash {chesslib::board {}.hash()};
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const summary = reopened->position_summary(start_hash);
    REQUIRE(summary.has_value());
    REQUIRE(summary->has_value());
    CHECK(summary->value_or(motif::db::position_postings_summary {}).distinct_game_count == 1U);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const build_seq_after_migration = reopened->manifest().derived_index_build_seq;
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    reopened->close();

    // A later open() of the now-migrated bundle needs no further rebuild.
    auto reopened_again = motif::db::database_manager::open(tdir.path);
    REQUIRE(reopened_again.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(reopened_again->manifest().derived_index_build_seq == build_seq_after_migration);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    reopened_again->close();
}

TEST_CASE("database_manager::open fails closed and is retryable when postings migration cannot publish",
          "[motif-db][database_manager][migration]")
{
    tmp_dir const tdir {"legacy_migrate_fail"};
    std::uint64_t build_seq_at_close {};

    {
        auto mgr = motif::db::database_manager::create(tdir.path, "legacy-fail-db");
        REQUIRE(mgr.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->store().insert(make_one_move_game("White", "Black")).has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->prepare_canonical_mutation().has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        CHECK_FALSE(mgr->manifest().position_postings.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        build_seq_at_close = mgr->manifest().derived_index_build_seq;
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        mgr->close();
    }

    {
        std::ofstream marker {tdir.path / "positions.duckdb", std::ios::binary};
        REQUIRE(marker.good());
        marker << "not a real duckdb file";
    }
    auto const duckdb_size_before = std::filesystem::file_size(tdir.path / "positions.duckdb");
    auto const manifest_before = motif::db::read_manifest(tdir.path / "manifest.json");
    REQUIRE(manifest_before.has_value());

    // Collide with the exact staging path open()'s migration attempt will
    // pass to position_postings::build(): a non-empty directory can't be
    // removed by rebuild_position_postings()'s leading cleanup nor opened
    // for writing as a regular file, so the build step fails cleanly before
    // writing anything -- games.db, manifest.json, and positions.duckdb are
    // never touched by that call (see rebuild_position_postings()).
    auto const staging_collision = tdir.path / fmt::format("positions.postings.{}.building", build_seq_at_close);
    REQUIRE(std::filesystem::create_directory(staging_collision));
    {
        std::ofstream const blocker {staging_collision / "blocker"};
        REQUIRE(blocker.good());
    }

    auto reopened = motif::db::database_manager::open(tdir.path);
    REQUIRE_FALSE(reopened.has_value());

    CHECK(std::filesystem::file_size(tdir.path / "positions.duckdb") == duckdb_size_before);
    auto const manifest_after = motif::db::read_manifest(tdir.path / "manifest.json");
    REQUIRE(manifest_after.has_value());
    CHECK_FALSE(manifest_after->position_postings.has_value());
    CHECK(manifest_after->derived_index_build_seq == manifest_before->derived_index_build_seq);

    // Retryable: clearing the collision lets a later open() succeed.
    std::filesystem::remove_all(staging_collision);
    auto retried = motif::db::database_manager::open(tdir.path);
    REQUIRE(retried.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(retried->manifest().position_postings.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    retried->close();
}

TEST_CASE("database_manager::open fails closed when postings-only repair cannot publish", "[motif-db][database_manager][position_postings]")
{
    tmp_dir const tdir {"postings_repair_fail"};

    std::uint64_t repair_build_seq {};
    {
        auto mgr = motif::db::database_manager::create(tdir.path, "repair-fail-db");
        REQUIRE(mgr.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->store().insert(make_one_move_game("White", "Black")).has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->rebuild_position_postings().has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        repair_build_seq = mgr->manifest().derived_index_build_seq;
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        mgr->close();
    }

    auto manifest = motif::db::read_manifest(tdir.path / "manifest.json");
    REQUIRE(manifest.has_value());
    REQUIRE(manifest->position_postings.has_value());
    auto const original_postings = manifest->position_postings.value_or(motif::db::derived_index_manifest_entry {});
    std::filesystem::remove(tdir.path / original_postings.filename);

    auto const staging_collision = tdir.path / fmt::format("positions.postings.{}.building", repair_build_seq);
    REQUIRE(std::filesystem::create_directory(staging_collision));
    {
        std::ofstream const blocker {staging_collision / "blocker"};
        REQUIRE(blocker.good());
    }

    auto reopened = motif::db::database_manager::open(tdir.path);
    REQUIRE_FALSE(reopened.has_value());
    CHECK_FALSE(std::filesystem::exists(tdir.path / "positions.duckdb"));

    auto const manifest_after = motif::db::read_manifest(tdir.path / "manifest.json");
    REQUIRE(manifest_after.has_value());
    CHECK(manifest_after->derived_index_build_seq == repair_build_seq);
    REQUIRE(manifest_after->position_postings.has_value());
    auto const persisted_postings = manifest_after->position_postings.value_or(motif::db::derived_index_manifest_entry {});
    CHECK(persisted_postings.filename == original_postings.filename);
    CHECK(persisted_postings.checksum == original_postings.checksum);
}

// ── patch_game_metadata
// ──────────────────────────────────────────────────────────

TEST_CASE("database_manager::patch_game_metadata marks postings stale and a rebuild reflects the new elo", "[motif-db][database_manager]")
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
    auto const game_id = mgr->store().insert(game);
    REQUIRE(game_id.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->store().set_manual_provenance(*game_id, {}, "new").has_value());
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
    patch.white_elo = patched_white_elo;
    patch.black_elo = patched_black_elo;
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->patch_game_metadata(*game_id, patch).has_value());

    // patch_game_metadata() marks postings stale immediately: queries fail
    // closed until an explicit rebuild.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK_FALSE(mgr->manifest().position_postings.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const rows_stale = mgr->query_position_matches(start_hash);
    REQUIRE_FALSE(rows_stale.has_value());

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

TEST_CASE("database_manager::patch_game_metadata marks postings stale and a rebuild reflects the new result",
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
    auto const game_id = mgr->store().insert(game);
    REQUIRE(game_id.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->store().set_manual_provenance(*game_id, {}, "new").has_value());
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

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK_FALSE(mgr->manifest().position_postings.has_value());

    // A rebuild must reflect the new result, not the pre-patch value: the
    // game row itself carries the new result, so a full replay-based rebuild
    // always sees the current value.
    REQUIRE(mgr->rebuild_position_postings().has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto rows_rebuilt = mgr->query_position_matches(start_hash);
    REQUIRE(rows_rebuilt.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(rows_rebuilt->size() == 1);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(rows_rebuilt->front().result == std::int8_t {-1});
}

TEST_CASE("database_manager::patch_game_metadata partial elo patch leaves other column unchanged after rebuild",
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
    auto const game_id = mgr->store().insert(game);
    REQUIRE(game_id.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->store().set_manual_provenance(*game_id, {}, "new").has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->rebuild_position_postings().has_value());

    // Patch only white elo.
    auto patch = motif::db::game_patch {};
    patch.white_elo = patched_white_elo;
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->patch_game_metadata(*game_id, patch).has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->rebuild_position_postings().has_value());

    auto const start_hash = motif::db::zobrist_hash {chesslib::board {}.hash()};
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
}
