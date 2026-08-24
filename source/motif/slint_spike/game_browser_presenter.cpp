#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

#include "motif/slint_spike/game_browser_presenter.hpp"

#include <tl/expected.hpp>

#include "motif/db/error.hpp"
#include "motif/db/types.hpp"

namespace motif::slint_spike
{

game_browser_presenter::game_browser_presenter(motif::db::database_manager& database) noexcept
    : database_(database)
{
}

auto game_browser_presenter::load_games() -> result<void>
{
    auto filter = motif::db::search_filter {};
    filter.limit = motif::db::max_search_limit;

    auto result = database_.store().find_games(filter);
    if (!result) {
        return fail_database(result.error());
    }

    navigator_.clear();
    state_ = {};
    state_.games = std::move(result->games);
    return {};
}

auto game_browser_presenter::select_game(std::size_t const row) -> result<void>
{
    if (row >= state_.games.size()) {
        return fail({error_code::invalid_argument, "Selected game row is out of range"});
    }

    auto const& entry = state_.games[row];
    auto game = database_.store().get(entry.id);
    if (!game) {
        return fail_database(game.error());
    }

    navigator_.load(*game);
    state_.selected_row = row;
    state_.has_selection = true;
    state_.selected_white = entry.white;
    state_.selected_black = entry.black;
    state_.selected_event = entry.event;
    state_.selected_date = entry.date;
    state_.selected_result = entry.result;
    state_.san_moves = navigator_.move_list();
    state_.error_text.clear();
    update_navigation_state();
    return {};
}

auto game_browser_presenter::sort_games(std::size_t const column, bool const ascending) -> result<void>
{
    if (column > 5) {
        return fail({error_code::invalid_argument, "Selected sort column is out of range"});
    }

    auto value = [column](motif::db::game_list_entry const& game) -> std::string const&
    {
        switch (column) {
            case 0:
                return game.white;
            case 1:
                return game.black;
            case 2:
                return game.result;
            case 3:
                return game.event;
            case 4:
                return game.date;
            case 5:
                return game.eco;
            default:
                return game.white;
        }
    };
    std::ranges::stable_sort(
        state_.games, [&](auto const& lhs, auto const& rhs) { return ascending ? value(lhs) < value(rhs) : value(rhs) < value(lhs); });

    navigator_.clear();
    state_.has_selection = false;
    state_.selected_row = 0;
    state_.selected_white.clear();
    state_.selected_black.clear();
    state_.selected_event.clear();
    state_.selected_date.clear();
    state_.selected_result.clear();
    state_.san_moves.clear();
    state_.current_fen.clear();
    state_.current_ply = 0;
    state_.error_text.clear();
    return {};
}

auto game_browser_presenter::advance() -> result<void>
{
    if (!state_.has_selection) {
        return fail({error_code::invalid_argument, "Select a game before navigating moves"});
    }

    navigator_.advance();
    state_.error_text.clear();
    update_navigation_state();
    return {};
}

auto game_browser_presenter::retreat() -> result<void>
{
    if (!state_.has_selection) {
        return fail({error_code::invalid_argument, "Select a game before navigating moves"});
    }

    navigator_.retreat();
    state_.error_text.clear();
    update_navigation_state();
    return {};
}

auto game_browser_presenter::state() const noexcept -> game_browser_state const&
{
    return state_;
}

void game_browser_presenter::update_navigation_state()
{
    state_.current_fen = navigator_.current_fen();
    state_.current_ply = navigator_.current_ply();
}

auto game_browser_presenter::fail(error error_value) -> result<void>
{
    state_.error_text = error_value.message;
    return tl::unexpected {std::move(error_value)};
}

auto game_browser_presenter::fail_database(motif::db::error const& database_error) -> result<void>
{
    auto message = database_error.message.empty() ? std::string {motif::db::to_string(database_error)} : database_error.message;
    return fail({error_code::database_failure, std::move(message)});
}

}  // namespace motif::slint_spike
