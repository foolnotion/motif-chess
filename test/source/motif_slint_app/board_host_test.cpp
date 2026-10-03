#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "motif/slint_app/board_host.hpp"

#include <catch2/catch_test_macros.hpp>
#include <slint.h>

#include "motif/chess/chess.hpp"
#include "motif/db/types.hpp"
#include "motif/slint_app/activity_gate.hpp"
#include "snapshot_platform.hpp"
#include "workspace.h"

// Reading a published Slint model back goes through its virtual interface, which
// the portability check flags at every instantiation point in this file.
// NOLINTBEGIN(portability-template-virtual-member-function)
namespace
{

using motif::slint_app::workspace_activity;

// Every busy condition on its own, plus the idle workspace.
constexpr auto busy_conditions = std::array {
    workspace_activity {.import_active = true},
    workspace_activity {.workspace_busy = true},
    workspace_activity {.browser_busy = true},
    workspace_activity {.board_busy = true},
    workspace_activity {.search_busy = true},
};

// The production registrars wired to a real window and a real runner, with the
// activity the test chooses.
struct host
{
    slint::ComponentHandle<WorkspaceWindow> window {WorkspaceWindow::create()};
    motif::slint_app::async_board_runner board {window};
    workspace_activity activity;
    bool rows_current {true};
    motif::slint_app::replace_controller confirmation {motif::slint_app::make_replace_confirmation(window, board)};

    host()
    {
        board.attach();
        auto const probe = motif::slint_app::activity_probe {[this]() -> workspace_activity { return activity; }};
        motif::slint_app::register_board_callbacks(*window, board, probe);
        motif::slint_app::register_play_callbacks(*window, board, probe, confirmation);
        motif::slint_app::register_explorer_callbacks(*window, board, probe, [this]() -> bool { return rows_current; });
        motif::slint_app::install_close_guard(*window, board, confirmation);
    }

    [[nodiscard]] auto tokens() const -> std::vector<std::string>
    {
        auto result = std::vector<std::string> {};
        auto const model = window->get_board_notation();
        for (auto index = std::size_t {0}; index < model->row_count(); ++index) {
            auto const token = model->row_data(index).value_or(NotationToken {});
            result.emplace_back(std::string_view {token.san.data(), token.san.size()});
        }
        return result;
    }
};

// A stored game made of `sans`, as the browser would hand it to the board.
auto make_stored_game(std::vector<std::string> const& sans) -> motif::db::game
{
    auto position = motif::chess::board {};
    auto game = motif::db::game {};
    game.white.name = "White Player";
    game.black.name = "Black Player";
    for (auto const& san : sans) {
        auto const move = motif::chess::apply_san(position, san);
        REQUIRE(move);
        game.moves.push_back(*move);
    }
    return game;
}

}  // namespace

TEST_CASE("board host: SAN entry acts through the registered callback when idle", "[motif-slint-app][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};

    test.window->invoke_board_play_san("e4");

    CHECK(test.tokens() == std::vector<std::string> {"e4"});
    CHECK(test.window->get_board_loaded());
    CHECK(test.window->get_board_unsaved());
    CHECK(test.window->get_board_edited());
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("board host: every registered board callback is rejected under each busy condition", "[motif-slint-app][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    for (auto const& busy : busy_conditions) {
        auto test = host {};
        test.window->invoke_board_play_san("e4");
        REQUIRE(test.tokens().size() == 1);
        test.activity = busy;
        auto const node = test.window->get_board_current_node();

        test.window->invoke_board_play_san("e5");
        test.window->invoke_board_square_activated(3, 1);  // d2: would select a pawn
        test.window->invoke_board_square_activated(3, 3);
        test.window->invoke_board_navigate_first();
        test.window->invoke_board_node_activated(0);
        test.window->invoke_board_orientation_toggled();
        test.window->invoke_board_new_game_requested();
        test.window->invoke_board_promotion_cancelled();

        CHECK(test.tokens() == std::vector<std::string> {"e4"});
        CHECK(test.window->get_board_current_node() == node);
        CHECK_FALSE(test.window->get_board_flipped());
        CHECK_FALSE(test.window->get_confirm_visible());
        CHECK(test.window->get_board_edited());
    }
}

