#pragma once

#include <functional>
#include <string_view>

#include "motif/slint_app/replace_gate.hpp"

namespace motif::slint_app
{

// Decides when an action that would discard the game on the board (loading a
// game, New Game, quitting, closing or switching the database) must wait for
// the user, and drives the confirmation through injected callbacks so the
// toolkit adapter stays thin and this logic is testable without Slint.
class replace_controller
{
  public:
    struct hooks
    {
        // True when the board holds moves the user would lose.
        std::function<bool()> edited;
        std::function<void(std::string_view action_label)> show;
        std::function<void()> hide;
        // Called after the user cancels, to give keyboard focus back.
        std::function<void()> restore_focus;
    };

    explicit replace_controller(hooks callbacks);

    // Runs `action` now when nothing would be lost, otherwise shows the
    // confirmation and keeps the action for accept().
    void request(std::string_view action_label, std::function<void()> action);
    void accept();
    void cancel();

    [[nodiscard]] auto pending() const noexcept -> bool;

  private:
    hooks hooks_;
    replace_gate gate_;
};

}  // namespace motif::slint_app
