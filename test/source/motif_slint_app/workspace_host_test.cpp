#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "motif/slint_app/workspace_host.hpp"

#include <catch2/catch_test_macros.hpp>
#include <slint.h>

#include "manual_event_loop.hpp"
#include "motif/slint_app/analysis_host.hpp"
#include "motif/slint_app/board_host.hpp"
#include "motif/slint_app/browser_host.hpp"
#include "motif/slint_app/search_host.hpp"
#include "snapshot_platform.hpp"
#include "workspace.h"

namespace
{

constexpr auto no_delay = std::chrono::milliseconds {0};
constexpr auto missing_game_id = motif::db::game_id {999999};
constexpr auto error_search_hash = motif::db::zobrist_hash {123456789};

auto unique_config_root() -> std::filesystem::path
{
    static auto suffix = std::size_t {0};
    return std::filesystem::temp_directory_path() / ("motif-workspace-host-test-" + std::to_string(++suffix));
}

struct workspace_host_fixture
{
    workspace_host_fixture(workspace_host_fixture const&) = delete;
    auto operator=(workspace_host_fixture const&) -> workspace_host_fixture& = delete;
    workspace_host_fixture(workspace_host_fixture&&) = delete;
    auto operator=(workspace_host_fixture&&) -> workspace_host_fixture& = delete;
    test_support::manual_event_loop loop;
    slint::ComponentHandle<WorkspaceWindow> window {WorkspaceWindow::create()};
    std::filesystem::path config_root {unique_config_root()};
    std::filesystem::path config_path {config_root / "config.json"};
    motif::slint_app::workspace_service service {config_path};
    motif::slint_app::async_board_runner board {window};
    motif::slint_app::async_browser_runner browser {window, loop.environment()};
    motif::slint_app::async_search_runner search {window, loop.environment()};
    motif::slint_app::async_analysis_runner analysis {
        window, service, loop.environment(), [this]() -> std::optional<std::string> { return board.current_fen(); }};
    std::vector<motif::slint_app::workspace_attach_step> steps;
    bool browser_busy_at_load {false};
    motif::slint_app::replace_controller confirmation {motif::slint_app::make_replace_confirmation(window, board)};
    bool search_ready_at_board_attach {false};
    bool follow_started_at_board_attach {false};
    bool board_empty_at_search_attach {false};
    bool board_handler_saw_search_busy {false};
    bool board_handler_saw_browser_busy {false};
    motif::slint_app::async_workspace_runner runner {service,
                                                     window,
                                                     browser,
                                                     board,
                                                     search,
                                                     analysis,
                                                     loop.environment(),
                                                     [this](motif::slint_app::workspace_attach_step const step) -> void
                                                     {
                                                         steps.push_back(step);
                                                         if (step == motif::slint_app::workspace_attach_step::browser_loaded) {
                                                             browser_busy_at_load = browser.is_busy();
                                                         }
                                                         if (step == motif::slint_app::workspace_attach_step::search_attached) {
                                                             board_empty_at_search_attach = !board.current_hash().has_value();
                                                         }
                                                         if (step == motif::slint_app::workspace_attach_step::board_attached) {
                                                             search_ready_at_board_attach = !window->get_search_rows_stale();
                                                             follow_started_at_board_attach =
                                                                 search.is_busy() || !window->get_search_searching();
                                                         }
                                                     }};

    workspace_host_fixture()
    {
        std::filesystem::create_directories(config_root);
        board.add_position_changed_handler([this](motif::db::zobrist_hash const hash) -> void
                                           { motif::slint_app::follow_board_position(search, browser, hash); });
        board.add_position_changed_handler(
            [this](motif::db::zobrist_hash const) -> void
            {
                board_handler_saw_search_busy = search.is_busy();
                board_handler_saw_browser_busy = browser.is_busy();
            });
        auto const idle = motif::slint_app::activity_probe {[]() -> motif::slint_app::workspace_activity { return {}; }};
        motif::slint_app::register_board_callbacks(*window, board, idle);
        motif::slint_app::register_play_callbacks(*window, board, idle, confirmation);
        motif::slint_app::register_workspace_callbacks(*window, service, runner, browser, board, search);
    }

    ~workspace_host_fixture() { std::filesystem::remove_all(config_root); }

    void drain_workspace()
    {
        while (runner.is_busy()) {
            loop.wait_for_tasks(no_delay, 1);
            (void)loop.run(no_delay);
        }
    }

    void settle_browser()
    {
        while (browser.is_busy()) {
            browser.wait_for_worker();
            (void)loop.run(no_delay);
        }
    }

    void close_workspace()
    {
        settle_browser();
        browser.detach();
        search.detach();
        service.close_active();
    }
};

}  // namespace

