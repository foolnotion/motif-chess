#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "motif/import/import_worker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chesslib/board/board.hpp>  // NOLINT(misc-include-cleaner)
#include <chesslib/util/san.hpp>
#include <pgnlib/types.hpp>  // NOLINT(misc-include-cleaner)
#include <sqlite3.h>

#include "motif/db/database_manager.hpp"
#include "motif/db/types.hpp"
#include "motif/import/error.hpp"

namespace
{

struct tmp_dir
{
    std::filesystem::path path;

    explicit tmp_dir(std::string const& suffix)
    {
        auto const base = std::filesystem::temp_directory_path();
        auto const tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = base / ("motif_iw_test_" + suffix + "_" + std::to_string(tick));
        std::filesystem::create_directories(path);
    }

    ~tmp_dir() { std::filesystem::remove_all(path); }

    tmp_dir(tmp_dir const&) = delete;
    auto operator=(tmp_dir const&) -> tmp_dir& = delete;
    tmp_dir(tmp_dir&&) = delete;
    auto operator=(tmp_dir&&) -> tmp_dir& = delete;
};

struct sqlite_deleter
{
    void operator()(sqlite3* database) const noexcept
    {
        if (database != nullptr) {
            sqlite3_close(database);
        }
    }
};

struct sqlite_stmt_deleter
{
    void operator()(sqlite3_stmt* stmt) const noexcept
    {
        if (stmt != nullptr) {
            sqlite3_finalize(stmt);
        }
    }
};

using unique_sqlite = std::unique_ptr<sqlite3, sqlite_deleter>;
using unique_sqlite_stmt = std::unique_ptr<sqlite3_stmt, sqlite_stmt_deleter>;

constexpr auto expected_white_elo = 2400;
constexpr auto expected_black_elo = 2300;

auto open_sqlite(std::filesystem::path const& path) -> unique_sqlite
{
    sqlite3* database {nullptr};
    if (sqlite3_open(path.c_str(), &database) != SQLITE_OK) {
        throw std::runtime_error {"failed to open sqlite database"};
    }
    return unique_sqlite {database};
}

auto count_rows(std::filesystem::path const& path, char const* sql) -> std::int64_t
{
    auto database = open_sqlite(path);
    sqlite3_stmt* stmt_raw {nullptr};
    if (sqlite3_prepare_v2(database.get(), sql, -1, &stmt_raw, nullptr) != SQLITE_OK) {
        throw std::runtime_error {"failed to prepare sqlite query"};
    }
    auto stmt = unique_sqlite_stmt {stmt_raw};
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        throw std::runtime_error {"failed to read sqlite row count"};
    }
    return sqlite3_column_int64(stmt.get(), 0);
}

auto expected_hashes(std::vector<pgn::move_node> const& moves) -> std::vector<motif::db::zobrist_hash>
{
    auto board = chesslib::board {};
    auto hashes = std::vector<motif::db::zobrist_hash> {};
    hashes.reserve(moves.size() + 1);

    // Starting position (ply = 0)
    hashes.push_back(motif::db::zobrist_hash {board.hash()});

    for (auto const& move_node : moves) {
        auto move = chesslib::san::from_string(board, move_node.san);
        if (!move.has_value()) {
            throw std::runtime_error {"failed to parse SAN in test helper"};
        }
        chesslib::move_maker maker {board, *move};
        maker.make();
        hashes.push_back(motif::db::zobrist_hash {board.hash()});
    }

    return hashes;
}

auto require_test(bool condition, std::string_view message) -> void
{
    if (!condition) {
        throw std::runtime_error {std::string {message}};
    }
}

auto check_stored_game(motif::db::game const& stored_game, std::size_t move_count) -> void
{
    require_test(stored_game.white.name == "White Player", "white player name mismatch");
    require_test(stored_game.white.elo == std::optional<std::int32_t> {expected_white_elo}, "white elo mismatch");
    require_test(stored_game.white.title == std::optional<std::string> {"GM"}, "white title mismatch");
    require_test(stored_game.black.name == "Black Player", "black player name mismatch");
    require_test(stored_game.black.elo == std::optional<std::int32_t> {expected_black_elo}, "black elo mismatch");
    require_test(stored_game.black.title == std::optional<std::string> {"IM"}, "black title mismatch");
    if (!stored_game.event_details.has_value()) {
        throw std::runtime_error {"missing stored event details"};
    }

    auto const& event_details = *stored_game.event_details;
    require_test(event_details.name == "Unit Test Open", "event name mismatch");
    require_test(event_details.site == std::optional<std::string> {"Testville"}, "event site mismatch");
    require_test(event_details.date == std::optional<std::string> {"2026.04.19"}, "event date mismatch");
    require_test(stored_game.date == std::optional<std::string> {"2026.04.19"}, "game date mismatch");
    require_test(stored_game.eco == std::optional<std::string> {"C60"}, "eco mismatch");
    require_test(stored_game.result == "1/2-1/2", "result mismatch");
    require_test(stored_game.moves.size() == move_count, "move count mismatch");
    require_test(stored_game.extra_tags.size() == 1, "extra tag count mismatch");
    require_test(stored_game.extra_tags[0] == std::pair<std::string, std::string> {"Round", "1"}, "extra tag mismatch");
}

