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

TEST_CASE("workspace host: injected event loop has the production scheduling contract", "[motif-slint-app]")
{
    auto loop = test_support::manual_event_loop {};
    CHECK(loop.environment().schedule != nullptr);
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

TEST_CASE("workspace host: dismiss error clears window and search state", "[motif-slint-app]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto test = workspace_host_fixture {};
    test.window->set_error_text("error");
    test.window->invoke_dismiss_error();
    CHECK(test.window->get_error_text().empty());
    CHECK(test.window->get_search_error_text().empty());
}

TEST_CASE("workspace host: preset and notation callbacks persist preferences", "[motif-slint-app]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto test = workspace_host_fixture {};
    test.window->invoke_preset_selected(WorkspacePreset::Analysis);
    CHECK(test.service.preset() == motif::slint_app::workspace_preset_kind::analysis);

    auto const before = test.window->get_notation_below();
    test.window->invoke_notation_layout_toggled();
    CHECK(test.service.notation_below() != before);
    CHECK(test.window->get_notation_below() != before);
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

    test.window->invoke_preset_selected(WorkspacePreset::Analysis);
    CHECK(test.window->get_workspace_preset() == WorkspacePreset::Review);
    CHECK(test.window->get_error_text() == "Could not save workspace preset preference.");

    auto const notation_before = test.window->get_notation_below();
    test.window->invoke_notation_layout_toggled();
    CHECK(test.window->get_notation_below() == notation_before);
    CHECK(test.window->get_error_text() == "Could not save workspace layout preference.");
}
