#include <array>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "motif/slint_app/search_host.hpp"

#include <catch2/catch_test_macros.hpp>
#include <slint.h>

#include "manual_event_loop.hpp"
#include "motif/chess/chess.hpp"
#include "motif/db/query_connection.hpp"
#include "motif/slint_app/board_host.hpp"
#include "motif/slint_app/browser_host.hpp"
#include "snapshot_platform.hpp"
#include "workspace.h"

namespace
{

using motif::slint_app::workspace_activity;
constexpr auto first_stale_hash = motif::db::zobrist_hash {123456789};
constexpr auto second_stale_hash = motif::db::zobrist_hash {987654321};
constexpr auto stale_total_sentinel = 73;
constexpr auto shutdown_hash = motif::db::zobrist_hash {1234};
constexpr auto no_delay = std::chrono::milliseconds {0};

struct query_blocker_scope
{
    query_blocker_scope()
        : blocker {std::make_shared<motif::db::query_connection_blocker>()}
    {
        motif::db::set_query_connection_blocker_for_tests(blocker);
    }

    query_blocker_scope(query_blocker_scope const&) = delete;
    auto operator=(query_blocker_scope const&) -> query_blocker_scope& = delete;
    query_blocker_scope(query_blocker_scope&&) = delete;
    auto operator=(query_blocker_scope&&) -> query_blocker_scope& = delete;

    ~query_blocker_scope() { motif::db::set_query_connection_blocker_for_tests({}); }

    std::shared_ptr<motif::db::query_connection_blocker> blocker;
};

struct query_open_gate_scope
{
    query_open_gate_scope()
        : gate {std::make_shared<motif::db::query_connection_open_gate>()}
    {
        motif::db::set_query_connection_open_gate_for_tests(gate);
    }

    query_open_gate_scope(query_open_gate_scope const&) = delete;
    auto operator=(query_open_gate_scope const&) -> query_open_gate_scope& = delete;
    query_open_gate_scope(query_open_gate_scope&&) = delete;
    auto operator=(query_open_gate_scope&&) -> query_open_gate_scope& = delete;

    ~query_open_gate_scope() { motif::db::set_query_connection_open_gate_for_tests({}); }

    std::shared_ptr<motif::db::query_connection_open_gate> gate;
};

template<typename Close>
auto close_within_one_second(Close close, query_blocker_scope const& blocker) -> bool
{
    auto mutex = std::mutex {};
    auto condition = std::condition_variable {};
    auto complete = false;
    auto watchdog = std::thread {[&]() -> void
                                 {
                                     auto lock = std::unique_lock {mutex};
                                     if (!condition.wait_for(lock, std::chrono::seconds {1}, [&]() -> bool { return complete; })) {
                                         blocker.blocker->release();
                                     }
                                 }};
    auto const started = std::chrono::steady_clock::now();
    close();
    auto const elapsed = std::chrono::steady_clock::now() - started;
    {
        std::scoped_lock const lock {mutex};
        complete = true;
    }
    condition.notify_one();
    watchdog.join();
    return elapsed <= std::chrono::seconds {1};
}

template<typename Close>
auto close_gate_within_one_second(Close close, query_open_gate_scope const& gate) -> bool
{
    auto mutex = std::mutex {};
    auto condition = std::condition_variable {};
    auto complete = false;
    auto watchdog = std::thread {[&]() -> void
                                 {
                                     auto lock = std::unique_lock {mutex};
                                     if (!condition.wait_for(lock, std::chrono::seconds {1}, [&]() -> bool { return complete; })) {
                                         gate.gate->release();
                                     }
                                 }};
    auto const started = std::chrono::steady_clock::now();
    close();
    auto const elapsed = std::chrono::steady_clock::now() - started;
    {
        std::scoped_lock const lock {mutex};
        complete = true;
    }
    condition.notify_one();
    watchdog.join();
    return elapsed <= std::chrono::seconds {1};
}

auto make_match_game() -> motif::db::game
{
    auto game = motif::db::game {};
    game.white.name = "Search White";
    game.black.name = "Search Black";
    auto position = motif::chess::board {};
    auto const first_move = motif::chess::apply_san(position, "e4");
    REQUIRE(first_move);
    game.moves.push_back(*first_move);
    auto const second_move = motif::chess::apply_san(position, "e5");
    REQUIRE(second_move);
    game.moves.push_back(*second_move);
    game.eco = "C20";
    return game;
}

auto make_starting_game() -> motif::db::game
{
    auto game = make_match_game();
    game.white.name = "Starting White";
    game.black.name = "Starting Black";
    game.moves.clear();
    return game;
}

struct search_host_fixture
{
    test_support::manual_event_loop loop;
    slint::ComponentHandle<WorkspaceWindow> window {WorkspaceWindow::create()};
    motif::slint_app::async_board_runner board {window};
    motif::slint_app::async_browser_runner browser {window, loop.environment()};
    motif::slint_app::async_search_runner search {window, loop.environment()};
    motif::slint_app::replace_controller confirmation {motif::slint_app::make_replace_confirmation(window, board)};
    workspace_activity activity;
    std::size_t retry_count {};
    std::optional<motif::db::database_manager> database;

