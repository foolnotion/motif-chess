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
    motif::slint_app::replace_controller confirmation {motif::slint_app::make_replace_confirmation(window, board)};

    host()
    {
        board.attach();
        auto const probe = motif::slint_app::activity_probe {[this]() -> workspace_activity { return activity; }};
        motif::slint_app::register_board_callbacks(*window, board, probe);
        motif::slint_app::register_play_callbacks(*window, board, probe, confirmation);
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

// NOLINTEND(portability-template-virtual-member-function)
