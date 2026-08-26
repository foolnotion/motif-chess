#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "motif/slint_app/board_presenter.hpp"

#include <tl/expected.hpp>

#include "motif/chess/chess.hpp"

namespace motif::slint_app
{

namespace
{

inline constexpr int max_square_index {7};

auto algebraic_from_square(board_square const square) -> std::string
{
    return {static_cast<char>('a' + square.file), static_cast<char>('1' + square.rank)};
}

auto square_from_algebraic(std::string const& value) -> std::optional<board_square>
{
    if (value.size() != 2 || value[0] < 'a' || value[0] > 'h' || value[1] < '1' || value[1] > '8') {
        return std::nullopt;
    }
    return board_square {.file = value[0] - 'a', .rank = value[1] - '1'};
}

constexpr std::size_t board_dimension {8};

auto index_of(board_square const square) -> std::size_t
{
    return (static_cast<std::size_t>(square.rank) * board_dimension) + static_cast<std::size_t>(square.file);
}

// Derives per-square render data (piece color + type) from the placement
// field of a FEN string. This is presentation-layer FEN reading for
// rendering, not chess-rules parsing: it never derives legality or move
// generation, which remain owned exclusively by motif::chess.
auto squares_from_fen(std::string const& fen) -> std::vector<std::string>
{
    auto squares = std::vector<std::string>(board_dimension * board_dimension, std::string {});
    auto const placement_end = fen.find(' ');
    auto const placement = placement_end == std::string::npos ? fen : fen.substr(0, placement_end);

    auto rank = static_cast<int>(board_dimension) - 1;
    auto file = 0;
    for (char const fen_char : placement) {
        if (fen_char == '/') {
            --rank;
            file = 0;
            continue;
        }
        if (fen_char >= '1' && fen_char <= '8') {
            file += fen_char - '0';
            continue;
        }
        if (rank < 0 || file < 0 || std::cmp_greater_equal(file, board_dimension)) {
            continue;
        }
        auto const color = static_cast<bool>(std::isupper(static_cast<unsigned char>(fen_char))) ? 'w' : 'b';
        auto const kind = static_cast<char>(std::toupper(static_cast<unsigned char>(fen_char)));
        squares[(static_cast<std::size_t>(rank) * board_dimension) + static_cast<std::size_t>(file)] = {color, kind};
        ++file;
    }
    return squares;
}

}  // namespace

board_presenter::board_presenter() noexcept
{
    reset_game_state(/*generation=*/0, /*panel_visible=*/true);
}

void board_presenter::apply_loaded_game(motif::db::game_id const game_key, motif::db::game const& game)
{
    ++state_.load_generation;
    load_game_state(game_key, game);
    refresh_navigation(/*refresh_move_list=*/true);
}

void board_presenter::dismiss_error() noexcept
{
    state_.error_text.clear();
}

void board_presenter::clear() noexcept
{
    auto const panel_visible = state_.panel_visible;
    auto const generation = state_.load_generation + 1;
    navigator_.clear();
    reset_game_state(generation, panel_visible);
}

void board_presenter::advance()
{
    navigator_.advance();
    refresh_navigation();
}

void board_presenter::retreat()
{
    navigator_.retreat();
    refresh_navigation();
}

void board_presenter::jump_to_start()
{
    navigator_.jump_to_start();
    refresh_navigation();
}

void board_presenter::jump_to_end()
{
    navigator_.jump_to_end();
    refresh_navigation();
}

void board_presenter::navigate_to(std::size_t const ply)
{
    navigator_.navigate_to(ply);
    refresh_navigation();
}

auto board_presenter::select_square(int const file, int const rank) -> board_result<void>
{
    if (!state_.loaded) {
        return fail({.code = error_code::invalid_argument, .message = "Load a game before selecting a square"});
    }
    if (file < 0 || file > max_square_index || rank < 0 || rank > max_square_index) {
        return fail({.code = error_code::invalid_argument, .message = "Square is out of range"});
    }
    auto const square = board_square {.file = file, .rank = rank};
    if (state_.selected_square == square) {
        clear_selection();
        return {};
    }

    state_.selected_square = square;
    state_.legal_targets.clear();
    state_.error_text.clear();

    auto position = motif::chess::parse_fen(state_.fen);
    if (position) {
        auto const from_name = algebraic_from_square(square);
        for (auto const& info : motif::chess::legal_moves(*position)) {
            if (info.from != from_name) {
                continue;
            }
            if (auto target = square_from_algebraic(info.to)) {
                state_.legal_targets.push_back(*target);
            }
        }
    }
    refresh_highlights();
    return {};
}

void board_presenter::clear_selection() noexcept
{
    state_.selected_square.reset();
    state_.legal_targets.clear();
    refresh_highlights();
}

void board_presenter::toggle_orientation() noexcept
{
    state_.orientation_flipped = !state_.orientation_flipped;
}

void board_presenter::set_panel_visible(bool const visible) noexcept
{
    state_.panel_visible = visible;
}

auto board_presenter::state() const noexcept -> board_state const&
{
    return state_;
}

void board_presenter::refresh_navigation(bool const refresh_move_list)
{
    state_.current_ply = navigator_.current_ply();
    state_.total_plies = navigator_.total_plies();
    state_.fen = navigator_.current_fen();
    state_.current_hash = navigator_.current_hash();
    state_.square_pieces = squares_from_fen(state_.fen);
    state_.current_san = navigator_.current_san();
    if (refresh_move_list) {
        state_.san_moves = navigator_.move_list();
    }
    clear_selection();

    if (auto const last = navigator_.last_move()) {
        state_.last_move_from = square_from_algebraic(last->from);
        state_.last_move_to = square_from_algebraic(last->to);
    } else {
        state_.last_move_from.reset();
        state_.last_move_to.reset();
    }
    refresh_highlights();
}

void board_presenter::reset_game_state(std::uint64_t const generation, bool const panel_visible) noexcept
{
    state_ = board_state {};
    state_.square_pieces.assign(board_dimension * board_dimension, std::string {});
    state_.square_highlights.assign(board_dimension * board_dimension, square_highlight::none);
    state_.current_hash = navigator_.current_hash();
    state_.load_generation = generation;
    state_.panel_visible = panel_visible;
}

void board_presenter::load_game_state(motif::db::game_id const game_key, motif::db::game const& game)
{
    navigator_.load(game);
    state_.loaded = true;
    state_.game_id = game_key;
    state_.white_name = game.white.name;
    state_.black_name = game.black.name;
    state_.result = game.result;
    state_.event_name = game.event_details ? game.event_details->name : std::string {};
    state_.date = game.date.value_or(std::string {});
    state_.error_text.clear();
}

void board_presenter::refresh_highlights()
{
    state_.square_highlights.assign(board_dimension * board_dimension, square_highlight::none);
    if (state_.last_move_from) {
        state_.square_highlights[index_of(*state_.last_move_from)] = square_highlight::last_move_from;
    }
    if (state_.last_move_to) {
        state_.square_highlights[index_of(*state_.last_move_to)] = square_highlight::last_move_to;
    }
    for (auto const& target : state_.legal_targets) {
        state_.square_highlights[index_of(target)] = square_highlight::legal_target;
    }
    if (state_.selected_square) {
        state_.square_highlights[index_of(*state_.selected_square)] = square_highlight::selected;
    }
}

auto board_presenter::fail(board_error error_value) -> board_result<void>
{
    state_.error_text = error_value.message;
    return tl::unexpected {std::move(error_value)};
}

}  // namespace motif::slint_app
