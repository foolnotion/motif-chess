#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <slint.h>

#include "motif/db/types.hpp"
#include "motif/slint_app/activity_gate.hpp"
#include "motif/slint_app/board_presenter.hpp"
#include "motif/slint_app/replace_controller.hpp"
#include "workspace.h"

// Slint-facing adapters for the board: publishing presenter state to the
// window, the UI-thread runner around the presenter, and the registrars for
// every board, play, New Game and close callback. They live here, not in
// main.cpp, so the headless tests execute the same registered callbacks the
// application does.
namespace motif::slint_app
{

void clear_board(WorkspaceWindow& window);
void publish_board(WorkspaceWindow& window, board_state const& state);

// UI-thread adapter around the toolkit-neutral board presenter. Database
// loading is performed once by async_browser_runner and the completed game
// is handed to this adapter, so board navigation remains synchronous and
// cannot race a duplicate game-store read.
class async_board_runner
{
  public:
    explicit async_board_runner(slint::ComponentWeakHandle<WorkspaceWindow> window)
        : state_(std::make_shared<state>(std::move(window)))
    {
    }

    ~async_board_runner() = default;

    async_board_runner(async_board_runner const&) = delete;
    auto operator=(async_board_runner const&) -> async_board_runner& = delete;
    async_board_runner(async_board_runner&&) = delete;
    auto operator=(async_board_runner&&) -> async_board_runner& = delete;

    void attach()
    {
        state_->presenter = std::make_shared<motif::slint_app::board_presenter>();
        publish();
    }

    void detach()
    {
        state_->presenter.reset();
        if (auto locked = state_->window.lock()) {
            clear_board(**locked);
        }
    }

    // Fired after publish whenever the presenter reports a position hash.
    using position_changed_handler = std::function<void(motif::db::zobrist_hash)>;

    void set_position_changed_handler(position_changed_handler handler) { state_->position_changed = std::move(handler); }

    [[nodiscard]] auto is_busy() const noexcept -> bool { return state_->busy; }

    // Empty if no board is attached or its move list cannot be replayed.
    [[nodiscard]] auto current_hash() const -> std::optional<motif::db::zobrist_hash>
    {
        return state_->presenter ? state_->presenter->state().current_hash : std::nullopt;
    }

    void advance()
    {
        act([](auto& presenter) -> auto { presenter.advance(); });
    }

    void retreat()
    {
        act([](auto& presenter) -> void { presenter.retreat(); });
    }

    void jump_to_start()
    {
        act([](auto& presenter) -> void { presenter.jump_to_start(); });
    }

    void jump_to_end()
    {
        act([](auto& presenter) -> void { presenter.jump_to_end(); });
    }

    void navigate_to(std::size_t const ply)
    {
        act([ply](auto& presenter) -> void { presenter.navigate_to(ply); });
    }

    void select_square(int const file, int const rank)
    {
        act([file, rank](auto& presenter) -> void { (void)presenter.select_square(file, rank); });
    }

    void navigate_to_node(std::uint32_t const node)
    {
        act([node](auto& presenter) -> void { presenter.navigate_to_node(node); });
    }

    // The SAN entry point for move lists that arrive as text (Story 13.2).
    // It plays through the same presenter path as a board click; callers apply
    // the same busy and import gate as the other board actions.
    void play_san(std::string san)
    {
        act([san = std::move(san)](auto& presenter) -> void { (void)presenter.play_san(san); });
    }

    void choose_promotion(char const piece)
    {
        act([piece](auto& presenter) -> void { (void)presenter.choose_promotion(piece); });
    }

    void cancel_promotion()
    {
        act([](auto& presenter) -> void { presenter.cancel_promotion(); });
    }

    void new_game()
    {
        act([](auto& presenter) -> void { presenter.start_new_game(); });
    }

    // True when replacing the board would discard the user's input.
    [[nodiscard]] auto edited() const -> bool { return state_->presenter && state_->presenter->state().edited; }

