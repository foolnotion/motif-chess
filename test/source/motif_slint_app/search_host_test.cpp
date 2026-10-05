#include <array>
#include <chrono>
#include <functional>
#include <optional>
#include <utility>

#include "motif/slint_app/search_host.hpp"

#include <catch2/catch_test_macros.hpp>
#include <slint.h>

#include "manual_event_loop.hpp"
#include "motif/chess/chess.hpp"
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
constexpr auto no_delay = std::chrono::milliseconds {0};

auto make_match_game() -> motif::db::game
{
    auto game = motif::db::game {};
    game.white.name = "Search White";
    game.black.name = "Search Black";
    game.result = "1-0";
    game.event_details = motif::db::event {.name = "Search Host", .site = "Local", .date = "2026.10.05"};
    game.date = "2026.10.05";
    game.eco = "C20";
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
        search.attach(*database);
        motif::slint_app::register_search_callbacks(*window,
                                                    search,
                                                    browser,
                                                    board,
                                                    motif::slint_app::activity_probe {[this]() -> workspace_activity
                                                                                      {
                                                                                          auto current = activity;
                                                                                          current.search_busy =
                                                                                              current.search_busy || search.is_busy();
                                                                                          return current;
                                                                                      }},
                                                    confirmation);
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

    for (auto const& busy : busy_cases) {
        test.activity = busy;
        auto const auto_search = test.window->get_search_auto_search();
        test.window->invoke_search_requested();
        test.window->invoke_search_page_requested(0);
        test.window->invoke_search_match_activated(0);
        test.window->invoke_search_auto_search_toggled(!auto_search);
        CHECK_FALSE(test.search.is_busy());
        CHECK(test.window->get_search_auto_search() == auto_search);
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
    REQUIRE(test.window->get_search_matches()->row_count() == 1);

    test.window->invoke_search_match_activated(0);
    test.activity.import_active = true;
    test.window->invoke_confirm_accepted();
    CHECK_FALSE(test.browser.is_busy());
    test.activity.import_active = false;

    test.window->invoke_search_match_activated(-1);
    CHECK_FALSE(test.browser.is_busy());
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
    motif::slint_app::follow_board_position(test.search, test.browser, hash);
    CHECK(test.loop.pending(no_delay) == 0);

    test.window->invoke_board_play_san("e4");
    test.drain_search();
    CHECK_FALSE(test.window->get_search_rows_stale());
}

TEST_CASE("search host: accepted confirmation activates the selected browser game", "[motif-slint-app]")
{
    auto const platform = snapshot_test::use_snapshot_platform();
    auto test = search_host_fixture {};

    test.window->invoke_search_requested();
    test.drain_search();
    REQUIRE(test.window->get_search_matches()->row_count() == 1);
    test.window->invoke_search_match_activated(0);
    test.window->invoke_confirm_accepted();
    if (test.browser.is_busy()) {
        test.browser.wait_for_worker();
        (void)test.loop.run(no_delay);
    }
    CHECK(test.board.current_fen().has_value());
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
    (void)test.loop.run(no_delay);
    CHECK_FALSE(test.search.is_busy());
    CHECK(test.window->get_search_total_matches() == 0);

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
