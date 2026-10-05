#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "motif/slint_app/browser_host.hpp"

#include <catch2/catch_test_macros.hpp>
#include <slint.h>

#include "motif/chess/chess.hpp"
#include "motif/db/database_manager.hpp"
#include "motif/db/query_connection.hpp"
#include "motif/db/types.hpp"
#include "motif/slint_app/activity_gate.hpp"
#include "motif/slint_app/board_host.hpp"
#include "motif/slint_app/file_picker.hpp"
#include "motif/slint_app/picker_host.hpp"
#include "snapshot_platform.hpp"
#include "workspace.h"

// Reading a published Slint model back goes through its virtual interface, which
// the portability check flags at every instantiation point in this file.
// Row counts, game numbers and widths are plain test figures.
// NOLINTBEGIN(portability-template-virtual-member-function,cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
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

constexpr auto no_delay = std::chrono::milliseconds {0};

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

  private:
    struct entry
    {
        std::chrono::milliseconds delay;
        std::function<void()> task;
    };

    mutable std::mutex mutex_;
    std::vector<entry> tasks_;
};

auto make_game(std::string white, std::string black, std::string result, std::vector<std::string> const& sans = {}) -> motif::db::game
{
    auto position = motif::chess::board {};
    auto game = motif::db::game {};
    game.white.name = std::move(white);
    game.black.name = std::move(black);
    game.result = std::move(result);
    game.event_details = motif::db::event {.name = "Browser Host", .site = "Local", .date = "2026.10.04"};
    game.date = "2026.10.04";
    game.eco = "C20";
    for (auto const& san : sans) {
        auto const move = motif::chess::apply_san(position, san);
        REQUIRE(move);
        game.moves.push_back(*move);
    }
    return game;
}

auto text(slint::SharedString const& value) -> std::string
{
    return std::string {std::string_view {value.data(), value.size()}};
}

// The production registrars and runner on a real window, a real board and a
// scratch database, with the workspace activity the test chooses and a loop the
// test turns. The activity probe adds the runner's own busy flag the way main()
// does.
struct host
{
    slint::ComponentHandle<WorkspaceWindow> window {WorkspaceWindow::create()};
    manual_loop loop;
    motif::slint_app::async_browser_runner browser {window, loop.environment()};
    motif::slint_app::async_board_runner board {window};
    motif::slint_app::replace_controller confirmation {motif::slint_app::make_replace_confirmation(window, board)};
    workspace_activity activity;
    std::size_t filter_changes {0};
    std::optional<motif::db::database_manager> database;

    host()
    {
        auto handlers = motif::slint_app::make_board_activation_handlers(slint::ComponentWeakHandle<WorkspaceWindow> {window}, board);
        browser.set_activation_handlers(handlers.loaded, handlers.failed);
        browser.set_external_activation_handlers(handlers.loaded_at, handlers.failed_at);
        motif::slint_app::clear_browser(*window);
        auto const probe = motif::slint_app::activity_probe {[this]() -> workspace_activity
                                                             {
                                                                 auto current = activity;
                                                                 current.browser_busy = current.browser_busy || browser.is_busy();
                                                                 return current;
                                                             }};
        // The position handler and the board hook are wired the way main() does.
        board.add_position_changed_handler([this](motif::db::zobrist_hash const hash) -> void { browser.position_changed(hash); });
        motif::slint_app::register_browser_callbacks(
            *window,
            browser,
            probe,
            confirmation,
            motif::slint_app::browser_hooks {
                .filters_changed = [this]() -> void { ++filter_changes; },
                .board_position = [this]() -> std::optional<motif::db::zobrist_hash> { return board.current_hash(); },
            });
        motif::slint_app::register_board_callbacks(*window, board, probe);
        motif::slint_app::register_play_callbacks(*window, board, probe, confirmation);
    }

    ~host()
    {
        // The window and its callbacks outlive the runner; nothing calls it any more.
        browser.wait_for_worker();
    }

    host(host const&) = delete;
    auto operator=(host const&) -> host& = delete;
    host(host&&) = delete;
    auto operator=(host&&) -> host& = delete;

    // Opens `source` the way the workspace runner does once a database is ready.
    void open(motif::db::database_manager& source)
    {
        board.attach();
        browser.attach(source);
        browser.load_initial();
        settle();
    }

    // Turns the loop until the worker has nothing left to deliver.
    void settle()
    {
        auto delivered = true;
        while (delivered) {
            browser.wait_for_worker();
            delivered = loop.run(no_delay) > 0;
        }
    }

    // The toggle's press, through the registered callback.
    void follow_on() { window->invoke_game_follow_toggled(/*arg_0=*/true); }

    void follow_off() { window->invoke_game_follow_toggled(/*arg_0=*/false); }

    // Plays moves on the board through the registered callback.
    void play(std::vector<std::string> const& sans)
    {
        for (auto const& san : sans) {
            window->invoke_board_play_san(slint::SharedString {san});
        }
    }

    [[nodiscard]] auto status() const -> std::string { return text(window->get_game_status_text()); }