TEST_CASE("board host: navigation callbacks move the cursor when idle and publish the node", "[motif-slint-app][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    test.window->invoke_board_play_san("e4");
    test.window->invoke_board_play_san("e5");
    auto const second = test.window->get_board_current_node();

    test.window->invoke_board_navigate_previous();
    CHECK(test.window->get_board_current_node() != second);
    test.window->invoke_board_navigate_next();
    CHECK(test.window->get_board_current_node() == second);
    test.window->invoke_board_navigate_first();
    CHECK(test.window->get_board_current_node() == 0);
    test.window->invoke_board_node_activated(second);
    CHECK(test.window->get_board_current_node() == second);
    test.window->invoke_board_orientation_toggled();
    CHECK(test.window->get_board_flipped());
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("board host: publishing maps variation tokens to lead, san, trail, node and depth", "[motif-slint-app][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    for (auto const* san : {"e4", "e5", "Nf3"}) {
        test.window->invoke_board_play_san(san);
    }
    test.window->invoke_board_navigate_first();
    test.window->invoke_board_navigate_next();  // after 1. e4
    test.window->invoke_board_play_san("c5");  // variation to 1... e5

    auto const model = test.window->get_board_notation();
    REQUIRE(model->row_count() == 4);
    auto const text = [](slint::SharedString const& value) -> std::string
    { return std::string {std::string_view {value.data(), value.size()}}; };
    auto const first = model->row_data(0).value_or(NotationToken {});
    CHECK(text(first.lead) == "1.");
    CHECK(text(first.san) == "e4");
    CHECK(first.depth == 0);
    auto const reply = model->row_data(1).value_or(NotationToken {});
    CHECK(text(reply.san) == "e5");
    auto const variation = model->row_data(2).value_or(NotationToken {});
    CHECK(text(variation.lead) == "(1...");
    CHECK(text(variation.san) == "c5");
    CHECK(text(variation.trail) == ")");
    CHECK(variation.depth == 1);
    CHECK(variation.node == test.window->get_board_current_node());
    auto const continuation = model->row_data(3).value_or(NotationToken {});
    CHECK(text(continuation.lead) == "2.");
    CHECK(text(continuation.san) == "Nf3");
    CHECK(test.window->get_board_notation_chars() >= 5);
    CHECK(test.window->get_board_edited());
}

