#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "motif/slint_app/board_host.hpp"

#include <slint.h>

#include "motif/slint_app/activity_gate.hpp"
#include "motif/slint_app/board_presenter.hpp"
#include "motif/slint_app/replace_controller.hpp"
#include "workspace.h"

namespace motif::slint_app
{

namespace
{

auto shared_string(std::string_view value) -> slint::SharedString
{
    return slint::SharedString {value};
}

auto to_ui_highlight(square_highlight const value) -> SquareHighlight
{
    switch (value) {
        case square_highlight::none:
            return SquareHighlight::None;
        case square_highlight::selected:
            return SquareHighlight::Selected;
        case square_highlight::legal_target:
            return SquareHighlight::LegalTarget;
        case square_highlight::last_move_from:
            return SquareHighlight::LastMoveFrom;
        case square_highlight::last_move_to:
            return SquareHighlight::LastMoveTo;
    }
    std::unreachable();
}

}  // namespace

void clear_board(WorkspaceWindow& window)
{
    window.set_board_loaded(false);
    window.set_board_square_pieces(std::make_shared<slint::VectorModel<slint::SharedString>>());
    window.set_board_square_highlights(std::make_shared<slint::VectorModel<SquareHighlight>>());
    window.set_board_flipped(false);
    window.set_board_white_name("");
    window.set_board_black_name("");
    window.set_board_event_name("");
    window.set_board_date("");
    window.set_board_result("");
    window.set_board_notation(std::make_shared<slint::VectorModel<NotationToken>>());
    window.set_board_current_node(0);
    window.set_board_notation_chars(0);
    window.set_board_unsaved(false);
    window.set_board_edited(false);
    window.set_board_promotion_pending(false);
    window.set_board_panel_visible(true);
}

void publish_board(WorkspaceWindow& window, board_state const& state)
{
    window.set_board_loaded(state.loaded || state.unsaved);

    auto pieces = std::vector<slint::SharedString> {};
    pieces.reserve(state.square_pieces.size());
    for (auto const& piece : state.square_pieces) {
        pieces.push_back(shared_string(piece));
    }
    window.set_board_square_pieces(std::make_shared<slint::VectorModel<slint::SharedString>>(std::move(pieces)));

    auto highlights = std::vector<SquareHighlight> {};
    highlights.reserve(state.square_highlights.size());
    for (auto const highlight : state.square_highlights) {
        highlights.push_back(to_ui_highlight(highlight));
    }
    window.set_board_square_highlights(std::make_shared<slint::VectorModel<SquareHighlight>>(std::move(highlights)));

    window.set_board_flipped(state.orientation_flipped);
    window.set_board_white_name(shared_string(state.white_name));
    window.set_board_black_name(shared_string(state.black_name));
    window.set_board_event_name(shared_string(state.event_name));
    window.set_board_date(shared_string(state.date));
    window.set_board_result(shared_string(state.result));

    auto notation = std::vector<NotationToken> {};
    notation.reserve(state.notation.size());
    for (auto const& token : state.notation) {
        notation.push_back(NotationToken {
            .lead = shared_string(token.lead()),
            .san = shared_string(token.san),
            .trail = shared_string(token.close),
            .node = static_cast<std::int32_t>(token.node),
            .depth = token.variation_depth,
        });
    }
    window.set_board_notation(std::make_shared<slint::VectorModel<NotationToken>>(std::move(notation)));
    window.set_board_notation_chars(static_cast<std::int32_t>(state.notation_chars));
    window.set_board_current_node(static_cast<std::int32_t>(state.current_node));
    window.set_board_unsaved(state.unsaved);
    window.set_board_edited(state.edited);
    window.set_board_promotion_pending(state.promotion.has_value());
    window.set_board_promotion_white(!state.promotion || state.promotion->white);
    window.set_board_panel_visible(state.panel_visible);
    window.set_board_error_text(shared_string(state.error_text));
}

auto make_replace_confirmation(slint::ComponentHandle<WorkspaceWindow> const& window, async_board_runner const& board) -> replace_controller
{
    auto const weak = slint::ComponentWeakHandle<WorkspaceWindow> {window};
    return replace_controller {replace_controller::hooks {
        .edited = [&board]() -> bool { return board.edited(); },
        .show = [weak](std::string_view const action_label) -> void
        {
            if (auto locked = weak.lock()) {
                (*locked)->set_confirm_message("This game has moves that are not saved to any database. Continuing discards them.");
                (*locked)->set_confirm_action_label(shared_string(action_label));
                (*locked)->set_confirm_visible(true);
            }
        },
        .hide = [weak]() -> void
        {
            if (auto locked = weak.lock()) {
                (*locked)->set_confirm_visible(false);
            }
        },
        // The review region takes keyboard focus back once the overlay is gone.
        .restore_focus = [weak]() -> void
        {
            if (auto locked = weak.lock()) {
                (*locked)->set_board_load_serial((*locked)->get_board_load_serial() + 1);
            }
        },
    }};
}

void register_board_callbacks(WorkspaceWindow& window, async_board_runner& board, activity_probe const& activity)
{
    window.on_board_square_activated([&board, activity](std::int32_t file, std::int32_t rank) -> void
                                     { (void)run_when_idle(activity(), [&]() -> void { board.select_square(file, rank); }); });
    window.on_board_node_activated(
        [&board, activity](std::int32_t node) -> void
        {
            if (node >= 0) {
                (void)run_when_idle(activity(), [&]() -> void { board.navigate_to_node(static_cast<std::uint32_t>(node)); });
            }
        });
    window.on_board_navigate_first([&board, activity]() -> void
                                   { (void)run_when_idle(activity(), [&]() -> void { board.jump_to_start(); }); });
    window.on_board_navigate_previous([&board, activity]() -> void
                                      { (void)run_when_idle(activity(), [&]() -> void { board.retreat(); }); });
    window.on_board_navigate_next([&board, activity]() -> void { (void)run_when_idle(activity(), [&]() -> void { board.advance(); }); });
    window.on_board_navigate_last([&board, activity]() -> void
                                  { (void)run_when_idle(activity(), [&]() -> void { board.jump_to_end(); }); });
    window.on_board_orientation_toggled([&board, activity]() -> void
                                        { (void)run_when_idle(activity(), [&]() -> void { board.toggle_orientation(); }); });
    window.on_board_panel_visibility_toggled(
        [&window, &board, activity]() -> void
        { (void)run_when_idle(activity(), [&]() -> void { board.set_panel_visible(!window.get_board_panel_visible()); }); });
}

void register_play_callbacks(WorkspaceWindow& window,
                             async_board_runner& board,
                             activity_probe const& activity,
                             replace_controller& confirmation)
{
    window.on_board_promotion_chosen(
        [&board, activity](slint::SharedString const& piece) -> void
        {
            if (!piece.empty()) {
                (void)run_when_idle(activity(),
                                    [&]() -> void { board.choose_promotion(std::string_view {piece.data(), piece.size()}.front()); });
            }
        });
    window.on_board_promotion_cancelled([&board, activity]() -> void
                                        { (void)run_when_idle(activity(), [&]() -> void { board.cancel_promotion(); }); });
    window.on_board_play_san(
        [&board, activity](slint::SharedString const& san) -> void
        {
            if (!san.empty()) {
                (void)run_when_idle(activity(), [&]() -> void { board.play_san(std::string {std::string_view {san.data(), san.size()}}); });
            }
        });
    window.on_board_new_game_requested(
        [&window, &board, &confirmation, activity]() -> void
        {
            (void)run_when_idle(activity(),
                                [&]() -> void
                                {
                                    confirmation.request("Start new game",
                                                         [&window, &board, activity]() -> void
                                                         {
                                                             (void)run_when_idle(
                                                                 activity(),
                                                                 [&]() -> void
                                                                 {
                                                                     board.new_game();
                                                                     window.set_board_load_serial(window.get_board_load_serial() + 1);
                                                                 });
                                                         });
                                });
        });
    window.on_confirm_accepted([&confirmation]() -> void { confirmation.accept(); });
    window.on_confirm_cancelled([&confirmation]() -> void { confirmation.cancel(); });
}

void install_close_guard(WorkspaceWindow& window, async_board_runner const& board, replace_controller& confirmation)
{
    window.window().on_close_requested(
        [&window, &board, &confirmation]() -> slint::CloseRequestResponse
        {
            if (!board.edited()) {
                return slint::CloseRequestResponse::HideWindow;
            }
            confirmation.request("Quit", [&window]() -> void { window.hide(); });
            return slint::CloseRequestResponse::KeepWindowShown;
        });
}

auto make_board_activation_handlers(slint::ComponentWeakHandle<WorkspaceWindow> const& window, async_board_runner& board)
    -> board_activation_handlers
{
    auto const announce_load = [window]() -> void
    {
        if (auto locked = window.lock()) {
            (*locked)->set_board_load_serial((*locked)->get_board_load_serial() + 1);
            (*locked)->set_error_text("");
        }
    };
    return board_activation_handlers {
        .loaded = [&board, announce_load](db::game_id const game_key, db::game const& game) -> void
        {
            board.apply_loaded_game(game_key, game);
            announce_load();
        },
        // A failed read must not touch the board: the replacement never loaded,
        // so the edited tree the user just confirmed to risk is still theirs.
        .failed = []() -> void {},
        .loaded_at = [&board, announce_load](db::game_id const game_key, db::game const& game, std::size_t const ply) -> void
        {
            board.apply_loaded_game_at(game_key, game, ply);
            announce_load();
        },
        .failed_at = []() -> void {},
    };
}

}  // namespace motif::slint_app
