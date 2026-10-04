#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "motif/slint_app/game_browser_presenter.hpp"

#include <tl/expected.hpp>

#include "motif/chess/chess.hpp"
#include "motif/db/error.hpp"
#include "motif/db/types.hpp"

namespace motif::slint_app
{

namespace
{

auto to_database_sort(browser_sort_column const column) noexcept -> motif::db::game_sort_column
{
    switch (column) {
        case browser_sort_column::white:
            return motif::db::game_sort_column::white;
        case browser_sort_column::black:
            return motif::db::game_sort_column::black;
        case browser_sort_column::result:
            return motif::db::game_sort_column::result;
        case browser_sort_column::event:
            return motif::db::game_sort_column::event;
        case browser_sort_column::date:
            return motif::db::game_sort_column::date;
        case browser_sort_column::eco:
            return motif::db::game_sort_column::eco;
    }
    std::unreachable();
}

auto nonempty_optional(std::string const& value) -> std::optional<std::string>
{
    return value.empty() ? std::nullopt : std::optional<std::string> {value};
}

}  // namespace

auto browser_status_text(game_browser_state const& state) -> std::string
{
    if (state.load_failed) {
        return state.following ? "Could not load the games for this position" : "Could not load the games";
    }
    if (state.following && state.rows_restricted && state.games.empty() && !state.loading) {
        auto const filtered = !state.player_filter.empty() || !state.result_filter.empty();
        return filtered ? "No games reach this position with these filters" : "No games reach this position";
    }
    return {};
}

game_browser_presenter::game_browser_presenter(motif::db::database_manager& database) noexcept
    : database_(&database)
    , starting_hash_ {motif::chess::board {}.hash()}
{
}

auto game_browser_presenter::prepare_initial_load() -> browser_result<browser_query>
{
    return make_query(0);
}

auto game_browser_presenter::set_filters(std::string player_name, std::string result) -> browser_result<browser_query>
{
    state_.player_filter = std::move(player_name);
    state_.result_filter = std::move(result);
    state_.restore_page.reset();
    return make_query(0);
}

auto game_browser_presenter::sort_games(std::size_t const column, bool const ascending) -> browser_result<browser_query>
{
    if (column >= browser_column_count) {
        return fail_query({.code = error_code::invalid_argument, .message = "Selected sort column is out of range"});
    }
    state_.sort_column = static_cast<browser_sort_column>(column);
    state_.sort_ascending = ascending;
    state_.restore_page.reset();
    return make_query(0);
}

auto game_browser_presenter::set_page(std::size_t const page_index) -> browser_result<browser_query>
{
    auto const total = state_.total_count <= 0 ? std::size_t {0} : static_cast<std::size_t>(state_.total_count);
    auto const page_count = total == 0 ? std::size_t {1} : ((total - 1) / browser_page_size) + 1;
    if (page_index >= page_count) {
        return fail_query({.code = error_code::invalid_argument, .message = "Requested game page is out of range"});
    }
    return make_query(page_index);
}

auto game_browser_presenter::next_page() -> browser_result<browser_query>
{
    if (!state_.has_next_page) {
        return fail_query({.code = error_code::invalid_argument, .message = "Already at the last game page"});
    }
    return make_query(state_.page_index + 1);
}

auto game_browser_presenter::previous_page() -> browser_result<browser_query>
{
    if (!state_.has_previous_page) {
        return fail_query({.code = error_code::invalid_argument, .message = "Already at the first game page"});
    }
    return make_query(state_.page_index - 1);
}

auto game_browser_presenter::execute_query(browser_query const& query) const -> browser_result<browser_page>
{
    auto const page_index = query.filter.offset / browser_page_size;
    if (!query.filter.position) {
        auto result = database_->store().find_games(query.filter);
        if (!result) {
            return tl::unexpected {from_database_error(result.error())};
        }
        return browser_page {
            .generation = query.generation,
            .games = std::move(result->games),
            .total_count = result->total_count,
            .page_index = page_index,
            .plies = {},
            .restricted = false,
            .drop_missing_selection = query.drop_missing_selection,
        };
    }

    // Only the database manager matches positions; the game store ignores them.
    auto result = database_->find_games(query.filter);
    if (!result) {
        return tl::unexpected {from_database_error(result.error())};
    }
    auto plies = std::vector<std::uint16_t> {};
    if (!result->games.empty()) {
        auto game_ids = std::vector<motif::db::game_id> {};
        game_ids.reserve(result->games.size());
        for (auto const& game : result->games) {
            game_ids.push_back(game.id);
        }
        auto matches = database_->query_position_first_matches(*query.filter.position, game_ids);
        if (!matches) {
            return tl::unexpected {from_database_error(matches.error())};
        }
        std::ranges::sort(*matches, {}, &motif::db::position_match::game_id);
        plies.reserve(result->games.size());
        for (auto const& game : result->games) {
            auto const found = std::ranges::lower_bound(*matches, game.id, {}, &motif::db::position_match::game_id);
            if (found == matches->end() || found->game_id != game.id) {
                return tl::unexpected {
                    browser_error {.code = error_code::database_failure, .message = "Position results changed while the game list was loaded."}};
            }
            plies.push_back(found->ply);
        }
    }
    return browser_page {
        .generation = query.generation,
        .games = std::move(result->games),
        .total_count = result->total_count,
        .page_index = page_index,
        .plies = std::move(plies),
        .restricted = true,
        .drop_missing_selection = query.drop_missing_selection,
    };
}

auto game_browser_presenter::apply_query(browser_page page) -> browser_result<bool>
{
    if (page.generation != state_.query_generation) {
        return false;
    }

    state_.games = std::move(page.games);
    state_.row_plies = std::move(page.plies);
    state_.rows_restricted = page.restricted;
    state_.total_count = page.total_count;
    state_.page_index = page.page_index;
    state_.has_previous_page = page.page_index > 0;
    auto const next_offset = static_cast<std::int64_t>((page.page_index + 1) * browser_page_size);
    state_.has_next_page = page.total_count > 0 && next_offset < page.total_count;
    state_.selected_row.reset();
    if (state_.has_selection) {
        auto const selected = std::ranges::find(state_.games, state_.selected_game_id, &motif::db::game_list_entry::id);
        if (selected != state_.games.end()) {
            state_.selected_row = static_cast<std::size_t>(std::distance(state_.games.begin(), selected));
        } else if (page.drop_missing_selection) {
            state_.has_selection = false;
        }
    }
    state_.error_text.clear();
    state_.loading = false;
    state_.load_failed = false;
    ++state_.result_serial;
    return true;
}

auto game_browser_presenter::set_follow(bool const enabled, std::optional<motif::db::zobrist_hash> const board_hash)
    -> browser_result<browser_query>
{
    if (enabled) {
        if (!board_hash) {
            return fail_query({.code = error_code::invalid_argument, .message = "The position on the board cannot be followed"});
        }
        if (!state_.following) {
            state_.restore_page = state_.page_index;
        }
        state_.following = true;
        state_.followed_hash = board_hash;
        return make_query(0, /*drop_missing_selection=*/true);
    }
    auto const restore = state_.restore_page.value_or(0);
    state_.following = false;
    state_.followed_hash.reset();
    state_.restore_page.reset();
    return make_query(restore);
}

auto game_browser_presenter::follow_position(motif::db::zobrist_hash const hash) -> browser_result<std::optional<browser_query>>
{
    if (!state_.following || state_.followed_hash == hash) {
        return std::optional<browser_query> {};
    }
    state_.followed_hash = hash;
    auto query = make_query(0, /*drop_missing_selection=*/true);
    if (!query) {
        return tl::unexpected {query.error()};
    }
    return std::optional<browser_query> {std::move(*query)};
}

auto game_browser_presenter::apply_query_error(std::uint64_t const generation, browser_error query_error) -> browser_result<bool>
{
    if (generation != state_.query_generation) {
        return false;
    }
    state_.loading = false;
    state_.error_text = std::move(query_error.message);
    // Rows of another position, or of a restriction that is gone, must not
    // stay on screen as if they were the answer.
    if (state_.following || state_.rows_restricted) {
        state_.games.clear();
        state_.row_plies.clear();
        state_.rows_restricted = false;
        state_.total_count = 0;
        state_.page_index = 0;
        state_.has_previous_page = false;
        state_.has_next_page = false;
        state_.selected_row.reset();
        state_.has_selection = false;
        state_.load_failed = true;
        ++state_.result_serial;
    }
    return true;
}

auto game_browser_presenter::select_game(std::size_t const row) -> browser_result<void>
{
    if (row >= state_.games.size()) {
        return fail({.code = error_code::invalid_argument, .message = "Selected game row is out of range"});
    }
    state_.selected_row = row;
    state_.selected_game_id = state_.games[row].id;
    state_.has_selection = true;
    state_.error_text.clear();
    return {};
}

auto game_browser_presenter::move_selection(int const delta) -> browser_result<std::size_t>
{
    if (state_.games.empty()) {
        auto failure = fail({.code = error_code::invalid_argument, .message = "No games are available for selection"});
        return tl::unexpected {failure.error()};
    }

    auto current = state_.selected_row.value_or(delta < 0 ? state_.games.size() - 1 : 0);
    if (delta < 0) {
        auto const magnitude = static_cast<std::size_t>(-(static_cast<std::int64_t>(delta)));
        current = magnitude > current ? 0 : current - magnitude;
    } else {
        auto const magnitude = static_cast<std::size_t>(delta);
        current = std::min(current + magnitude, state_.games.size() - 1);
    }
    if (auto selected = select_game(current); !selected) {
        return tl::unexpected {selected.error()};
    }
    return current;
}

auto game_browser_presenter::prepare_activation() -> browser_result<activation_request>
{
    if (!state_.has_selection) {
        auto failure = fail({.code = error_code::invalid_argument, .message = "Select a game before activation"});
        return tl::unexpected {failure.error()};
    }
    // A followed list opens a game where it reaches the followed position, and
    // knows that ply only for a row on screen.
    auto ply = std::optional<std::size_t> {};
    if (state_.rows_restricted) {
        if (!state_.selected_row || *state_.selected_row >= state_.row_plies.size()) {
            auto failure = fail({.code = error_code::invalid_argument, .message = "Select a game in the list to open it at this position"});
            return tl::unexpected {failure.error()};
        }
        ply = state_.row_plies[*state_.selected_row];
    }
    ++state_.activation_generation;
    state_.error_text.clear();
    return activation_request {.generation = state_.activation_generation, .game_id = state_.selected_game_id, .ply = ply};
}

auto game_browser_presenter::execute_activation(activation_request const& request) const -> browser_result<loaded_game>
{
    auto game = database_->store().get(request.game_id);
    if (!game) {
        return tl::unexpected {from_database_error(game.error())};
    }
    return loaded_game {.generation = request.generation, .game_id = request.game_id, .game = std::move(*game)};
}

auto game_browser_presenter::apply_activation(loaded_game game) -> browser_result<bool>
{
    if (game.generation != state_.activation_generation || !state_.has_selection || game.game_id != state_.selected_game_id) {
        return false;
    }
    state_.active_game_id = game.game_id;
    state_.active_game = std::move(game.game);
    state_.error_text.clear();
    return true;
}

auto game_browser_presenter::apply_activation_error(std::uint64_t const generation, browser_error activation_error) -> browser_result<bool>
{
    if (generation != state_.activation_generation) {
        return false;
    }
    state_.error_text = std::move(activation_error.message);
    return true;
}

auto game_browser_presenter::prepare_external_activation(motif::db::game_id const game_id) -> browser_result<activation_request>
{
    ++state_.activation_generation;
    state_.error_text.clear();
    return activation_request {.generation = state_.activation_generation, .game_id = game_id};
}

auto game_browser_presenter::apply_external_activation(loaded_game game) -> browser_result<bool>
{
    if (game.generation != state_.activation_generation) {
        return false;
    }
    state_.active_game_id = game.game_id;
    state_.active_game = std::move(game.game);
    // Keep the row selection in sync with the activated game so the list
    // highlights the game now on the board even when activation came from
    // outside the browser (e.g. a search-match row).
    state_.selected_game_id = state_.active_game_id;
    state_.has_selection = true;
    auto const selected = std::ranges::find(state_.games, state_.active_game_id, &motif::db::game_list_entry::id);
    if (selected != state_.games.end()) {
        state_.selected_row = static_cast<std::size_t>(std::distance(state_.games.begin(), selected));
    } else {
        state_.selected_row.reset();
    }
    state_.error_text.clear();
    return true;
}

auto game_browser_presenter::resize_column(std::size_t const column, std::int32_t const width) -> browser_result<void>
{
    if (column >= browser_column_count) {
        return fail({.code = error_code::invalid_argument, .message = "Selected column is out of range"});
    }
    state_.column_widths.at(column) = std::clamp(width, browser_minimum_column_width, browser_maximum_column_width);
    state_.error_text.clear();
    return {};
}

void game_browser_presenter::dismiss_error() noexcept
{
    state_.error_text.clear();
}

auto game_browser_presenter::state() const noexcept -> game_browser_state const&
{
    return state_;
}

auto game_browser_presenter::make_query(std::size_t const page_index, bool const drop_missing_selection) -> browser_result<browser_query>
{
    constexpr auto max_page_index = std::numeric_limits<std::size_t>::max() / browser_page_size;
    if (page_index > max_page_index) {
        return fail_query({.code = error_code::invalid_argument, .message = "Requested game page is too large"});
    }
    ++state_.query_generation;
    state_.error_text.clear();
    state_.loading = true;
    state_.load_failed = false;
    auto const restricted = state_.following && state_.followed_hash && *state_.followed_hash != starting_hash_;
    return browser_query {
        .generation = state_.query_generation,
        .filter =
            motif::db::search_filter {
                .player_name = nonempty_optional(state_.player_filter),
                .player_color = motif::db::player_color::either,
                .min_elo = std::nullopt,
                .max_elo = std::nullopt,
                .result = nonempty_optional(state_.result_filter),
                .eco_prefix = std::nullopt,
                .position = restricted ? state_.followed_hash : std::nullopt,
                .offset = page_index * browser_page_size,
                .limit = browser_page_size,
                .sort_column = to_database_sort(state_.sort_column),
                .sort_ascending = state_.sort_ascending,
            },
        .drop_missing_selection = drop_missing_selection,
    };
}

auto game_browser_presenter::fail(browser_error error_value) -> browser_result<void>
{
    state_.error_text = error_value.message;
    return tl::unexpected {std::move(error_value)};
}

auto game_browser_presenter::fail_query(browser_error error_value) -> browser_result<browser_query>
{
    state_.error_text = error_value.message;
    return tl::unexpected {std::move(error_value)};
}

auto game_browser_presenter::from_database_error(motif::db::error const& database_error) -> browser_error
{
    return browser_error {
        .code = error_code::database_failure,
        .message = database_error.message.empty() ? std::string {motif::db::to_string(database_error)} : database_error.message,
    };
}

}  // namespace motif::slint_app
