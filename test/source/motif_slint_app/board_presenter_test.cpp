#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "motif/slint_app/board_presenter.hpp"

#include <catch2/catch_test_macros.hpp>

#include "motif/chess/chess.hpp"
#include "motif/db/database_manager.hpp"
#include "motif/db/types.hpp"
#include "test_helpers.hpp"

namespace
{

constexpr std::size_t long_game_plies {300};
constexpr auto interaction_limit = std::chrono::milliseconds {100};
constexpr std::size_t midgame_ply {150};
constexpr int e2_file {4};
constexpr int e2_rank {1};
constexpr int e4_rank {3};
constexpr int out_of_range_square {8};
constexpr std::uint32_t unknown_node {999};

void skip_perf_unless_release_build()
{
    if (test_helpers::is_sanitized_build) {
        SKIP("performance checks are skipped in sanitize builds");
    }
#ifndef NDEBUG
    SKIP("performance checks require a Release build");
#endif
}

auto make_game(std::vector<std::string> const& sans) -> motif::db::game
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
    game.white.name = "White Player";
    game.black.name = "Black Player";
    game.result = "1-0";
    game.event_details = motif::db::event {.name = "Board Open", .site = "Local", .date = "2026.08.26"};
    game.date = "2026.08.26";
    game.eco = "C20";
    game.moves = std::move(moves);
    return game;
}

auto make_long_game(std::size_t const plies) -> motif::db::game
{
    auto sans = std::vector<std::string> {};
    sans.reserve(plies);
    constexpr auto cycle = std::array<std::string_view, 4> {"Nf3", "Nf6", "Ng1", "Ng8"};
    for (std::size_t ply = 0; ply < plies; ++ply) {
        sans.emplace_back(cycle.at(ply % cycle.size()));
    }
    return make_game(sans);
}

auto insert(motif::db::database_manager& database, motif::db::game const& game) -> motif::db::game_id
{
    auto inserted = database.insert_game(game);
    REQUIRE(inserted.has_value());
    return *inserted;
}

void load(motif::db::database_manager& database, motif::slint_app::board_presenter& presenter, motif::db::game_id const game_key)
{
    auto game = database.store().get(game_key);
    REQUIRE(game.has_value());
    presenter.apply_loaded_game(game_key, *game);
}

}  // namespace

TEST_CASE("board_presenter: empty state before any load", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    auto presenter = motif::slint_app::board_presenter {};

    CHECK_FALSE(presenter.state().loaded);
    CHECK(presenter.state().total_plies == 0);
    CHECK(presenter.state().current_ply == 0);
    CHECK(presenter.state().san_moves.empty());
    CHECK(presenter.state().white_name.empty());
    CHECK(presenter.state().error_text.empty());
    CHECK_FALSE(presenter.state().last_move_from.has_value());
    CHECK_FALSE(presenter.state().selected_square.has_value());
}

TEST_CASE("board_presenter: the empty board shows the playable starting position without a game", "[motif-slint-app]")
{
    auto presenter = motif::slint_app::board_presenter {};

    auto const& squares = presenter.state().square_pieces;
    REQUIRE(squares.size() == 64);
    CHECK(squares[0] == "wR");
    CHECK(squares[e2_file + (e2_rank * 8)] == "wP");
    CHECK(squares[4 + (7 * 8)] == "bK");
    CHECK(std::ranges::count_if(squares, [](auto const& square) -> bool { return !square.empty(); }) == 32);
    CHECK_FALSE(presenter.state().loaded);
    CHECK_FALSE(presenter.state().unsaved);
    CHECK_FALSE(presenter.state().edited);
    CHECK(presenter.movetext().empty());
}

TEST_CASE("board_presenter: square_pieces updates after navigation", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    auto const game_key = insert(*database, make_game({"e4"}));
    auto presenter = motif::slint_app::board_presenter {};
    load(*database, presenter, game_key);

    presenter.advance();
    auto const& squares = presenter.state().square_pieces;
    CHECK(squares[e2_file + (e2_rank * 8)].empty());  // e2 vacated
    CHECK(squares[e2_file + (e4_rank * 8)] == "wP");  // e4 occupied
}