    void toggle_orientation()
    {
        act([](auto& presenter) -> void { presenter.toggle_orientation(); });
    }

    void set_panel_visible(bool const visible)
    {
        act([visible](auto& presenter) -> void { presenter.set_panel_visible(visible); });
    }

    void apply_loaded_game(motif::db::game_id const game_key, motif::db::game const& game)
    {
        act([game_key, &game](auto& presenter) -> void { presenter.apply_loaded_game(game_key, game); });
    }

    // Loads a game and navigates to a specific ply as one atomic operation,
    // publishing once. Used by search-match activation so a transient
    // ply-0 publish for the newly loaded game never fires a wasted
    // auto-search for a hash the UI immediately navigates away from.
    void apply_loaded_game_at(motif::db::game_id const game_key, motif::db::game const& game, std::size_t const ply)
    {
        act(
            [game_key, &game, ply](auto& presenter) -> void
            {
                presenter.apply_loaded_game(game_key, game);
                presenter.navigate_to(ply);
            });
    }

    void clear(bool const publish_position_change)
    {
        if (!state_->presenter) {
            return;
        }
        state_->presenter->clear();
        publish(publish_position_change);
    }

    void dismiss_error()
    {
        if (state_->presenter) {
            state_->presenter->dismiss_error();
            if (auto locked = state_->window.lock()) {
                publish_board(**locked, state_->presenter->state());
            }
        }
    }

  private:
    struct state
    {
        explicit state(slint::ComponentWeakHandle<WorkspaceWindow> window_value)
            : window(std::move(window_value))
        {
        }

        slint::ComponentWeakHandle<WorkspaceWindow> window;
        std::shared_ptr<motif::slint_app::board_presenter> presenter;
        bool busy {false};
        position_changed_handler position_changed;
    };

    std::shared_ptr<state> state_;

    template<typename Operation>
    void act(Operation const& operation)
    {
        if (!state_->presenter) {
            return;
        }
        operation(*state_->presenter);
        publish();
    }

    void publish(bool const publish_position_change = true)
    {
        if (state_->presenter) {
            if (auto locked = state_->window.lock()) {
                publish_board(**locked, state_->presenter->state());
            }
            if (publish_position_change && state_->position_changed) {
                if (auto const hash = state_->presenter->state().current_hash) {
                    state_->position_changed(*hash);
                }
            }
        }
    }
};

// Reports the workspace's current activity. Built once in main(); every board,
// play, New Game and replace callback decides through run_when_idle on it.
using activity_probe = std::function<workspace_activity()>;

// In-window confirmation before something replaces an edited or unsaved game.
auto make_replace_confirmation(slint::ComponentHandle<WorkspaceWindow> const& window, async_board_runner const& board)
    -> replace_controller;

// Square clicks, notation and replay navigation, orientation and panel
// visibility.
void register_board_callbacks(WorkspaceWindow& window, async_board_runner& board, activity_probe const& activity);

// Promotion choice, SAN entry, New Game and the answers to the confirmation.
void register_play_callbacks(WorkspaceWindow& window,
                             async_board_runner& board,
                             activity_probe const& activity,
                             replace_controller& confirmation);

// Closing the window with an edited or unsaved game asks first. Cancelling
// keeps the window and the game; confirming hides the window.
void install_close_guard(WorkspaceWindow& window, async_board_runner const& board, replace_controller& confirmation);

// What the game browser's activation reports back to the board. A game that
// loaded replaces the board; a failed read changes nothing, so the user's
// edited tree survives and the browser reports the error on its own.
struct board_activation_handlers
{
    std::function<void(motif::db::game_id, motif::db::game const&)> loaded;
    std::function<void()> failed;
    std::function<void(motif::db::game_id, motif::db::game const&, std::size_t)> loaded_at;
    std::function<void()> failed_at;
};

auto make_board_activation_handlers(slint::ComponentWeakHandle<WorkspaceWindow> const& window, async_board_runner& board)
    -> board_activation_handlers;

}  // namespace motif::slint_app
