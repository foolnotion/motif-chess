#pragma once

#include <utility>

namespace motif::slint_app
{

// What the workspace is busy with right now. Board actions, board replacement
// and workspace changes all wait for every flag to be clear.
struct workspace_activity
{
    bool import_active {false};
    bool workspace_busy {false};
    bool browser_busy {false};
    bool board_busy {false};
    bool search_busy {false};

    [[nodiscard]] constexpr auto idle() const noexcept -> bool
    {
        return !import_active && !workspace_busy && !browser_busy && !board_busy && !search_busy;
    }
};

// Runs `action` only when the workspace is idle and reports whether it ran.
// The one place the five-way gate is decided, so every board callback and its
// tests share it. A rejected action has no effect at all.
template<typename Action>
auto run_when_idle(workspace_activity const& activity, Action&& action) -> bool
{
    if (!activity.idle()) {
        return false;
    }
    std::forward<Action>(action)();
    return true;
}

}  // namespace motif::slint_app
