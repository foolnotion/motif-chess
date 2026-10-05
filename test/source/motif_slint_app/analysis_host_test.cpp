#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "motif/slint_app/analysis_host.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <slint.h>

#include "fake_uci_engine.hpp"
#include "motif/slint_app/activity_gate.hpp"
#include "motif/slint_app/board_host.hpp"
#include "motif/slint_app/workspace_service.hpp"
#include "snapshot_platform.hpp"
#include "workspace.h"

// Reading a published Slint model back goes through its virtual interface, which
// the portability check flags at every instantiation point in this file.
// Test values are plain numbers.
// NOLINTBEGIN(portability-template-virtual-member-function,cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
namespace
{

using motif::slint_app::analysis_flush_interval;
using motif::slint_app::analysis_follow_delay;
using motif::slint_app::workspace_activity;

// The event loop of these tests: tasks the runner schedules wait here until a
// test runs them, which stands in for the loop turning. Tasks are run on the
// test thread, which plays the event-loop thread.
class manual_loop
{
  public:
    [[nodiscard]] auto environment() -> motif::slint_app::event_loop_environment
    {
        return motif::slint_app::event_loop_environment {
            .schedule = [this](std::chrono::milliseconds const delay, std::function<void()> task) -> void
            {
                std::scoped_lock const lock {mutex_};
                tasks_.push_back(entry {.delay = delay, .task = std::move(task)});
            }};
    }

    [[nodiscard]] auto pending(std::chrono::milliseconds const delay) const -> std::size_t
    {
        std::scoped_lock const lock {mutex_};
        return static_cast<std::size_t>(std::ranges::count_if(tasks_, [delay](entry const& item) -> bool { return item.delay == delay; }));
    }

    // Runs, in the order scheduled, the tasks that wait with this delay.
    auto run(std::chrono::milliseconds const delay) -> std::size_t
    {
        auto due = std::vector<entry> {};
        {
            std::scoped_lock const lock {mutex_};
            auto const split = std::ranges::stable_partition(tasks_, [delay](entry const& item) -> bool { return item.delay != delay; });
            due.assign(std::make_move_iterator(split.begin()), std::make_move_iterator(split.end()));
            tasks_.erase(split.begin(), split.end());
        }
        for (auto& item : due) {
            item.task();
        }
        return due.size();
    }

    auto run_all() -> std::size_t
    {
        auto total = std::size_t {0};
        for (;;) {
            auto batch = std::vector<entry> {};
            {
                std::scoped_lock const lock {mutex_};
                batch.swap(tasks_);
            }
            if (batch.empty()) {
                return total;
            }
            for (auto& item : batch) {
                item.task();
            }
            total += batch.size();
        }
    }

    // Lets the loop turn until `done` holds.
    template<typename Predicate>
    auto pump_until(Predicate const& done) -> bool
    {
        return fake_uci::wait_for(
            [this, &done]() -> bool
            {
                static_cast<void>(run(analysis_flush_interval));
                return done();
            });
    }

  private:
    struct entry
    {
        std::chrono::milliseconds delay;
        std::function<void()> task;
    };

    mutable std::mutex mutex_;
    std::vector<entry> tasks_;
};

struct temp_directory
{
    std::filesystem::path path;

