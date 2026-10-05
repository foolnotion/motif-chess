#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "motif/slint_app/picker_host.hpp"

#include <catch2/catch_test_macros.hpp>
#include <slint.h>

#include "fake_uci_engine.hpp"
#include "motif/slint_app/activity_gate.hpp"
#include "motif/slint_app/board_host.hpp"
#include "motif/slint_app/event_loop.hpp"
#include "motif/slint_app/file_picker.hpp"
#include "snapshot_platform.hpp"
#include "workspace.h"

// Test values are plain numbers.
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
namespace
{

using motif::slint_app::async_picker_runner;
using motif::slint_app::picker_failure_text;
using motif::slint_app::picker_filter;
using motif::slint_app::picker_kind;
using motif::slint_app::picker_outcome;
using motif::slint_app::picker_shutdown_wait;
using motif::slint_app::picker_thread;
using motif::slint_app::scripted_file_picker;
using motif::slint_app::workspace_activity;

constexpr auto patience = std::chrono::seconds {10};
constexpr auto settle = std::chrono::milliseconds {150};

// The event loop of these tests: tasks the runner schedules wait here until a
// test runs them on the test thread, which plays the event-loop thread.
class manual_loop
{
  public:
    [[nodiscard]] auto environment() -> motif::slint_app::event_loop_environment
    {
        return motif::slint_app::event_loop_environment {.schedule = [this](std::chrono::milliseconds, std::function<void()> task) -> void
                                                         {
                                                             std::scoped_lock const lock {mutex_};
                                                             tasks_.push_back(std::move(task));
                                                         }};
    }

    [[nodiscard]] auto pending() const -> std::size_t
    {
        std::scoped_lock const lock {mutex_};
        return tasks_.size();
    }

    // Waits for the worker's answer to arrive, then runs it.
    auto run_when_posted() -> std::size_t
    {
        REQUIRE(fake_uci::wait_for([this]() -> bool { return pending() > 0; }));
        return run_all();
    }

    auto run_all() -> std::size_t
    {
        auto batch = std::vector<std::function<void()>> {};
        {
            std::scoped_lock const lock {mutex_};
            batch.swap(tasks_);
        }
        for (auto& task : batch) {
            task();
        }
        return batch.size();
    }

  private:
    mutable std::mutex mutex_;
    std::vector<std::function<void()>> tasks_;
};

struct temp_directory
{
    std::filesystem::path path {std::filesystem::temp_directory_path() / ("motif_picker_host_" + fake_uci::unique_stamp())};

    temp_directory()
    {
        std::filesystem::create_directories(path / "bundle");
        std::ofstream {path / "games.pgn"} << "1. e4 *\n";
    }

    ~temp_directory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    temp_directory(temp_directory const&) = delete;
    auto operator=(temp_directory const&) -> temp_directory& = delete;
    temp_directory(temp_directory&&) = delete;
    auto operator=(temp_directory&&) -> temp_directory& = delete;
};

auto text_of(slint::SharedString const& value) -> std::string
{
    return std::string {std::string_view {value.data(), value.size()}};
}

auto choose_script(std::string path) -> scripted_file_picker::script
{
    return {.behaviour = scripted_file_picker::mode::choose, .text = std::move(path)};
}

auto hang_script(bool const abortable) -> scripted_file_picker::script
{
    return {.behaviour = scripted_file_picker::mode::hang, .abortable = abortable};
}

// The production registrar and runner on a real window, with the picker and the
// event loop the test chooses.
struct host
{
    // Slint allows one platform per process; the first window needs it installed.
    std::shared_ptr<snapshot_test::snapshot_state> platform {snapshot_test::use_snapshot_platform()};
    temp_directory directory;
    slint::ComponentHandle<WorkspaceWindow> window {WorkspaceWindow::create()};
    manual_loop loop;
    std::shared_ptr<scripted_file_picker> picker;
    std::unique_ptr<async_picker_runner> runner;
    // Everything a path field's neighbours could submit.
    std::vector<std::string> submitted;

    explicit host(scripted_file_picker::script const& script)
        : picker {std::make_shared<scripted_file_picker>(script)}
        , runner {std::make_unique<async_picker_runner>(window, loop.environment(), picker)}
    {
        motif::slint_app::register_picker_callbacks(*window, *runner);
        window->on_open_requested([this](slint::SharedString const&) -> void { submitted.emplace_back("open"); });
        window->on_create_requested([this](slint::SharedString const&, slint::SharedString const&) -> void
                                    { submitted.emplace_back("create"); });
        window->on_import_requested([this](slint::SharedString const&) -> void { submitted.emplace_back("import"); });
        window->on_analysis_engine_added([this](slint::SharedString const&) -> void { submitted.emplace_back("add-engine"); });
    }