    [[nodiscard]] auto names() const -> std::vector<std::string>
    {
        auto result = std::vector<std::string> {};
        for (auto row = std::size_t {0}; row < row_count(); ++row) {
            result.push_back(white(row));
        }
        return result;
    }

    [[nodiscard]] auto row_count() const -> std::size_t { return window->get_game_rows()->row_count(); }

    [[nodiscard]] auto white(std::size_t const row) const -> std::string
    {
        return text(window->get_game_rows()->row_data(row).value_or(GameRow {}).white);
    }
};

// A scratch database of `count` games: "White 000", "White 001", ... with
// alternating results.
auto make_numbered_database(std::size_t const count) -> motif::db::database_manager
{
    auto created = motif::db::database_manager::create_scratch();
    REQUIRE(created.has_value());
    auto database = std::move(*created);
    REQUIRE(database.writer().begin_transaction().has_value());
    for (auto index = std::size_t {0}; index < count; ++index) {
        auto name = std::to_string(index);
        name.insert(0, 3 - std::min<std::size_t>(name.size(), 3), '0');
        char const* const result = index % 2 == 0 ? "1-0" : "0-1";
        auto const game = make_game("White " + name, "Black " + name, result, {"e4", "e5", "Nf3"});
        REQUIRE(database.writer().insert(game).has_value());
    }
    REQUIRE(database.writer().commit_transaction().has_value());
    REQUIRE(database.rebuild_position_postings().has_value());
    return database;
}

auto hash_of(std::vector<std::string> const& sans) -> motif::db::zobrist_hash
{
    auto position = motif::chess::board {};
    for (auto const& san : sans) {
        REQUIRE(motif::chess::apply_san(position, san));
    }
    return motif::db::zobrist_hash {position.hash()};
}

// Five games. Three reach "1. Nf3 Nc6": Early and Knight at ply 2, Late by a
// detour at ply 6. White ascending, they list as Early, Knight, Late.
auto make_opening_database(bool const index = true) -> motif::db::database_manager
{
    auto created = motif::db::database_manager::create_scratch();
    REQUIRE(created.has_value());
    auto database = std::move(*created);
    auto const games = std::array {
        make_game("Early", "Ann", "1-0", {"Nf3", "Nc6"}),
        make_game("Late", "Bea", "0-1", {"Nf3", "Nf6", "Ng1", "Ng8", "Nf3", "Nc6"}),
        make_game("Knight", "Cy", "1-0", {"Nf3", "Nc6", "Nc3", "Nf6"}),
        make_game("Other", "Di", "0-1", {"e4", "e5"}),
        make_game("Pawn", "Ed", "1/2-1/2", {"e4", "c5"}),
    };
    for (auto const& game : games) {
        REQUIRE(database.insert_game(game).has_value());
    }
    if (index) {
        REQUIRE(database.rebuild_position_postings().has_value());
    }
    return database;
}

