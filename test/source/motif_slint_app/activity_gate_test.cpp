#include <array>
#include <chrono>
#include <functional>
#include <optional>
#include <string>

#include "motif/slint_app/activity_gate.hpp"

#include <catch2/catch_test_macros.hpp>

#include "motif/slint_app/analysis_host.hpp"
#include "motif/slint_app/board_host.hpp"
#include "motif/slint_app/browser_host.hpp"
#include "motif/slint_app/import_service.hpp"
#include "motif/slint_app/search_host.hpp"
#include "motif/slint_app/workspace_host.hpp"

namespace
{

using motif::slint_app::workspace_activity;

// One activity per flag, each with only that flag set.
constexpr auto busy_cases = std::array {
    workspace_activity {.import_active = true},
    workspace_activity {.workspace_busy = true},
    workspace_activity {.browser_busy = true},
    workspace_activity {.search_busy = true},
};

}  // namespace

TEST_CASE("activity gate: SAN entry acts when the workspace is idle", "[motif-slint-app]")
{
    auto presenter = motif::slint_app::board_presenter {};

    auto const ran = motif::slint_app::run_when_idle(workspace_activity {}, [&presenter]() -> void { (void)presenter.play_san("e4"); });

    CHECK(ran);
    CHECK(presenter.state().total_plies == 1);
    CHECK(presenter.state().unsaved);
}

TEST_CASE("activity gate: SAN entry is rejected under every busy condition and changes nothing", "[motif-slint-app]")
{
    for (auto const& activity : busy_cases) {
        auto presenter = motif::slint_app::board_presenter {};
        auto const fen = presenter.state().fen;

        auto const ran = motif::slint_app::run_when_idle(activity, [&presenter]() -> void { (void)presenter.play_san("e4"); });

        CHECK(!ran);
        CHECK(presenter.state().fen == fen);
        CHECK(presenter.state().total_plies == 0);
        CHECK(!presenter.state().unsaved);
    }
}

TEST_CASE("activity gate: only an all-clear activity is idle", "[motif-slint-app]")
{
    // Catch2's enum flags intentionally combine bit values; clang-tidy's analyzer
    // incorrectly diagnoses that third-party expression.
    // NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
    CHECK(workspace_activity {}.idle());
    for (auto const& activity : busy_cases) {
        CHECK_FALSE(activity.idle());
    }
    CHECK_FALSE((workspace_activity {.import_active = true, .search_busy = true}).idle());
    // NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
}

TEST_CASE("activity gate: the concrete composition starts idle", "[motif-slint-app]")
{
    auto const environment = motif::slint_app::event_loop_environment {
        .schedule = [](std::chrono::milliseconds const, std::function<void()> const& task) -> void { task(); }};
    auto window = WorkspaceWindow::create();
    auto importer = motif::slint_app::import_service {};
    auto service = motif::slint_app::workspace_service {};
    auto board = motif::slint_app::async_board_runner {window};
    auto browser = motif::slint_app::async_browser_runner {window, environment};
    auto search = motif::slint_app::async_search_runner {window, environment};
    auto analysis = motif::slint_app::async_analysis_runner {
        window, service, environment, [&board]() -> std::optional<std::string> { return board.current_fen(); }};
    auto workspace = motif::slint_app::async_workspace_runner {service, window, browser, board, search, analysis, environment};

    auto const activity = motif::slint_app::current_activity(importer, workspace, browser, search);
    CHECK(activity.idle());
    CHECK_FALSE(activity.import_active);
    CHECK_FALSE(activity.workspace_busy);
    CHECK_FALSE(activity.browser_busy);
    CHECK_FALSE(activity.search_busy);
}