    explicit search_host_fixture(bool const index = true)
    {
        auto created = motif::db::database_manager::create_scratch();
        REQUIRE(created);
        database = std::move(*created);
        REQUIRE(database->insert_game(make_match_game()));
        REQUIRE(database->insert_game(make_starting_game()));
        if (index) {
            REQUIRE(database->rebuild_position_postings());
        }
        auto const activation =
            motif::slint_app::make_board_activation_handlers(slint::ComponentWeakHandle<WorkspaceWindow> {window}, board);
        browser.set_activation_handlers(activation.loaded, activation.failed);
        browser.set_external_activation_handlers(activation.loaded_at, activation.failed_at);
        board.add_position_changed_handler([this](motif::db::zobrist_hash const hash) -> void
                                           { motif::slint_app::follow_board_position(search, browser, hash); });
        board.attach();
        browser.attach(*database);
        browser.load_initial();
        if (browser.is_busy()) {
            browser.wait_for_worker();
            (void)loop.run(no_delay);
        }
        search.attach(*database);
        auto const probe = motif::slint_app::activity_probe {[this]() -> workspace_activity
                                                             {
                                                                 auto current = activity;
                                                                 current.browser_busy = current.browser_busy || browser.is_busy();
                                                                 current.search_busy = current.search_busy || search.is_busy();
                                                                 return current;
                                                             }};
        motif::slint_app::register_board_callbacks(*window, board, probe);
        motif::slint_app::register_play_callbacks(*window, board, probe, confirmation);
        motif::slint_app::register_search_callbacks(*window, search, browser, board, probe, confirmation);
        window->on_confirm_accepted([this]() -> void { confirmation.accept(); });
        window->on_confirm_cancelled([this]() -> void { confirmation.cancel(); });
    }

    void drain_search()
    {
        if (search.is_busy()) {
            loop.wait_for_tasks(no_delay, 1);
            (void)loop.run(no_delay);
        }
    }
};

}  // namespace

TEST_CASE("search host: clear search resets all published search state", "[motif-slint-app]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto window = WorkspaceWindow::create();
    motif::slint_app::clear_search(*window);

    CHECK(window->get_search_total_matches() == 0);
    CHECK_FALSE(window->get_search_busy());
    CHECK_FALSE(window->get_search_rows_stale());
}

TEST_CASE("search host: real registrar runs request, page and auto callbacks", "[motif-slint-app]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto test = search_host_fixture {};

    test.window->invoke_search_requested();
    test.drain_search();
    CHECK_FALSE(test.search.is_busy());

    test.window->invoke_search_page_requested(0);
    test.drain_search();
    CHECK_FALSE(test.window->get_search_searching());

    test.window->invoke_search_page_requested(-1);
    CHECK_FALSE(test.search.is_busy());

    test.window->invoke_search_auto_search_toggled(/*arg_0=*/false);
    CHECK_FALSE(test.window->get_search_auto_search());
    test.window->invoke_search_auto_search_toggled(/*arg_0=*/true);
    test.drain_search();
    CHECK(test.window->get_search_auto_search());
}