    ~host()
    {
        runner.reset();
        // Release a dialog thread the test left blocked.
        picker->finish(picker_outcome::cancelled());
    }

    host(host const&) = delete;
    auto operator=(host const&) -> host& = delete;
    host(host&&) = delete;
    auto operator=(host&&) -> host& = delete;

    [[nodiscard]] auto error() const -> std::string { return text_of(window->get_error_text()); }

    // Presses Browse for the Open field and waits for the dialog to be entered.
    void open_dialog(BrowseTarget const target = BrowseTarget::DatabaseOpen)
    {
        window->invoke_browse_requested(target);
        REQUIRE(picker->wait_for_calls(1, patience));
    }
};

struct target_case
{
    BrowseTarget target;
    char const* label;
    picker_kind kind;
    std::vector<picker_filter> filters;
    std::function<void(WorkspaceWindow&, std::string const&)> set_text;
    std::function<std::string(WorkspaceWindow&)> get_text;
};

auto target_cases() -> std::vector<target_case>
{
    return {
        {BrowseTarget::DatabaseOpen,
         "open database",
         picker_kind::folder,
         {},
         [](WorkspaceWindow& window, std::string const& text) -> void { window.set_open_path_text(slint::SharedString {text}); },
         [](WorkspaceWindow& window) -> std::string { return text_of(window.get_open_path_text()); }},
        {BrowseTarget::DatabaseCreate,
         "create database",
         picker_kind::folder,
         {},
         [](WorkspaceWindow& window, std::string const& text) -> void { window.set_create_path_text(slint::SharedString {text}); },
         [](WorkspaceWindow& window) -> std::string { return text_of(window.get_create_path_text()); }},
        {BrowseTarget::ImportPgn,
         "import PGN",
         picker_kind::file,
         {{.name = "PGN files", .spec = "pgn"}},
         [](WorkspaceWindow& window, std::string const& text) -> void { window.set_import_path_text(slint::SharedString {text}); },
         [](WorkspaceWindow& window) -> std::string { return text_of(window.get_import_path_text()); }},
        {BrowseTarget::Engine,
         "add engine",
         picker_kind::file,
         {},
         [](WorkspaceWindow& window, std::string const& text) -> void { window.set_analysis_engine_path_text(slint::SharedString {text}); },
         [](WorkspaceWindow& window) -> std::string { return text_of(window.get_analysis_engine_path_text()); }},
    };
}

}  // namespace

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("picker host: each Browse button opens its dialog seeded from its field and fills only that field", "[motif-slint-app][picker]")
{
    for (auto const& item : target_cases()) {
        INFO(item.label);
        auto test = host {choose_script("/chosen/path")};
        // A folder field seeds its own folder; a file field seeds the folder of the file.
        auto const typed = (test.directory.path / (item.kind == picker_kind::file ? "games.pgn" : "bundle")).string();
        item.set_text(*test.window, typed);
        test.window->set_create_name_text("My games");
        test.window->set_error_text("earlier message");

        test.window->invoke_browse_requested(item.target);
        CHECK(test.window->get_picker_open());
        CHECK(test.runner->open());
        REQUIRE(test.loop.run_when_posted() == 1);

        auto const requests = test.picker->requests();
        REQUIRE(requests.size() == 1);
        CHECK(requests.front().kind == item.kind);
        CHECK(requests.front().filters == item.filters);
        CHECK(requests.front().start_path == (item.kind == picker_kind::file ? test.directory.path.string() : typed));

        CHECK(item.get_text(*test.window) == "/chosen/path");
        CHECK_FALSE(test.window->get_picker_open());
        CHECK_FALSE(test.runner->open());
        CHECK(test.submitted.empty());
        CHECK(text_of(test.window->get_create_name_text()) == "My games");
        CHECK(test.error() == "earlier message");

        for (auto const& other : target_cases()) {
            if (other.target != item.target) {
                CHECK(other.get_text(*test.window).empty());
            }
        }
    }
}

TEST_CASE("picker host: a dialog runs on a worker thread, never on the event loop", "[motif-slint-app][picker]")
{
    auto test = host {choose_script("/chosen")};
    test.window->invoke_browse_requested(BrowseTarget::ImportPgn);
    REQUIRE(test.loop.run_when_posted() == 1);
    CHECK(test.picker->last_thread() != std::this_thread::get_id());
}

