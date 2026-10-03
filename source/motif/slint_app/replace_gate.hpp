#pragma once

#include <cstdint>
#include <functional>

namespace motif::slint_app
{

// Holds an action that would replace the board (loading a game, starting a new
// one) until the user confirms it, when running it would discard moves they
// made. Toolkit-neutral: the adapter shows the confirmation and calls accept()
// or cancel(). Nothing is discarded before accept().
class replace_gate
{
  public:
    enum class outcome : std::uint8_t
    {
        ran,
        deferred,
    };

    // Runs `action` now when nothing would be lost, otherwise keeps it for
    // accept() and returns deferred. A deferred action still waiting is
    // replaced by the newer request.
    auto request(bool would_discard, std::function<void()> action) -> outcome;

    [[nodiscard]] auto pending() const noexcept -> bool;

    // Runs the held action once. Does nothing when none is held.
    void accept();
    // Drops the held action; nothing runs.
    void cancel() noexcept;

  private:
    std::function<void()> pending_;
};

}  // namespace motif::slint_app