TEST_CASE("search host: every busy source rejects every registered action and republishes auto state", "[motif-slint-app]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto test = search_host_fixture {};
    auto const busy_cases = std::array {workspace_activity {.import_active = true},
                                        workspace_activity {.workspace_busy = true},
                                        workspace_activity {.browser_busy = true},
                                        workspace_activity {.search_busy = true}};

    test.window->invoke_search_requested();
    test.drain_search();
    REQUIRE(test.window->get_search_matches()->row_count() == 2);
    for (auto const& busy : busy_cases) {
        test.activity = busy;
        auto const persisted = test.window->get_search_auto_search();
        test.window->invoke_search_requested();
        test.window->invoke_search_page_requested(0);
        test.window->invoke_search_match_activated(0);
        test.window->set_search_auto_search(!persisted);
        test.window->invoke_search_auto_search_toggled(!persisted);
        CHECK_FALSE(test.search.is_busy());
        CHECK(test.window->get_search_auto_search() == persisted);
    }
}

TEST_CASE("search host: exported board follow obeys auto preference and reset tracker", "[motif-slint-app]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto test = search_host_fixture {};
    auto const board_hash = test.board.current_hash();
    REQUIRE(board_hash);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const hash = *board_hash;

    test.search.set_auto_search(/*enabled=*/false);
    motif::slint_app::follow_board_position(test.search, test.browser, hash);
    CHECK(test.window->get_search_rows_stale());
    CHECK(test.loop.pending(no_delay) == 0);

    test.search.set_auto_search(/*enabled=*/true);
    motif::slint_app::follow_board_position(test.search, test.browser, hash);
    test.drain_search();
    CHECK_FALSE(test.window->get_search_rows_stale());
    CHECK(motif::slint_app::explorer_rows_current(test.board, test.search));

    motif::slint_app::follow_board_position(test.search, test.browser, hash);
    CHECK(test.loop.pending(no_delay) == 0);

    REQUIRE(test.database);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    test.search.attach(*test.database);
    CHECK_FALSE(test.window->get_search_rows_stale());
    motif::slint_app::follow_board_position(test.search, test.browser, hash);
    test.drain_search();
    CHECK_FALSE(test.window->get_search_rows_stale());
}

TEST_CASE("search host: match activation rechecks activity after confirmation", "[motif-slint-app]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto test = search_host_fixture {};

    test.window->invoke_search_requested();
    test.drain_search();
    REQUIRE(test.window->get_search_matches()->row_count() == 2);

    test.window->invoke_board_play_san("e4");
    REQUIRE(test.board.edited());
    test.drain_search();
    auto const board_before = test.board.current_fen();
    test.window->invoke_search_match_activated(0);
    CHECK(test.window->get_confirm_visible());
    test.activity.import_active = true;
    test.window->invoke_confirm_accepted();
    CHECK_FALSE(test.browser.is_busy());
    test.browser.wait_for_worker();
    (void)test.loop.run(no_delay);
    CHECK(test.board.current_fen() == board_before);
    CHECK(test.board.edited());
    test.activity.import_active = false;

    test.window->invoke_search_match_activated(-1);
    CHECK_FALSE(test.window->get_confirm_visible());
}

TEST_CASE("search host: same hash does not re-query and board notifications use production follow helper", "[motif-slint-app]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto test = search_host_fixture {};
    auto const board_hash = test.board.current_hash();
    REQUIRE(board_hash);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const hash = *board_hash;

    motif::slint_app::follow_board_position(test.search, test.browser, hash);
    test.drain_search();
    REQUIRE(motif::slint_app::explorer_rows_current(test.board, test.search));
    auto const starting_total = test.window->get_search_total_matches();
    motif::slint_app::follow_board_position(test.search, test.browser, hash);
    CHECK(test.loop.pending(no_delay) == 0);

    test.window->invoke_board_play_san("e4");
    test.drain_search();
    CHECK(motif::slint_app::explorer_rows_current(test.board, test.search));
    CHECK(test.window->get_search_total_matches() != starting_total);
}