TEST_CASE("board host: New Game asks first while edited and the answer is applied", "[motif-slint-app][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    test.window->invoke_board_new_game_requested();  // nothing to lose yet
    CHECK(test.tokens().empty());
    test.window->invoke_board_play_san("d4");

    test.window->invoke_board_new_game_requested();
    CHECK(test.window->get_confirm_visible());
    CHECK(test.tokens().size() == 1);

    test.window->invoke_confirm_cancelled();
    CHECK_FALSE(test.window->get_confirm_visible());
    CHECK(test.tokens().size() == 1);

    test.window->invoke_board_new_game_requested();
    test.window->invoke_confirm_accepted();
    CHECK_FALSE(test.window->get_confirm_visible());
    CHECK(test.tokens().empty());
    CHECK(test.window->get_board_unsaved());
    CHECK_FALSE(test.window->get_board_edited());
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("board host: closing the window asks while edited and keeps the window on cancel", "[motif-slint-app][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    test.window->show();

    test.window->window().dispatch_close_requested_event();
    CHECK_FALSE(test.window->get_confirm_visible());  // nothing edited: the window closes

    auto edited = host {};
    edited.window->show();
    edited.window->invoke_board_play_san("e4");
    edited.window->window().dispatch_close_requested_event();
    CHECK(edited.window->get_confirm_visible());
    CHECK(edited.window->window().is_visible());

    edited.window->invoke_confirm_cancelled();
    CHECK(edited.window->window().is_visible());
    CHECK(edited.tokens().size() == 1);

    edited.window->window().dispatch_close_requested_event();
    edited.window->invoke_confirm_accepted();
    CHECK_FALSE(edited.window->window().is_visible());
}

TEST_CASE("board host: a failed game read leaves the edited board alone and a loaded game replaces it", "[motif-slint-app][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    test.window->invoke_board_play_san("e4");
    test.window->invoke_board_play_san("e5");
    auto handlers = motif::slint_app::make_board_activation_handlers(slint::ComponentWeakHandle<WorkspaceWindow> {test.window}, test.board);
    auto const serial = test.window->get_board_load_serial();

    handlers.failed();
    handlers.failed_at();

    CHECK(test.tokens() == std::vector<std::string> {"e4", "e5"});
    CHECK(test.window->get_board_edited());
    CHECK(test.window->get_board_unsaved());
    CHECK(test.window->get_board_load_serial() == serial);
    CHECK(test.board.edited());

    auto game = motif::db::game {};
    game.white.name = "Loaded";
    handlers.loaded(motif::db::game_id {1}, game);
    CHECK_FALSE(test.window->get_board_edited());
    CHECK_FALSE(test.window->get_board_unsaved());
    CHECK(test.window->get_board_load_serial() == serial + 1);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("board host: activating an explorer row plays its move and the board reports the new position", "[motif-slint-app][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    auto positions = std::vector<motif::db::zobrist_hash> {};
    test.board.set_position_changed_handler([&positions](motif::db::zobrist_hash const hash) -> void { positions.push_back(hash); });

    test.window->invoke_search_continuation_activated("e4");
    REQUIRE(test.tokens() == std::vector<std::string> {"e4"});
    REQUIRE(positions.size() == 1);
    CHECK(test.board.current_hash() == positions.back());
    CHECK(test.window->get_board_unsaved());  // no game was loaded: a new unsaved game

    test.window->invoke_search_continuation_activated("c5");
    CHECK(test.tokens() == std::vector<std::string> {"e4", "c5"});
    REQUIRE(positions.size() == 2);
    CHECK(positions.front() != positions.back());

    // Off the end of the line: stepping back and playing another move branches.
    test.window->invoke_board_navigate_previous();
    test.window->invoke_search_continuation_activated("e5");
    CHECK(test.tokens().size() > 2);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("board host: explorer rows are ignored under each busy condition and when the table is stale", "[motif-slint-app][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    for (auto const& busy : busy_conditions) {
        auto test = host {};
        test.window->invoke_board_play_san("e4");
        auto positions = std::size_t {0};
        test.board.set_position_changed_handler([&positions](motif::db::zobrist_hash) -> void { ++positions; });
        test.activity = busy;

        test.window->invoke_search_continuation_activated("e5");

        CHECK(test.tokens() == std::vector<std::string> {"e4"});
        CHECK(positions == 0);
    }

    auto stale = host {};
    stale.window->invoke_board_play_san("e4");
    stale.rows_current = false;  // the rows on screen belong to another position
    stale.window->invoke_search_continuation_activated("e5");
    CHECK(stale.tokens() == std::vector<std::string> {"e4"});
}

TEST_CASE("board host: a rejected explorer move leaves the board unchanged and shows the board's message", "[motif-slint-app][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    test.window->invoke_board_play_san("e4");
    auto const hash = test.board.current_hash();
    auto const node = test.window->get_board_current_node();

    test.window->invoke_search_continuation_activated("Qh5");  // not legal for Black after 1. e4

    CHECK(test.tokens() == std::vector<std::string> {"e4"});
    CHECK(test.window->get_board_current_node() == node);
    CHECK_FALSE(test.window->get_board_error_text().empty());
    CHECK(test.board.current_hash() == hash);

    test.window->invoke_search_continuation_activated("");
    CHECK(test.tokens() == std::vector<std::string> {"e4"});
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("board host: an explorer row follows a loaded game's main line and branches off it", "[motif-slint-app][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    auto handlers = motif::slint_app::make_board_activation_handlers(slint::ComponentWeakHandle<WorkspaceWindow> {test.window}, test.board);
    auto const game = make_stored_game({"e4", "e5", "Nf3"});
    handlers.loaded(motif::db::game_id {1}, game);
    REQUIRE_FALSE(test.window->get_board_edited());
    auto const loaded_tokens = test.tokens();

    test.window->invoke_search_continuation_activated("e4");  // the game's own first move
    CHECK(test.tokens() == loaded_tokens);
    CHECK_FALSE(test.window->get_board_edited());
    CHECK(test.window->get_board_current_node() != 0);

    test.window->invoke_search_continuation_activated("c5");  // legal, but the game played e5
    CHECK(test.tokens().size() > loaded_tokens.size());
    CHECK(test.window->get_board_edited());
}

// Sample figures are literal on purpose: the test asserts them on the published model.
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
TEST_CASE("board host: publishing the explorer maps rows, bar percentages and the total row", "[motif-slint-app][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    auto table = motif::slint_app::explorer_table {};
    table.rows.push_back(motif::slint_app::explorer_row {.san = "e4",
                                                         .count = 7,
                                                         .share_percent = 70,
                                                         .share_text = "70%",
                                                         .split = {.white = 50, .draw = 30, .black = 20},
                                                         .white_elo = "2400",
                                                         .black_elo = "—",
                                                         .eco = "B00",
                                                         .opening_name = "King's Pawn",
                                                         .accessible_label = "label"});
    table.total = {.count = 10, .split = {.white = 40, .draw = 40, .black = 20}, .accessible_label = "total label"};

    motif::slint_app::publish_explorer(*test.window, table);

    auto const model = test.window->get_search_continuations();
    REQUIRE(model->row_count() == 1);
    auto const row = model->row_data(0).value_or(ContinuationRow {});
    CHECK(std::string_view {row.san.data(), row.san.size()} == "e4");
    CHECK(row.count == 7);
    CHECK(std::string_view {row.share_text.data(), row.share_text.size()} == "70%");
    CHECK(row.white_percent == 50);
    CHECK(row.draw_percent == 30);
    CHECK(row.black_percent == 20);
    CHECK(std::string_view {row.black_elo.data(), row.black_elo.size()} == "—");
    CHECK(std::string_view {row.accessible_label.data(), row.accessible_label.size()} == "label");
    auto const total = test.window->get_search_continuation_total();
    CHECK(total.count == 10);
    CHECK(total.white_percent == 40);
    CHECK(total.draw_percent == 40);
    CHECK(total.black_percent == 20);
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

// NOLINTEND(portability-template-virtual-member-function)
