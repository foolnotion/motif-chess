#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "motif/slint_app/game_browser_presenter.hpp"

#include <catch2/catch_test_macros.hpp>

#include "motif/chess/chess.hpp"
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
    REQUIRE(presenter.state().games.size() > 1);
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertions expand to control flow.
TEST_CASE("game_browser_presenter: external activation loads by id independent of row selection", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    auto const selected_id = database->insert_game(make_game("Selected White", "Selected Black"));
    auto const external_id = database->insert_game(make_game("External White", "External Black"));
    REQUIRE(selected_id.has_value());
    REQUIRE(external_id.has_value());
    auto presenter = motif::slint_app::game_browser_presenter {*database};

    auto const initial = presenter.prepare_initial_load();
    REQUIRE(initial.has_value());
    auto page = presenter.execute_query(*initial);
    REQUIRE(page.has_value());
    REQUIRE(presenter.apply_query(std::move(*page)).value_or(false));
    REQUIRE(presenter.select_game(0).has_value());

    auto request = presenter.prepare_external_activation(*external_id);
    REQUIRE(request.has_value());
    auto loaded = presenter.execute_activation(*request);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->game_id == *external_id);
    auto applied = presenter.apply_external_activation(std::move(*loaded));
    REQUIRE(applied.has_value());
    REQUIRE(*applied);

    CHECK(presenter.state().active_game_id == *external_id);
    CHECK(presenter.state().selected_game_id == *external_id);
    if (auto const selected_row = presenter.state().selected_row) {
        CHECK(presenter.state().games[*selected_row].id == *external_id);
    } else {
        FAIL("external activation must select its game row");
    }
}

// ---------------------------------------------------------------------------
// Follow board (Story 13.5)
// ---------------------------------------------------------------------------

