#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include <fmt/base.h>
#include <fmt/format.h>
#include <slint.h>

#include "game_browser.h"
#include "motif/db/database_manager.hpp"
#include "motif/db/types.hpp"
#include "motif/slint_spike/game_browser_presenter.hpp"

namespace
{

auto shared_string(std::string const& value) -> slint::SharedString
{
    return slint::SharedString {std::string_view {value}};
}

auto join_moves(std::vector<std::string> const& moves) -> std::string
{
    auto text = std::string {};
    for (std::size_t index = 0; index < moves.size(); ++index) {
        if (!text.empty()) {
            text += ' ';
        }
        if (index % 2 == 0) {
            text += fmt::format("{}. ", (index / 2) + 1);
        }
        text += moves[index];
    }
    return text;
}

void update_window(GameBrowser& window, motif::slint_spike::game_browser_state const& state)
{
    auto rows = std::vector<GameRow> {};
    rows.reserve(state.games.size());
    for (auto const& game : state.games) {
        rows.push_back(GameRow {
            .white = shared_string(game.white),
            .black = shared_string(game.black),
            .result = shared_string(game.result),
            .event = shared_string(game.event),
            .date = shared_string(game.date),
            .eco = shared_string(game.eco),
        });
    }

    window.set_games(std::make_shared<slint::VectorModel<GameRow>>(std::move(rows)));
    window.set_selected_row(state.has_selection ? static_cast<std::int32_t>(state.selected_row) : -1);
    window.set_selected_players(shared_string(fmt::format("{} - {}", state.selected_white, state.selected_black)));
    window.set_selected_event(shared_string(state.selected_event));
    window.set_selected_date(shared_string(state.selected_date));
    window.set_selected_result(shared_string(state.selected_result));
    window.set_moves(shared_string(join_moves(state.san_moves)));
    window.set_fen(shared_string(state.current_fen));
    window.set_current_ply(static_cast<std::int32_t>(state.current_ply));
    window.set_error_text(shared_string(state.error_text));
}

}  // namespace

auto main(int const argc, char const* const* argv) -> int
{
    if (argc != 2) {
        fmt::print(stderr, "usage: motif_slint_spike <database-bundle>\n");
        return 2;
    }

    auto database = motif::db::database_manager::open(std::filesystem::path {argv[1]});
    if (!database) {
        fmt::print(stderr, "failed to open database bundle: {}\n", database.error().message);
        return 1;
    }

    auto presenter = motif::slint_spike::game_browser_presenter {*database};
    if (!presenter.load_games()) {
        fmt::print(stderr, "failed to load games: {}\n", presenter.state().error_text);
        return 1;
    }

    auto window = GameBrowser::create();
    update_window(*window, presenter.state());

    window->on_select_row(
        [&](std::int32_t const row)
        {
            if (row >= 0) {
                static_cast<void>(presenter.select_game(static_cast<std::size_t>(row)));
                update_window(*window, presenter.state());
            }
        });
    window->on_sort_requested(
        [&](std::int32_t const column, bool const ascending)
        {
            if (column >= 0) {
                static_cast<void>(presenter.sort_games(static_cast<std::size_t>(column), ascending));
                update_window(*window, presenter.state());
            }
        });
    window->on_previous(
        [&]
        {
            static_cast<void>(presenter.retreat());
            update_window(*window, presenter.state());
        });
    window->on_next(
        [&]
        {
            static_cast<void>(presenter.advance());
            update_window(*window, presenter.state());
        });

    window->run();
    return 0;
}