auto check_stored_positions(motif::db::database_manager const& manager,
                            std::vector<motif::db::zobrist_hash> const& hashes,
                            motif::db::game_id const game_id) -> void
{
    for (std::size_t index = 0; index < hashes.size(); ++index) {
        auto const positions = manager.query_position_matches(hashes[index]);
        require_test(positions.has_value(), "position query failed");
        require_test(positions->size() == 1, "stored position count mismatch");
        auto const& position = positions->front();
        require_test(position.game_id == game_id, "game id mismatch");
        require_test(position.ply == static_cast<std::uint16_t>(index), "ply mismatch");
        require_test(position.result == 0, "result mismatch");
        require_test(position.white_elo == std::optional<std::int16_t> {static_cast<std::int16_t>(expected_white_elo)},
                     "white elo mismatch");
        require_test(position.black_elo == std::optional<std::int16_t> {static_cast<std::int16_t>(expected_black_elo)},
                     "black elo mismatch");
    }
}

auto make_move(int number, std::string san) -> pgn::move_node
{
    return pgn::move_node {
        .number = number,
        .san = std::move(san),
        .comment = {},
        .nags = {},
        .variations = {},
    };
}

auto make_game(std::string white, std::string black, pgn::result result, std::vector<pgn::move_node> moves) -> pgn::game
{
    return pgn::game {
        .tags = {{"White", std::move(white)}, {"Black", std::move(black)}},
        .moves = std::move(moves),
        .result = result,
    };
}

}  // namespace

// ── AC1: valid game is stored with correct positions ─────────────────────────

TEST_CASE("import_worker: valid 5-move game stores metadata and position rows", "[motif-import]")
{
    tmp_dir const tdir {"valid_game"};
    auto mgr = motif::db::database_manager::create(tdir.path, "test").value();  // NOLINT(bugprone-unchecked-optional-access)

    motif::import::import_worker worker {mgr};

    auto moves = std::vector<pgn::move_node> {
        make_move(1, "e4"),
        make_move(1, "e5"),
        make_move(2, "Nf3"),
        make_move(2, "Nc6"),
        make_move(3, "Bb5"),
    };
    auto game = pgn::game {
        .tags = {{"White", "White Player"},
                 {"Black", "Black Player"},
                 {"WhiteElo", "2400"},
                 {"BlackElo", "2300"},
                 {"WhiteTitle", "GM"},
                 {"BlackTitle", "IM"},
                 {"Event", "Unit Test Open"},
                 {"Site", "Testville"},
                 {"Date", "2026.04.19"},
                 {"ECO", "C60"},
                 {"Round", "1"}},
        .moves = moves,
        .result = pgn::result::draw,
    };

    auto res = worker.process(game);
    REQUIRE(res.has_value());  // NOLINT(bugprone-unchecked-optional-access)
    CHECK(res->positions_inserted == 5 + 1);  // NOLINT(bugprone-unchecked-optional-access)

    auto stored_game = mgr.store().get(res->game_id);  // NOLINT(bugprone-unchecked-optional-access)
    REQUIRE(stored_game.has_value());
    check_stored_game(*stored_game, moves.size());

    auto metrics = motif::db::position_postings_build_metrics {};
    REQUIRE(mgr.rebuild_position_postings(&metrics).has_value());
    CHECK(metrics.occurrence_count == 5 + 1);
    auto const game_id = res->game_id;  // NOLINT(bugprone-unchecked-optional-access)
    auto const hashes = expected_hashes(moves);
    check_stored_positions(mgr, hashes, game_id);
    mgr.close();
}

// ── AC2: existing player is reused — no duplicates ───────────────────────────

