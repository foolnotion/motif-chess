#include <array>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <thread>

#include "motif/slint_app/activity_gate.hpp"

#include <catch2/catch_test_macros.hpp>
#include <pthread.h>
#include <sys/stat.h>

#include "manual_event_loop.hpp"
#include "motif/slint_app/analysis_host.hpp"
#include "motif/slint_app/board_host.hpp"
#include "motif/slint_app/browser_host.hpp"
#include "motif/slint_app/import_service.hpp"
#include "motif/slint_app/search_host.hpp"
#include "motif/slint_app/workspace_host.hpp"
#include "snapshot_platform.hpp"

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
    auto const platform = snapshot_test::use_snapshot_platform();
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

TEST_CASE("activity gate: concrete idle composition has independent flags", "[motif-slint-app]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto loop = test_support::manual_event_loop {};
    auto window = WorkspaceWindow::create();
    auto importer = motif::slint_app::import_service {};
    auto service = motif::slint_app::workspace_service {};
    auto board = motif::slint_app::async_board_runner {window};
    auto browser = motif::slint_app::async_browser_runner {window, loop.environment()};
    auto search = motif::slint_app::async_search_runner {window, loop.environment()};
    auto analysis = motif::slint_app::async_analysis_runner {
        window, service, loop.environment(), [&board]() -> std::optional<std::string> { return board.current_fen(); }};
    auto workspace = motif::slint_app::async_workspace_runner {service, window, browser, board, search, analysis, loop.environment()};

    auto const state = motif::slint_app::current_activity(importer, workspace, browser, search);
    CHECK_FALSE(state.import_active);
    CHECK_FALSE(state.workspace_busy);
    CHECK_FALSE(state.browser_busy);
    CHECK_FALSE(state.search_busy);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("activity gate: FIFO import maps only the concrete import flag", "[motif-slint-app]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto loop = test_support::manual_event_loop {};
    auto window = WorkspaceWindow::create();
    auto service = motif::slint_app::workspace_service {};
    auto board = motif::slint_app::async_board_runner {window};
    auto browser = motif::slint_app::async_browser_runner {window, loop.environment()};
    auto search = motif::slint_app::async_search_runner {window, loop.environment()};
    auto analysis = motif::slint_app::async_analysis_runner {
        window, service, loop.environment(), [&board]() -> std::optional<std::string> { return board.current_fen(); }};
    auto workspace = motif::slint_app::async_workspace_runner {service, window, browser, board, search, analysis, loop.environment()};
    auto created = motif::db::database_manager::create_scratch();
    REQUIRE(created);
    auto database = std::move(*created);
    auto const fifo = std::filesystem::temp_directory_path() / "motif-activity-import.fifo";
    std::filesystem::remove(fifo);
    REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
    auto importer = motif::slint_app::import_service {};
    REQUIRE(importer.start(&database, fifo));
    REQUIRE(importer.active());

    auto const activity = motif::slint_app::current_activity(importer, workspace, browser, search);
    CHECK(activity.import_active);
    CHECK_FALSE(activity.workspace_busy);
    CHECK_FALSE(activity.browser_busy);
    CHECK_FALSE(activity.search_busy);

    auto writer = std::thread {[fifo]() -> void
                               {
                                   auto blocked_signals = sigset_t {};
                                   static_cast<void>(sigemptyset(&blocked_signals));
                                   static_cast<void>(sigaddset(&blocked_signals, SIGPIPE));
                                   static_cast<void>(pthread_sigmask(SIG_BLOCK, &blocked_signals, nullptr));
                                   auto stream = std::ofstream {fifo};
                                   stream << "[Event \"FIFO\"]\n[Site \"Local\"]\n[Date \"2026.10.05\"]\n[Round \"1\"]\n[White "
                                             "\"White\"]\n[Black \"Black\"]\n[Result \"1-0\"]\n\n1. e4 e5 1-0\n";
                               }};
    writer.join();
    importer.request_cancel();
    std::filesystem::remove(fifo);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("activity gate: workspace runner maps only workspace busy", "[motif-slint-app]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto loop = test_support::manual_event_loop {};
    auto window = WorkspaceWindow::create();
    auto importer = motif::slint_app::import_service {};
    auto service = motif::slint_app::workspace_service {};
    auto board = motif::slint_app::async_board_runner {window};
    auto browser = motif::slint_app::async_browser_runner {window, loop.environment()};
    auto search = motif::slint_app::async_search_runner {window, loop.environment()};
    auto analysis = motif::slint_app::async_analysis_runner {
        window, service, loop.environment(), [&board]() -> std::optional<std::string> { return board.current_fen(); }};
    auto workspace = motif::slint_app::async_workspace_runner {service, window, browser, board, search, analysis, loop.environment()};
    REQUIRE(workspace.run_scratch() == motif::slint_app::async_workspace_runner::start_outcome::accepted);

    auto const active = motif::slint_app::current_activity(importer, workspace, browser, search);
    CHECK_FALSE(active.import_active);
    CHECK(active.workspace_busy);
    CHECK_FALSE(active.browser_busy);
    CHECK_FALSE(active.search_busy);
    loop.wait_for_tasks(std::chrono::milliseconds {0}, 1);
    (void)loop.run(std::chrono::milliseconds {0});
    if (browser.is_busy()) {
        browser.wait_for_worker();
        (void)loop.run(std::chrono::milliseconds {0});
    }
    if (search.is_busy()) {
        loop.wait_for_tasks(std::chrono::milliseconds {0}, 1);
        (void)loop.run(std::chrono::milliseconds {0});
    }
    CHECK(motif::slint_app::current_activity(importer, workspace, browser, search).idle());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("activity gate: browser and search runners map their own busy flags", "[motif-slint-app]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto loop = test_support::manual_event_loop {};
    auto window = WorkspaceWindow::create();
    auto importer = motif::slint_app::import_service {};
    auto service = motif::slint_app::workspace_service {};
    auto board = motif::slint_app::async_board_runner {window};
    auto browser = motif::slint_app::async_browser_runner {window, loop.environment()};
    auto search = motif::slint_app::async_search_runner {window, loop.environment()};
    auto analysis = motif::slint_app::async_analysis_runner {
        window, service, loop.environment(), [&board]() -> std::optional<std::string> { return board.current_fen(); }};
    auto workspace = motif::slint_app::async_workspace_runner {service, window, browser, board, search, analysis, loop.environment()};
    auto created = motif::db::database_manager::create_scratch();
    REQUIRE(created);
    auto database = std::move(*created);
    board.attach();
    browser.attach(database);
    browser.load_initial();
    auto browser_activity = motif::slint_app::current_activity(importer, workspace, browser, search);
    CHECK_FALSE(browser_activity.import_active);
    CHECK_FALSE(browser_activity.workspace_busy);
    CHECK(browser_activity.browser_busy);
    CHECK_FALSE(browser_activity.search_busy);
    browser.wait_for_worker();
    (void)loop.run(std::chrono::milliseconds {0});
    CHECK(motif::slint_app::current_activity(importer, workspace, browser, search).idle());

    search.attach(database);
    auto const board_hash = board.current_hash();
    REQUIRE(board_hash);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    search.search(*board_hash, browser.active_filter());
    auto search_activity = motif::slint_app::current_activity(importer, workspace, browser, search);
    CHECK_FALSE(search_activity.import_active);
    CHECK_FALSE(search_activity.workspace_busy);
    CHECK_FALSE(search_activity.browser_busy);
    CHECK((search_activity.search_busy || !window->get_search_searching()));
    if (search.is_busy()) {
        loop.wait_for_tasks(std::chrono::milliseconds {0}, 1);
        (void)loop.run(std::chrono::milliseconds {0});
    }
    CHECK(motif::slint_app::current_activity(importer, workspace, browser, search).idle());
}