namespace
{

using moves = std::vector<std::string>;

// The value of an optional a test has established, in a form the static analysis accepts.
template<typename T>
auto value_of(std::optional<T> const& optional_value) -> T const&
{
    if (!optional_value) {
        throw std::logic_error("the test expected a value");
    }
    return *optional_value;
}

auto make_played_game(moves const& sans, std::string white, std::string black, std::string result = "1-0") -> motif::db::game
{
    auto position = motif::chess::board {};
    auto game = make_game(std::move(white), std::move(black), std::move(result));
    for (auto const& san : sans) {
        auto const move = motif::chess::apply_san(position, san);
        REQUIRE(move);
        game.moves.push_back(*move);
    }
    return game;
}

auto hash_after(moves const& sans) -> motif::db::zobrist_hash
{
    auto position = motif::chess::board {};
    for (auto const& san : sans) {
        auto const move = motif::chess::apply_san(position, san);
        REQUIRE(move);
    }
    return motif::db::zobrist_hash {position.hash()};
}

auto starting_hash() -> motif::db::zobrist_hash
{
    return motif::db::zobrist_hash {motif::chess::board {}.hash()};
}

void add_played(motif::db::database_manager& database, moves const& sans, std::string white, std::string black, std::string result = "1-0")
{
    auto const game = make_played_game(sans, std::move(white), std::move(black), std::move(result));
    REQUIRE(database.insert_game(game).has_value());
}

void index_positions(motif::db::database_manager& database)
{
    REQUIRE(database.rebuild_position_postings().has_value());
}

auto names(motif::slint_app::game_browser_presenter const& presenter) -> std::vector<std::string>
{
    auto result = std::vector<std::string> {};
    for (auto const& game : presenter.state().games) {
        result.push_back(game.white);
    }
    return result;
}

// Runs the query a follow call prepared, the way the runner's worker does.
void land(motif::slint_app::game_browser_presenter& presenter,
          motif::slint_app::browser_result<motif::slint_app::browser_query> const& query)
{
    REQUIRE(query.has_value());
    REQUIRE(load(presenter, *query).value_or(false));
}

// Four games: three reach the knights position (two by transposition), one does not.
auto make_knights_database() -> motif::db::database_manager
{
    auto created = motif::db::database_manager::create_scratch();
    REQUIRE(created.has_value());
    auto database = std::move(*created);
    add_played(database, {"Nf3", "Nc6", "Nc3", "Nf6"}, "Alice", "Bob", "1-0");
    add_played(database, {"Nc3", "Nf6", "Nf3", "Nc6"}, "Carol", "Dan", "0-1");
    add_played(database, {"e4", "e5"}, "Erin", "Frank", "1-0");
    add_played(database, {"Nc3", "Nc6", "Nf3", "Nf6"}, "Alice", "Hal", "0-1");
    index_positions(database);
    return database;
}

auto knights() -> moves
{
    return {"Nf3", "Nc6", "Nc3", "Nf6"};
}

// Reports a new board position and lands the refresh it asks for.
void follow_to(motif::slint_app::game_browser_presenter& presenter, motif::db::zobrist_hash const hash)
{
    auto query = presenter.follow_position(hash);
    REQUIRE(query.has_value());
    auto const& refresh = *query;
    if (!refresh) {
        FAIL("the position change asked for no refresh");
        return;
    }
    REQUIRE(load(presenter, *refresh).value_or(false));
}

auto follow_on(motif::slint_app::game_browser_presenter& presenter, motif::db::zobrist_hash const hash)
    -> motif::slint_app::browser_result<motif::slint_app::browser_query>
{
    return presenter.set_follow(/*enabled=*/true, hash);
}

auto follow_off(motif::slint_app::game_browser_presenter& presenter) -> motif::slint_app::browser_result<motif::slint_app::browser_query>
{
    return presenter.set_follow(/*enabled=*/false, std::nullopt);
}

void load_first_page(motif::slint_app::game_browser_presenter& presenter)
{
    land(presenter, presenter.prepare_initial_load());
}

}  // namespace

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("game_browser_presenter: following lists the games that reach the position, transpositions included, under the filters",
          "[motif-slint-app][follow]")
{
    auto database = make_knights_database();
    auto presenter = motif::slint_app::game_browser_presenter {database};
    load_first_page(presenter);
    REQUIRE(presenter.state().games.size() == 4);

    auto query = follow_on(presenter, hash_after(knights()));
    REQUIRE(query.has_value());
    CHECK(query->filter.position == hash_after(knights()));
    CHECK(query->filter.offset == 0);
    CHECK(presenter.state().loading);
    land(presenter, query);

    CHECK(presenter.state().following);
    CHECK_FALSE(presenter.state().loading);
    CHECK(presenter.state().rows_restricted);
    CHECK(presenter.state().total_count == 3);
    CHECK(names(presenter) == std::vector<std::string> {"Alice", "Alice", "Carol"});  // White ascending, as before
    CHECK(presenter.state().row_plies == std::vector<std::uint16_t> {4, 4, 4});

    // The player filter intersects with the position, and so does the result.
    land(presenter, presenter.set_filters("Alice", ""));
    CHECK(presenter.state().total_count == 2);
    CHECK(names(presenter) == std::vector<std::string> {"Alice", "Alice"});
    land(presenter, presenter.set_filters("Alice", "1-0"));
    CHECK(presenter.state().total_count == 1);
    CHECK(presenter.state().games.front().black == "Bob");
    land(presenter, presenter.set_filters("", "0-1"));
    CHECK(presenter.state().total_count == 2);
    CHECK(presenter.state().following);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("game_browser_presenter: the starting position restricts nothing and matches the unfollowed list", "[motif-slint-app][follow]")
{
    auto database = make_knights_database();
    auto presenter = motif::slint_app::game_browser_presenter {database};
    load_first_page(presenter);
    auto const unfollowed_names = names(presenter);
    auto const unfollowed_total = presenter.state().total_count;

    auto query = follow_on(presenter, starting_hash());
    REQUIRE(query.has_value());
    CHECK_FALSE(query->filter.position.has_value());
    land(presenter, query);

    CHECK(presenter.state().following);
    CHECK_FALSE(presenter.state().rows_restricted);
    CHECK(names(presenter) == unfollowed_names);
    CHECK(presenter.state().total_count == unfollowed_total);
    CHECK(presenter.state().row_plies.empty());
    CHECK(motif::slint_app::browser_status_text(presenter.state()).empty());

    // Moving off the start restricts; coming back lifts the restriction again.
    follow_to(presenter, hash_after({"e4"}));
    CHECK(presenter.state().rows_restricted);
    CHECK(presenter.state().total_count == 1);
    follow_to(presenter, starting_hash());
    CHECK_FALSE(presenter.state().rows_restricted);
    CHECK(presenter.state().total_count == unfollowed_total);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("game_browser_presenter: a position no game reaches gives an empty list that says so", "[motif-slint-app][follow]")
{
    auto database = make_knights_database();
    auto presenter = motif::slint_app::game_browser_presenter {database};
    load_first_page(presenter);

    land(presenter, follow_on(presenter, hash_after({"d4", "d5", "c4"})));
    CHECK(presenter.state().games.empty());
    CHECK(presenter.state().total_count == 0);
    CHECK(presenter.state().rows_restricted);
    CHECK(motif::slint_app::browser_status_text(presenter.state()) == "No games reach this position");

    land(presenter, presenter.set_filters("Alice", ""));
    CHECK(motif::slint_app::browser_status_text(presenter.state()) == "No games reach this position with these filters");

    // Without Follow board an empty list needs no explanation.
    land(presenter, presenter.set_filters("Nobody", ""));
    land(presenter, follow_off(presenter));
    CHECK(presenter.state().games.empty());
    CHECK(motif::slint_app::browser_status_text(presenter.state()).empty());
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("game_browser_presenter: turning follow off removes only the position and returns to the page shown before",
          "[motif-slint-app][follow]")
{
    auto created = motif::db::database_manager::create_scratch();
    REQUIRE(created.has_value());
    auto database = std::move(*created);
    REQUIRE(database.writer().begin_transaction().has_value());
    for (auto index = std::size_t {0}; index < selection_game_count; ++index) {
        REQUIRE(database.writer().insert(make_played_game({"e4"}, "White " + std::to_string(1000 + index), "Black")).has_value());
    }
    REQUIRE(database.writer().commit_transaction().has_value());
    index_positions(database);
    auto presenter = motif::slint_app::game_browser_presenter {database};
    load_first_page(presenter);
    land(presenter, presenter.set_page(1));
    REQUIRE(presenter.state().page_index == 1);

    // On: the followed list starts at its first page and shows the games under the filters.
    land(presenter, follow_on(presenter, hash_after({"e4"})));
    CHECK(presenter.state().page_index == 0);
    CHECK(std::cmp_equal(presenter.state().total_count, selection_game_count));
    land(presenter, presenter.set_page(1));
    CHECK(presenter.state().page_index == 1);

    // Off: back on the page that was showing at turn-on, with the selected game still selected.
    REQUIRE(presenter.select_game(0).has_value());
    auto const selected_id = presenter.state().selected_game_id;
    land(presenter, follow_off(presenter));
    CHECK(presenter.state().has_selection);
    CHECK(presenter.state().selected_game_id == selected_id);
    CHECK_FALSE(presenter.state().following);
    CHECK_FALSE(presenter.state().rows_restricted);
    CHECK(presenter.state().page_index == 1);
    CHECK(presenter.state().games.size() == selection_game_count - motif::slint_app::browser_page_size);

    // A filter or sort change while following makes the old page meaningless.
    land(presenter, follow_on(presenter, hash_after({"e4"})));
    land(presenter, presenter.set_page(1));
    land(presenter, presenter.sort_games(1, /*ascending=*/false));
    land(presenter, follow_off(presenter));
    CHECK(presenter.state().page_index == 0);
    CHECK_FALSE(presenter.state().sort_ascending);

    land(presenter, presenter.set_page(1));
    land(presenter, follow_on(presenter, hash_after({"e4"})));
    land(presenter, presenter.set_filters("White 1005", ""));
    land(presenter, follow_off(presenter));
    CHECK(presenter.state().page_index == 0);
    CHECK(presenter.state().player_filter == "White 1005");
    CHECK(presenter.state().total_count == 1);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("game_browser_presenter: a position change keeps a selected game that is still listed and drops one that is not",
          "[motif-slint-app][follow]")
{
    auto database = make_knights_database();
    auto presenter = motif::slint_app::game_browser_presenter {database};
    load_first_page(presenter);
    land(presenter, follow_on(presenter, hash_after(knights())));

    // Carol's game (row 2) reaches the knights position and, one move earlier, Nc3.
    REQUIRE(presenter.select_game(2).has_value());
    auto const carol = presenter.state().selected_game_id;
    follow_to(presenter, hash_after({"Nc3"}));
    CHECK(presenter.state().has_selection);
    REQUIRE(presenter.state().selected_row.has_value());
    CHECK(presenter.state().games.at(value_of(presenter.state().selected_row)).id == carol);

    // Erin's game does not reach Nc3: no hidden selection survives.
    land(presenter, presenter.set_filters("", ""));
    follow_to(presenter, hash_after({"e4", "e5"}));
    REQUIRE(presenter.select_game(0).has_value());
    follow_to(presenter, hash_after({"Nc3"}));
    CHECK_FALSE(presenter.state().has_selection);
    CHECK_FALSE(presenter.state().selected_row.has_value());
    CHECK_FALSE(presenter.prepare_activation().has_value());

    // Filters, sort and paging keep a selection that is off the visible rows, as they always did.
    REQUIRE(presenter.select_game(0).has_value());
    land(presenter, presenter.set_filters("Nobody", ""));
    CHECK(presenter.state().has_selection);
    CHECK_FALSE(presenter.state().selected_row.has_value());
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("game_browser_presenter: follow_position acts only for a followed list and a new position", "[motif-slint-app][follow]")
{
    auto database = make_knights_database();
    auto presenter = motif::slint_app::game_browser_presenter {database};
    load_first_page(presenter);

    auto unfollowed = presenter.follow_position(hash_after({"e4"}));
    REQUIRE(unfollowed.has_value());
    CHECK_FALSE(unfollowed->has_value());

    land(presenter, follow_on(presenter, hash_after({"e4"})));
    auto const generation = presenter.state().query_generation;
    auto same = presenter.follow_position(hash_after({"e4"}));
    REQUIRE(same.has_value());
    CHECK_FALSE(same->has_value());
    CHECK(presenter.state().query_generation == generation);

    auto changed = presenter.follow_position(hash_after({"Nf3"}));
    REQUIRE(changed.has_value());
    REQUIRE(changed->has_value());
    CHECK(value_of(*changed).generation == generation + 1);
    CHECK(value_of(*changed).filter.offset == 0);
    CHECK(value_of(*changed).filter.position == hash_after({"Nf3"}));
    CHECK(presenter.state().followed_hash == hash_after({"Nf3"}));
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("game_browser_presenter: an older follow result never replaces a newer one", "[motif-slint-app][follow]")
{
    auto database = make_knights_database();
    auto presenter = motif::slint_app::game_browser_presenter {database};
    load_first_page(presenter);
    land(presenter, follow_on(presenter, starting_hash()));

    auto older = presenter.follow_position(hash_after({"e4"}));
    auto newer = presenter.follow_position(hash_after(knights()));
    REQUIRE(older.has_value());
    REQUIRE(newer.has_value());
    REQUIRE(older->has_value());
    REQUIRE(newer->has_value());
    auto const serial = presenter.state().result_serial;

    auto older_page = presenter.execute_query(value_of(*older));
    auto newer_page = presenter.execute_query(value_of(*newer));
    REQUIRE(older_page.has_value());
    REQUIRE(newer_page.has_value());
    REQUIRE(presenter.apply_query(std::move(*newer_page)).value_or(false));
    CHECK_FALSE(presenter.apply_query(std::move(*older_page)).value_or(true));

    CHECK(presenter.state().total_count == 3);
    CHECK(presenter.state().result_serial == serial + 1);
    CHECK_FALSE(presenter.state().loading);

    // An older failure is dropped as well.
    auto stale = presenter.apply_query_error(value_of(*older).generation,
                                             {.code = motif::slint_app::error_code::database_failure, .message = "late"});
    REQUIRE(stale.has_value());
    CHECK_FALSE(*stale);
    CHECK(presenter.state().games.size() == 3);
    CHECK(presenter.state().error_text.empty());
}

// A landed result bumps the serial; selection and activation do not.
TEST_CASE("game_browser_presenter: result_serial moves with landed results only", "[motif-slint-app][follow]")
{
    auto database = make_knights_database();
    auto presenter = motif::slint_app::game_browser_presenter {database};
    CHECK(presenter.state().result_serial == 0);
    load_first_page(presenter);
    auto const first = presenter.state().result_serial;
    CHECK(first > 0);

    REQUIRE(presenter.select_game(1).has_value());
    REQUIRE(presenter.move_selection(1).has_value());
    CHECK(presenter.state().result_serial == first);

    land(presenter, presenter.set_filters("Alice", ""));
    CHECK(presenter.state().result_serial == first + 1);
    land(presenter, presenter.sort_games(1, /*ascending=*/false));
    CHECK(presenter.state().result_serial == first + 2);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("game_browser_presenter: a failed follow refresh drops the rows, says so, and the next change retries",
          "[motif-slint-app][follow]")
{
    // No position index: the database manager refuses a positioned query.
    auto created = motif::db::database_manager::create_scratch();
    REQUIRE(created.has_value());
    auto database = std::move(*created);
    add_played(database, {"e4", "e5"}, "Alice", "Bob");
    add_played(database, {"d4", "d5"}, "Carol", "Dan");
    auto presenter = motif::slint_app::game_browser_presenter {database};
    load_first_page(presenter);
    REQUIRE(presenter.select_game(0).has_value());
    REQUIRE(presenter.state().games.size() == 2);
    auto const serial = presenter.state().result_serial;

    land(presenter, follow_on(presenter, hash_after({"e4"})));

    CHECK(presenter.state().following);
    CHECK(presenter.state().games.empty());
    CHECK(presenter.state().total_count == 0);
    CHECK_FALSE(presenter.state().has_selection);
    CHECK_FALSE(presenter.state().has_next_page);
    CHECK_FALSE(presenter.state().loading);
    CHECK(presenter.state().load_failed);
    CHECK_FALSE(presenter.state().error_text.empty());
    CHECK(presenter.state().result_serial > serial);
    CHECK(motif::slint_app::browser_status_text(presenter.state()) == "Could not load the games for this position");

    // The same position retries after a failure (a remount, for instance).
    auto again = presenter.follow_position(hash_after({"e4"}));
    REQUIRE(again.has_value());
    CHECK(again->has_value());

    // The index appears (for example the import finished); the next position change retries.
    index_positions(database);
    follow_to(presenter, hash_after({"d4"}));
    CHECK_FALSE(presenter.state().load_failed);
    CHECK(presenter.state().error_text.empty());
    CHECK(presenter.state().total_count == 1);
    CHECK(presenter.state().games.front().white == "Carol");

    // Turning follow off lists everything again.
    land(presenter, follow_off(presenter));
    CHECK(presenter.state().total_count == 2);
}

TEST_CASE("game_browser_presenter: following is refused without a board position and leaves the list alone", "[motif-slint-app][follow]")
{
    auto database = make_knights_database();
    auto presenter = motif::slint_app::game_browser_presenter {database};
    load_first_page(presenter);
    auto const generation = presenter.state().query_generation;

    auto refused = presenter.set_follow(/*enabled=*/true, std::nullopt);

    CHECK_FALSE(refused.has_value());
    CHECK_FALSE(presenter.state().following);
    CHECK_FALSE(presenter.state().error_text.empty());
    CHECK(presenter.state().query_generation == generation);
    CHECK_FALSE(presenter.state().loading);
    CHECK(presenter.state().games.size() == 4);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("game_browser_presenter: activating a followed row carries the first ply that reaches the position", "[motif-slint-app][follow]")
{
    auto created = motif::db::database_manager::create_scratch();
    REQUIRE(created.has_value());
    auto database = std::move(*created);
    add_played(database, {"Nf3", "Nc6"}, "Early", "Bob");
    add_played(database, {"Nf3", "Nf6", "Ng1", "Ng8", "Nf3", "Nc6"}, "Late", "Dan");
    add_played(database, {"e4", "e5"}, "Other", "Fay");
    index_positions(database);
    auto presenter = motif::slint_app::game_browser_presenter {database};
    load_first_page(presenter);

    // Unfollowed rows open at the start, as before.
    REQUIRE(presenter.select_game(0).has_value());
    auto plain = presenter.prepare_activation();
    REQUIRE(plain.has_value());
    CHECK_FALSE(plain->ply.has_value());

    land(presenter, follow_on(presenter, hash_after({"Nf3", "Nc6"})));
    REQUIRE(names(presenter) == std::vector<std::string> {"Early", "Late"});
    CHECK(presenter.state().row_plies == std::vector<std::uint16_t> {2, 6});

    REQUIRE(presenter.select_game(1).has_value());
    auto late = presenter.prepare_activation();
    REQUIRE(late.has_value());
    REQUIRE(late->ply.has_value());
    CHECK(value_of(late->ply) == 6);
    CHECK(late->game_id == presenter.state().games.at(1).id);
    auto loaded = presenter.execute_activation(*late);
    REQUIRE(loaded.has_value());
    REQUIRE(presenter.apply_external_activation(std::move(*loaded)).value_or(false));
    CHECK(presenter.state().selected_row == 1);
    CHECK(presenter.state().active_game_id == late->game_id);

    REQUIRE(presenter.select_game(0).has_value());
    auto early = presenter.prepare_activation();
    REQUIRE(early.has_value());
    CHECK(early->ply == 2);

    // A selection that is not a visible row is not opened.
    land(presenter, presenter.set_filters("Late", ""));
    REQUIRE(presenter.select_game(0).has_value());
    land(presenter, presenter.set_filters("Early", ""));
    REQUIRE(presenter.state().has_selection);
    REQUIRE_FALSE(presenter.state().selected_row.has_value());
    auto const generation = presenter.state().activation_generation;
    CHECK_FALSE(presenter.prepare_activation().has_value());
    CHECK_FALSE(presenter.state().error_text.empty());
    CHECK(presenter.state().activation_generation == generation);
}
