#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "motif/db/database_manager.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chesslib/board/board.hpp>
#include <chesslib/board/move_codec.hpp>
#include <chesslib/core/types.hpp>
#include <duckdb.h>
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

constexpr auto select_position_hashes_sql = R"sql(
    SELECT zobrist_hash
    FROM position
)sql";

struct duckdb_handle_guard
{
    duckdb_database db {};
    duckdb_connection con {};
    duckdb_result res {};

    duckdb_handle_guard() = default;
    duckdb_handle_guard(duckdb_handle_guard const&) = delete;
    auto operator=(duckdb_handle_guard const&) -> duckdb_handle_guard& = delete;
    duckdb_handle_guard(duckdb_handle_guard&&) = delete;
    auto operator=(duckdb_handle_guard&&) -> duckdb_handle_guard& = delete;

    ~duckdb_handle_guard()
    {
        duckdb_destroy_result(&res);
        duckdb_disconnect(&con);
        duckdb_close(&db);
    }
};

auto read_position_hashes(std::filesystem::path const& duckdb_path) -> std::vector<std::uint64_t>
{
    auto handles = duckdb_handle_guard {};
    REQUIRE(duckdb_open(duckdb_path.c_str(), &handles.db) == DuckDBSuccess);
    REQUIRE(duckdb_connect(handles.db, &handles.con) == DuckDBSuccess);
    REQUIRE(duckdb_query(handles.con, select_position_hashes_sql, &handles.res) == DuckDBSuccess);

    auto const row_count = duckdb_row_count(&handles.res);
    std::vector<std::uint64_t> hashes;
    hashes.reserve(static_cast<std::size_t>(row_count));
    for (idx_t row_idx = 0; row_idx < row_count; ++row_idx) {
        hashes.push_back(duckdb_value_uint64(&handles.res, 0, row_idx));
    }

    return hashes;
}

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

auto count_position_rows(std::filesystem::path const& duckdb_path) -> std::int64_t
{  // NOLINT(llvm-prefer-static-over-anonymous-namespace)
    auto handles = duckdb_handle_guard {};
    if (duckdb_open(duckdb_path.c_str(), &handles.db) != DuckDBSuccess) {
        return -1;
    }
    if (duckdb_connect(handles.db, &handles.con) != DuckDBSuccess) {
        return -1;
    }
    if (duckdb_query(handles.con, "SELECT COUNT(*) FROM position", &handles.res) != DuckDBSuccess) {
        return -1;
    }
    return duckdb_value_int64(&handles.res, 0, 0);
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

// ── DuckDB: positions.duckdb
// ──────────────────────────────────────────────────

TEST_CASE("database_manager::create produces positions.duckdb in bundle dir", "[motif-db][database_manager][duckdb]")
{
    tmp_dir const tdir {"duckdb_create"};

    auto res = motif::db::database_manager::create(tdir.path, "duck-db");
    REQUIRE(res.has_value());

    CHECK(std::filesystem::exists(tdir.path / "positions.duckdb"));
}

TEST_CASE("database_manager::rebuild_position_store on empty DB returns 0 rows", "[motif-db][database_manager][duckdb]")
{
    tmp_dir const tdir {"duckdb_rebuild_empty"};

    auto mgr = motif::db::database_manager::create(tdir.path, "empty-db");
    REQUIRE(mgr.has_value());

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto rebuild_res = mgr->rebuild_position_store();
    REQUIRE(rebuild_res.has_value());

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto count = mgr->positions().row_count();
    REQUIRE(count.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(*count == 0);
}

TEST_CASE("database_manager::rebuild_position_store after N-move game returns N rows", "[motif-db][database_manager][duckdb]")
{
    tmp_dir const tdir {"duckdb_rebuild_nmove"};

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

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto rebuild_res = mgr->rebuild_position_store();
    REQUIRE(rebuild_res.has_value());

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto count = mgr->positions().row_count();
    REQUIRE(count.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(*count == std::ssize(moves) + 1);
}

TEST_CASE("database_manager::rebuild_position_store is idempotent", "[motif-db][database_manager][duckdb]")
{
    tmp_dir const tdir {"duckdb_rebuild_idem"};

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

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->rebuild_position_store().has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->rebuild_position_store().has_value());

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto count = mgr->positions().row_count();
    REQUIRE(count.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(*count == 2);
}

TEST_CASE("database_manager::rebuild_position_store rejects out-of-range elo", "[motif-db][database_manager][duckdb]")
{
    tmp_dir const tdir {"duckdb_rebuild_elo_range"};

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
    auto rebuild_res = mgr->rebuild_position_store();
    REQUIRE_FALSE(rebuild_res.has_value());
    CHECK(rebuild_res.error() == motif::db::error_code::io_failure);
}

TEST_CASE("database_manager::rebuild_position_store defaults to sorted-by-zobrist", "[motif-db][database_manager][duckdb]")
{
    tmp_dir const tdir {"duckdb_rebuild_sorted_default"};

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

    auto mgr = motif::db::database_manager::create(tdir.path, "sorted-default-db");
    REQUIRE(mgr.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->store().insert(test_game_a).has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->store().insert(test_game_b).has_value());

    // Default call — should sort by zobrist (new default behavior)
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto rebuild_res = mgr->rebuild_position_store();
    REQUIRE(rebuild_res.has_value());

    auto hashes = read_position_hashes(tdir.path / "positions.duckdb");
    REQUIRE(hashes.size() == 4);
    CHECK(std::ranges::is_sorted(hashes));
}

// ── remove_game
// ───────────────────────────────────────────────────────────────

TEST_CASE("database_manager::remove_game deletes both SQLite row and DuckDB positions", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"remove_game"};

    auto mgr = motif::db::database_manager::create(tdir.path, "remove-db");
    REQUIRE(mgr.has_value());

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const gid = mgr->store().insert(make_one_move_game("White", "Black"));
    REQUIRE(gid.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->rebuild_position_store().has_value());

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->positions().row_count().value_or(-1) > 0);

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->remove_game(*gid).has_value());

    // Game gone from SQLite.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const get_res = mgr->store().get(*gid);
    REQUIRE_FALSE(get_res.has_value());
    CHECK(get_res.error() == motif::db::error_code::not_found);

    // Position rows gone from DuckDB.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(mgr->positions().row_count().value_or(-1) == 0);
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

TEST_CASE("database_manager::remove_user_game rejects imported games without changing positions", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"remove_imported_game"};

    auto mgr = motif::db::database_manager::create(tdir.path, "remove-imported-db");
    REQUIRE(mgr.has_value());

    auto const gid = mgr->store().insert(make_one_move_game("White", "Black"));
    REQUIRE(gid.has_value());
    REQUIRE(mgr->rebuild_position_store().has_value());
    auto const position_count = mgr->positions().row_count();
    REQUIRE(position_count.has_value());

    auto const remove_result = mgr->remove_user_game(*gid);
    REQUIRE_FALSE(remove_result.has_value());
    CHECK(remove_result.error() == motif::db::error_code::not_editable);
    CHECK(mgr->store().get(*gid).has_value());
    auto const position_count_after = mgr->positions().row_count();
    REQUIRE(position_count_after.has_value());
    CHECK(*position_count_after == *position_count);
}