using names_list = std::vector<std::string>;

}  // namespace

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("browser host: filters, sort, paging, selection and column width act through the registered callbacks when idle",
          "[motif-slint-app][host][browser]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto database = make_numbered_database(120);
    auto test = host {};
    test.open(database);
    REQUIRE(test.row_count() == 100);
    CHECK(test.window->get_game_total_count() == 120);
    CHECK(test.white(0) == "White 000");

    test.window->invoke_game_page_requested(1);
    test.settle();
    CHECK(test.row_count() == 20);
    CHECK(test.window->get_game_page_index() == 1);

    test.window->invoke_game_sort_requested(0, /*arg_1=*/false);
    test.settle();
    CHECK(test.window->get_game_page_index() == 0);
    CHECK(test.window->get_game_sort_column() == 0);
    CHECK_FALSE(test.window->get_game_sort_ascending());
    CHECK(test.white(0) == "White 119");

    test.window->invoke_game_filters_changed("White 007", "");
    test.settle();
    CHECK(test.row_count() == 1);
    CHECK(test.window->get_game_total_count() == 1);
    CHECK(test.filter_changes == 1);

    test.window->invoke_game_filters_changed("", "0-1");
    test.settle();
    CHECK(test.window->get_game_total_count() == 60);
    CHECK(test.filter_changes == 2);

    test.window->invoke_game_selected(2);
    CHECK(test.window->get_game_selected_row() == 2);
    test.window->invoke_game_move_selection(1);
    CHECK(test.window->get_game_selected_row() == 3);

    test.window->invoke_game_column_resized(0, 300);
    CHECK(test.window->get_game_column_widths()->row_data(0).value_or(0) == 300);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("browser host: every registered callback is rejected under each busy condition", "[motif-slint-app][host][browser]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto database = make_numbered_database(120);
    for (auto const& busy : busy_conditions) {
        auto test = host {};
        test.open(database);
        test.window->invoke_game_selected(1);
        test.activity = busy;

        test.window->invoke_game_filters_changed("White 007", "1-0");
        test.window->invoke_game_page_requested(1);
        test.window->invoke_game_sort_requested(2, /*arg_1=*/false);
        test.window->invoke_game_selected(4);
        test.window->invoke_game_move_selection(1);
        test.window->invoke_game_column_resized(0, 300);
        test.window->invoke_game_activate_requested();

        CHECK(test.loop.pending(no_delay) == 0);
        CHECK_FALSE(test.browser.is_busy());
        CHECK_FALSE(test.window->get_browser_busy());
        CHECK(test.filter_changes == 0);
        CHECK(test.row_count() == 100);
        CHECK(test.window->get_game_total_count() == 120);
        CHECK(test.window->get_game_page_index() == 0);
        CHECK(test.window->get_game_sort_column() == 0);
        CHECK(test.window->get_game_selected_row() == 1);
        CHECK(text(test.window->get_game_player_filter()).empty());
        CHECK(test.window->get_game_column_widths()->row_data(0).value_or(0) != 300);
        CHECK_FALSE(test.window->get_board_loaded());
        CHECK_FALSE(test.window->get_confirm_visible());
    }
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("browser host: opening a game loads it on the board, and asks first when the board is edited", "[motif-slint-app][host][browser]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto database = make_numbered_database(5);
    auto test = host {};
    test.open(database);
    test.window->invoke_game_selected(2);

    test.window->invoke_game_activate_requested();
    CHECK(test.browser.is_busy());
    CHECK(test.window->get_browser_busy());
    test.settle();
    CHECK_FALSE(test.browser.is_busy());
    CHECK(test.window->get_board_loaded());
    CHECK(text(test.window->get_board_white_name()) == "White 002");
    CHECK(text(test.window->get_game_active_title()).starts_with("White 002"));

    // The user branches the loaded game from its first position; replacing it now needs a yes.
    test.window->invoke_board_play_san("d4");
    REQUIRE(test.window->get_board_edited());
    test.window->invoke_game_selected(3);
    test.window->invoke_game_activate_requested();
    CHECK(test.window->get_confirm_visible());
    CHECK_FALSE(test.browser.is_busy());
    test.window->invoke_confirm_cancelled();
    test.settle();
    CHECK(text(test.window->get_board_white_name()) == "White 002");

    test.window->invoke_game_activate_requested();
    test.window->invoke_confirm_accepted();
    test.settle();
    CHECK(text(test.window->get_board_white_name()) == "White 003");
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("browser host: a request made while another runs waits, and the newest request wins", "[motif-slint-app][host][browser]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto database = make_numbered_database(12);
    auto test = host {};
    test.open(database);

    test.browser.set_filters("White 001", "");
    REQUIRE(test.browser.is_busy());
    test.browser.set_filters("White 002", "");
    test.browser.set_filters("White 003", "");
    test.settle();

    CHECK_FALSE(test.browser.is_busy());
    REQUIRE(test.row_count() == 1);
    CHECK(test.white(0) == "White 003");
    CHECK(text(test.window->get_game_player_filter()) == "White 003");
}

TEST_CASE("browser host: a result of a replaced workspace is dropped and the runner goes idle", "[motif-slint-app][host][browser]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto database = make_numbered_database(12);
    auto test = host {};
    test.open(database);
    REQUIRE(test.row_count() == 12);

    test.browser.set_filters("White 001", "");
    test.browser.wait_for_worker();
    test.browser.detach();
    CHECK(test.row_count() == 0);
    test.settle();

    CHECK(test.row_count() == 0);
    CHECK_FALSE(test.browser.is_busy());
    CHECK_FALSE(test.window->get_browser_busy());
}

TEST_CASE("browser host: a result that arrives after the runner is gone is dropped", "[motif-slint-app][host][browser]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto database = make_numbered_database(12);
    auto loop = manual_loop {};
    auto window = WorkspaceWindow::create();
    motif::slint_app::clear_browser(*window);
    {
        auto browser = motif::slint_app::async_browser_runner {window, loop.environment()};
        browser.attach(database);
        browser.load_initial();
        browser.wait_for_worker();
    }

    CHECK(loop.run(no_delay) == 1);
    CHECK(window->get_game_rows()->row_count() == 0);
}

TEST_CASE("browser host: detach interrupts a blocked dedicated SQLite query", "[motif-slint-app][browser][shutdown]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto database = make_numbered_database(12);
    auto loop = manual_loop {};
    auto window = WorkspaceWindow::create();
    auto blocker = std::make_shared<motif::db::query_connection_blocker>();
    motif::db::set_query_connection_blocker_for_tests(blocker);
    auto browser = motif::slint_app::async_browser_runner {window, loop.environment()};
    browser.attach(database);
    browser.load_initial();
    blocker->wait_until_entered();

    auto const started = std::chrono::steady_clock::now();
    browser.detach();
    auto const elapsed = std::chrono::steady_clock::now() - started;
    motif::db::set_query_connection_blocker_for_tests({});

    CHECK(elapsed <= std::chrono::seconds {1});
    CHECK_FALSE(browser.is_busy());
    CHECK(window->get_game_rows()->row_count() == 0);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("browser host: Follow board lists the games that reach the board position and refreshes with every move",
          "[motif-slint-app][host][browser][follow]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto database = make_opening_database();
    auto test = host {};
    test.open(database);
    test.play({"Nf3", "Nc6"});
    REQUIRE(test.row_count() == 5);
    CHECK(test.loop.pending(no_delay) == 0);  // not following: a position change asks the database nothing

    test.follow_on();

    // The old rows stay, marked as about to be replaced, until the new ones land.
    CHECK(test.window->get_game_following());
    CHECK(test.window->get_game_refreshing());
    CHECK(test.window->get_browser_busy());
    CHECK(test.row_count() == 5);
    test.settle();
    CHECK(test.names() == names_list {"Early", "Knight", "Late"});
    CHECK(test.window->get_game_total_count() == 3);
    CHECK_FALSE(test.window->get_game_refreshing());
    CHECK_FALSE(test.window->get_browser_busy());
    CHECK(test.status().empty());
    CHECK(test.window->get_game_following());

    // Board input is ignored while the refresh runs, then the list follows the move.
    test.window->invoke_board_play_san("Nc3");
    CHECK(test.browser.is_busy());
    test.window->invoke_board_play_san("Nf6");
    test.settle();
    CHECK(test.names() == names_list {"Knight"});
    test.window->invoke_board_play_san("Nf6");
    test.settle();
    CHECK(test.names() == names_list {"Knight"});

    // Back at the start every game is listed, as in the unfollowed list.
    test.window->invoke_board_navigate_first();
    test.settle();
    CHECK(test.row_count() == 5);
    CHECK(test.window->get_game_total_count() == 5);
    CHECK(test.status().empty());

    // Off the book the list is empty and says why; with a filter it says so too.
    test.play({"h3"});
    test.settle();
    CHECK(test.row_count() == 0);
    CHECK(test.status() == "No games reach this position");
    test.window->invoke_game_filters_changed("Early", "");
    test.settle();
    CHECK(test.status() == "No games reach this position with these filters");
    CHECK(test.window->get_game_following());
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("browser host: filters and sort narrow a followed list and the position stays applied",
          "[motif-slint-app][host][browser][follow]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto database = make_opening_database();
    auto test = host {};
    test.open(database);
    test.play({"Nf3", "Nc6"});
    test.follow_on();
    test.settle();
    REQUIRE(test.names() == names_list {"Early", "Knight", "Late"});

    test.window->invoke_game_filters_changed("", "1-0");
    test.settle();
    CHECK(test.names() == names_list {"Early", "Knight"});
    CHECK(test.window->get_game_total_count() == 2);
    CHECK(test.filter_changes == 1);

    test.window->invoke_game_sort_requested(0, /*arg_1=*/false);
    test.settle();
    CHECK(test.names() == names_list {"Knight", "Early"});
    CHECK(test.window->get_game_following());

    test.window->invoke_game_filters_changed("Late", "1-0");
    test.settle();
    CHECK(test.row_count() == 0);
    CHECK(test.status() == "No games reach this position with these filters");
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("browser host: the newest position wins and an older completion never renders", "[motif-slint-app][host][browser][follow]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto database = make_opening_database();
    auto test = host {};
    test.open(database);
    test.play({"Nf3", "Nc6"});
    test.follow_on();
    test.settle();
    REQUIRE(test.row_count() == 3);

    // Two position changes reach the runner while a query is still running (the
    // board's own gate cannot produce this, so the runner is asked directly).
    test.browser.position_changed(hash_of({"e4"}));
    test.browser.wait_for_worker();  // its result is ready but the loop has not applied it
    test.browser.position_changed(hash_of({"e4", "e5"}));
    REQUIRE(test.browser.is_busy());

    // The loop turns once: the older result is dropped, the newer query starts.
    REQUIRE(test.loop.run(no_delay) == 1);
    CHECK(test.row_count() == 3);  // the rows of e4 never appeared
    CHECK(test.window->get_game_refreshing());
    CHECK(test.browser.is_busy());
    test.settle();
    CHECK(test.names() == names_list {"Other"});
    CHECK_FALSE(test.window->get_game_refreshing());
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("browser host: a selected game stays selected across a position change or is deselected with it",
          "[motif-slint-app][host][browser][follow]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto database = make_opening_database();
    auto test = host {};
    test.open(database);
    test.play({"Nf3", "Nc6"});
    test.follow_on();
    test.settle();
    test.window->invoke_game_selected(1);  // Knight
    REQUIRE(test.window->get_game_selected_row() == 1);

    test.play({"Nc3"});
    test.settle();
    CHECK(test.names() == names_list {"Knight"});
    CHECK(test.window->get_game_selected_row() == 0);

    // Late does not reach 1. Nf3 Nc6 2. Nc3, so selecting it and moving on leaves no hidden selection.
    test.window->invoke_board_navigate_previous();
    test.settle();
    test.window->invoke_game_selected(2);
    REQUIRE(test.window->get_game_selected_row() == 2);
    test.window->invoke_board_navigate_next();
    test.settle();
    CHECK(test.names() == names_list {"Knight"});
    CHECK(test.window->get_game_selected_row() == -1);
    // Nothing to open: no question about discarding the board, just the reason.
    test.window->invoke_game_activate_requested();
    CHECK_FALSE(test.browser.is_busy());
    CHECK(test.loop.pending(no_delay) == 0);
    CHECK_FALSE(test.window->get_confirm_visible());
    CHECK_FALSE(text(test.window->get_browser_error_text()).empty());
    CHECK(test.window->get_board_edited());
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("browser host: opening a followed row lands on the followed position and leaves the list alone",
          "[motif-slint-app][host][browser][follow]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto database = make_opening_database();
    auto test = host {};
    test.open(database);
    test.play({"Nf3", "Nc6"});
    test.follow_on();
    test.settle();
    REQUIRE(test.names() == names_list {"Early", "Knight", "Late"});
    auto const position = test.board.current_hash();
    REQUIRE(position.has_value());

    // Late reaches the position at ply 6; the typed moves are replaced after the confirmation.
    test.window->invoke_game_selected(2);
    auto const serial = test.window->get_game_result_serial();
    test.window->invoke_game_activate_requested();
    REQUIRE(test.window->get_confirm_visible());
    test.window->invoke_confirm_accepted();
    test.settle();

    CHECK(text(test.window->get_board_white_name()) == "Late");
    CHECK(test.board.current_hash() == position);
    CHECK(test.window->get_board_notation()->row_count() == 6);
    CHECK(test.window->get_board_current_node() != 0);
    CHECK_FALSE(test.window->get_board_edited());
    // Nothing refreshed: same rows, same selection, no new result, nothing running.
    CHECK(test.names() == names_list {"Early", "Knight", "Late"});
    CHECK(test.window->get_game_selected_row() == 2);
    CHECK(test.window->get_game_result_serial() == serial);
    CHECK(test.window->get_game_page_index() == 0);
    CHECK_FALSE(test.browser.is_busy());
    CHECK(test.loop.pending(no_delay) == 0);
    CHECK(text(test.window->get_game_active_title()).starts_with("Late"));

    // Stepping the opened game moves the list with it, and the game stays listed and selected.
    test.window->invoke_board_navigate_previous();
    test.settle();
    CHECK(test.names() == names_list {"Early", "Knight", "Late"});  // all three played 1. Nf3 as White
    test.window->invoke_board_navigate_previous();
    test.settle();
    CHECK(test.window->get_game_selected_row() == 2);
    CHECK(test.window->get_game_result_serial() != serial);
    test.window->invoke_board_navigate_first();
    test.settle();
    CHECK(test.row_count() == 5);
    CHECK(test.window->get_game_selected_row() == 2);  // Late among all five, White ascending
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("browser host: Follow board is refused under each busy condition and the toggle snaps back",
          "[motif-slint-app][host][browser][follow]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto database = make_opening_database();
    for (auto const& busy : busy_conditions) {
        auto test = host {};
        test.open(database);
        test.play({"Nf3", "Nc6"});
        test.activity = busy;

        test.window->set_game_following(true);  // what a pressed, self-toggling control would claim
        test.follow_on();

        CHECK_FALSE(test.window->get_game_following());
        CHECK_FALSE(test.browser.is_busy());
        CHECK(test.loop.pending(no_delay) == 0);
        CHECK(test.row_count() == 5);
    }

    // Turning it off is gated the same way.
    auto test = host {};
    test.open(database);
    test.play({"Nf3", "Nc6"});
    test.follow_on();
    test.settle();
    REQUIRE(test.row_count() == 3);
    test.activity = workspace_activity {.import_active = true};
    test.follow_off();
    CHECK(test.window->get_game_following());
    CHECK(test.row_count() == 3);
    CHECK_FALSE(test.browser.is_busy());
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("browser host: Follow board is refused while the confirmation is shown", "[motif-slint-app][host][browser][follow]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto database = make_opening_database();
    auto test = host {};
    test.open(database);
    test.play({"Nf3", "Nc6"});
    test.window->invoke_board_new_game_requested();  // edited board: asks first
    REQUIRE(test.window->get_confirm_visible());

    test.window->set_game_following(true);  // what a pressed, self-toggling control would claim
    test.follow_on();
    CHECK_FALSE(test.window->get_game_following());
    CHECK_FALSE(test.browser.is_busy());
    CHECK(test.loop.pending(no_delay) == 0);
    CHECK(test.row_count() == 5);

    // Answered, it is accepted again.
    test.window->invoke_confirm_cancelled();
    REQUIRE_FALSE(test.window->get_confirm_visible());
    test.follow_on();
    test.settle();
    CHECK(test.window->get_game_following());
    CHECK(test.row_count() == 3);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("browser host: turning Follow board off returns to the page shown before and keeps the filters",
          "[motif-slint-app][host][browser][follow]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto database = make_numbered_database(120);
    auto test = host {};
    test.open(database);
    test.play({"e4"});
    test.window->invoke_game_page_requested(1);
    test.settle();
    REQUIRE(test.window->get_game_page_index() == 1);

    test.follow_on();
    test.settle();
    CHECK(test.window->get_game_page_index() == 0);
    CHECK(test.window->get_game_total_count() == 120);

    test.follow_off();
    test.settle();
    CHECK_FALSE(test.window->get_game_following());
    CHECK(test.window->get_game_page_index() == 1);
    CHECK(test.row_count() == 20);

    // Filters changed while following stay when it goes off, from the first page.
    test.follow_on();
    test.settle();
    test.window->invoke_game_filters_changed("White 007", "");
    test.settle();
    test.follow_off();
    test.settle();
    CHECK(test.window->get_game_page_index() == 0);
    CHECK(test.row_count() == 1);
    CHECK(text(test.window->get_game_player_filter()) == "White 007");
}

TEST_CASE("browser host: Follow board is refused with a message when the board has no position", "[motif-slint-app][host][browser][follow]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto database = make_opening_database();
    auto test = host {};
    test.open(database);
    test.board.detach();

    test.window->set_game_following(true);
    test.follow_on();

    CHECK_FALSE(test.window->get_game_following());
    CHECK_FALSE(text(test.window->get_browser_error_text()).empty());
    CHECK(test.row_count() == 5);
    CHECK_FALSE(test.browser.is_busy());
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("browser host: the Explorer preset runs no follow queries and the list catches up when it comes back",
          "[motif-slint-app][host][browser][follow]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto database = make_opening_database();
    auto test = host {};
    test.window->set_has_database(true);
    test.open(database);
    state->window_adapter->render();
    test.play({"Nf3", "Nc6"});
    test.follow_on();
    test.settle();
    REQUIRE(test.names() == names_list {"Early", "Knight", "Late"});

    test.window->set_workspace_preset(WorkspacePreset::Explorer);
    state->window_adapter->render();
    test.play({"Nc3"});
    test.settle();
    CHECK(test.loop.pending(no_delay) == 0);
    CHECK_FALSE(test.browser.is_busy());
    CHECK(test.names() == names_list {"Early", "Knight", "Late"});  // kept as it was
    CHECK(test.window->get_game_following());

    test.window->set_workspace_preset(WorkspacePreset::Review);
    state->window_adapter->render();  // mounting the browser reports it
    CHECK(test.browser.is_busy());
    test.settle();
    CHECK(test.names() == names_list {"Knight"});

    // Back without a position change: nothing to refresh.
    test.window->set_workspace_preset(WorkspacePreset::Analysis);
    state->window_adapter->render();
    test.settle();
    CHECK(test.loop.pending(no_delay) == 0);
    CHECK(test.names() == names_list {"Knight"});
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("browser host: a workspace change turns Follow board off and drops results still in flight",
          "[motif-slint-app][host][browser][follow]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto first = make_opening_database();
    auto second = make_numbered_database(3);
    auto test = host {};
    test.open(first);
    test.play({"Nf3", "Nc6"});
    test.follow_on();
    test.settle();
    REQUIRE(test.window->get_game_following());

    test.browser.position_changed(hash_of({"e4"}));
    test.browser.wait_for_worker();  // the e4 list is computed but not yet applied
    test.open(second);  // another database is opened in its place
    CHECK_FALSE(test.window->get_game_following());
    test.settle();

    CHECK_FALSE(test.window->get_game_following());
    CHECK(test.names() == names_list {"White 000", "White 001", "White 002"});
    CHECK(test.window->get_game_total_count() == 3);
    CHECK_FALSE(test.window->get_game_refreshing());
    CHECK_FALSE(test.browser.is_busy());

    // Closing it clears the toggle and the list.
    test.follow_on();
    test.browser.detach();
    CHECK_FALSE(test.window->get_game_following());
    CHECK(test.row_count() == 0);
    CHECK(test.status().empty());
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("browser host: a failed refresh clears the rows with a visible message and the next position change retries",
          "[motif-slint-app][host][browser][follow]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto database = make_opening_database(/*index=*/false);  // no position index: a positioned query fails
    auto test = host {};
    test.open(database);
    REQUIRE(test.row_count() == 5);
    test.play({"e4"});

    test.follow_on();
    test.settle();

    CHECK(test.window->get_game_following());
    CHECK(test.row_count() == 0);
    CHECK(test.window->get_game_total_count() == 0);
    CHECK(test.status() == "Could not load the games for this position");
    CHECK_FALSE(text(test.window->get_browser_error_text()).empty());
    CHECK_FALSE(test.window->get_game_refreshing());
    CHECK_FALSE(test.window->get_game_progress_visible());
    CHECK_FALSE(test.browser.is_busy());
    CHECK(test.window->get_game_selected_row() == -1);

    // The index appears; the next position change retries and the message goes.
    REQUIRE(database.rebuild_position_postings().has_value());
    test.play({"e5"});
    test.settle();
    CHECK(test.names() == names_list {"Other"});
    CHECK(test.status().empty());
    CHECK(text(test.window->get_browser_error_text()).empty());
    CHECK(test.window->get_game_following());
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("browser host: a query that outlasts the delay shows the progress bar until it lands, a quick one never does",
          "[motif-slint-app][host][browser][follow]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto database = make_numbered_database(120);
    auto test = host {};
    test.open(database);
    REQUIRE(test.loop.run(motif::slint_app::browser_progress_delay) == 1);  // the first load's timer, long over

    // A slow one: the delay passes while the query is still in flight.
    test.window->invoke_game_page_requested(1);
    REQUIRE(test.loop.pending(motif::slint_app::browser_progress_delay) == 1);
    test.browser.wait_for_worker();
    CHECK_FALSE(test.window->get_game_progress_visible());
    test.loop.run(motif::slint_app::browser_progress_delay);
    CHECK(test.window->get_game_progress_visible());
    CHECK(test.browser.is_busy());
    test.settle();
    CHECK_FALSE(test.window->get_game_progress_visible());

    // A quick one: the result lands first and the late timer finds nothing to show.
    test.window->invoke_game_sort_requested(0, /*arg_1=*/false);
    REQUIRE(test.loop.pending(motif::slint_app::browser_progress_delay) == 1);
    test.settle();
    test.loop.run(motif::slint_app::browser_progress_delay);
    CHECK_FALSE(test.window->get_game_progress_visible());

    // The timer of one query does not show the bar for the next.
    test.window->invoke_game_page_requested(1);
    test.browser.wait_for_worker();
    test.loop.run(no_delay);  // lands; the old timer is still waiting
    test.window->invoke_game_page_requested(0);
    REQUIRE(test.loop.pending(motif::slint_app::browser_progress_delay) == 2);
    test.loop.run(motif::slint_app::browser_progress_delay);
    CHECK(test.window->get_game_progress_visible());  // the newest query is still running
    test.settle();
    CHECK_FALSE(test.window->get_game_progress_visible());
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("browser host: the progress bar goes away when the query fails", "[motif-slint-app][host][browser][follow]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto database = make_opening_database(/*index=*/false);
    auto test = host {};
    test.open(database);
    test.play({"e4"});
    test.follow_on();
    test.browser.wait_for_worker();
    test.loop.run(motif::slint_app::browser_progress_delay);
    CHECK(test.window->get_game_progress_visible());
    test.settle();
    CHECK_FALSE(test.window->get_game_progress_visible());
    CHECK(test.status() == "Could not load the games for this position");
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("browser host: an import that finishes reloads the followed list for the board position",
          "[motif-slint-app][host][browser][follow]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto database = make_opening_database();
    auto test = host {};
    test.open(database);
    test.play({"Nf3", "Nc6"});
    test.follow_on();
    test.settle();
    test.window->invoke_game_page_requested(0);
    REQUIRE(test.row_count() == 3);

    // The import adds a game that reaches the position, then main() reloads the list.
    REQUIRE(database.insert_game(make_game("Zed", "Zoe", "1-0", {"Nf3", "Nc6", "e4"})).has_value());
    REQUIRE(database.rebuild_position_postings().has_value());
    test.browser.load_initial();
    test.settle();

    CHECK(test.names() == names_list {"Early", "Knight", "Late", "Zed"});
    CHECK(test.window->get_game_following());
}

TEST_CASE("browser host: a runner destroyed while refreshes are in flight or queued shuts down cleanly",
          "[motif-slint-app][host][browser][follow]")
{
    // Built for the sanitizer runs: toggling, position churn and shutdown with work still queued.
    auto const state = snapshot_test::use_snapshot_platform();
    auto database = make_numbered_database(120);
    auto loop = manual_loop {};
    auto window = WorkspaceWindow::create();
    motif::slint_app::clear_browser(*window);
    {
        auto browser = motif::slint_app::async_browser_runner {window, loop.environment()};
        browser.attach(database);
        browser.load_initial();
        browser.wait_for_worker();
        loop.run(no_delay);
        browser.set_follow(/*enabled=*/true, hash_of({"e4"}));
        for (auto round = 0; round < 40; ++round) {
            browser.position_changed(hash_of(round % 2 == 0 ? std::vector<std::string> {"e4", "e5"} : std::vector<std::string> {"e4"}));
            if (round % 8 == 0) {
                browser.wait_for_worker();
                loop.run(no_delay);
            }
        }
        browser.set_follow(/*enabled=*/false, std::nullopt);
    }
    // Everything still scheduled belongs to a runner that is gone: it must not touch the window.
    loop.run(no_delay);
    loop.run(motif::slint_app::browser_progress_delay);
    CHECK_FALSE(window->get_game_progress_visible());
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("browser host: returning from the Explorer while a search runs still catches the list up",
          "[motif-slint-app][host][browser][follow]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto database = make_opening_database();
    auto test = host {};
    test.window->set_has_database(true);
    test.open(database);
    state->window_adapter->render();
    test.play({"Nf3", "Nc6"});
    test.follow_on();
    test.settle();
    REQUIRE(test.names() == names_list {"Early", "Knight", "Late"});

    test.window->set_workspace_preset(WorkspacePreset::Explorer);
    state->window_adapter->render();
    test.play({"Nc3"});  // the Explorer's auto-search would now be running
    test.activity = workspace_activity {.search_busy = true};
    test.window->set_workspace_preset(WorkspacePreset::Review);
    state->window_adapter->render();  // mounts while the search runs

    CHECK(test.browser.is_busy());  // the catch-up was queued, not dropped
    test.activity = workspace_activity {};
    test.settle();
    CHECK(test.names() == names_list {"Knight"});
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("browser host: an import that ends in the Explorer runs no follow query and the list catches up on return",
          "[motif-slint-app][host][browser][follow]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto database = make_opening_database();
    auto test = host {};
    test.window->set_has_database(true);
    test.open(database);
    state->window_adapter->render();
    test.play({"Nf3", "Nc6"});
    test.follow_on();
    test.settle();
    REQUIRE(test.row_count() == 3);

    test.window->set_workspace_preset(WorkspacePreset::Explorer);
    state->window_adapter->render();
    test.play({"Nc3"});
    REQUIRE(database.insert_game(make_game("Zed", "Zoe", "1-0", {"Nf3", "Nc6", "Nc3"})).has_value());
    REQUIRE(database.rebuild_position_postings().has_value());
    test.browser.load_initial();  // what main() does when an import finishes

    CHECK_FALSE(test.browser.is_busy());
    CHECK(test.loop.pending(no_delay) == 0);
    CHECK(test.row_count() == 3);

    test.window->set_workspace_preset(WorkspacePreset::Review);
    state->window_adapter->render();
    test.settle();
    CHECK(test.names() == names_list {"Knight", "Zed"});
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("browser host: mounting during an import waits for the reload that ends it", "[motif-slint-app][host][browser][follow]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto database = make_opening_database();
    auto test = host {};
    test.window->set_has_database(true);
    test.open(database);
    state->window_adapter->render();
    test.play({"Nf3", "Nc6"});
    test.follow_on();
    test.settle();

    test.window->set_workspace_preset(WorkspacePreset::Explorer);
    state->window_adapter->render();
    test.play({"Nc3"});
    test.activity = workspace_activity {.import_active = true};
    test.window->set_workspace_preset(WorkspacePreset::Review);
    state->window_adapter->render();
    CHECK_FALSE(test.browser.is_busy());  // the index is unusable until the import ends
    CHECK(test.row_count() == 3);

    test.activity = workspace_activity {};
    test.browser.load_initial();
    test.settle();
    CHECK(test.names() == names_list {"Knight"});
}

// A file dialog is not workspace activity (Story 13.6): a browser query still starts
// and completes through the registered callbacks while one is open, and the board
// keeps answering, as it does with no dialog.
// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("browser host: filters, paging, sort and Follow board still run while a file dialog is open",
          "[motif-slint-app][host][browser][picker]")
{
    auto const state = snapshot_test::use_snapshot_platform();
    auto database = make_numbered_database(120);
    auto test = host {};
    test.open(database);
    REQUIRE(test.row_count() == 100);

    auto picker = std::make_shared<motif::slint_app::scripted_file_picker>(motif::slint_app::scripted_file_picker::script {
        .behaviour = motif::slint_app::scripted_file_picker::mode::hang, .text = {}, .abortable = true});
    auto dialogs = motif::slint_app::async_picker_runner {test.window, test.loop.environment(), picker};
    motif::slint_app::register_picker_callbacks(*test.window, dialogs);
    test.window->invoke_browse_requested(BrowseTarget::ImportPgn);
    REQUIRE(picker->wait_for_calls(1, std::chrono::seconds {10}));
    REQUIRE(test.window->get_picker_open());

    test.window->invoke_game_page_requested(1);
    test.settle();
    CHECK(test.row_count() == 20);
    CHECK(test.window->get_game_page_index() == 1);

    test.window->invoke_game_sort_requested(0, /*arg_1=*/false);
    test.settle();
    CHECK(test.white(0) == "White 119");

    test.window->invoke_game_filters_changed("White 007", "");
    test.settle();
    CHECK(test.row_count() == 1);
    CHECK(test.window->get_game_total_count() == 1);
    CHECK(test.filter_changes == 1);

    test.window->invoke_game_filters_changed("", "");
    test.settle();
    test.window->invoke_game_selected(2);
    CHECK(test.window->get_game_selected_row() == 2);

    test.follow_on();
    test.settle();
    CHECK(test.window->get_game_following());
    test.play({"e4"});
    test.settle();
    CHECK(test.window->get_board_current_node() > 0);
    test.follow_off();
    test.settle();
    CHECK_FALSE(test.window->get_game_following());

    // Through all of it the dialog stayed open and was not an activity.
    CHECK(test.window->get_picker_open());
    CHECK(dialogs.open());

    picker->finish(motif::slint_app::picker_outcome::cancelled());
    dialogs.shutdown();
}

// NOLINTEND(portability-template-virtual-member-function,cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
