#pragma once

#include <functional>
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
    bool search_busy {false};

    [[nodiscard]] constexpr auto idle() const noexcept -> bool
    {
        return !import_active && !workspace_busy && !browser_busy && !search_busy;
    }
};

// The one composition of the live activity: an import, a workspace change
// (open, create, scratch), a game load and a search. Templated over the runners
// so the composition is tested without them; main() passes the real ones.
template<typename Importer, typename Workspace, typename Browser, typename Search>
[[nodiscard]] auto current_activity(Importer const& importer, Workspace const& workspace, Browser const& browser, Search const& search)
    -> workspace_activity
{
    return workspace_activity {.import_active = importer.active(),
                               .workspace_busy = workspace.is_busy(),
                               .browser_busy = browser.is_busy(),
                               .search_busy = search.is_busy()};
}

// Reports the workspace's current activity. Built once in main(); every board,
// play, New Game, replace, database-switch and search callback decides through
// run_when_idle on it.
using activity_probe = std::function<workspace_activity()>;

// Runs `action` only when the workspace is idle and reports whether it ran.
// The one place the gate is decided, so every callback and its tests share it.
// A rejected action has no effect at all.
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