TEST_CASE("picker host: a platform that needs the event loop gets its dialog there, after the request returns", "[motif-slint-app][picker]")
{
    auto test = host {{.behaviour = scripted_file_picker::mode::choose, .text = "/chosen", .thread = picker_thread::event_loop}};
    test.window->invoke_browse_requested(BrowseTarget::ImportPgn);

    CHECK(test.window->get_picker_open());
    CHECK_FALSE(test.picker->wait_for_calls(1, settle));
    CHECK(text_of(test.window->get_import_path_text()).empty());

    REQUIRE(test.loop.run_all() == 1);
    CHECK(test.picker->last_thread() == std::this_thread::get_id());
    CHECK(text_of(test.window->get_import_path_text()) == "/chosen");
    CHECK_FALSE(test.window->get_picker_open());
    CHECK(test.loop.pending() == 0);
}

TEST_CASE("picker host: a field with no text lets the system choose where the dialog opens", "[motif-slint-app][picker]")
{
    auto test = host {choose_script("/chosen")};
    test.window->invoke_browse_requested(BrowseTarget::ImportPgn);
    REQUIRE(test.loop.run_when_posted() == 1);
    REQUIRE(test.picker->requests().size() == 1);
    CHECK(test.picker->requests().front().start_path.empty());
}

TEST_CASE("picker host: cancelling changes no field and shows nothing", "[motif-slint-app][picker]")
{
    auto test = host {{.behaviour = scripted_file_picker::mode::cancel}};
    test.window->set_open_path_text("/typed/by/hand");
    test.window->set_error_text("earlier message");

    test.window->invoke_browse_requested(BrowseTarget::DatabaseOpen);
    CHECK(test.window->get_picker_open());
    REQUIRE(test.loop.run_when_posted() == 1);

    CHECK_FALSE(test.window->get_picker_open());
    CHECK(text_of(test.window->get_open_path_text()) == "/typed/by/hand");
    CHECK(test.error() == "earlier message");
    CHECK(test.submitted.empty());
}

TEST_CASE("picker host: a dialog that cannot open shows the reason and leaves the typed path alone", "[motif-slint-app][picker]")
{
    auto test = host {{.behaviour = scripted_file_picker::mode::fail, .text = "The name org.freedesktop.portal.Desktop was not provided"}};
    test.window->set_open_path_text("/typed/by/hand");

    test.window->invoke_browse_requested(BrowseTarget::DatabaseOpen);
    REQUIRE(test.loop.run_when_posted() == 1);

    CHECK(test.error()
          == "The file dialog is not available: The name org.freedesktop.portal.Desktop was not provided. Type the path instead.");
    CHECK(text_of(test.window->get_open_path_text()) == "/typed/by/hand");
    CHECK_FALSE(test.window->get_picker_open());
    CHECK(test.submitted.empty());

    // The next press tries again: a portal that appeared meanwhile works without a restart.
    test.window->invoke_browse_requested(BrowseTarget::DatabaseOpen);
    REQUIRE(test.loop.run_when_posted() == 1);
    CHECK(test.picker->requests().size() == 2);
}

TEST_CASE("picker failure text names the reason and the fallback", "[motif-slint-app][picker]")
{
    CHECK(picker_failure_text("The name org.freedesktop.portal.Desktop was not provided")
          == "The file dialog is not available: The name org.freedesktop.portal.Desktop was not provided. Type the path instead.");
    CHECK(picker_failure_text("No portal.\n") == "The file dialog is not available: No portal. Type the path instead.");
    CHECK(picker_failure_text("") == "The file dialog is not available. Type the path instead.");
}

TEST_CASE("picker host: a request while a dialog is open is ignored", "[motif-slint-app][picker]")
{
    auto test = host {hang_script(/*abortable=*/true)};
    test.open_dialog(BrowseTarget::DatabaseOpen);
    CHECK(test.runner->open());

    test.window->invoke_browse_requested(BrowseTarget::ImportPgn);
    test.window->invoke_browse_requested(BrowseTarget::Engine);
    CHECK_FALSE(test.picker->wait_for_calls(2, settle));
    CHECK(test.picker->requests().size() == 1);
    CHECK(test.loop.pending() == 0);

    test.picker->finish(picker_outcome::chosen("/first"));
    REQUIRE(test.loop.run_when_posted() == 1);
    CHECK(text_of(test.window->get_open_path_text()) == "/first");
    CHECK(text_of(test.window->get_import_path_text()).empty());
    CHECK(text_of(test.window->get_analysis_engine_path_text()).empty());

    // Once answered, the next dialog opens.
    test.window->invoke_browse_requested(BrowseTarget::Engine);
    CHECK(test.picker->wait_for_calls(2, patience));
}

TEST_CASE("picker host: the answer replaces text typed while the dialog was open", "[motif-slint-app][picker]")
{
    auto test = host {hang_script(/*abortable=*/true)};
    test.window->set_import_path_text("/before");
    test.open_dialog(BrowseTarget::ImportPgn);

    test.window->set_import_path_text("/typed/meanwhile");
    test.picker->finish(picker_outcome::chosen("/from/dialog"));
    REQUIRE(test.loop.run_when_posted() == 1);

    CHECK(text_of(test.window->get_import_path_text()) == "/from/dialog");
    CHECK(test.submitted.empty());
}

