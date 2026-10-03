#include <functional>
#include <utility>

#include "motif/slint_app/replace_gate.hpp"

namespace motif::slint_app
{

auto replace_gate::request(bool const would_discard, std::function<void()> action) -> outcome
{
    if (!would_discard) {
        pending_ = nullptr;
        action();
        return outcome::ran;
    }
    pending_ = std::move(action);
    return outcome::deferred;
}

auto replace_gate::pending() const noexcept -> bool
{
    return static_cast<bool>(pending_);
}

void replace_gate::accept()
{
    // Cleared first so the action can raise a new request.
    auto action = std::exchange(pending_, nullptr);
    if (action) {
        action();
    }
}

void replace_gate::cancel() noexcept
{
    pending_ = nullptr;
}

}  // namespace motif::slint_app
