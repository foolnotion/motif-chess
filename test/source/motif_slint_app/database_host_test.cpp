#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "motif/slint_app/database_host.hpp"

#include <catch2/catch_test_macros.hpp>
#include <slint.h>

#include "motif/slint_app/activity_gate.hpp"
#include "motif/slint_app/board_host.hpp"
#include "motif/slint_app/replace_controller.hpp"
#include "snapshot_platform.hpp"
#include "workspace.h"

namespace
{

using motif::slint_app::workspace_activity;

// Every busy condition on its own.
constexpr auto busy_conditions = std::array {
    workspace_activity {.import_active = true},
    workspace_activity {.workspace_busy = true},
    workspace_activity {.browser_busy = true},
    workspace_activity {.search_busy = true},
};

// The five ways of leaving the current database, as the window raises them.
enum class action : std::uint8_t
{
    create,
    open,
    recent,
    scratch,
    close,
};

constexpr auto every_action = std::array {action::create, action::open, action::recent, action::scratch, action::close};

// The production registrar wired to a real window and board, with fake
// workspace actions that record what they were asked to do.
struct host
{
    slint::ComponentHandle<WorkspaceWindow> window {WorkspaceWindow::create()};
    motif::slint_app::async_board_runner board {window};
    motif::slint_app::replace_controller confirmation {motif::slint_app::make_replace_confirmation(window, board)};
    workspace_activity activity;
    bool work_starts {true};
    std::vector<std::string> calls;

    host()
    {
        board.attach();
        auto const probe = motif::slint_app::activity_probe {[this]() -> workspace_activity { return activity; }};
        motif::slint_app::register_play_callbacks(*window, board, probe, confirmation);
        motif::slint_app::register_database_callbacks(*window,
                                                      probe,
                                                      confirmation,
                                                      motif::slint_app::database_actions {
                                                          .create = [this](std::string const& dir_path, std::string const& name) -> bool
                                                          {
                                                              calls.push_back("create " + dir_path + "|" + name);
                                                              return work_starts;
                                                          },
                                                          .open = [this](std::string const& path) -> bool
                                                          {
                                                              calls.push_back("open " + path);
                                                              return work_starts;
                                                          },
                                                          .scratch = [this]() -> bool
                                                          {
                                                              calls.emplace_back("scratch");
                                                              return work_starts;
                                                          },
                                                          .close = [this]() -> void { calls.emplace_back("close"); },
                                                      });
    }

    // Puts a move on the board so that replacing it needs the user's confirmation.
    void edit_board()
    {
        window->invoke_board_play_san("e4");
        REQUIRE(window->get_board_edited());
    }

    void invoke(action const which)
    {
        switch (which) {
            case action::create:
                window->invoke_create_requested("/data/dbs", "club");
                return;
            case action::open:
                window->invoke_open_requested("/data/dbs/club");
                return;
            case action::recent:
                window->invoke_recent_activated("/data/dbs/recent");
                return;
            case action::scratch:
                window->invoke_scratch_requested();
                return;
            case action::close:
                window->invoke_close_requested();
                return;
        }
    }

    [[nodiscard]] auto error() const -> std::string
    {
        auto const text = window->get_error_text();
        return std::string {std::string_view {text.data(), text.size()}};
    }
};

}  // namespace

TEST_CASE("database host: with nothing to lose every action runs at once", "[motif-slint-app][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};

    test.window->invoke_create_requested("  /data/dbs  ", " club\n");
    test.window->invoke_open_requested(" /data/dbs/club ");
    test.window->invoke_recent_activated("/data/dbs/recent");
    test.window->invoke_scratch_requested();
    test.window->invoke_close_requested();

    CHECK(test.calls
          == std::vector<std::string> {"create /data/dbs|club", "open /data/dbs/club", "open /data/dbs/recent", "scratch", "close"});
    CHECK_FALSE(test.window->get_confirm_visible());
}