TEST_CASE("board_presenter: first/previous/next/last navigation", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    auto const game_key = insert(*database, make_game({"e4", "e5", "Nf3", "Nc6"}));

    auto presenter = motif::slint_app::board_presenter {};
    load(*database, presenter, game_key);

    presenter.advance();
    CHECK(presenter.state().current_ply == 1);
    CHECK(presenter.state().current_san == "e4");

    presenter.advance();
    presenter.advance();
    presenter.advance();
    CHECK(presenter.state().current_ply == 4);

    presenter.retreat();
    CHECK(presenter.state().current_ply == 3);
    CHECK(presenter.state().current_san == "Nf3");

    presenter.jump_to_start();
    CHECK(presenter.state().current_ply == 0);
    CHECK(presenter.state().current_san.empty());

    presenter.jump_to_end();
    CHECK(presenter.state().current_ply == 4);
    CHECK(presenter.state().current_san == "Nc6");
}

TEST_CASE("board_presenter: direct-ply navigation clamps out-of-range requests", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    auto const game_key = insert(*database, make_game({"e4", "e5", "Nf3"}));

    auto presenter = motif::slint_app::board_presenter {};
    load(*database, presenter, game_key);

    presenter.navigate_to(2);
    CHECK(presenter.state().current_ply == 2);

    constexpr std::size_t beyond_end {9999};
    presenter.navigate_to(beyond_end);
    CHECK(presenter.state().current_ply == presenter.state().total_plies);
}

TEST_CASE("board_presenter: navigation boundaries are safe no-ops", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    auto presenter = motif::slint_app::board_presenter {};

    // No game loaded — navigation must not crash and stays at ply 0.
    presenter.advance();
    presenter.retreat();
    CHECK(presenter.state().current_ply == 0);

    auto const game_key = insert(*database, make_game({"e4"}));
    load(*database, presenter, game_key);

    presenter.retreat();
    CHECK(presenter.state().current_ply == 0);

    presenter.jump_to_end();
    auto const end_ply = presenter.state().current_ply;
    presenter.advance();
    CHECK(presenter.state().current_ply == end_ply);
}

TEST_CASE("board_presenter: clear resets every field but preserves the panel visibility preference", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    auto const game_key = insert(*database, make_game({"e4", "e5"}));

    auto presenter = motif::slint_app::board_presenter {};
    load(*database, presenter, game_key);
    presenter.advance();
    presenter.toggle_orientation();
    presenter.set_panel_visible(/*visible=*/false);
    REQUIRE(presenter.select_square(e2_file, e2_rank).has_value());

    presenter.clear();

    auto const& state = presenter.state();
    CHECK_FALSE(state.loaded);
    CHECK(state.current_ply == 0);
    CHECK(state.total_plies == 0);
    CHECK(state.san_moves.empty());
    CHECK(state.white_name.empty());
    CHECK(state.result.empty());
    CHECK_FALSE(state.selected_square.has_value());
    CHECK(state.legal_targets.empty());
    CHECK_FALSE(state.last_move_from.has_value());
    CHECK_FALSE(state.orientation_flipped);
    CHECK(state.error_text.empty());
    CHECK_FALSE(state.panel_visible);
}

TEST_CASE("board_presenter: orientation toggle persists across a new game load", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    auto const first_key = insert(*database, make_game({"e4"}));
    auto const second_key = insert(*database, make_game({"d4"}));

    auto presenter = motif::slint_app::board_presenter {};
    load(*database, presenter, first_key);
    presenter.toggle_orientation();
    CHECK(presenter.state().orientation_flipped);

    load(*database, presenter, second_key);
    CHECK(presenter.state().orientation_flipped);

    presenter.toggle_orientation();
    CHECK_FALSE(presenter.state().orientation_flipped);
}

TEST_CASE("board_presenter: the empty board accepts selection and still rejects squares off the board", "[motif-slint-app]")
{
    auto presenter = motif::slint_app::board_presenter {};

    REQUIRE(presenter.select_square(e2_file, e2_rank).has_value());
    CHECK(presenter.state().selected_square.has_value());
    CHECK(presenter.state().legal_targets.size() == 2);

    CHECK_FALSE(presenter.select_square(out_of_range_square, 0).has_value());
    CHECK(presenter.state().selected_square.has_value());
    CHECK_FALSE(presenter.state().unsaved);
}