TEST_CASE("search host: accepted confirmation activates the selected browser game", "[motif-slint-app]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto test = search_host_fixture {};

    test.window->invoke_search_requested();
    test.drain_search();
    REQUIRE(test.window->get_search_matches()->row_count() == 2);
    test.window->invoke_board_play_san("e4");
    REQUIRE(test.board.edited());
    test.drain_search();
    auto const board_before = test.board.current_fen();
    test.window->invoke_search_match_activated(0);
    REQUIRE(test.window->get_confirm_visible());
    test.window->invoke_confirm_accepted();
    test.browser.wait_for_worker();
    (void)test.loop.run(no_delay);
    CHECK(test.window->get_board_white_name() == "Search White");
    CHECK_FALSE(test.board.edited());
}

TEST_CASE("search host: database failure publishes error after one import retry", "[motif-slint-app]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto test = search_host_fixture {/*index=*/false};
    test.search.set_retry_on_database_failure(
        [&test]() -> bool
        {
            ++test.retry_count;
            return true;
        });

    test.window->invoke_search_requested();
    test.drain_search();

    CHECK(test.retry_count == 1);
    CHECK_FALSE(test.window->get_search_error_text().empty());
    CHECK_FALSE(test.window->get_search_searching());
    CHECK_FALSE(test.search.is_busy());
}

TEST_CASE("search host: replaced and detached presenters ignore stale worker completions", "[motif-slint-app]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto test = search_host_fixture {};
    auto replacement_created = motif::db::database_manager::create_scratch();
    REQUIRE(replacement_created);
    auto replacement = std::move(*replacement_created);

    test.search.search(first_stale_hash, test.browser.active_filter());
    REQUIRE(test.search.is_busy());
    test.loop.wait_for_tasks(no_delay, 1);
    test.search.attach(replacement);
    test.window->set_search_total_matches(stale_total_sentinel);
    (void)test.loop.run(no_delay);
    CHECK_FALSE(test.search.is_busy());
    CHECK(test.window->get_search_total_matches() == stale_total_sentinel);

    REQUIRE(test.database);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    test.search.attach(*test.database);
    test.search.search(second_stale_hash, test.browser.active_filter());
    REQUIRE(test.search.is_busy());
    test.loop.wait_for_tasks(no_delay, 1);
    test.search.detach();
    test.window->set_search_total_matches(stale_total_sentinel);
    (void)test.loop.run(no_delay);
    CHECK(test.window->get_search_total_matches() == stale_total_sentinel);
    CHECK_FALSE(test.search.is_busy());
}

TEST_CASE("search host: queued completion after window invalidation is ignored", "[motif-slint-app]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto loop = test_support::manual_event_loop {};
    auto window = std::optional<slint::ComponentHandle<WorkspaceWindow>> {};
    window.emplace(WorkspaceWindow::create());
    auto created = motif::db::database_manager::create_scratch();
    REQUIRE(created);
    auto database = std::move(*created);
    auto search = std::make_unique<motif::slint_app::async_search_runner>(*window, loop.environment());
    search->attach(database);
    search->search(first_stale_hash, {});
    REQUIRE(search->is_busy());
    loop.wait_for_tasks(no_delay, 1);
    window.reset();
    (void)loop.run(no_delay);
    // Existing lifetime semantics deliberately leave busy set when a queued
    // completion loses its window. The assertion proves it did not publish.
    CHECK(search->is_busy());
    search->detach();
    CHECK_FALSE(search->is_busy());
}