TEST_CASE("database_manager::remove_user_game deletes a manual game from both stores", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"remove_manual_game"};

    auto mgr = motif::db::database_manager::create(tdir.path, "remove-manual-db");
    REQUIRE(mgr.has_value());

    auto const gid = mgr->store().insert(make_one_move_game("White", "Black"));
    REQUIRE(gid.has_value());
    REQUIRE(mgr->store().set_manual_provenance(*gid, std::nullopt, "pending").has_value());
    REQUIRE(mgr->rebuild_position_store().has_value());
    REQUIRE(mgr->remove_user_game(*gid).has_value());

    auto const game = mgr->store().get(*gid);
    REQUIRE_FALSE(game.has_value());
    CHECK(game.error() == motif::db::error_code::not_found);
    CHECK(mgr->positions().row_count().value_or(-1) == 0);
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
    REQUIRE(mgr->rebuild_position_store().has_value());

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

// Regression test for a game that revisits the queried position many times
// (e.g. via repetition/transposition): under a row-limited query, that
// game's rows alone could exhaust a small limit and starve later games out
// of ply data entirely, even though those games are within the
// limit-bounded distinct-game selection. find_games_by_position() must
// return exactly one (lowest) ply per game and must never drop a game
// that's within its own bound.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("database_manager::find_games_by_position never fabricates a ply for a game crowded out by repetition",
          "[motif-db][database_manager]")
{
    tmp_dir const tdir {"find_games_by_position_repetition"};

    auto mgr = motif::db::database_manager::create(tdir.path, "repetition-db");
    REQUIRE(mgr.has_value());

    auto const repeated_id = mgr->store().insert(make_one_move_game("Repeated White", "Repeated Black"));
    REQUIRE(repeated_id.has_value());
    auto const second_id = mgr->store().insert(make_one_move_game("Second White", "Second Black"));
    REQUIRE(second_id.has_value());
    auto const third_id = mgr->store().insert(make_one_move_game("Third White", "Third Black"));
    REQUIRE(third_id.has_value());
    REQUIRE(mgr->rebuild_position_store().has_value());

    // A made-up hash standing in for a position reached at various plies;
    // its actual chess legality is irrelevant to this query-boundary test.
    constexpr auto target_hash = motif::db::zobrist_hash {0xABCDEF0123456789ULL};
    constexpr std::uint16_t repeated_low_ply = 6;
    constexpr std::uint16_t repeated_high_ply = 10;
    constexpr std::uint16_t other_ply = 2;

    std::vector<motif::db::position_row> extra_rows {
        // repeated_id revisits target_hash twice; its lowest ply is 6, not 2 --
        // if repetition rows starved the row budget under a naive row limit,
        // a game with a real ply of 2 could be replaced by fabricated ply 0.
        {.zobrist_hash = target_hash,
         .game_id = *repeated_id,
         .ply = repeated_low_ply,
         .encoded_move = 1,
         .result = 1,
         .white_elo = std::nullopt,
         .black_elo = std::nullopt},
        {.zobrist_hash = target_hash,
         .game_id = *repeated_id,
         .ply = repeated_high_ply,
         .encoded_move = 1,
         .result = 1,
         .white_elo = std::nullopt,
         .black_elo = std::nullopt},
        {.zobrist_hash = target_hash,
         .game_id = *second_id,
         .ply = other_ply,
         .encoded_move = 1,
         .result = 1,
         .white_elo = std::nullopt,
         .black_elo = std::nullopt},
        {.zobrist_hash = target_hash,
         .game_id = *third_id,
         .ply = other_ply,
         .encoded_move = 1,
         .result = 1,
         .white_elo = std::nullopt,
         .black_elo = std::nullopt},
    };
    REQUIRE(mgr->positions().insert_batch(extra_rows).has_value());

    // Bound to 2 distinct games: repeated_id and second_id (lowest two game
    // ids), excluding third_id.
    auto const combined = mgr->find_games_by_position(target_hash, 2);
    REQUIRE(combined.has_value());
    auto const& [games_res, ply_matches] = *combined;

    CHECK(games_res.games.size() == 2);
    CHECK(games_res.total_count == 3);  // true total distinct games matching, unbounded by limit

    REQUIRE(ply_matches.size() == 2);
    auto find_ply = [&ply_matches](motif::db::game_id const game_key) -> std::optional<std::uint16_t>
    {
        for (auto const& match : ply_matches) {
            if (match.game_id == game_key) {
                return match.ply;
            }
        }
        return std::nullopt;
    };
    auto const repeated_ply = find_ply(*repeated_id);
    REQUIRE(repeated_ply.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- checked above
    CHECK(*repeated_ply == repeated_low_ply);  // lowest of its two repeated visits

    auto const second_ply = find_ply(*second_id);
    REQUIRE(second_ply.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- checked above
    CHECK(*second_ply == other_ply);  // never fabricated as 0
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
    REQUIRE(mgr->rebuild_position_store().has_value());

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
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK_FALSE(manifest_after->position_index_dirty);
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

TEST_CASE("database_manager::open rebuilds position store when dirty flag is set", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"manifest_rebuild_dirty"};
    auto const duckdb_path = tdir.path / "positions.duckdb";

    {
        auto mgr = motif::db::database_manager::create(tdir.path, "rebuild-dirty-db");
        REQUIRE(mgr.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->store().insert(make_one_move_game("W", "B")).has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->rebuild_position_store().has_value());
    }

    // Simulate a crash: manually force dirty=true into the manifest without
    // touching the DuckDB file (simulates unclean shutdown).
    {
        auto manifest_dirty = motif::db::read_manifest(tdir.path / "manifest.json");
        REQUIRE(manifest_dirty.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        manifest_dirty->position_index_dirty = true;
        REQUIRE(motif::db::write_manifest(tdir.path / "manifest.json", *manifest_dirty).has_value());
    }

    auto const rows_before = count_position_rows(duckdb_path);
    REQUIRE(rows_before > 0);

    // Re-open: should detect dirty flag and rebuild (same rows expected since
    // the game store is unchanged).
    auto reopened = motif::db::database_manager::open(tdir.path);
    REQUIRE(reopened.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const rows_after = reopened->positions().row_count();
    REQUIRE(rows_after.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(*rows_after == rows_before);
}

TEST_CASE("database_manager::open repairs an interrupted raw ingest before rebuilding positions", "[motif-db][database_manager]")
{
    tmp_dir const tdir {"open_repairs_raw_ingest"};
    auto const duckdb_path = tdir.path / "positions.duckdb";

    auto const dup_game = make_one_move_game("Raw White", "Raw Black");
    auto const other_game = make_one_move_game("Other White", "Other Black");

    {
        auto mgr = motif::db::database_manager::create(tdir.path, "raw-ingest-db");
        REQUIRE(mgr.has_value());

        // Simulate a raw bulk-ingest window interrupted before deduplicate()
        // recreated game_identity_lookup: drop the index, insert exact
        // duplicate rows (as insert_raw() would during a fresh import) plus
        // one distinct game, and close without ever calling deduplicate().
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->writer().drop_identity_index().has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->writer().insert_raw(dup_game).has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->writer().insert_raw(dup_game).has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE(mgr->writer().insert_raw(other_game).has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        auto const count_before_close = mgr->store().count_games();
        REQUIRE(count_before_close.has_value());
        CHECK(*count_before_close == 3);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        REQUIRE_FALSE(*mgr->writer().identity_index_exists());
    }  // close() -- identity index is still absent on disk here.

    // Re-open: open() must detect the missing identity index, deduplicate
    // (removing the exact-duplicate raw row) before exposing reads, and
    // rebuild the DuckDB position store from the now-canonical game set.
    auto reopened = motif::db::database_manager::open(tdir.path);
    REQUIRE(reopened.has_value());

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(reopened->writer().identity_index_exists().has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(*reopened->writer().identity_index_exists());

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const count_after = reopened->store().count_games();
    REQUIRE(count_after.has_value());
    CHECK(*count_after == 2);  // one duplicate pair collapsed to one survivor

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const games = reopened->find_games(motif::db::search_filter {});
    REQUIRE(games.has_value());
    CHECK(games->games.size() == 2);

    // Rebuilt position rows reflect only the two canonical (deduplicated)
    // games: one move each, one starting-position row each -> 4 rows total.
    auto const rows_after = count_position_rows(duckdb_path);
    CHECK(rows_after == 4);
}

// ── patch_game_metadata
// ──────────────────────────────────────────────────────────

TEST_CASE("database_manager::patch_game_metadata syncs elo to DuckDB position rows", "[motif-db][database_manager]")
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
    REQUIRE(mgr->rebuild_position_store().has_value());

    auto const start_hash = motif::db::zobrist_hash {chesslib::board {}.hash()};

    // Verify initial elos are in DuckDB.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto rows_before = mgr->positions().query_by_zobrist(start_hash);
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

    // DuckDB position rows must reflect the new elos without a rebuild.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto rows_after = mgr->positions().query_by_zobrist(start_hash);
    REQUIRE(rows_after.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(rows_after->size() == 1);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(rows_after->front().white_elo == std::optional<std::int16_t> {static_cast<std::int16_t>(patched_white_elo)});
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(rows_after->front().black_elo == std::optional<std::int16_t> {static_cast<std::int16_t>(patched_black_elo)});
}

TEST_CASE("database_manager::patch_game_metadata syncs result to DuckDB, surviving a rebuild", "[motif-db][database_manager]")
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
    REQUIRE(mgr->rebuild_position_store().has_value());

    auto const start_hash = motif::db::zobrist_hash {chesslib::board {}.hash()};

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto rows_before = mgr->positions().query_by_zobrist(start_hash);
    REQUIRE(rows_before.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(rows_before->size() == 1);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(rows_before->front().result == std::int8_t {1});

    auto patch = motif::db::game_patch {};
    patch.result = "0-1";
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->patch_game_metadata(*game_id, patch).has_value());

    // Reflected immediately, without a rebuild.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto rows_after = mgr->positions().query_by_zobrist(start_hash);
    REQUIRE(rows_after.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(rows_after->size() == 1);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(rows_after->front().result == std::int8_t {-1});

    // A full rebuild must not resurrect the pre-patch value: game_result's
    // insert path uses ON CONFLICT DO NOTHING, so it only stays correct if
    // rebuild drops game_result along with position before replaying.
    REQUIRE(mgr->rebuild_position_store().has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto rows_rebuilt = mgr->positions().query_by_zobrist(start_hash);
    REQUIRE(rows_rebuilt.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(rows_rebuilt->size() == 1);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(rows_rebuilt->front().result == std::int8_t {-1});
}

TEST_CASE("database_manager::patch_game_metadata partial elo patch leaves other column unchanged", "[motif-db][database_manager]")
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
    REQUIRE(mgr->rebuild_position_store().has_value());

    // Patch only white elo.
    auto patch = motif::db::game_patch {};
    patch.white_elo = patched_white_elo;
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(mgr->patch_game_metadata(*game_id, patch).has_value());

    auto const start_hash = motif::db::zobrist_hash {chesslib::board {}.hash()};
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto rows = mgr->positions().query_by_zobrist(start_hash);
    REQUIRE(rows.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(rows->size() == 1);
    // White elo updated; black elo unchanged.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(rows->front().white_elo == std::optional<std::int16_t> {static_cast<std::int16_t>(patched_white_elo)});
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(rows->front().black_elo == std::optional<std::int16_t> {static_cast<std::int16_t>(initial_black_elo)});
}
