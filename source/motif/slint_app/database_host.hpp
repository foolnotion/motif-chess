#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "motif/slint_app/activity_gate.hpp"
#include "motif/slint_app/replace_controller.hpp"
#include "workspace.h"

// Slint-facing registrar for every way of leaving the current database or
// opening another (create, open, recent, scratch, close), and the rule for
// when a queued launch import may start. They live here, not in main.cpp, so
// the headless tests execute the same registered callbacks the application does.
namespace motif::slint_app
{

// The text a path or name field holds, without surrounding whitespace.
[[nodiscard]] auto trim_whitespace(std::string_view value) -> std::string_view;

// What a confirmed database action does once the gate lets it through. main()
// supplies the workspace runner and the hosts to detach; tests supply fakes.
struct database_actions
{
    // Each of these starts background work and returns true when the work
    // started, which is when the window should show itself busy.
    std::function<bool(std::string const& dir_path, std::string const& name)> create;
    std::function<bool(std::string const& path)> open;
    std::function<bool()> scratch;
    // Detaches every host from the database and closes it, then republishes.
    std::function<void()> close;
};

// The error-bar text for a Create, Open, Recent, Scratch or Close request made
// while the workspace is busy. The wording of a confirmation accepted after the
// workspace became busy is different and stays private to the host.
inline constexpr auto request_busy_notice = std::string_view {"Nothing was changed: the workspace is busy. Try again when it is idle."};

// Create, open, recent, scratch and close run only when the workspace is idle
// at request time. A busy request does nothing except tell the user in the error
// bar, and is not kept. Then comes the replace confirmation when an edited or
// unsaved game is on the board. The confirmation is a separate moment: an
// import can start while the dialog is open, so the gate is checked again when
// the user accepts. A request that became busy in the meantime does nothing
// except tell the user why, in its own words, in the error bar; the rejected
// request is not kept.
void register_database_callbacks(WorkspaceWindow& window,
                                 activity_probe const& activity,
                                 replace_controller& confirmation,
                                 database_actions actions);

// Everything that decides whether the next queued launch import may start.
struct launch_import_state
{
    workspace_activity activity;
    // The previous import has finished but its completion has not been handled.
    bool completion_pending {false};
    // A replace confirmation is open: the user is deciding about the database
    // an import would write to.
    bool confirmation_pending {false};
    bool database_open {false};
};

[[nodiscard]] constexpr auto may_start_launch_import(launch_import_state const& state) noexcept -> bool
{
    return state.activity.idle() && !state.completion_pending && !state.confirmation_pending && state.database_open;
}

}  // namespace motif::slint_app