TEST_CASE("import_worker: second game with same player reuses player row", "[motif-import]")
{
    tmp_dir const tdir {"player_reuse"};
    auto mgr = motif::db::database_manager::create(tdir.path, "test").value();  // NOLINT(bugprone-unchecked-optional-access)

    motif::import::import_worker worker {mgr};

    auto moves1 = std::vector<pgn::move_node> {make_move(1, "e4"), make_move(1, "e5")};
    auto game1 = make_game("Kasparov", "Karpov", pgn::result::white, moves1);

    pgn::game const game2 {
        .tags = {{"White", "Kasparov"}, {"Black", "Karpov"}, {"Date", "2000.01.01"}},
        .moves = {make_move(1, "d4"), make_move(1, "d5")},
        .result = pgn::result::black,
    };

    REQUIRE(worker.process(game1).has_value());
    auto const player_count_after_first = count_rows(tdir.path / "games.db", "SELECT count(*) FROM player");
    CHECK(player_count_after_first == 2);

    REQUIRE(worker.process(game2).has_value());
    auto const player_count_after_second = count_rows(tdir.path / "games.db", "SELECT count(*) FROM player");
    CHECK(player_count_after_second == player_count_after_first);

    // Verify two distinct games are stored
    auto game1_res = mgr.store().get(motif::db::game_id {1});
    auto game2_res = mgr.store().get(motif::db::game_id {2});
    REQUIRE(game1_res.has_value());
    REQUIRE(game2_res.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(game1_res->white.name == "Kasparov");
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(game2_res->white.name == "Kasparov");

    mgr.close();
}

// ── AC3: duplicate game returns error_code::duplicate ────────────────────────

TEST_CASE("import_worker: duplicate game returns error_code::duplicate", "[motif-import]")
{
    tmp_dir const tdir {"duplicate"};
    auto mgr = motif::db::database_manager::create(tdir.path, "test").value();  // NOLINT(bugprone-unchecked-optional-access)

    motif::import::import_worker worker {mgr};

    auto moves = std::vector<pgn::move_node> {make_move(1, "e4"), make_move(1, "e5")};
    auto game = make_game("Player A", "Player B", pgn::result::draw, moves);

    REQUIRE(worker.process(game).has_value());

    auto dup_res = worker.process(game);
    REQUIRE_FALSE(dup_res.has_value());
    CHECK(dup_res.error() == motif::import::error_code::duplicate);

    // No additional game row was inserted
    auto missing = mgr.store().get(motif::db::game_id {2});
    CHECK_FALSE(missing.has_value());

    mgr.close();
}

// ── AC4: %clk comment is silently ignored ────────────────────────────────────

TEST_CASE("import_worker: %clk annotation is silently ignored", "[motif-import]")
{
    tmp_dir const tdir {"clk_comment"};
    auto mgr = motif::db::database_manager::create(tdir.path, "test").value();  // NOLINT(bugprone-unchecked-optional-access)

    motif::import::import_worker worker {mgr};

    pgn::game const game {
        .tags = {{"White", "Alice"}, {"Black", "Bob"}},
        .moves = {pgn::move_node {
            .number = 1,
            .san = "e4",
            .comment = std::optional<std::string> {"[%clk 0:05:00]"},
            .nags = {},
            .variations = {},
        }},
        .result = pgn::result::unknown,
    };

    auto res = worker.process(game);
    REQUIRE(res.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(res->positions_inserted == 2);  // starting position + 1 move

    mgr.close();
}

// ── SAN parse failure ────────────────────────────────────────────────────────

TEST_CASE("import_worker: illegal SAN returns parse_error, no row inserted", "[motif-import]")
{
    tmp_dir const tdir {"san_fail"};
    auto mgr = motif::db::database_manager::create(tdir.path, "test").value();  // NOLINT(bugprone-unchecked-optional-access)

    motif::import::import_worker worker {mgr};

    auto bad_moves = std::vector<pgn::move_node> {make_move(1, "XXXX_invalid")};
    auto bad_game = make_game("A", "B", pgn::result::unknown, bad_moves);

    auto res = worker.process(bad_game);
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error() == motif::import::error_code::parse_error);

    // No game row was inserted
    auto missing = mgr.store().get(motif::db::game_id {1});
    CHECK_FALSE(missing.has_value());

    mgr.close();
}

// ── No Elo tags → null elo in position rows ──────────────────────────────────

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertions inflate fixture validation complexity.
TEST_CASE("import_worker: game without Elo tags stores null elo", "[motif-import]")
{
    tmp_dir const tdir {"no_elo"};
    auto mgr = motif::db::database_manager::create(tdir.path, "test").value();  // NOLINT(bugprone-unchecked-optional-access)

    motif::import::import_worker worker {mgr};

    auto moves = std::vector<pgn::move_node> {make_move(1, "d4")};
    auto game = make_game("NoElo White", "NoElo Black", pgn::result::unknown, moves);

    auto res = worker.process(game);
    REQUIRE(res.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(res->positions_inserted == 2);  // starting position + 1 move

    REQUIRE(mgr.rebuild_position_postings().has_value());
    auto const hashes = expected_hashes(moves);
    REQUIRE(hashes.size() == 2);
    for (std::size_t ply = 0; ply < hashes.size(); ++ply) {
        auto const positions = mgr.query_position_matches(hashes[ply]);
        REQUIRE(positions.has_value());
        REQUIRE(positions->size() == 1);
        CHECK(positions->front().game_id == res->game_id);  // NOLINT(bugprone-unchecked-optional-access)
        CHECK(positions->front().ply == ply);
        CHECK_FALSE(positions->front().white_elo.has_value());
        CHECK_FALSE(positions->front().black_elo.has_value());
    }
    mgr.close();
}

// ── Zero-move game ───────────────────────────────────────────────────────────

TEST_CASE("import_worker: header-only game is rejected as empty_game", "[motif-import]")
{
    tmp_dir const tdir {"zero_moves"};
    auto mgr = motif::db::database_manager::create(tdir.path, "test").value();  // NOLINT(bugprone-unchecked-optional-access)

    motif::import::import_worker worker {mgr};

    auto game = make_game("Header White", "Header Black", pgn::result::unknown, {});

    auto res = worker.process(game);
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error() == motif::import::error_code::empty_game);

    mgr.close();
}