TEST_CASE("board_presenter: selected square exposes legal targets from motif::chess", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    auto const game_key = insert(*database, make_game({"e4", "e5"}));

    auto presenter = motif::slint_app::board_presenter {};
    load(*database, presenter, game_key);

    // e2 pawn (file 4, rank 1) can reach e3 and e4 from the starting position.
    REQUIRE(presenter.select_square(e2_file, e2_rank).has_value());
    auto const& selected = presenter.state();
    REQUIRE(selected.selected_square.has_value());
    auto const selected_square = selected.selected_square.value_or(motif::slint_app::board_square {});
    CHECK(selected_square.file == e2_file);
    CHECK(selected_square.rank == e2_rank);
    CHECK(selected.legal_targets.size() == 2);
    CHECK(selected.square_highlights.at(e2_file + (e2_rank * 8)) == motif::slint_app::square_highlight::selected);
    CHECK(selected.square_highlights.at(e2_file + (e4_rank * 8)) == motif::slint_app::square_highlight::legal_target);

    // Clicking the same square again clears the selection.
    REQUIRE(presenter.select_square(e2_file, e2_rank).has_value());
    CHECK_FALSE(presenter.state().selected_square.has_value());
    CHECK(presenter.state().legal_targets.empty());

    auto out_of_range = presenter.select_square(out_of_range_square, 0);
    CHECK_FALSE(out_of_range.has_value());

    presenter.advance();
    CHECK_FALSE(presenter.state().selected_square.has_value());
}

TEST_CASE("board_presenter: last-move squares reflect the move that reached the current ply", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    auto const game_key = insert(*database, make_game({"e4", "e5"}));

    auto presenter = motif::slint_app::board_presenter {};
    load(*database, presenter, game_key);
    CHECK_FALSE(presenter.state().last_move_from.has_value());

    presenter.advance();
    REQUIRE(presenter.state().last_move_from.has_value());
    REQUIRE(presenter.state().last_move_to.has_value());
    auto const last_move_from = presenter.state().last_move_from.value_or(motif::slint_app::board_square {});
    auto const last_move_to = presenter.state().last_move_to.value_or(motif::slint_app::board_square {});
    CHECK(presenter.state().square_highlights.at(e2_file + (e2_rank * 8)) == motif::slint_app::square_highlight::last_move_from);
    CHECK(presenter.state().square_highlights.at(e2_file + (e4_rank * 8)) == motif::slint_app::square_highlight::last_move_to);
    CHECK(last_move_from.rank == e2_rank);
    CHECK(last_move_to.file == e2_file);  // e4
    CHECK(last_move_to.rank == e4_rank);

    presenter.retreat();
    CHECK_FALSE(presenter.state().last_move_from.has_value());
}

TEST_CASE("board_presenter: hide/restore panel visibility is independent of game state", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    auto presenter = motif::slint_app::board_presenter {};

    CHECK(presenter.state().panel_visible);
    presenter.set_panel_visible(/*visible=*/false);
    CHECK_FALSE(presenter.state().panel_visible);
    presenter.set_panel_visible(/*visible=*/true);
    CHECK(presenter.state().panel_visible);
}