TEST_CASE("workspace host: publishes an inactive workspace", "[motif-slint-app]")

{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto window = WorkspaceWindow::create();
    auto service = motif::slint_app::workspace_service {};

    motif::slint_app::publish_workspace(*window, service);

    CHECK_FALSE(window->get_has_database());
    CHECK_FALSE(window->get_is_scratch());
}

TEST_CASE("workspace host: scratch succeeds with the frozen attach order", "[motif-slint-app]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto test = workspace_host_fixture {};

    REQUIRE(test.runner.run_scratch() == motif::slint_app::async_workspace_runner::start_outcome::accepted);
    test.drain_workspace();

    CHECK(test.service.has_active());
    CHECK(test.steps
          == std::vector {motif::slint_app::workspace_attach_step::analysis_changed,
                          motif::slint_app::workspace_attach_step::browser_attached,
                          motif::slint_app::workspace_attach_step::browser_loaded,
                          motif::slint_app::workspace_attach_step::search_attached,
                          motif::slint_app::workspace_attach_step::board_attached});
    CHECK(test.browser_busy_at_load);
    CHECK(test.follow_started_at_board_attach);
    CHECK(test.board_empty_at_search_attach);
    CHECK(test.board_handler_saw_search_busy);
    CHECK(test.board_handler_saw_browser_busy);
}

TEST_CASE("workspace host: busy start is rejected and missing open recovers", "[motif-slint-app]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto test = workspace_host_fixture {};

    REQUIRE(test.runner.run_scratch() == motif::slint_app::async_workspace_runner::start_outcome::accepted);
    CHECK(test.runner.run_scratch() == motif::slint_app::async_workspace_runner::start_outcome::busy);
    test.drain_workspace();

    REQUIRE(test.runner.run_open("/definitely/not/a/motif/bundle") == motif::slint_app::async_workspace_runner::start_outcome::accepted);
    test.drain_workspace();
    CHECK_FALSE(test.window->get_error_text().empty());
    CHECK_FALSE(test.runner.is_busy());
}

TEST_CASE("workspace host: create and open run through the injected loop", "[motif-slint-app]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto test = workspace_host_fixture {};
    auto const bundle = std::filesystem::temp_directory_path() / "motif-workspace-host-created";
    std::filesystem::remove_all(bundle);

    REQUIRE(test.runner.run_create(bundle.string(), "Created") == motif::slint_app::async_workspace_runner::start_outcome::accepted);
    test.drain_workspace();
    CHECK(test.service.has_active());
    CHECK(test.window->get_display_name() == "Created");

    test.close_workspace();
    REQUIRE(test.runner.run_open(bundle.string()) == motif::slint_app::async_workspace_runner::start_outcome::accepted);
    test.drain_workspace();
    CHECK(test.service.has_active());
    std::filesystem::remove_all(bundle);
}

TEST_CASE("workspace host: recent removal is ignored while busy and applied when idle", "[motif-slint-app]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto test = workspace_host_fixture {};
    auto const bundle = std::filesystem::temp_directory_path() / "motif-workspace-host-recent";
    std::filesystem::remove_all(bundle);
    REQUIRE(test.runner.run_create(bundle.string(), "Recent") == motif::slint_app::async_workspace_runner::start_outcome::accepted);
    test.drain_workspace();
    REQUIRE(test.service.recent_databases().size() == 1);

    REQUIRE(test.runner.run_scratch() == motif::slint_app::async_workspace_runner::start_outcome::accepted);
    test.window->invoke_recent_removed(slint::SharedString {bundle.string()});
    CHECK(test.service.recent_databases().size() == 1);
    test.drain_workspace();
    test.window->invoke_recent_removed(slint::SharedString {bundle.string()});
    CHECK(test.service.recent_databases().empty());
    std::filesystem::remove_all(bundle);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace host: dismiss error clears real browser board search and window errors", "[motif-slint-app]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto test = workspace_host_fixture {};
    REQUIRE(test.runner.run_scratch() == motif::slint_app::async_workspace_runner::start_outcome::accepted);
    test.drain_workspace();
    test.settle_browser();
    test.window->invoke_board_play_san("not-a-move");
    auto unindexed_created = motif::db::database_manager::create_scratch();
    REQUIRE(unindexed_created);
    auto unindexed = std::move(*unindexed_created);
    auto game = motif::db::game {};
    game.white.name = "Error White";
    game.black.name = "Error Black";
    game.result = "1-0";
    REQUIRE(unindexed.insert_game(game));
    test.browser.attach(unindexed);
    test.browser.load_initial();
    test.browser.wait_for_worker();
    (void)test.loop.run(no_delay);
    test.browser.activate_by_id(missing_game_id, 0);
    test.browser.wait_for_worker();
    (void)test.loop.run(no_delay);
    while (test.search.is_busy()) {
        test.loop.wait_for_tasks(no_delay, 1);
        (void)test.loop.run(no_delay);
    }

    test.search.attach(unindexed);
    test.search.search(error_search_hash, {});
    test.loop.wait_for_tasks(no_delay, 1);
    (void)test.loop.run(no_delay);
    test.window->set_error_text("error");
    REQUIRE_FALSE(test.window->get_board_error_text().empty());
    REQUIRE_FALSE(test.window->get_search_error_text().empty());
    REQUIRE_FALSE(test.window->get_browser_error_text().empty());
    test.window->invoke_dismiss_error();
    CHECK(test.window->get_error_text().empty());
    CHECK(test.window->get_board_error_text().empty());
    CHECK(test.window->get_search_error_text().empty());
    CHECK(test.window->get_browser_error_text().empty());
}

