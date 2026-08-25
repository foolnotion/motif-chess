#include <chrono>
#include <cstddef>
#include <string>
#include <utility>

#include "motif/slint_app/game_browser_presenter.hpp"

#include <catch2/catch_test_macros.hpp>

#include "motif/db/database_manager.hpp"
#include "motif/db/types.hpp"
#include "test_helpers.hpp"

namespace
{

constexpr std::size_t large_game_count {1'025};
constexpr std::size_t last_page_index {10};
constexpr std::size_t selection_game_count {125};
constexpr std::size_t performance_game_count {100'000};
constexpr auto interaction_limit = std::chrono::milliseconds {100};

void skip_perf_unless_release_build()
{
    if (test_helpers::is_sanitized_build) {
        SKIP("performance checks are skipped in sanitize builds");
    }
#ifndef NDEBUG
    SKIP("performance checks require a Release build");
#endif
}

auto make_game(std::string white, std::string black, std::string result = "1-0") -> motif::db::game
{
    auto game = motif::db::game {};
    game.white.name = std::move(white);
    game.black.name = std::move(black);
    game.result = std::move(result);
    game.event_details = motif::db::event {.name = "Browser Open", .site = "Local", .date = "2026.08.25"};
    game.date = "2026.08.25";
    game.eco = "C20";
    return game;
}

void insert_games(motif::db::database_manager& database, std::size_t const count)
{
    REQUIRE(database.writer().begin_transaction().has_value());
    for (std::size_t index = 0; index < count; ++index) {
        REQUIRE(database.writer()
                    .insert(make_game("White " + std::to_string(index), "Black " + std::to_string(index), index % 2 == 0 ? "1-0" : "0-1"))
                    .has_value());
    }
    REQUIRE(database.writer().commit_transaction().has_value());
}

auto load(motif::slint_app::game_browser_presenter& presenter, motif::slint_app::browser_query const& query)
    -> motif::slint_app::browser_result<bool>
{
    auto page = presenter.execute_query(query);
    if (!page) {
        return presenter.apply_query_error(query.generation, page.error());
    }
    return presenter.apply_query(std::move(*page));
}

}  // namespace

TEST_CASE("game_browser_presenter: empty database publishes an empty first page", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    auto presenter = motif::slint_app::game_browser_presenter {*database};

    auto query = presenter.prepare_initial_load();
    REQUIRE(query.has_value());
    REQUIRE(load(presenter, *query).value_or(false));

    CHECK(presenter.state().games.empty());
    CHECK(presenter.state().total_count == 0);
    CHECK(presenter.state().page_index == 0);
    CHECK_FALSE(presenter.state().has_next_page);
    CHECK_FALSE(presenter.state().has_selection);
}

TEST_CASE("game_browser_presenter: pages through more than the storage query cap", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    insert_games(*database, large_game_count);
    auto presenter = motif::slint_app::game_browser_presenter {*database};

    auto first = presenter.prepare_initial_load();
    REQUIRE(first.has_value());
    REQUIRE(load(presenter, *first).value_or(false));
    CHECK(std::cmp_equal(presenter.state().total_count, large_game_count));
    CHECK(presenter.state().has_next_page);

    auto last = presenter.set_page(last_page_index);
    REQUIRE(last.has_value());
    REQUIRE(load(presenter, *last).value_or(false));
    CHECK(presenter.state().page_index == last_page_index);
    CHECK(presenter.state().games.size() == 25);
    CHECK(presenter.state().games.front().id.value > 0);
    CHECK_FALSE(presenter.state().has_next_page);
    CHECK(presenter.state().has_previous_page);
}

TEST_CASE("game_browser_presenter: 100K first page and activation meet interaction target", "[motif-slint-app][performance]")
{
    skip_perf_unless_release_build();
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    insert_games(*database, performance_game_count);
    auto presenter = motif::slint_app::game_browser_presenter {*database};

    auto query = presenter.prepare_initial_load();
    REQUIRE(query.has_value());
    auto const page_started = std::chrono::steady_clock::now();
    REQUIRE(load(presenter, *query).value_or(false));
    auto const page_elapsed = std::chrono::steady_clock::now() - page_started;
    CHECK(page_elapsed < interaction_limit);
    REQUIRE(presenter.state().games.size() == motif::slint_app::browser_page_size);

    REQUIRE(presenter.select_game(0).has_value());
    auto activation = presenter.prepare_activation();
    REQUIRE(activation.has_value());
    auto const activation_started = std::chrono::steady_clock::now();
    auto loaded = presenter.execute_activation(*activation);
    REQUIRE(loaded.has_value());
    REQUIRE(presenter.apply_activation(std::move(*loaded)).value_or(false));
    auto const activation_elapsed = std::chrono::steady_clock::now() - activation_started;
    CHECK(activation_elapsed < interaction_limit);
}

TEST_CASE("game_browser_presenter: filtering combines player and result", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    REQUIRE(database->insert_game(make_game("Alice", "Bob", "1-0")).has_value());
    REQUIRE(database->insert_game(make_game("Alice", "Carol", "0-1")).has_value());
    REQUIRE(database->insert_game(make_game("Dana", "Erin", "1-0")).has_value());
    auto presenter = motif::slint_app::game_browser_presenter {*database};

    auto query = presenter.set_filters("alice", "1-0");
    REQUIRE(query.has_value());
    REQUIRE(load(presenter, *query).value_or(false));

    REQUIRE(presenter.state().games.size() == 1);
    CHECK(presenter.state().games.front().white == "Alice");
    CHECK(presenter.state().games.front().black == "Bob");
    CHECK(presenter.state().total_count == 1);
}

TEST_CASE("game_browser_presenter: sorting is global and deterministic", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    REQUIRE(database->insert_game(make_game("Zulu", "Beta")).has_value());
    REQUIRE(database->insert_game(make_game("alpha", "Gamma")).has_value());
    REQUIRE(database->insert_game(make_game("Mike", "Delta")).has_value());
    auto presenter = motif::slint_app::game_browser_presenter {*database};

    auto ascending = presenter.sort_games(0, /*ascending=*/true);
    REQUIRE(ascending.has_value());
    REQUIRE(load(presenter, *ascending).value_or(false));
    CHECK(presenter.state().games.at(0).white == "alpha");
    CHECK(presenter.state().games.at(2).white == "Zulu");

    auto descending = presenter.sort_games(0, /*ascending=*/false);
    REQUIRE(descending.has_value());
    REQUIRE(load(presenter, *descending).value_or(false));
    CHECK(presenter.state().games.at(0).white == "Zulu");
    CHECK(presenter.state().games.at(2).white == "alpha");
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertions are the branches.
TEST_CASE("game_browser_presenter: selection identity survives sort filter and page changes", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    insert_games(*database, selection_game_count);
    auto presenter = motif::slint_app::game_browser_presenter {*database};

    auto initial = presenter.prepare_initial_load();
    REQUIRE(initial.has_value());
    REQUIRE(load(presenter, *initial).value_or(false));
    REQUIRE(presenter.select_game(4).has_value());
    REQUIRE(presenter.state().selected_row.has_value());
    auto const selected_id = presenter.state().selected_game_id;
    auto const selected_white = presenter.state().games.at(presenter.state().selected_row.value_or(0)).white;

    auto sorted = presenter.sort_games(0, /*ascending=*/false);
    REQUIRE(sorted.has_value());
    REQUIRE(load(presenter, *sorted).value_or(false));
    CHECK(presenter.state().selected_game_id == selected_id);
    CHECK(presenter.state().has_selection);

    auto const hidden_page = presenter.state().selected_row.has_value() ? std::size_t {1} : std::size_t {0};
    auto second_page = presenter.set_page(hidden_page);
    REQUIRE(second_page.has_value());
    REQUIRE(load(presenter, *second_page).value_or(false));
    CHECK(presenter.state().selected_game_id == selected_id);
    CHECK(presenter.state().has_selection);
    CHECK_FALSE(presenter.state().selected_row.has_value());

    auto filtered = presenter.set_filters(selected_white, "");
    REQUIRE(filtered.has_value());
    REQUIRE(load(presenter, *filtered).value_or(false));
    CHECK(presenter.state().selected_game_id == selected_id);
    CHECK(presenter.state().selected_row.has_value());
    CHECK(presenter.state().games.at(presenter.state().selected_row.value_or(0)).id == selected_id);
}

TEST_CASE("game_browser_presenter: stale query results cannot replace current state", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    REQUIRE(database->insert_game(make_game("Alice", "Bob")).has_value());
    REQUIRE(database->insert_game(make_game("Carol", "Dana")).has_value());
    auto presenter = motif::slint_app::game_browser_presenter {*database};

    auto stale_query = presenter.set_filters("Alice", "");
    REQUIRE(stale_query.has_value());
    auto stale_page = presenter.execute_query(*stale_query);
    REQUIRE(stale_page.has_value());

    auto current_query = presenter.set_filters("Carol", "");
    REQUIRE(current_query.has_value());
    REQUIRE(load(presenter, *current_query).value_or(false));
    REQUIRE(presenter.state().games.size() == 1);
    CHECK(presenter.state().games.front().white == "Carol");

    auto const applied = presenter.apply_query(std::move(*stale_page));
    REQUIRE(applied.has_value());
    CHECK_FALSE(*applied);
    REQUIRE(presenter.state().games.size() == 1);
    CHECK(presenter.state().games.front().white == "Carol");
}

TEST_CASE("game_browser_presenter: activation loads the selected game by identity", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    REQUIRE(database->insert_game(make_game("Alice", "Bob")).has_value());
    REQUIRE(database->insert_game(make_game("Carol", "Dana")).has_value());
    auto presenter = motif::slint_app::game_browser_presenter {*database};

    auto query = presenter.prepare_initial_load();
    REQUIRE(query.has_value());
    REQUIRE(load(presenter, *query).value_or(false));
    REQUIRE(presenter.select_game(1).has_value());

    auto activation = presenter.prepare_activation();
    REQUIRE(activation.has_value());
    auto loaded = presenter.execute_activation(*activation);
    REQUIRE(loaded.has_value());
    auto applied = presenter.apply_activation(std::move(*loaded));
    REQUIRE(applied.has_value());
    REQUIRE(*applied);

    CHECK(presenter.state().active_game_id == presenter.state().selected_game_id);
    REQUIRE(presenter.state().active_game.has_value());
    CHECK(presenter.state().active_game.value_or(motif::db::game {}).white.name == "Carol");
    CHECK(presenter.state().error_text.empty());
}

TEST_CASE("game_browser_presenter: keyboard selection and visible errors are safe", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    REQUIRE(database->insert_game(make_game("Alice", "Bob")).has_value());
    auto presenter = motif::slint_app::game_browser_presenter {*database};

    CHECK_FALSE(presenter.prepare_activation().has_value());
    CHECK_FALSE(presenter.state().error_text.empty());

    auto query = presenter.prepare_initial_load();
    REQUIRE(query.has_value());
    REQUIRE(load(presenter, *query).value_or(false));
    auto moved_down = presenter.move_selection(1);
    REQUIRE(moved_down.has_value());
    CHECK(*moved_down == 0);
    CHECK(presenter.state().selected_row == 0);
    auto moved_up = presenter.move_selection(-1);
    REQUIRE(moved_up.has_value());
    CHECK(*moved_up == 0);
    CHECK(presenter.state().selected_row == 0);

    auto const current_generation = presenter.state().query_generation;
    auto visible_error = presenter.apply_query_error(current_generation,
                                                     motif::slint_app::browser_error {
                                                         .code = motif::slint_app::error_code::database_failure,
                                                         .message = "query failed",
                                                     });
    REQUIRE(visible_error.has_value());
    CHECK(*visible_error);
    CHECK(presenter.state().error_text == "query failed");

    auto stale_error = presenter.apply_query_error(current_generation - 1,
                                                   motif::slint_app::browser_error {
                                                       .code = motif::slint_app::error_code::database_failure,
                                                       .message = "stale failure",
                                                   });
    REQUIRE(stale_error.has_value());
    CHECK_FALSE(*stale_error);
    CHECK(presenter.state().error_text == "query failed");
}

TEST_CASE("game_browser_presenter: column resizing is validated and stored", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    auto presenter = motif::slint_app::game_browser_presenter {*database};

    REQUIRE(presenter.resize_column(3, 240).has_value());
    CHECK(presenter.state().column_widths.at(3) == 240);

    REQUIRE(presenter.resize_column(3, 20).has_value());
    CHECK(presenter.state().column_widths.at(3) == motif::slint_app::browser_minimum_column_width);

    REQUIRE(presenter.resize_column(3, 5000).has_value());
    CHECK(presenter.state().column_widths.at(3) == motif::slint_app::browser_maximum_column_width);

    CHECK_FALSE(presenter.resize_column(motif::slint_app::browser_column_count, 100).has_value());
    CHECK_FALSE(presenter.state().error_text.empty());
}

TEST_CASE("game_browser_presenter: move_selection starts fresh when the selection is off-page", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    insert_games(*database, selection_game_count);
    auto presenter = motif::slint_app::game_browser_presenter {*database};

    auto initial = presenter.prepare_initial_load();
    REQUIRE(initial.has_value());
    REQUIRE(load(presenter, *initial).value_or(false));
    REQUIRE(presenter.select_game(4).has_value());
    auto const selected_id = presenter.state().selected_game_id;

    auto second_page = presenter.set_page(1);
    REQUIRE(second_page.has_value());
    REQUIRE(load(presenter, *second_page).value_or(false));
    CHECK(presenter.state().has_selection);
    CHECK(presenter.state().selected_game_id == selected_id);
    CHECK_FALSE(presenter.state().selected_row.has_value());

    auto moved_down = presenter.move_selection(1);
    REQUIRE(moved_down.has_value());
    CHECK(*moved_down == 1);
    REQUIRE(presenter.state().selected_row.has_value());
    CHECK(presenter.state().selected_row == 1);
    CHECK(presenter.state().has_selection);
    CHECK(presenter.state().selected_game_id == presenter.state().games.at(1).id);
    CHECK(presenter.state().selected_game_id != selected_id);
}