TEST_CASE("search host: destroyed runner ignores queued completion with window alive", "[motif-slint-app]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto loop = test_support::manual_event_loop {};
    auto window = WorkspaceWindow::create();
    auto created = motif::db::database_manager::create_scratch();
    REQUIRE(created);
    auto database = std::move(*created);
    {
        auto search = std::make_unique<motif::slint_app::async_search_runner>(window, loop.environment());
        search->attach(database);
        search->search(first_stale_hash, {});
        REQUIRE(search->is_busy());
        loop.wait_for_tasks(no_delay, 1);
    }
    window->set_search_total_matches(stale_total_sentinel);
    (void)loop.run(no_delay);
    CHECK(window->get_search_total_matches() == stale_total_sentinel);
}

TEST_CASE("search host: detach interrupts a blocked dedicated SQLite query", "[motif-slint-app][search][shutdown]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto test = search_host_fixture {};
    auto blocker = query_blocker_scope {};
    test.search.search(shutdown_hash, test.browser.active_filter());
    REQUIRE(blocker.blocker->wait_until_entered_for(std::chrono::seconds {1}));

    CHECK(close_within_one_second([&]() -> void { test.search.detach(); }, blocker));
    test.window->set_search_total_matches(stale_total_sentinel);
    (void)test.loop.run(no_delay);
    CHECK_FALSE(test.search.is_busy());
    CHECK(test.window->get_search_total_matches() == stale_total_sentinel);
    CHECK(blocker.blocker->observed_sqlite_interrupt());
}

TEST_CASE("search host: destruction interrupts a blocked dedicated SQLite query", "[motif-slint-app][search][shutdown]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto loop = test_support::manual_event_loop {};
    auto window = WorkspaceWindow::create();
    auto created = motif::db::database_manager::create_scratch();
    REQUIRE(created);
    auto database = std::move(*created);
    auto blocker = query_blocker_scope {};
    auto search = std::make_unique<motif::slint_app::async_search_runner>(window, loop.environment());
    search->attach(database);
    search->search(shutdown_hash, {});
    REQUIRE(blocker.blocker->wait_until_entered_for(std::chrono::seconds {1}));

    CHECK(close_within_one_second([&]() -> void { search.reset(); }, blocker));
    window->set_search_total_matches(stale_total_sentinel);
    (void)loop.run(no_delay);
    CHECK(window->get_search_total_matches() == stale_total_sentinel);
    CHECK(blocker.blocker->observed_sqlite_interrupt());
}

TEST_CASE("search host: detach cancels before connection publication", "[motif-slint-app][search][shutdown]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto test = search_host_fixture {};
    auto gate = query_open_gate_scope {};
    auto blocker = query_blocker_scope {};
    test.search.search(shutdown_hash, test.browser.active_filter());
    REQUIRE(gate.gate->wait_until_entered_for(std::chrono::seconds {1}));

    CHECK(close_gate_within_one_second([&]() -> void { test.search.detach(); }, gate));
    CHECK_FALSE(blocker.blocker->wait_until_entered_for(no_delay));
    test.window->set_search_total_matches(stale_total_sentinel);
    (void)test.loop.run(no_delay);
    CHECK(test.window->get_search_total_matches() == stale_total_sentinel);
}

TEST_CASE("search host: destruction cancels before connection publication", "[motif-slint-app][search][shutdown]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto loop = test_support::manual_event_loop {};
    auto window = WorkspaceWindow::create();
    auto created = motif::db::database_manager::create_scratch();
    REQUIRE(created);
    auto database = std::move(*created);
    auto gate = query_open_gate_scope {};
    auto blocker = query_blocker_scope {};
    auto search = std::make_unique<motif::slint_app::async_search_runner>(window, loop.environment());
    search->attach(database);
    search->search(shutdown_hash, {});
    REQUIRE(gate.gate->wait_until_entered_for(std::chrono::seconds {1}));

    CHECK(close_gate_within_one_second([&]() -> void { search.reset(); }, gate));
    CHECK_FALSE(blocker.blocker->wait_until_entered_for(no_delay));
    window->set_search_total_matches(stale_total_sentinel);
    (void)loop.run(no_delay);
    CHECK(window->get_search_total_matches() == stale_total_sentinel);
}