TEST_CASE("board_presenter: no persisted comments, NAGs, or variations produce empty state without errors", "[motif-slint-app]")
{
    // motif::db::game has no comment/NAG/variation fields today; loading and
    // navigating a stored game must not fail or fabricate placeholder data.
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    auto const game_key = insert(*database, make_game({"e4", "e5", "Nf3", "Nc6"}));

    auto presenter = motif::slint_app::board_presenter {};
    load(*database, presenter, game_key);
    presenter.jump_to_end();

    CHECK(presenter.state().error_text.empty());
    CHECK(presenter.state().san_moves.size() == 4);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("board_presenter: a 300-ply game exposes and navigates every ply", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    auto const game_key = insert(*database, make_long_game(long_game_plies));

    auto presenter = motif::slint_app::board_presenter {};
    load(*database, presenter, game_key);
    REQUIRE(presenter.state().san_moves.size() == long_game_plies);
    CHECK(presenter.state().san_moves.front() == "Nf3");
    CHECK(presenter.state().san_moves.back() == "Ng8");

    auto const start_fen = presenter.state().fen;
    for (std::size_t ply = 0; ply < long_game_plies; ++ply) {
        presenter.advance();
    }
    CHECK(presenter.state().current_ply == long_game_plies);
    CHECK(presenter.state().fen.starts_with(start_fen.substr(0, start_fen.find(" 0 1"))));

    presenter.advance();
    CHECK(presenter.state().current_ply == long_game_plies);

    presenter.navigate_to(midgame_ply);
    CHECK(presenter.state().current_ply == midgame_ply);
    REQUIRE(presenter.state().last_move_from.has_value());
}

TEST_CASE("board_presenter: 300-ply load and navigation meet the interaction target", "[motif-slint-app][performance]")
{
    skip_perf_unless_release_build();
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    auto const game_key = insert(*database, make_long_game(long_game_plies));

    auto presenter = motif::slint_app::board_presenter {};
    auto const load_started = std::chrono::steady_clock::now();
    load(*database, presenter, game_key);
    auto const load_elapsed = std::chrono::steady_clock::now() - load_started;
    CHECK(load_elapsed < interaction_limit);

    auto const nav_started = std::chrono::steady_clock::now();
    presenter.jump_to_end();
    presenter.retreat();
    presenter.advance();
    presenter.jump_to_start();
    presenter.navigate_to(midgame_ply);
    auto const nav_elapsed = std::chrono::steady_clock::now() - nav_started;
    CHECK(nav_elapsed < interaction_limit);

    // Branching the loaded line from its middle is one move on a long game.
    auto const move_started = std::chrono::steady_clock::now();
    CHECK(presenter.play_san("e4").has_value());
    auto const move_elapsed = std::chrono::steady_clock::now() - move_started;
    CHECK(move_elapsed < interaction_limit);
}

namespace
{

// Clicks the two squares of a move written as "e2e4", as a pointer would.
void click(motif::slint_app::board_presenter& presenter, std::string_view const squares)
{
    REQUIRE(squares.size() == 4);
    REQUIRE(presenter.select_square(squares[0] - 'a', squares[1] - '1').has_value());
    REQUIRE(presenter.select_square(squares[2] - 'a', squares[3] - '1').has_value());
}

auto loaded_presenter(motif::db::database_manager& database, std::vector<std::string> const& sans) -> motif::slint_app::board_presenter
{
    auto presenter = motif::slint_app::board_presenter {};
    load(database, presenter, insert(database, make_game(sans)));
    return presenter;
}

}  // namespace

TEST_CASE("board_presenter: the first legal move on an empty board starts an unsaved game", "[motif-slint-app]")
{
    auto presenter = motif::slint_app::board_presenter {};

    click(presenter, "e2e4");

    auto const& state = presenter.state();
    CHECK(state.unsaved);
    CHECK_FALSE(state.loaded);
    CHECK(state.edited);
    CHECK(state.white_name == "White");
    CHECK(state.black_name == "Black");
    CHECK(state.result == "*");
    CHECK(state.current_ply == 1);
    CHECK(state.total_plies == 1);
    CHECK(state.current_san == "e4");
    CHECK(state.square_pieces[e2_file + (e4_rank * 8)] == "wP");
    CHECK(state.square_pieces[e2_file + (e2_rank * 8)].empty());
    CHECK_FALSE(state.selected_square.has_value());
    REQUIRE(state.notation.size() == 1);
    CHECK(state.notation.front().san == "e4");
    CHECK(state.notation.front().number == "1.");
    CHECK(presenter.movetext() == "1. e4 *");
    CHECK(state.current_hash.has_value());
}

TEST_CASE("board_presenter: a main-line click advances a loaded game without editing it", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    auto presenter = loaded_presenter(*database, {"e4", "e5", "Nf3"});
    auto const game_id = presenter.state().game_id;

    click(presenter, "e2e4");

    auto const& state = presenter.state();
    CHECK(state.current_ply == 1);
    CHECK(state.loaded);
    CHECK_FALSE(state.unsaved);
    CHECK_FALSE(state.edited);
    CHECK(state.game_id == game_id);
    CHECK(state.notation.size() == 3);
    CHECK(presenter.movetext() == "1. e4 e5 2. Nf3 1-0");
}

TEST_CASE("board_presenter: an off-line move creates a variation in a loaded game", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    auto presenter = loaded_presenter(*database, {"e4", "e5", "Nf3"});
    presenter.navigate_to(1);

    click(presenter, "c7c5");

    auto const& state = presenter.state();
    CHECK(state.loaded);
    CHECK(state.edited);
    CHECK(state.current_san == "c5");
    CHECK(state.current_ply == 2);
    CHECK(state.total_plies == 3);
    CHECK(state.san_moves == std::vector<std::string> {"e4", "e5", "Nf3"});
    CHECK(state.result == "1-0");
    CHECK(presenter.movetext() == "1. e4 e5 (1... c5) 2. Nf3 1-0");
    REQUIRE(state.notation.size() == 4);
    CHECK(state.notation[2].variation_depth == 1);
    CHECK(state.notation[2].open == "(");
    CHECK(state.notation[2].close == ")");
    CHECK(state.notation[2].node == state.current_node);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("board_presenter: variations nest and PGN numbering stays valid", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    auto presenter = loaded_presenter(*database, {"e4", "e5", "Nf3"});
    presenter.navigate_to(1);
    for (auto const* san : {"c5", "Nf3", "d6"}) {
        REQUIRE(presenter.play_san(san).has_value());
    }
    click(presenter, "b8c6");  // illegal here (white to move): leaves the line alone
    CHECK(presenter.state().current_san == "d6");
    presenter.retreat();
    REQUIRE(presenter.play_san("Nc6").has_value());

    CHECK(presenter.movetext() == "1. e4 e5 (1... c5 2. Nf3 d6 (2... Nc6)) 2. Nf3 1-0");
    CHECK(presenter.state().total_plies == 3);

    presenter.navigate_to(1);
    REQUIRE(presenter.play_san("d5").has_value());
    CHECK(presenter.movetext() == "1. e4 e5 (1... c5 2. Nf3 d6 (2... Nc6)) (1... d5) 2. Nf3 1-0");
}

TEST_CASE("board_presenter: a black reply after a white alternative is renumbered", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    auto presenter = loaded_presenter(*database, {"e4", "e5"});

    click(presenter, "d2d4");

    CHECK(presenter.movetext() == "1. e4 (1. d4) 1... e5 1-0");
}

TEST_CASE("board_presenter: playing an existing variation again enters it without a duplicate", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    auto presenter = loaded_presenter(*database, {"e4", "e5"});
    presenter.navigate_to(1);
    click(presenter, "c7c5");
    auto const variation_node = presenter.state().current_node;
    auto const tokens = presenter.state().notation.size();

    presenter.navigate_to(1);
    click(presenter, "c7c5");

    CHECK(presenter.state().current_node == variation_node);
    CHECK(presenter.state().notation.size() == tokens);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("board_presenter: navigation alone never marks a game edited", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    auto presenter = loaded_presenter(*database, {"e4", "e5", "Nf3"});
    presenter.navigate_to(1);
    click(presenter, "c7c5");

    // Jumping around a tree with a variation reaches every token.
    for (auto const& token : std::vector<motif::slint_app::notation_token>(presenter.state().notation)) {
        presenter.navigate_to_node(token.node);
        CHECK(presenter.state().current_node == token.node);
        CHECK(presenter.state().current_san == token.san);
    }
    CHECK(presenter.state().edited);

    auto other = loaded_presenter(*database, {"e4", "e5"});
    other.advance();
    other.jump_to_end();
    other.jump_to_start();
    other.navigate_to(2);
    other.navigate_to_node(0);
    other.navigate_to_node(unknown_node);
    CHECK_FALSE(other.state().edited);
    CHECK(other.state().current_ply == 0);
}

TEST_CASE("board_presenter: an illegal click changes nothing and clears the selection", "[motif-slint-app]")
{
    auto presenter = motif::slint_app::board_presenter {};
    auto const fen = presenter.state().fen;

    REQUIRE(presenter.select_square(e2_file, e2_rank).has_value());
    REQUIRE(presenter.select_square(e2_file, e4_rank + 2).has_value());  // e6: not a target

    CHECK_FALSE(presenter.state().selected_square.has_value());
    CHECK(presenter.state().legal_targets.empty());
    CHECK(presenter.state().fen == fen);
    CHECK_FALSE(presenter.state().unsaved);
    CHECK(presenter.state().error_text.empty());
}

TEST_CASE("board_presenter: clicking another movable piece reselects instead of moving", "[motif-slint-app]")
{
    auto presenter = motif::slint_app::board_presenter {};

    REQUIRE(presenter.select_square(e2_file, e2_rank).has_value());
    REQUIRE(presenter.select_square(6, 0).has_value());  // g1 knight

    auto const selected = presenter.state().selected_square.value_or(motif::slint_app::board_square {});
    CHECK(selected.file == 6);
    CHECK(selected.rank == 0);
    CHECK(presenter.state().legal_targets.size() == 2);
    CHECK_FALSE(presenter.state().unsaved);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("board_presenter: promotion waits for a piece, can be cancelled, and then applies", "[motif-slint-app]")
{
    auto presenter = motif::slint_app::board_presenter {};
    for (auto const* san : {"h4", "g5", "hxg5", "a6", "g6", "a5", "gxh7", "a4"}) {
        REQUIRE(presenter.play_san(san).has_value());
    }
    auto const fen = presenter.state().fen;
    auto const plies = presenter.state().total_plies;

    click(presenter, "h7g8");
    REQUIRE(presenter.state().promotion.has_value());
    auto const pending = presenter.state().promotion.value_or(motif::slint_app::pending_promotion {});
    CHECK(pending.white);
    CHECK(pending.to == motif::slint_app::board_square {.file = 6, .rank = 7});
    CHECK(presenter.state().fen == fen);
    CHECK(presenter.state().total_plies == plies);

    presenter.cancel_promotion();
    CHECK_FALSE(presenter.state().promotion.has_value());
    CHECK(presenter.state().fen == fen);
    CHECK(presenter.state().total_plies == plies);
    CHECK_FALSE(presenter.state().selected_square.has_value());

    click(presenter, "h7g8");
    auto const wrong = presenter.choose_promotion('k');
    CHECK_FALSE(wrong.has_value());
    CHECK(presenter.state().promotion.has_value());
    CHECK(presenter.state().fen == fen);

    REQUIRE(presenter.choose_promotion('Q').has_value());
    CHECK_FALSE(presenter.state().promotion.has_value());
    CHECK(presenter.state().current_san.starts_with("hxg8=Q"));
    CHECK(presenter.state().square_pieces[6 + (7 * 8)] == "wQ");
    CHECK(presenter.state().total_plies == plies + 1);

    CHECK_FALSE(presenter.choose_promotion('q').has_value());  // nothing pending
}

TEST_CASE("board_presenter: SAN entry acts as board input and reports bad input", "[motif-slint-app]")
{
    auto by_san = motif::slint_app::board_presenter {};
    auto by_click = motif::slint_app::board_presenter {};

    REQUIRE(by_san.play_san("e4").has_value());
    click(by_click, "e2e4");
    CHECK(by_san.state().fen == by_click.state().fen);
    CHECK(by_san.movetext() == by_click.movetext());
    CHECK(by_san.state().unsaved);

    auto const fen = by_san.state().fen;
    auto const plies = by_san.state().total_plies;
    auto const invalid = by_san.play_san("Qh5");
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error().code == motif::slint_app::error_code::invalid_argument);
    CHECK_FALSE(by_san.state().error_text.empty());
    CHECK(by_san.state().fen == fen);
    CHECK(by_san.state().total_plies == plies);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("board_presenter: New Game resets to an empty unsaved game and keeps the orientation", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    auto presenter = loaded_presenter(*database, {"e4", "e5"});
    presenter.toggle_orientation();
    presenter.set_panel_visible(/*visible=*/false);
    click(presenter, "g1f3");
    REQUIRE(presenter.state().edited);

    presenter.start_new_game();

    auto const& state = presenter.state();
    CHECK(state.unsaved);
    CHECK_FALSE(state.loaded);
    CHECK_FALSE(state.edited);
    CHECK(state.total_plies == 0);
    CHECK(state.notation.empty());
    CHECK(state.white_name == "White");
    CHECK(state.result == "*");
    CHECK(state.orientation_flipped);
    CHECK_FALSE(state.panel_visible);
    CHECK(state.square_pieces[e2_file + (e2_rank * 8)] == "wP");
    CHECK(presenter.movetext() == "*");

    click(presenter, "d2d4");
    CHECK(presenter.state().edited);
    CHECK(presenter.movetext() == "1. d4 *");
}

TEST_CASE("board_presenter: loading a stored game replaces an unsaved one", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    auto const game_key = insert(*database, make_game({"d4", "d5"}));
    auto presenter = motif::slint_app::board_presenter {};
    click(presenter, "e2e4");
    REQUIRE(presenter.state().unsaved);

    load(*database, presenter, game_key);

    CHECK(presenter.state().loaded);
    CHECK_FALSE(presenter.state().unsaved);
    CHECK_FALSE(presenter.state().edited);
    CHECK(presenter.state().white_name == "White Player");
    CHECK(presenter.movetext() == "1. d4 d5 1-0");
}
