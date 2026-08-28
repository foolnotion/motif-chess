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

TEST_CASE("board_presenter: empty state has no phantom starting position", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    auto presenter = motif::slint_app::board_presenter {};

    auto const& squares = presenter.state().square_pieces;
    REQUIRE(squares.size() == 64);
    CHECK(std::ranges::all_of(squares, [](auto const& square) -> auto { return square.empty(); }));
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

TEST_CASE("board_presenter: unloaded board rejects square selection", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    auto presenter = motif::slint_app::board_presenter {};

    auto selected = presenter.select_square(e2_file, e2_rank);
    CHECK_FALSE(selected.has_value());
    CHECK_FALSE(presenter.state().selected_square.has_value());
    CHECK(presenter.state().legal_targets.empty());
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
}
