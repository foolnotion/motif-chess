#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "motif/slint_spike/game_browser_presenter.hpp"

#include <catch2/catch_test_macros.hpp>

#include "motif/chess/chess.hpp"
#include "motif/db/database_manager.hpp"
#include "motif/db/types.hpp"

namespace
{

auto make_game(std::string white, std::string black, std::vector<std::string> const& sans) -> motif::db::game
{
    auto board = motif::chess::board {};
    auto moves = std::vector<std::uint16_t> {};
    moves.reserve(sans.size());
    for (auto const& san : sans) {
        auto move = motif::chess::apply_san(board, san);
        REQUIRE(move);
        moves.push_back(*move);
    }

    auto game = motif::db::game {};
    game.white.name = std::move(white);
    game.black.name = std::move(black);
    game.result = "1-0";
    game.event_details = motif::db::event {.name = "Spike Open", .site = "Local", .date = "2026.08.24"};
    game.date = "2026.08.24";
    game.eco = "C20";
    game.moves = std::move(moves);
    return game;
}

auto make_repetition_game(std::size_t const plies) -> motif::db::game
{
    auto sans = std::vector<std::string> {};
    sans.reserve(plies);
    constexpr auto cycle = std::array<std::string_view, 4> {"Nf3", "Nf6", "Ng1", "Ng8"};
    for (std::size_t ply = 0; ply < plies; ++ply) {
        sans.emplace_back(cycle[ply % cycle.size()]);
    }
    return make_game("Long White", "Long Black", sans);
}

}  // namespace

TEST_CASE("game_browser_presenter: loads real rows and selected game navigation", "[slint-spike]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database);
    REQUIRE(database->insert_game(make_game("Zed", "Beta", {"e4", "e5", "Nf3"})).has_value());
    REQUIRE(database->insert_game(make_game("Alpha", "Delta", {"d4", "d5", "c4"})).has_value());

    auto presenter = motif::slint_spike::game_browser_presenter {*database};
    REQUIRE(presenter.load_games());
    REQUIRE(presenter.state().games.size() == 2);

    REQUIRE(presenter.select_game(0));
    auto const start_fen = presenter.state().current_fen;
    CHECK(presenter.state().has_selection);
    CHECK(presenter.state().selected_white == "Zed");
    CHECK(presenter.state().selected_black == "Beta");
    CHECK(presenter.state().selected_event == "Spike Open");
    CHECK(presenter.state().san_moves == std::vector<std::string> {"e4", "e5", "Nf3"});
    CHECK(presenter.state().current_ply == 0);
    CHECK_FALSE(start_fen.empty());

    REQUIRE(presenter.advance());
    CHECK(presenter.state().current_ply == 1);
    CHECK(presenter.state().current_fen != start_fen);

    REQUIRE(presenter.retreat());
    CHECK(presenter.state().current_ply == 0);
    CHECK(presenter.state().current_fen == start_fen);
}

TEST_CASE("game_browser_presenter: sorting resets selection", "[slint-spike]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database);
    REQUIRE(database->insert_game(make_game("Zed", "Beta", {"e4"})).has_value());
    REQUIRE(database->insert_game(make_game("Alpha", "Delta", {"d4"})).has_value());

    auto presenter = motif::slint_spike::game_browser_presenter {*database};
    REQUIRE(presenter.load_games());
    REQUIRE(presenter.select_game(0));
    REQUIRE(presenter.sort_games(0, true));

    CHECK(presenter.state().games[0].white == "Alpha");
    CHECK_FALSE(presenter.state().has_selection);
    CHECK(presenter.state().san_moves.empty());

    REQUIRE(presenter.sort_games(0, false));
    CHECK(presenter.state().games[0].white == "Zed");

    REQUIRE(presenter.sort_games(5, true));
    CHECK(presenter.state().games[0].eco == "C20");
}

TEST_CASE("game_browser_presenter: invalid actions are presented safely", "[slint-spike]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database);

    auto presenter = motif::slint_spike::game_browser_presenter {*database};
    REQUIRE(presenter.load_games());

    auto selection = presenter.select_game(0);
    CHECK_FALSE(selection);
    CHECK(presenter.state().error_text == "Selected game row is out of range");

    auto navigation = presenter.advance();
    CHECK_FALSE(navigation);
    CHECK(presenter.state().error_text == "Select a game before navigating moves");

    auto sorting = presenter.sort_games(6, true);
    CHECK_FALSE(sorting);
    CHECK(presenter.state().error_text == "Selected sort column is out of range");
}

TEST_CASE("game_browser_presenter: long game exposes and navigates all plies", "[slint-spike]")
{
    constexpr std::size_t long_game_plies {300};
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database);
    REQUIRE(database->insert_game(make_repetition_game(long_game_plies)).has_value());

    auto presenter = motif::slint_spike::game_browser_presenter {*database};
    REQUIRE(presenter.load_games());
    REQUIRE(presenter.select_game(0));
    REQUIRE(presenter.state().san_moves.size() == long_game_plies);
    CHECK(presenter.state().san_moves[0] == "Nf3");
    CHECK(presenter.state().san_moves[299] == "Ng8");

    auto const start_fen = presenter.state().current_fen;
    for (std::size_t ply = 0; ply < long_game_plies; ++ply) {
        REQUIRE(presenter.advance());
    }
    CHECK(presenter.state().current_ply == long_game_plies);
    CHECK(presenter.state().current_fen.starts_with(start_fen.substr(0, start_fen.find(" 0 1"))));

    REQUIRE(presenter.advance());
    CHECK(presenter.state().current_ply == long_game_plies);
}
