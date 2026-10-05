#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include <slint.h>

#include "motif/slint_app/event_loop.hpp"
#include "motif/slint_app/file_picker.hpp"
#include "workspace.h"

// Slint-facing adapter for the Browse buttons (Story 13.6): the runner that
// shows one file dialog at a time and puts the answer into a path field, and
// the registrar for the window's `browse-requested` callback. They live here,
// not in main.cpp, so the headless tests execute the same registered callback
// the application does.
namespace motif::slint_app
{

// A dialog the user has not answered cannot be closed from outside, so exit
// waits this long for it and then abandons its thread.
constexpr auto picker_shutdown_wait = std::chrono::milliseconds {100};

// The error-bar text for a dialog that could not be shown. `message` is the
// toolkit's own reason and may be empty.
[[nodiscard]] auto picker_failure_text(std::string_view message) -> std::string;

// Shows the file dialog for a Browse button and writes the chosen path into the
// field the button belongs to. A choice only sets the text: nothing is
// submitted, trimmed or validated here, and the dialog is not workspace
// activity. Every method except the destructor's shutdown must be called on the
// event-loop thread.
//
// A dialog blocks until the user answers and nothing can close it, so it runs on
// a worker thread of its own (on the event-loop thread where the picker says the
// platform needs that) and its answer is posted back to the event loop. A
// request while a dialog is open is ignored. At shutdown the worker is asked to
// abort, given picker_shutdown_wait to finish, and otherwise detached: it then
// touches nothing of the runner or the window.
class async_picker_runner
{
  public:
    async_picker_runner(slint::ComponentWeakHandle<WorkspaceWindow> window,
                        event_loop_environment environment,
                        std::shared_ptr<file_picker> picker);
    ~async_picker_runner();

    async_picker_runner(async_picker_runner const&) = delete;
    auto operator=(async_picker_runner const&) -> async_picker_runner& = delete;
    async_picker_runner(async_picker_runner&&) = delete;
    auto operator=(async_picker_runner&&) -> async_picker_runner& = delete;

    // Opens the dialog for `target`, seeded from the current text of its field.
    // Does nothing while a dialog is open.
    void request(BrowseTarget target, std::string_view field_text);

    // True from the request until its answer has been applied.
    [[nodiscard]] auto open() const -> bool;

    // Stops delivering answers and ends the dialog's thread: aborts it, waits at
    // most picker_shutdown_wait, then detaches it. Safe to call twice.
    void shutdown();

  private:
    struct impl;
    std::shared_ptr<impl> impl_;
};

// Wires the window's Browse buttons to the runner.
void register_picker_callbacks(WorkspaceWindow& window, async_picker_runner& picker);

}  // namespace motif::slint_app
