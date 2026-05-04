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

    std::uint32_t game_id {};
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

    std::uint32_t game_id {};
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

    constexpr std::uint32_t absent_id = 99999U;
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const res = mgr->remove_game(absent_id);
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error() == motif::db::error_code::not_found);
}

// ── manifest: game_count and dirty flag
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

    auto const start_hash = chesslib::board {}.hash();

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

    auto const start_hash = chesslib::board {}.hash();
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
