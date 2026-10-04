#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <slint.h>

#include "motif/db/database_manager.hpp"
#include "motif/db/types.hpp"
#include "motif/slint_app/activity_gate.hpp"
#include "motif/slint_app/board_host.hpp"
#include "motif/slint_app/event_loop.hpp"
#include "motif/slint_app/game_browser_presenter.hpp"
#include "motif/slint_app/replace_controller.hpp"
#include "workspace.h"

// Slint-facing adapters for the game browser: publishing the presenter's state
// to the window, the runner that drives the presenter from the event loop and a
// worker thread, and the registrar for every browser callback. They live here,
// not in main.cpp, so the headless tests execute the same registered callbacks
// the application does.
namespace motif::slint_app
{

// A list query that is still running after this long shows a progress bar; a
// quicker one shows none.
constexpr auto browser_progress_delay = std::chrono::milliseconds {150};

void clear_browser(WorkspaceWindow& window);
// `result_serial_base` keeps the published result serial rising when the
// presenter behind the state is replaced.
void publish_browser(WorkspaceWindow& window, game_browser_state const& state, std::uint64_t result_serial_base = 0);

// Off-event-loop adapter around the toolkit-neutral browser presenter. Every
// method must be called on the event-loop thread. Queries and activations run
// one at a time on a worker thread; a request that arrives while one runs waits
// in a slot and the newest request wins. A completion is applied on the event
// loop only if its epoch (bumped when the workspace changes) and its generation
// (one per request) are still current and the runner is still alive.
class async_browser_runner
{
  public:
    async_browser_runner(slint::ComponentWeakHandle<WorkspaceWindow> window, event_loop_environment environment);
    ~async_browser_runner();

    async_browser_runner(async_browser_runner const&) = delete;
    auto operator=(async_browser_runner const&) -> async_browser_runner& = delete;
    async_browser_runner(async_browser_runner&&) = delete;
    auto operator=(async_browser_runner&&) -> async_browser_runner& = delete;

    void attach(motif::db::database_manager& database);
    void detach();

    using activation_success_handler = std::function<void(motif::db::game_id, motif::db::game const&)>;
    using activation_failure_handler = std::function<void()>;
    void set_activation_handlers(activation_success_handler success, activation_failure_handler failure);

    // Handlers for activations that carry a ply: a search-result row, or a
    // game-list row while the list follows the board. They reuse the row
    // activation's single-read pipeline, so the completion hands the caller's
    // ply back with the loaded game.
    using external_activation_success_handler = std::function<void(motif::db::game_id, motif::db::game const&, std::size_t)>;
    void set_external_activation_handlers(external_activation_success_handler success, activation_failure_handler failure);

    [[nodiscard]] auto is_busy() const noexcept -> bool;

    // Snapshot of the browser's current player/result filters, reused as-is by
    // search queries so the two presenters share one filter model without
    // coupling to each other directly.
    [[nodiscard]] auto active_filter() const -> motif::db::search_filter;

    void load_initial();
    void set_filters(std::string player, std::string result);
    void set_page(std::size_t page_index);
    void sort(std::size_t column, bool ascending);
    void select(std::size_t row);
    void move_selection(int delta);
    // A game can be opened: one is selected and, in a followed list, it is a
    // row on screen (only those have a ply to open at).
    [[nodiscard]] auto can_activate() const -> bool;
    void activate();
    // Activates an arbitrary game by id at a specific ply (e.g. a search match
    // row), without touching the browser's row selection.
    void activate_by_id(motif::db::game_id game_id, std::size_t ply);
    void resize_column(std::size_t column, std::int32_t width);
    void dismiss_error();

    // Follow board (Story 13.5). set_follow() switches the list to or from the
    // games that reach the board's position; the caller has decided that the
    // workspace is idle. A refusal (no replayable position) leaves it as it was
    // and shows why. Either way the toggle is published as the runner holds it.
    void set_follow(bool enabled, std::optional<motif::db::zobrist_hash> board_position);
    // The board published a position. A followed list refreshes for it, unless
    // the browser is not on screen (the Explorer preset) or it follows that
    // position already.
    void position_changed(motif::db::zobrist_hash position);
    // The browser was mounted: a position change it missed is applied now.
    void shown(std::optional<motif::db::zobrist_hash> board_position);
    // Publishes the runner's state as it is, for example to snap a refused
    // toggle back.
    void republish();

    // Joins the worker thread if one is running. For tests and process exit.
    void wait_for_worker();

  private:
    struct impl;
    std::shared_ptr<impl> impl_;

    void follow_board_position(motif::db::zobrist_hash position);
};

// What the browser's callbacks need from the rest of the application.
struct browser_hooks
{
    // The player or result filter changed; the Explorer's search follows it.
    std::function<void()> filters_changed;
    // The position on the board, or nothing when none can be determined.
    std::function<std::optional<motif::db::zobrist_hash>()> board_position;
};

// Every game-list callback decides through run_when_idle on the workspace's
// activity, and opening a game first asks the replace confirmation. Turning
// Follow board on or off is gated the same way and, when refused, is published
// back so the toggle shows the state it still has.
void register_browser_callbacks(WorkspaceWindow& window,
                                async_browser_runner& browser,
                                activity_probe const& activity,
                                replace_controller& confirmation,
                                browser_hooks hooks);

}  // namespace motif::slint_app
