#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "motif/slint_app/board_presenter.hpp"

#include <fmt/format.h>
#include <tl/expected.hpp>

#include "motif/chess/chess.hpp"
#include "motif/navigator/game_navigator.hpp"

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

auto lower_case_letter(char const letter) -> char
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
}

constexpr std::string_view promotion_pieces {"qrbn"};

}  // namespace

board_presenter::board_presenter()
{
    reset_game_state(/*generation=*/0, /*panel_visible=*/true);
}

void board_presenter::apply_loaded_game(motif::db::game_id const game_key, motif::db::game const& game)
{
    ++state_.load_generation;
    load_game_state(game_key, game);
    refresh_navigation(/*tree_changed=*/true);
}

void board_presenter::dismiss_error() noexcept
{
    state_.error_text.clear();
}

void board_presenter::clear()
{
    auto const panel_visible = state_.panel_visible;
    auto const generation = state_.load_generation + 1;
    navigator_.clear();
    reset_game_state(generation, panel_visible);
}

void board_presenter::start_new_game()
{
    auto const panel_visible = state_.panel_visible;
    auto const flipped = state_.orientation_flipped;
    navigator_.clear();
    reset_game_state(state_.load_generation, panel_visible);
    state_.orientation_flipped = flipped;
    begin_unsaved_game();
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

void board_presenter::navigate_to_node(std::uint32_t const node)
{
    if (navigator_.navigate_to_node(node)) {
        refresh_navigation();
    }
}

auto board_presenter::select_square(int const file, int const rank) -> board_result<void>
{
    if (file < 0 || file > max_square_index || rank < 0 || rank > max_square_index) {
        return fail({.code = error_code::invalid_argument, .message = "Square is out of range"});
    }
    auto const square = board_square {.file = file, .rank = rank};
    // A click elsewhere abandons a promotion that was waiting for a piece.
    state_.promotion.reset();

    if (state_.selected_square == square) {
        clear_selection();
        return {};
    }
    if (state_.selected_square && std::ranges::find(state_.legal_targets, square) != state_.legal_targets.end()) {
        return play_move(*state_.selected_square, square, std::nullopt);
    }

    auto const had_selection = state_.selected_square.has_value();
    select(square);
    if (had_selection && state_.legal_targets.empty()) {
        // Neither a legal target nor a piece that can move: not a move.
        clear_selection();
    }
    return {};
}

auto board_presenter::choose_promotion(char const piece) -> board_result<void>
{
    if (!state_.promotion) {
        return fail({.code = error_code::invalid_argument, .message = "No promotion is pending"});
    }
    auto const letter = lower_case_letter(piece);
    if (!promotion_pieces.contains(letter)) {
        return fail({.code = error_code::invalid_argument, .message = "Promote to a queen, rook, bishop or knight"});
    }
    auto const pending = *state_.promotion;
    return play_move(pending.from, pending.to, letter);
}

void board_presenter::cancel_promotion() noexcept
{
    state_.promotion.reset();
    clear_selection();
}

auto board_presenter::play_san(std::string_view const san) -> board_result<void>
{
    auto applied = navigator_.apply_san(san);
    if (!applied) {
        clear_selection();
        state_.promotion.reset();
        return fail({.code = error_code::invalid_argument, .message = fmt::format("\"{}\" is not a legal move here", san)});
    }
    finish_move(applied->outcome);
    return {};
}

auto board_presenter::play_move(board_square const from, board_square const target, std::optional<char> const promotion)
    -> board_result<void>
{
    auto applied = navigator_.apply_move(algebraic_from_square(from), algebraic_from_square(target), promotion);
    if (!applied) {
        if (applied.error() == motif::navigator::game_navigator::move_error::promotion_required) {
            auto const& piece = state_.square_pieces[index_of(from)];
            state_.promotion = pending_promotion {.from = from, .to = target, .white = !piece.empty() && piece.front() == 'w'};
            return {};
        }
        clear_selection();
        state_.promotion.reset();
        return fail({.code = error_code::invalid_argument, .message = "That move is not legal here"});
    }
    finish_move(applied->outcome);
    return {};
}

void board_presenter::finish_move(motif::navigator::game_navigator::move_outcome const outcome)
{
    using motif::navigator::game_navigator;
    auto const grew = outcome == game_navigator::move_outcome::extended_line || outcome == game_navigator::move_outcome::created_variation;
    if (!state_.loaded && !state_.unsaved) {
        begin_unsaved_game();
    }
    if (state_.loaded && grew) {
        moves_added_ = true;
    }
    state_.error_text.clear();
    refresh_navigation(grew);
}

void board_presenter::begin_unsaved_game()
{
    ++state_.load_generation;
    state_.unsaved = true;
    state_.white_name = "White";
    state_.black_name = "Black";
    state_.result = "*";
    refresh_navigation(/*tree_changed=*/true);
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

auto board_presenter::movetext() const -> std::string
{
    if (!state_.loaded && !state_.unsaved) {
        return {};
    }
    auto text = std::string {};
    for (auto const& token : state_.notation) {
        if (!text.empty()) {
            text += ' ';
        }
        text += token.open;
        if (!token.number.empty()) {
            text += token.number;
            text += ' ';
        }
        text += token.san;
        text += token.close;
    }
    if (!text.empty()) {
        text += ' ';
    }
    text += state_.result.empty() ? std::string_view {"*"} : std::string_view {state_.result};
    return text;
}

void board_presenter::select(board_square const square)
{
    state_.selected_square = square;
    state_.legal_targets.clear();
    state_.error_text.clear();

    auto const from_name = algebraic_from_square(square);
    for (auto const& info : motif::chess::legal_moves(navigator_.current_board())) {
        if (info.from != from_name) {
            continue;
        }
        // A promotion is four legal moves for one square pair.
        if (auto target = square_from_algebraic(info.to);
            target && std::ranges::find(state_.legal_targets, *target) == state_.legal_targets.end())
        {
            state_.legal_targets.push_back(*target);
        }
    }
    refresh_highlights();
}

void board_presenter::update_edit_state()
{
    state_.edited = state_.unsaved ? navigator_.has_game() : (state_.loaded && moves_added_);
}

void board_presenter::rebuild_notation()
{
    using motif::navigator::game_navigator;
    using node_id = game_navigator::node_id;
    constexpr auto none = std::numeric_limits<node_id>::max();

    // One open line of play: `next` is the node still to emit. Variations are
    // frames above the line they interrupt, so a nested variation needs no
    // recursion and the interrupted line resumes when they are all closed.
    struct line_frame
    {
        node_id next;
        bool open_pending;
        int level;
    };

    state_.notation.clear();
    auto const first_moves = navigator_.children(game_navigator::root_node);
    if (first_moves.empty()) {
        return;
    }

    auto stack = std::vector<line_frame> {{.next = first_moves.front(), .open_pending = false, .level = 0}};
    auto pending_open = std::string {};
    auto number_due = true;
    while (!stack.empty()) {
        auto& top = stack.back();
        if (top.open_pending) {
            pending_open += '(';
            top.open_pending = false;
            number_due = true;
        }
        if (top.next == none) {
            auto const closes_variation = top.level > 0;
            stack.pop_back();
            if (closes_variation && !state_.notation.empty()) {
                state_.notation.back().close += ')';
                number_due = true;
            }
            continue;
        }

        auto const node = top.next;
        auto const level = top.level;
        auto const continuation = navigator_.children(node);
        top.next = continuation.empty() ? none : continuation.front();

        auto const ply = navigator_.depth(node);
        auto const white_moved = ply % 2 == 1;
        auto token = notation_token {};
        token.open = std::exchange(pending_open, std::string {});
        if (white_moved || number_due) {
            token.number = fmt::format("{}{}", (ply + 1) / 2, white_moved ? "." : "...");
        }
        token.san = navigator_.san(node);
        token.node = node;
        token.variation_depth = level;
        number_due = false;
        state_.notation.push_back(std::move(token));

        // Alternatives to the first reply follow it, before the line goes on.
        auto const siblings = navigator_.children(navigator_.parent(node));
        if (siblings.front() == node) {
            for (auto index = siblings.size(); index-- > 1;) {
                stack.push_back({.next = siblings[index], .open_pending = true, .level = level + 1});
            }
        }
    }
}

void board_presenter::refresh_navigation(bool const tree_changed)
{
    state_.current_ply = navigator_.current_ply();
    state_.total_plies = navigator_.total_plies();
    state_.fen = navigator_.current_fen();
    state_.current_hash = navigator_.current_hash();
    state_.square_pieces = squares_from_fen(state_.fen);
    state_.current_san = navigator_.current_san();
    state_.current_node = navigator_.current_node();
    if (tree_changed) {
        state_.san_moves = navigator_.move_list();
        rebuild_notation();
    }
    update_edit_state();
    state_.promotion.reset();
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

void board_presenter::reset_game_state(std::uint64_t const generation, bool const panel_visible)
{
    state_ = board_state {};
    moves_added_ = false;
    state_.fen = navigator_.current_fen();
    state_.square_pieces = squares_from_fen(state_.fen);
    state_.square_highlights.assign(board_dimension * board_dimension, square_highlight::none);
    state_.current_hash = navigator_.current_hash();
    state_.load_generation = generation;
    state_.panel_visible = panel_visible;
}

void board_presenter::load_game_state(motif::db::game_id const game_key, motif::db::game const& game)
{
    navigator_.load(game);
    moves_added_ = false;
    state_.loaded = true;
    state_.unsaved = false;
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