TEST_CASE("database host: the entered paths are published trimmed and the window turns busy when work starts", "[motif-slint-app][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};

    test.window->invoke_create_requested("  /data/dbs  ", " club\n");

    CHECK(test.window->get_create_path_text() == "/data/dbs");
    CHECK(test.window->get_create_name_text() == "club");
    CHECK(test.window->get_busy());
}

TEST_CASE("database host: a refused start leaves the window usable", "[motif-slint-app][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    test.work_starts = false;

    test.window->invoke_open_requested("/data/dbs/club");

    CHECK(test.calls.size() == 1);
    CHECK_FALSE(test.window->get_busy());
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("database host: an edited board holds the action until the user accepts", "[motif-slint-app][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    for (auto const which : every_action) {
        auto test = host {};
        test.edit_board();

        test.invoke(which);

        CHECK(test.window->get_confirm_visible());
        CHECK(test.calls.empty());

        test.window->invoke_confirm_accepted();

        CHECK_FALSE(test.window->get_confirm_visible());
        CHECK(test.calls.size() == 1);
        CHECK_FALSE(test.confirmation.pending());
    }
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("database host: every action is rejected under each busy condition and changes nothing", "[motif-slint-app][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    for (auto const& busy : busy_conditions) {
        for (auto const which : every_action) {
            auto test = host {};
            test.activity = busy;

            test.invoke(which);

            CHECK(test.calls.empty());
            CHECK_FALSE(test.window->get_confirm_visible());
            CHECK_FALSE(test.window->get_busy());
        }
    }
}

// The regression for the confirm-time race: the user opens the dialog while idle,
// an import (or any other activity) starts meanwhile, and accepting must not
// close or replace the database under it.
// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("database host: accepting after the workspace became busy does nothing and says why", "[motif-slint-app][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    for (auto const& busy : busy_conditions) {
        for (auto const which : every_action) {
            auto test = host {};
            test.edit_board();
            test.invoke(which);
            REQUIRE(test.window->get_confirm_visible());
            test.activity = busy;

            test.window->invoke_confirm_accepted();

            CHECK(test.calls.empty());
            CHECK_FALSE(test.window->get_confirm_visible());
            CHECK_FALSE(test.confirmation.pending());
            CHECK_FALSE(test.window->get_busy());
            CHECK(test.error().contains("became busy"));
            CHECK(test.window->get_board_edited());

            // Nothing is left half-armed: once idle again the same request works.
            test.activity = workspace_activity {};
            test.invoke(which);
            test.window->invoke_confirm_accepted();
            CHECK(test.calls.size() == 1);
        }
    }
}

TEST_CASE("database host: cancelling the confirmation never runs the action", "[motif-slint-app][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    test.edit_board();
    test.window->invoke_close_requested();

    test.window->invoke_confirm_cancelled();

    CHECK(test.calls.empty());
    CHECK_FALSE(test.window->get_confirm_visible());
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("database host: a queued launch import starts only when everything is clear", "[motif-slint-app][host]")
{
    using motif::slint_app::launch_import_state;
    using motif::slint_app::may_start_launch_import;

    CHECK(may_start_launch_import(launch_import_state {.database_open = true}));
    CHECK_FALSE(may_start_launch_import(launch_import_state {}));
    CHECK_FALSE(may_start_launch_import(launch_import_state {.completion_pending = true, .database_open = true}));
    CHECK_FALSE(may_start_launch_import(launch_import_state {.confirmation_pending = true, .database_open = true}));
    for (auto const& busy : busy_conditions) {
        CHECK_FALSE(may_start_launch_import(launch_import_state {.activity = busy, .database_open = true}));
    }
}

TEST_CASE("database host: an open confirmation holds back the launch import until it is answered", "[motif-slint-app][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    auto const ready = [&test]() -> bool
    {
        return motif::slint_app::may_start_launch_import(motif::slint_app::launch_import_state {
            .activity = test.activity, .confirmation_pending = test.confirmation.pending(), .database_open = true});
    };
    CHECK(ready());

    test.edit_board();
    test.window->invoke_close_requested();
    REQUIRE(test.confirmation.pending());
    CHECK_FALSE(ready());

    test.window->invoke_confirm_cancelled();
    CHECK(ready());
}