    explicit temp_directory(std::string const& label)
        : path {std::filesystem::temp_directory_path() / ("motif_analysis_host_" + label + "_" + fake_uci::unique_stamp())}
    {
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

// The production registrars and runner on a real window and a real board, with
// the workspace activity the test chooses. The board's position-changed handler
// is wired the way main() does it.
struct host
{
    temp_directory directory {"host"};
    std::filesystem::path config_path {directory.path / "config.json"};
    slint::ComponentHandle<WorkspaceWindow> window {WorkspaceWindow::create()};
    motif::slint_app::workspace_service config {config_path};
    motif::slint_app::async_board_runner board {window};
    manual_loop loop;
    workspace_activity activity;
    std::unique_ptr<motif::slint_app::async_analysis_runner> analysis;

    host() { make_analysis(); }

    // main() builds the runner after the config exists; so does this.
    void make_analysis()
    {
        board.attach();
        analysis = std::make_unique<motif::slint_app::async_analysis_runner>(
            window, config, loop.environment(), [this]() -> std::optional<std::string> { return board.current_fen(); });
        board.add_position_changed_handler([this](motif::db::zobrist_hash) -> void { analysis->position_changed(); });
        auto const probe = motif::slint_app::activity_probe {[this]() -> workspace_activity { return activity; }};
        motif::slint_app::register_analysis_callbacks(*window, *analysis, probe);
        analysis->load();
    }

    ~host()
    {
        // The window and its callbacks outlive the runner; nothing calls it any more.
        analysis.reset();
    }

    host(host const&) = delete;
    auto operator=(host const&) -> host& = delete;
    host(host&&) = delete;
    auto operator=(host&&) -> host& = delete;

    [[nodiscard]] auto status() const -> std::string
    {
        auto const text = window->get_analysis_status_text();
        return std::string {std::string_view {text.data(), text.size()}};
    }

    [[nodiscard]] auto notice() const -> std::string
    {
        auto const text = window->get_analysis_notice();
        return std::string {std::string_view {text.data(), text.size()}};
    }

    [[nodiscard]] auto names() const -> std::vector<std::string>
    {
        auto result = std::vector<std::string> {};
        auto const model = window->get_analysis_engine_names();
        for (auto index = std::size_t {0}; index < model->row_count(); ++index) {
            auto const name = model->row_data(index).value_or(slint::SharedString {});
            result.emplace_back(std::string_view {name.data(), name.size()});
        }
        return result;
    }

    [[nodiscard]] auto lines() const -> std::vector<AnalysisLine>
    {
        auto result = std::vector<AnalysisLine> {};
        auto const model = window->get_analysis_lines();
        for (auto index = std::size_t {0}; index < model->row_count(); ++index) {
            result.push_back(model->row_data(index).value_or(AnalysisLine {}));
        }
        return result;
    }

    static auto text(slint::SharedString const& value) -> std::string
    {
        return std::string {std::string_view {value.data(), value.size()}};
    }

    void add_engine(std::string const& path) { window->invoke_analysis_engine_added(slint::SharedString {path}); }

    void start() { window->invoke_analysis_start_requested(); }

    void stop() { window->invoke_analysis_stop_requested(); }

    void play(std::string const& san) { board.play_san(san); }
};

auto make_engine(std::string const& body) -> std::unique_ptr<fake_uci::fake_engine>
{
    return std::make_unique<fake_uci::fake_engine>(body);
}

// An engine that reports one line and searches until stopped.
auto make_waiting_engine() -> std::unique_ptr<fake_uci::fake_engine>
{
    return make_engine(fake_uci::wait_for_stop_body());
}

auto process_gone(pid_t const process) -> bool
{
    return fake_uci::wait_for([process]() -> bool { return !fake_uci::process_alive(process); });
}

constexpr auto after_e4_fen = "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1";

}  // namespace

TEST_CASE("analysis host: with no engine the panel says so and Start does nothing", "[motif-slint-app][analysis][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};

    CHECK(test.status() == "No engine configured - add an engine path");
    CHECK_FALSE(test.window->get_analysis_can_start());
    CHECK(test.names().empty());

    test.start();

    CHECK_FALSE(test.window->get_analysis_active());
    CHECK(test.status() == "No engine configured - add an engine path");
    CHECK(test.loop.pending(analysis_flush_interval) == 0);
}

TEST_CASE("analysis host: adding an engine appends it to the config, selects it and clears the path field",
          "[motif-slint-app][analysis][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    test.window->set_analysis_engine_path_text("/opt/engines/stockfish");

    test.add_engine("/opt/engines/stockfish");
    test.add_engine("/usr/local/bin/lc0");

    CHECK(test.names() == std::vector<std::string> {"stockfish", "lc0"});
    CHECK(test.window->get_analysis_selected_engine() == 1);
    CHECK(test.window->get_analysis_can_start());
    CHECK(test.window->get_analysis_engine_path_text().empty());
    CHECK(test.status() == "Ready");
    CHECK(motif::slint_app::workspace_service {test.config_path}.engines().paths
          == std::vector<std::string> {"/opt/engines/stockfish", "/usr/local/bin/lc0"});
}

TEST_CASE("analysis host: two paths with one file name get numbered names", "[motif-slint-app][analysis][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};

    test.add_engine("/opt/a/stockfish");
    test.add_engine("/opt/b/stockfish");

    CHECK(test.names() == std::vector<std::string> {"stockfish", "stockfish (2)"});
}

TEST_CASE("analysis host: an empty or duplicate path is ignored with a visible message", "[motif-slint-app][analysis][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    test.add_engine("/opt/engines/stockfish");

    test.window->set_analysis_engine_path_text("   ");
    test.add_engine("   ");
    CHECK(test.names().size() == 1);
    CHECK_FALSE(test.notice().empty());
    CHECK(test.window->get_analysis_engine_path_text() == "   ");

    test.add_engine("/opt/engines/stockfish");
    CHECK(test.names().size() == 1);
    CHECK_THAT(test.notice(), Catch::Matchers::ContainsSubstring("already"));
}

TEST_CASE("analysis host: an engine list that cannot be saved is rolled back and the error shown", "[motif-slint-app][analysis][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    test.add_engine("/opt/engines/stockfish");
    std::filesystem::remove_all(test.config_path.parent_path());
    {
        auto blocker = std::ofstream {test.config_path.parent_path()};
        blocker << "not a directory";
    }

    test.add_engine("/opt/engines/lc0");

    CHECK(test.names() == std::vector<std::string> {"stockfish"});
    CHECK_FALSE(test.notice().empty());
    CHECK(test.config.engines().paths.size() == 1);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("analysis host: selection, removal and option changes that cannot be saved leave the stored state on screen",
          "[motif-slint-app][analysis][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    test.add_engine("/e/one");
    test.add_engine("/e/two");
    std::filesystem::remove_all(test.config_path.parent_path());
    {
        auto blocker = std::ofstream {test.config_path.parent_path()};
        blocker << "not a directory";
    }
    // The window's controls hold the new value before the host hears of it.
    auto const expect_stored = [&test]() -> void
    {
        CHECK(test.names() == std::vector<std::string> {"one", "two"});
        CHECK(test.window->get_analysis_selected_engine() == 1);
        CHECK(test.window->get_analysis_multipv() == 3);
        CHECK(test.window->get_analysis_limit_kind() == 0);
        CHECK(test.window->get_analysis_limit_value() == 20);
        CHECK_FALSE(test.notice().empty());
    };

    test.window->set_analysis_selected_engine(0);
    test.window->invoke_analysis_engine_selected(0);
    expect_stored();

    test.window->set_analysis_notice("");
    test.window->invoke_analysis_engine_removed();
    expect_stored();

    test.window->set_analysis_multipv(5);
    test.window->invoke_analysis_multipv_changed(5);
    expect_stored();

    test.window->set_analysis_limit_kind(1);
    test.window->invoke_analysis_limit_kind_changed(1);
    expect_stored();

    test.window->set_analysis_limit_value(77);
    test.window->invoke_analysis_limit_value_changed(77);
    expect_stored();
    CHECK(test.config.engines().paths.size() == 2);
}

namespace
{

// A two-line burst engine, started through the real window callbacks.
auto start_burst(host& test, std::unique_ptr<fake_uci::fake_engine> const& engine) -> void
{
    test.add_engine(engine->path());
    test.window->invoke_analysis_multipv_changed(2);
    test.start();
    test.analysis->wait_until_idle();
    // Give the engine time to print all 400 pairs; the checks below hold however
    // many of them have arrived.
    REQUIRE(fake_uci::wait_for([&]() -> bool { return test.loop.pending(analysis_flush_interval) >= 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds {400});
}

}  // namespace

TEST_CASE("analysis host: a burst of engine lines is one scheduled flush that draws the newest line of each rank",
          "[motif-slint-app][analysis][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    auto const engine = make_engine(fake_uci::burst_body(2));

    start_burst(test, engine);

    CHECK(test.loop.pending(analysis_flush_interval) == 1);
    CHECK(test.lines().empty());
    REQUIRE(test.loop.pump_until([&]() -> bool { return test.lines().size() == 2 && host::text(test.lines()[0].score) == "+4.00"; }));
    auto const lines = test.lines();
    CHECK(lines[0].rank == 1);
    CHECK(host::text(lines[0].score) == "+4.00");
    CHECK(lines[1].rank == 2);
    CHECK(host::text(lines[1].score) == "+14.00");
    // Nothing is left over once the burst is drawn.
    CHECK(test.loop.pending(analysis_flush_interval) == 0);
    test.stop();
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("analysis host: a burst queued for an older workspace epoch or session is not drawn", "[motif-slint-app][analysis][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    for (auto const change_epoch : {true, false}) {
        auto test = host {};
        auto const engine = make_engine(fake_uci::burst_body(2));
        start_burst(test, engine);
        REQUIRE(test.loop.pending(analysis_flush_interval) == 1);

        if (change_epoch) {
            test.analysis->workspace_changed();
        } else {
            test.play("e4");  // a new session generation
        }
        static_cast<void>(test.loop.run(analysis_flush_interval));

        INFO((change_epoch ? std::string {"workspace epoch changed"} : std::string {"session replaced"}));
        CHECK(test.lines().empty());
        CHECK(test.status() == (change_epoch ? "Ready" : "Analysing"));
        test.analysis->shutdown();
    }
}

TEST_CASE("analysis host: removing the selected engine selects the first remaining one, then the empty state",
          "[motif-slint-app][analysis][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    test.add_engine("/e/one");
    test.add_engine("/e/two");

    test.window->invoke_analysis_engine_removed();
    CHECK(test.names() == std::vector<std::string> {"one"});
    CHECK(test.window->get_analysis_selected_engine() == 0);

    test.window->invoke_analysis_engine_removed();
    CHECK(test.names().empty());
    CHECK(test.window->get_analysis_selected_engine() == -1);
    CHECK(test.status() == "No engine configured - add an engine path");
    CHECK_FALSE(test.window->get_analysis_can_start());
    CHECK(test.config.engines().paths.empty());
}

TEST_CASE("analysis host: engine, MultiPV and limit choices persist and come back on the next run", "[motif-slint-app][analysis][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    test.add_engine("/e/one");
    test.add_engine("/e/two");

    test.window->invoke_analysis_engine_selected(0);
    test.window->invoke_analysis_multipv_changed(5);
    test.window->invoke_analysis_limit_kind_changed(1);
    test.window->invoke_analysis_limit_value_changed(2500);

    auto const stored = motif::slint_app::workspace_service {test.config_path}.engines();
    CHECK(stored.selected == "/e/one");
    CHECK(stored.multipv == 5);
    CHECK(stored.limit == motif::slint_app::analysis_limit_kind::movetime);
    CHECK(stored.movetime_ms == 2500);
    CHECK(stored.depth == 20);
    CHECK(test.window->get_analysis_limit_kind() == 1);
    CHECK(test.window->get_analysis_limit_value() == 2500);

    // Switching back to depth shows the depth, not the time.
    test.window->invoke_analysis_limit_kind_changed(0);
    CHECK(test.window->get_analysis_limit_value() == 20);
    // Out-of-range values are clamped on the way in.
    test.window->invoke_analysis_limit_value_changed(1000);
    CHECK(test.window->get_analysis_limit_value() == 99);
}

TEST_CASE("analysis host: Start analyses the board position and shows live lines from White's side", "[motif-slint-app][analysis][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    auto const engine = make_engine(fake_uci::wait_for_stop_body());
    test.add_engine(engine->path());

    test.start();

    CHECK(test.window->get_analysis_active());
    CHECK(test.status() == "Analysing");
    CHECK(test.lines().empty());
    REQUIRE(test.loop.pump_until([&]() -> bool { return !test.lines().empty(); }));
    auto const lines = test.lines();
    REQUIRE(lines.size() == 1);
    CHECK(host::text(lines[0].score) == "+0.13");
    CHECK(lines[0].depth == 1);
    CHECK(host::text(lines[0].moves) == "1.e4 e5");
    CHECK(test.status() == "Analysing");
    auto const positions = engine->commands_starting_with("position");
    REQUIRE(positions.size() == 1);
    CHECK(positions.front() == "position startpos");
    CHECK(engine->commands_starting_with("go depth 20").size() == 1);
    test.stop();
}

TEST_CASE("analysis host: a limit of movetime is sent as movetime and MultiPV as an option", "[motif-slint-app][analysis][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    auto const engine = make_waiting_engine();
    test.add_engine(engine->path());
    test.window->invoke_analysis_multipv_changed(2);
    test.window->invoke_analysis_limit_kind_changed(1);
    test.window->invoke_analysis_limit_value_changed(3000);

    test.start();
    test.analysis->wait_until_idle();

    REQUIRE(fake_uci::wait_for([&]() -> bool { return engine->commands_starting_with("go movetime 3000").size() == 1; }));
    CHECK(engine->commands_starting_with("go depth").empty());
    CHECK(engine->commands_starting_with("setoption name MultiPV value 2").size() == 1);
    test.stop();
}

TEST_CASE("analysis host: black to move is scored from White's side", "[motif-slint-app][analysis][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    test.play("e4");
    auto const engine = make_waiting_engine();
    test.add_engine(engine->path());

    test.start();

    REQUIRE(test.loop.pump_until([&]() -> bool { return !test.lines().empty(); }));
    // The fake engine reports cp 13 for the side to move, which is Black here.
    CHECK(host::text(test.lines()[0].score) == "-0.13");
    auto const positions = engine->commands_starting_with("position");
    REQUIRE(positions.size() == 1);
    CHECK(positions.front() == std::string {"position fen "} + after_e4_fen);
    test.stop();
}

TEST_CASE("analysis host: a finished analysis keeps its best line, releases the engine and keeps following the board",
          "[motif-slint-app][analysis][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    auto const engine = make_engine(fake_uci::fast_complete_body());
    test.add_engine(engine->path());

    test.start();

    REQUIRE(test.loop.pump_until([&]() -> bool { return test.status() == "Finished"; }));
    CHECK(test.lines().size() == 1);
    CHECK(test.window->get_analysis_active());
    CHECK_FALSE(test.window->get_analysis_lines_stale());
    // Nothing more to report: the engine process is freed without waiting for a restart.
    test.analysis->wait_until_idle();
    auto const processes = engine->process_ids();
    REQUIRE(processes.size() == 1);
    CHECK(process_gone(processes.front()));

    // Still following: the next position gets its own session.
    test.play("e4");
    CHECK(test.status() == "Analysing");
    CHECK(test.lines().empty());
    REQUIRE(test.loop.run(analysis_follow_delay) == 1);
    REQUIRE(test.loop.pump_until([&]() -> bool { return test.status() == "Finished"; }));
    CHECK(engine->process_ids().size() == 2);
    test.stop();
}

TEST_CASE("analysis host: a position change stops the old analysis, clears the lines and analyses the new position",
          "[motif-slint-app][analysis][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    auto const engine = make_waiting_engine();
    test.add_engine(engine->path());
    test.start();
    REQUIRE(test.loop.pump_until([&]() -> bool { return !test.lines().empty(); }));
    test.analysis->wait_until_idle();
    auto const first = engine->process_ids();
    REQUIRE(first.size() == 1);

    test.play("e4");

    // At once: the old lines are gone and the panel is analysing the new position.
    CHECK(test.status() == "Analysing");
    CHECK(test.lines().empty());
    CHECK(test.window->get_analysis_active());
    CHECK(test.loop.pending(analysis_follow_delay) == 1);
    test.analysis->wait_until_idle();
    CHECK(process_gone(first.front()));
    CHECK(engine->process_ids().size() == 1);

    REQUIRE(test.loop.run(analysis_follow_delay) == 1);
    test.analysis->wait_until_idle();
    REQUIRE(test.loop.pump_until([&]() -> bool { return !test.lines().empty(); }));
    auto const positions = engine->commands_starting_with("position");
    REQUIRE(positions.size() == 2);
    CHECK(positions.back() == std::string {"position fen "} + after_e4_fen);
    CHECK(engine->process_ids().size() == 2);
    test.stop();
}

TEST_CASE("analysis host: a burst of position changes costs one restart on the final position", "[motif-slint-app][analysis][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    auto const engine = make_waiting_engine();
    test.add_engine(engine->path());
    test.start();
    REQUIRE(test.loop.pump_until([&]() -> bool { return !test.lines().empty(); }));

    for (auto const* san : {"e4", "e5", "Nf3", "Nc6", "Bb5"}) {
        test.play(san);
    }

    CHECK(test.loop.pending(analysis_follow_delay) == 1);
    REQUIRE(test.loop.run(analysis_follow_delay) == 1);
    test.analysis->wait_until_idle();
    REQUIRE(fake_uci::wait_for([&]() -> bool { return engine->commands_starting_with("position").size() == 2; }));
    auto const positions = engine->commands_starting_with("position");
    REQUIRE(positions.size() == 2);
    CHECK(positions.back() == "position fen r1bqkbnr/pppp1ppp/2n5/1B2p3/4P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 3 3");
    CHECK(engine->process_ids().size() == 2);
    test.stop();
}

TEST_CASE("analysis host: moves that do not change the position, such as a selection, do not restart the engine",
          "[motif-slint-app][analysis][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    auto const engine = make_waiting_engine();
    test.add_engine(engine->path());
    test.start();
    REQUIRE(test.loop.pump_until([&]() -> bool { return !test.lines().empty(); }));

    test.board.select_square(4, 1);  // e2: only a selection
    test.board.toggle_orientation();

    CHECK(test.loop.pending(analysis_follow_delay) == 0);
    CHECK(test.status() == "Analysing");
    CHECK(test.lines().size() == 1);
    test.stop();
}

TEST_CASE("analysis host: Stop releases the engine, keeps the last lines marked stale and stops following the board",
          "[motif-slint-app][analysis][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    auto const engine = make_waiting_engine();
    test.add_engine(engine->path());
    test.start();
    REQUIRE(test.loop.pump_until([&]() -> bool { return !test.lines().empty(); }));

    test.stop();

    CHECK(test.status() == "Stopped");
    CHECK_FALSE(test.window->get_analysis_active());
    CHECK(test.window->get_analysis_lines_stale());
    CHECK(test.lines().size() == 1);
    CHECK(test.window->get_analysis_can_start());
    test.analysis->wait_until_idle();
    auto const processes = engine->process_ids();
    REQUIRE(processes.size() == 1);
    CHECK(process_gone(processes.front()));

    // The bestmove an engine prints on stop must not turn this into "Finished".
    static_cast<void>(test.loop.run_all());
    CHECK(test.status() == "Stopped");
    CHECK(test.lines().size() == 1);

    test.play("e4");
    CHECK(test.loop.pending(analysis_follow_delay) == 0);
    test.analysis->wait_until_idle();
    CHECK(engine->process_ids().size() == 1);
    CHECK(test.status() == "Stopped");
}

TEST_CASE("analysis host: events of a replaced session that are still queued never update the panel", "[motif-slint-app][analysis][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    auto const engine = make_waiting_engine();
    test.add_engine(engine->path());
    test.start();
    // The engine's line is queued for the event loop but the loop has not turned.
    REQUIRE(fake_uci::wait_for([&]() -> bool { return test.loop.pending(analysis_flush_interval) == 1; }));

    test.play("e4");
    static_cast<void>(test.loop.run(analysis_flush_interval));

    CHECK(test.lines().empty());
    CHECK(test.status() == "Analysing");
    test.stop();
}

TEST_CASE("analysis host: a late event after Stop is dropped", "[motif-slint-app][analysis][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    auto const engine = make_waiting_engine();
    test.add_engine(engine->path());
    test.start();
    REQUIRE(fake_uci::wait_for([&]() -> bool { return test.loop.pending(analysis_flush_interval) == 1; }));

    test.stop();
    static_cast<void>(test.loop.run(analysis_flush_interval));

    CHECK(test.lines().empty());
    CHECK(test.status() == "Stopped");
}

TEST_CASE("analysis host: an engine that cannot start is a visible failure and Start stays available", "[motif-slint-app][analysis][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    test.add_engine("/nonexistent/dir/no-such-engine");

    test.start();

    REQUIRE(test.loop.pump_until([&]() -> bool { return test.status() != "Analysing"; }));
    CHECK(test.status() == "Engine could not start - check the path (no-such-engine)");
    CHECK_FALSE(test.window->get_analysis_active());
    CHECK(test.window->get_analysis_can_start());
    // The rest of the app stays usable: the board still plays.
    test.play("e4");
    CHECK(test.board.current_fen() == std::optional<std::string> {after_e4_fen});
    // Nothing restarts by itself.
    test.analysis->wait_until_idle();
    CHECK(test.loop.pending(analysis_follow_delay) == 0);
}

TEST_CASE("analysis host: a file that is not executable fails to start in the same visible way", "[motif-slint-app][analysis][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    auto const plain_file = test.directory.path / "not-an-engine";
    std::filesystem::create_directories(test.directory.path);
    {
        auto file = std::ofstream {plain_file};
        file << "plain text\n";
    }
    test.add_engine(plain_file.string());

    test.start();

    REQUIRE(test.loop.pump_until([&]() -> bool { return test.status() != "Analysing"; }));
    CHECK(test.status() == "Engine could not start - check the path (not-an-engine)");
    CHECK(test.window->get_analysis_can_start());
}

TEST_CASE("analysis host: a crashing engine is visible, offers Start again and is not restarted by itself",
          "[motif-slint-app][analysis][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    auto const engine = make_engine(fake_uci::crash_body());
    test.add_engine(engine->path());

    test.start();

    REQUIRE(test.loop.pump_until([&]() -> bool { return test.status().starts_with("Engine crashed"); }));
    CHECK(test.status() == "Engine crashed: engine process crashed");
    CHECK_FALSE(test.window->get_analysis_active());
    CHECK(test.window->get_analysis_can_start());
    test.analysis->wait_until_idle();
    auto const processes = engine->process_ids();
    REQUIRE(processes.size() == 1);
    CHECK(process_gone(processes.front()));
    CHECK(test.loop.pending(analysis_follow_delay) == 0);

    // Start is the restart.
    test.start();
    CHECK(test.window->get_analysis_active());
    REQUIRE(test.loop.pump_until([&]() -> bool { return test.status().starts_with("Engine crashed"); }));
    CHECK(engine->process_ids().size() == 2);
}

TEST_CASE("analysis host: changing the workspace stops and releases the analysis and drops its lines", "[motif-slint-app][analysis][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    auto const engine = make_waiting_engine();
    test.add_engine(engine->path());
    test.start();
    REQUIRE(fake_uci::wait_for([&]() -> bool { return test.loop.pending(analysis_flush_interval) == 1; }));

    test.analysis->workspace_changed();
    // The event that was queued before the change is of the old workspace.
    static_cast<void>(test.loop.run_all());

    CHECK_FALSE(test.window->get_analysis_active());
    CHECK(test.lines().empty());
    CHECK(test.status() == "Ready");
    test.analysis->wait_until_idle();
    auto const processes = engine->process_ids();
    REQUIRE(processes.size() == 1);
    CHECK(process_gone(processes.front()));

    // A position change after the workspace changed starts nothing.
    test.play("e4");
    CHECK(test.loop.pending(analysis_follow_delay) == 0);
}

TEST_CASE("analysis host: leaving the Analysis preset stops the analysis", "[motif-slint-app][analysis][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    auto const engine = make_waiting_engine();
    test.add_engine(engine->path());
    test.window->set_workspace_preset(WorkspacePreset::Analysis);
    state->window_adapter->render();  // the change handler notes the preset the window is showing
    test.start();
    REQUIRE(test.loop.pump_until([&]() -> bool { return !test.lines().empty(); }));

    test.window->set_workspace_preset(WorkspacePreset::Review);
    state->window_adapter->render();  // change handlers run while the frame is prepared

    CHECK_FALSE(test.window->get_analysis_active());
    CHECK(test.status() == "Stopped");
    test.analysis->wait_until_idle();
    auto const processes = engine->process_ids();
    REQUIRE(processes.size() == 1);
    CHECK(process_gone(processes.front()));
}

TEST_CASE("analysis host: shutting down while analysing releases the engine and nothing runs afterwards",
          "[motif-slint-app][analysis][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    auto const engine = make_waiting_engine();
    test.add_engine(engine->path());
    test.start();
    REQUIRE(fake_uci::wait_for([&]() -> bool { return test.loop.pending(analysis_flush_interval) == 1; }));
    test.play("e4");
    auto const processes = engine->process_ids();
    REQUIRE_FALSE(processes.empty());

    test.analysis->shutdown();

    for (auto const process : processes) {
        CHECK(process_gone(process));
    }
    // Tasks the loop still holds find a runner that is shut down, then a destroyed one.
    static_cast<void>(test.loop.run_all());
    test.analysis.reset();
    static_cast<void>(test.loop.run_all());
    // No task changed the panel after the shutdown: it still shows the cleared "Analysing" of the last position change.
    CHECK(test.status() == "Analysing");
    CHECK(test.lines().empty());
}

TEST_CASE("analysis host: destroying the runner with analysis on ends the engine and leaves the queued tasks harmless",
          "[motif-slint-app][analysis][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    auto const engine = make_waiting_engine();
    test.add_engine(engine->path());
    test.start();
    REQUIRE(fake_uci::wait_for([&]() -> bool { return test.loop.pending(analysis_flush_interval) == 1; }));
    test.play("e4");
    auto const processes = engine->process_ids();

    test.analysis.reset();

    for (auto const process : processes) {
        CHECK(process_gone(process));
    }
    CHECK(test.loop.run_all() >= 1);
}

TEST_CASE("analysis host: the controls that shape a session are locked while analysis is on", "[motif-slint-app][analysis][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    auto const engine = make_waiting_engine();
    test.add_engine(engine->path());
    test.add_engine("/opt/other/engine");
    test.window->invoke_analysis_engine_selected(0);
    test.start();

    test.window->invoke_analysis_multipv_changed(1);
    test.window->invoke_analysis_engine_selected(1);
    test.window->invoke_analysis_engine_removed();
    test.add_engine("/opt/third/engine");

    CHECK(test.names().size() == 2);
    CHECK(test.window->get_analysis_selected_engine() == 0);
    CHECK(test.window->get_analysis_multipv() == 3);
    CHECK(test.config.engines().multipv == 3);
    test.stop();
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("analysis host: engine list changes wait for a workspace operation, but Start and Stop never do",
          "[motif-slint-app][analysis][host]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto test = host {};
    auto const engine = make_waiting_engine();
    test.add_engine(engine->path());
    test.activity = workspace_activity {.workspace_busy = true};

    test.add_engine("/opt/other/engine");
    // The window's spin box and combo boxes already hold the new values.
    test.window->set_analysis_multipv(1);
    test.window->invoke_analysis_multipv_changed(1);
    CHECK(test.window->get_analysis_multipv() == 3);
    CHECK_FALSE(test.notice().empty());
    test.window->set_analysis_notice("");
    test.window->set_analysis_limit_kind(1);
    test.window->invoke_analysis_limit_kind_changed(1);
    CHECK(test.window->get_analysis_limit_kind() == 0);
    CHECK_FALSE(test.notice().empty());
    test.window->set_analysis_notice("");
    test.window->set_analysis_limit_value(5);
    test.window->invoke_analysis_limit_value_changed(5);
    CHECK(test.window->get_analysis_limit_value() == 20);
    CHECK_FALSE(test.notice().empty());
    test.window->invoke_analysis_engine_removed();
    CHECK(test.names().size() == 1);
    CHECK(test.config.engines().multipv == 3);
    CHECK_FALSE(test.notice().empty());

    // Every other busy condition leaves them alone too.
    for (auto const& busy : {workspace_activity {.import_active = true},
                             workspace_activity {.browser_busy = true},
                             workspace_activity {.board_busy = true},
                             workspace_activity {.search_busy = true},
                             workspace_activity {.workspace_busy = true}})
    {
        test.activity = busy;
        test.start();
        CHECK(test.window->get_analysis_active());
        test.stop();
        CHECK_FALSE(test.window->get_analysis_active());
    }
}

// NOLINTEND(portability-template-virtual-member-function,cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