TEST_CASE("picker host: an answer is stored even when the field can no longer be edited", "[motif-slint-app][picker]")
{
    auto test = host {hang_script(/*abortable=*/true)};
    test.open_dialog(BrowseTarget::DatabaseOpen);

    test.window->set_busy(true);
    test.picker->finish(picker_outcome::chosen("/chosen"));
    REQUIRE(test.loop.run_when_posted() == 1);

    CHECK(text_of(test.window->get_open_path_text()) == "/chosen");
    CHECK(test.submitted.empty());
    CHECK_FALSE(test.window->get_picker_open());
}

TEST_CASE("picker host: board actions and the workspace gate are unaffected by an open dialog", "[motif-slint-app][picker]")
{
    auto test = host {hang_script(/*abortable=*/true)};
    auto board = motif::slint_app::async_board_runner {test.window};
    board.attach();
    auto const activity = workspace_activity {};
    auto const probe = motif::slint_app::activity_probe {[&activity]() -> workspace_activity { return activity; }};
    motif::slint_app::register_board_callbacks(*test.window, board, probe);

    test.open_dialog(BrowseTarget::DatabaseOpen);
    REQUIRE(test.window->get_picker_open());
    CHECK_FALSE(test.window->get_board_flipped());

    test.window->invoke_board_orientation_toggled();
    CHECK(test.window->get_board_flipped());
    // The dialog is not an activity: the gate that board actions consult says idle.
    CHECK(probe().idle());
    CHECK(test.window->get_picker_open());
}

TEST_CASE("picker host: shutdown aborts an abortable dialog and delivers nothing afterwards", "[motif-slint-app][picker]")
{
    auto test = host {hang_script(/*abortable=*/true)};
    test.window->set_open_path_text("/typed");
    test.open_dialog();

    auto const started = std::chrono::steady_clock::now();
    test.runner->shutdown();
    CHECK(std::chrono::steady_clock::now() - started < patience);

    CHECK(test.picker->wait_for_return(patience));
    CHECK(test.loop.run_all() == 0);
    CHECK(text_of(test.window->get_open_path_text()) == "/typed");

    test.runner->shutdown();
}

TEST_CASE("picker host: shutdown does not wait for a user who has not answered", "[motif-slint-app][picker]")
{
    auto test = host {hang_script(/*abortable=*/false)};
    test.window->set_open_path_text("/typed");
    test.open_dialog();

    auto const started = std::chrono::steady_clock::now();
    test.runner->shutdown();
    auto const waited = std::chrono::steady_clock::now() - started;
    CHECK(waited >= picker_shutdown_wait - std::chrono::milliseconds {10});
    CHECK(waited < std::chrono::seconds {3});
    CHECK_FALSE(test.picker->wait_for_return(std::chrono::milliseconds {20}));

    // The user answers after the process gave up on the dialog: nothing happens.
    test.picker->finish(picker_outcome::chosen("/too/late"));
    CHECK(test.picker->wait_for_return(patience));
    std::this_thread::sleep_for(settle);
    CHECK(test.loop.run_all() == 0);
    CHECK(text_of(test.window->get_open_path_text()) == "/typed");
}

TEST_CASE("picker host: an answer scheduled before shutdown is dropped when it runs after", "[motif-slint-app][picker]")
{
    auto test = host {choose_script("/chosen")};
    test.window->invoke_browse_requested(BrowseTarget::DatabaseOpen);
    REQUIRE(fake_uci::wait_for([&test]() -> bool { return test.loop.pending() > 0; }));

    test.runner->shutdown();
    CHECK(test.loop.run_all() == 1);
    CHECK(text_of(test.window->get_open_path_text()).empty());
}

TEST_CASE("picker host: destroying the runner with a dialog open ends the dialog", "[motif-slint-app][picker]")
{
    auto test = host {hang_script(/*abortable=*/true)};
    test.open_dialog();

    test.runner.reset();
    CHECK(test.picker->wait_for_return(patience));
    CHECK(test.loop.run_all() == 0);
}

TEST_CASE("picker host: an answer for a window that is gone does nothing", "[motif-slint-app][picker]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto loop = manual_loop {};
    auto picker = std::make_shared<scripted_file_picker>(choose_script("/chosen"));
    auto runner = std::unique_ptr<async_picker_runner> {};
    {
        auto window = WorkspaceWindow::create();
        runner = std::make_unique<async_picker_runner>(window, loop.environment(), picker);
        runner->request(BrowseTarget::DatabaseOpen, "");
    }
    CHECK(loop.run_when_posted() == 1);
    CHECK_FALSE(runner->open());
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