TEST_CASE("workspace host: preset and notation callbacks persist preferences", "[motif-slint-app]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto test = workspace_host_fixture {};
    test.window->invoke_preset_selected(WorkspacePreset::Analysis);
    CHECK(test.service.preset() == motif::slint_app::workspace_preset_kind::analysis);

    auto const before = test.window->get_notation_below();
    test.window->invoke_notation_layout_toggled();
    CHECK(test.window->get_notation_below() != before);
    CHECK(test.service.notation_below() == test.window->get_notation_below());
}

TEST_CASE("workspace host: failed preference saves roll back visible preset and notation", "[motif-slint-app]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto test = workspace_host_fixture {};
    std::filesystem::remove_all(test.config_root);
    {
        auto blocker = std::ofstream {test.config_root};
        blocker << "blocking file";
    }

    test.window->set_workspace_preset(WorkspacePreset::Analysis);
    test.window->invoke_preset_selected(WorkspacePreset::Analysis);
    CHECK(test.window->get_workspace_preset() == WorkspacePreset::Review);
    CHECK(test.window->get_error_text() == "Could not save workspace preset preference.");

    auto const notation_before = test.window->get_notation_below();
    test.window->invoke_notation_layout_toggled();
    CHECK(test.window->get_notation_below() == notation_before);
    CHECK(test.window->get_error_text() == "Could not save workspace layout preference.");
}

TEST_CASE("workspace host: queued completion after window invalidation is ignored", "[motif-slint-app]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto loop = test_support::manual_event_loop {};
    auto window = std::optional<slint::ComponentHandle<WorkspaceWindow>> {};
    window.emplace(WorkspaceWindow::create());
    auto service = motif::slint_app::workspace_service {};
    auto board = motif::slint_app::async_board_runner {*window};
    auto browser = motif::slint_app::async_browser_runner {*window, loop.environment()};
    auto search = motif::slint_app::async_search_runner {*window, loop.environment()};
    auto analysis = motif::slint_app::async_analysis_runner {
        *window, service, loop.environment(), [&board]() -> std::optional<std::string> { return board.current_fen(); }};
    auto runner = motif::slint_app::async_workspace_runner {service, *window, browser, board, search, analysis, loop.environment()};
    REQUIRE(runner.run_scratch() == motif::slint_app::async_workspace_runner::start_outcome::accepted);
    loop.wait_for_tasks(no_delay, 1);
    window.reset();
    (void)loop.run(no_delay);
    // Existing lifetime semantics deliberately leave busy set when a queued
    // completion loses its window. Destruction subsequently joins safely.
    CHECK(runner.is_busy());
}

TEST_CASE("workspace host: destroyed runner ignores queued completion with window alive", "[motif-slint-app]")
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
    auto const display_before = window->get_display_name();
    auto steps = std::size_t {0};
    {
        auto runner =
            motif::slint_app::async_workspace_runner {service,
                                                      window,
                                                      browser,
                                                      board,
                                                      search,
                                                      analysis,
                                                      loop.environment(),
                                                      [&steps](motif::slint_app::workspace_attach_step const) -> void { ++steps; }};
        REQUIRE(runner.run_scratch() == motif::slint_app::async_workspace_runner::start_outcome::accepted);
        loop.wait_for_tasks(no_delay, 1);
    }
    (void)loop.run(no_delay);
    // The service operation already activated scratch on its worker; the queued
    // host completion must not attach collaborators or republish the window.
    CHECK(service.has_active());
    CHECK(steps == 0);
    CHECK(window->get_display_name() == display_before);
    CHECK_FALSE(window->get_has_database());
}
