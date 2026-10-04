#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <slint.h>

#include "motif/slint_app/analysis_presenter.hpp"
#include "motif/slint_app/board_host.hpp"
#include "motif/slint_app/workspace_service.hpp"
#include "workspace.h"

// Slint-facing adapters for the engine panel (Story 13.3): publishing the
// presenter's state to the window, the runner that drives presenter and engine
// service from the event loop, and the registrar for every panel callback. They
// live here, not in main.cpp, so the headless tests execute the same registered
// callbacks the application does.
namespace motif::slint_app
{

// Engine info lines are redrawn at most about ten times a second.
constexpr auto analysis_flush_interval = std::chrono::milliseconds {100};
// A position change restarts the analysis after this pause, so a burst of
// navigation costs one engine launch for the position it ends on.
constexpr auto analysis_follow_delay = std::chrono::milliseconds {150};

// How the runner reaches the event loop. `schedule` may be called from any
// thread and runs the task on the event-loop thread after the delay.
struct analysis_environment
{
    std::function<void(std::chrono::milliseconds, std::function<void()>)> schedule;
};

// Slint's event loop: invoke_from_event_loop, and a single-shot timer for delays.
[[nodiscard]] auto make_event_loop_environment() -> analysis_environment;

void publish_analysis(WorkspaceWindow& window, analysis_state const& state);

// Event-loop adapter around analysis_presenter and analysis_service. Every
// method must be called on the event-loop thread. Nothing here blocks except
// wait_until_idle(), shutdown() and the destructor; those are for tests and
// process exit, not for a running event loop.
//
// Engine events arrive on foreign threads. Each carries the epoch (bumped when
// the workspace changes) and the generation (one per session) it belongs to, is
// coalesced with its neighbours, and is applied on the event loop only if its
// epoch and generation are still current and the runner is still alive.
class async_analysis_runner
{
  public:
    // The current board position as FEN, or nullopt when no board is attached.
    using position_provider = std::function<std::optional<std::string>()>;

    async_analysis_runner(slint::ComponentWeakHandle<WorkspaceWindow> window,
                          workspace_service& config,
                          analysis_environment environment,
                          position_provider position);
    ~async_analysis_runner();

    async_analysis_runner(async_analysis_runner const&) = delete;
    auto operator=(async_analysis_runner const&) -> async_analysis_runner& = delete;
    async_analysis_runner(async_analysis_runner&&) = delete;
    auto operator=(async_analysis_runner&&) -> async_analysis_runner& = delete;

    // Reads the stored engine list and options and publishes them.
    void load();

    // Analyses the board's current position and keeps following it until stop().
    void start();
    // Stops, releases the engine and keeps the last lines on screen, marked stale.
    void stop();
    // The board published a position. Restarts a running analysis on a new
    // position after a short pause; anything else is ignored.
    void position_changed();
    // The workspace was closed or replaced: stop, release and drop the lines.
    void workspace_changed();

    void select_engine(int index);
    void add_engine(std::string_view path);
    void remove_engine();
    void set_multipv(int value);
    void set_limit_kind(int kind);
    void set_limit_value(int value);
    // Shows a notice in the panel.
    void notify(std::string message);

    // Stops delivering events and ends the engine session. Blocks.
    void shutdown();

    // Blocks until the engine service has carried out everything queued.
    void wait_until_idle();

    [[nodiscard]] auto state() const -> analysis_state const&;

  private:
    struct impl;
    std::shared_ptr<impl> impl_;
};

// Start and Stop are never gated: a running engine does not block the board and
// the board does not block it. Changes to the stored engine list and options
// wait for a workspace operation to finish, because that operation's worker
// thread owns the stored configuration. Leaving the Analysis preset stops the
// analysis.
void register_analysis_callbacks(WorkspaceWindow& window, async_analysis_runner& analysis, activity_probe const& activity);

}  // namespace motif::slint_app
