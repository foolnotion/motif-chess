#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "motif/app/game_navigator.hpp"

#include <catch2/catch_test_macros.hpp>

#include "motif/chess/chess.hpp"
#include "motif/db/types.hpp"

namespace
{

// Build a motif::db::game from a sequence of SAN moves from the starting position.
auto make_game(std::vector<std::string> const& san_moves) -> motif::db::game
{
    auto pos = motif::chess::board {};
    auto moves = std::vector<std::uint16_t> {};
    for (auto const& san : san_moves) {
        auto result = motif::chess::apply_san(pos, san);
        REQUIRE(result);
        moves.push_back(*result);
    }

    auto game = motif::db::game {};
    game.moves = moves;
    return game;
}

}  // namespace

TEST_CASE("game_navigator: initial state", "[game_navigator]")
{
    motif::app::game_navigator const nav;

    SECTION("no game loaded — reports empty")
    {
        CHECK_FALSE(nav.has_game());
        CHECK(nav.current_ply() == 0);
        CHECK(nav.total_plies() == 0);
        CHECK(nav.current_san().empty());
    }

    SECTION("FEN at ply 0 is the starting position")
    {
        auto const fen = nav.current_fen();
        CHECK(fen.starts_with("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR"));
    }
}

TEST_CASE("game_navigator: load and basic properties", "[game_navigator]")
{
    motif::app::game_navigator nav;
    auto const game = make_game({"e4", "e5", "Nf3", "Nc6", "Bb5"});
    nav.load(game);

    CHECK(nav.has_game());
    CHECK(nav.total_plies() == 5);
    CHECK(nav.current_ply() == 0);
    CHECK(nav.current_san().empty());  // no move led to ply 0
}

TEST_CASE("game_navigator: advance and retreat", "[game_navigator]")
{
    motif::app::game_navigator nav;
    auto const game = make_game({"e4", "e5", "Nf3"});
    nav.load(game);

    SECTION("advance moves forward")
    {
        nav.advance();
        CHECK(nav.current_ply() == 1);
        CHECK(nav.current_san() == "e4");

        nav.advance();
        CHECK(nav.current_ply() == 2);
        CHECK(nav.current_san() == "e5");
    }

    SECTION("retreat at start is no-op")
    {
        nav.retreat();
        CHECK(nav.current_ply() == 0);
    }

    SECTION("advance at end is no-op")
    {
        nav.jump_to_end();
        auto const end_ply = nav.current_ply();
        nav.advance();
        CHECK(nav.current_ply() == end_ply);
    }

    SECTION("retreat after advance returns to correct ply")
    {
        nav.advance();
        nav.advance();
        nav.retreat();
        CHECK(nav.current_ply() == 1);
        CHECK(nav.current_san() == "e4");
    }
}

TEST_CASE("game_navigator: jump to start and end", "[game_navigator]")
{
    motif::app::game_navigator nav;
    auto const game = make_game({"d4", "d5", "c4", "c6", "Nc3", "Nf6"});
    nav.load(game);

    SECTION("jump_to_end reaches last ply")
    {
        nav.jump_to_end();
        CHECK(nav.current_ply() == 6);
        CHECK(nav.current_san() == "Nf6");
    }

    SECTION("jump_to_start after jump_to_end returns to 0")
    {
        nav.jump_to_end();
        nav.jump_to_start();
        CHECK(nav.current_ply() == 0);
        CHECK(nav.current_san().empty());
    }
}

TEST_CASE("game_navigator: navigate_to", "[game_navigator]")
{
    motif::app::game_navigator nav;
    auto const game = make_game({"e4", "e5", "Nf3", "Nc6"});
    nav.load(game);

    SECTION("navigate to specific ply")
    {
        nav.navigate_to(3);
        CHECK(nav.current_ply() == 3);
        CHECK(nav.current_san() == "Nf3");
    }

    SECTION("navigate beyond end clamps to total_plies")
    {
        constexpr std::size_t beyond_end = 9999;
        nav.navigate_to(beyond_end);
        CHECK(nav.current_ply() == nav.total_plies());
    }

    SECTION("navigate to 0 is start position")
    {
        nav.navigate_to(0);
        CHECK(nav.current_ply() == 0);
        CHECK(nav.current_san().empty());
    }
}

TEST_CASE("game_navigator: FEN reflects position at current ply", "[game_navigator]")
{
    motif::app::game_navigator nav;
    auto const game = make_game({"e4", "e5"});
    nav.load(game);

    auto const start_fen = nav.current_fen();
    CHECK(start_fen.starts_with("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR"));

    nav.advance();  // after e4
    auto const after_e4_fen = nav.current_fen();
    CHECK(after_e4_fen != start_fen);
    // After e4: ranks 6 and 5 are empty (two "8"), rank 4 has the e-pawn at file e (4P3).
    CHECK(after_e4_fen.contains("8/8/4P3"));

    nav.advance();  // after e4 e5
    auto const after_e5_fen = nav.current_fen();
    CHECK(after_e5_fen != after_e4_fen);

    nav.retreat();
    CHECK(nav.current_fen() == after_e4_fen);
}

TEST_CASE("game_navigator: move_list returns SAN for all moves", "[game_navigator]")
{
    motif::app::game_navigator nav;
    auto const game = make_game({"e4", "e5", "Nf3", "Nc6", "Bb5"});
    nav.load(game);

    auto const moves = nav.move_list();
    REQUIRE(moves.size() == 5);
    CHECK(moves[0] == "e4");
    CHECK(moves[1] == "e5");
    CHECK(moves[2] == "Nf3");
    CHECK(moves[3] == "Nc6");
    CHECK(moves[4] == "Bb5");
}

TEST_CASE("game_navigator: clear resets state", "[game_navigator]")
{
    motif::app::game_navigator nav;
    auto const game = make_game({"e4", "e5"});
    nav.load(game);
    nav.jump_to_end();

    nav.clear();

    CHECK_FALSE(nav.has_game());
    CHECK(nav.current_ply() == 0);
    CHECK(nav.total_plies() == 0);
}
