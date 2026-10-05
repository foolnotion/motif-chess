#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <slint.h>

#include "motif/slint_app/activity_gate.hpp"
#include "motif/slint_app/analysis_host.hpp"
#include "motif/slint_app/board_host.hpp"
#include "motif/slint_app/browser_host.hpp"
#include "motif/slint_app/database_host.hpp"
#include "motif/slint_app/import_service.hpp"
#include "motif/slint_app/nfd_file_picker.hpp"
#include "motif/slint_app/pgn_launch_queue.hpp"
#include "motif/slint_app/picker_host.hpp"
#include "motif/slint_app/replace_controller.hpp"
#include "motif/slint_app/search_host.hpp"
#include "motif/slint_app/workspace_host.hpp"
#include "motif/slint_app/workspace_service.hpp"
#include "workspace.h"

namespace
{

using motif::slint_app::async_board_runner;
using motif::slint_app::async_browser_runner;
using motif::slint_app::trim_whitespace;
using replace_confirmation = motif::slint_app::replace_controller;
using motif::slint_app::async_search_runner;
using motif::slint_app::async_workspace_runner;

auto shared_string(std::string_view value) -> slint::SharedString
{
    return slint::SharedString {value};
}

auto to_ui_count(std::size_t count) -> int
{
    constexpr auto maximum = static_cast<std::size_t>(std::numeric_limits<int>::max());
    return static_cast<int>(std::min(count, maximum));
}

auto to_ui_count(std::int64_t count) -> int
{
    if (count <= 0) {
        return 0;
    }
    constexpr auto maximum = static_cast<std::int64_t>(std::numeric_limits<int>::max());
    return static_cast<int>(std::min(count, maximum));
}

void publish_launch_queue(WorkspaceWindow& window, motif::slint_app::pgn_launch_queue const& queue)
{
    window.set_queued_import_count(to_ui_count(queue.pending()));
}

auto import_state_text(motif::slint_app::import_state const state) -> std::string_view
{
    using motif::slint_app::import_state;
    switch (state) {
        case import_state::idle:
            return "Idle";
        case import_state::running:
            return "Importing";
        case import_state::canceling:
            return "Canceling";
        case import_state::completed:
            return "Completed";
        case import_state::canceled:
            return "Canceled";
        case import_state::failed:
            return "Failed";
    }
    std::unreachable();
}

auto import_phase_text(motif::import::import_phase const phase) -> std::string_view
{
    using motif::import::import_phase;
    switch (phase) {
        case import_phase::idle:
            return "Preparing";
        case import_phase::ingesting:
            return "Reading games";
        case import_phase::deduplicating:
            return "Removing duplicates";
        case import_phase::rebuilding:
            return "Rebuilding position index";
    }
    std::unreachable();
}

auto publish_import_state(WorkspaceWindow& window, motif::slint_app::import_service const& importer) -> bool
{
    auto const snapshot = importer.snapshot();
    auto const active =
        snapshot.state == motif::slint_app::import_state::running || snapshot.state == motif::slint_app::import_state::canceling;
    window.set_import_active(active);
    window.set_import_status_visible(snapshot.state != motif::slint_app::import_state::idle);
    window.set_import_state_text(shared_string(import_state_text(snapshot.state)));
    window.set_import_phase_text(shared_string(import_phase_text(snapshot.phase)));
    window.set_import_processed(to_ui_count(snapshot.processed));
    window.set_import_total(to_ui_count(snapshot.total));
    window.set_import_committed(to_ui_count(snapshot.committed));
    window.set_import_skipped(to_ui_count(snapshot.skipped));
    window.set_import_errors(to_ui_count(snapshot.errors));
    if (snapshot.state == motif::slint_app::import_state::failed) {
        window.set_error_text(shared_string(snapshot.error_message));
    }
    return active;
}

void publish_import_start_error(WorkspaceWindow& window, motif::slint_app::import_error_code const error)
{
    switch (error) {
        case motif::slint_app::import_error_code::invalid_argument:
            window.set_error_text("A PGN file path is required.");
            return;
        case motif::slint_app::import_error_code::no_database:
            window.set_error_text("Open a database before importing.");
            return;
        case motif::slint_app::import_error_code::busy:
            window.set_error_text("An import is already running.");
            return;
    }
    std::unreachable();
}

// A bundle of long-lived objects owned by main(); every member outlives the context,
// so references are the honest type here.
// NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
struct import_context
{
    WorkspaceWindow& window;
    motif::slint_app::workspace_service& service;
    motif::slint_app::import_service& importer;
    async_browser_runner& browser;
    async_board_runner const& board;
    async_search_runner& search;
    motif::slint_app::pgn_launch_queue& launch_queue;
    slint::Timer& poll_timer;
    motif::slint_app::activity_probe const& activity;
    replace_confirmation const& confirmation;
};

// NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)

constexpr auto import_poll_interval = std::chrono::milliseconds {200};

// A failed import abandons the rest of the launch queue so one bad file does
// not hide behind (or silently trigger) imports of the files after it;
// a canceled import is the user's own decision to stop.
void abandon_launch_queue_after_unsuccessful_import(import_context& context)
{
    auto const state = context.importer.snapshot().state;
    if (state != motif::slint_app::import_state::failed && state != motif::slint_app::import_state::canceled) {
        return;
    }
    auto const dropped = context.launch_queue.clear();
    publish_launch_queue(context.window, context.launch_queue);
    if (state == motif::slint_app::import_state::failed && dropped > 0) {
        auto const error = context.window.get_error_text();
        context.window.set_error_text(shared_string(fmt::format("{} Skipped {} remaining queued PGN file{}.",
                                                                std::string_view {error.data(), error.size()},
                                                                dropped,
                                                                dropped == 1 ? "" : "s")));
    }
}

// Starts one import through the existing import service and polls it to
// completion. Shared by the Import PGN button and the launch-argument queue.
auto begin_import(import_context& context, std::string_view const path) -> bool
{
    context.window.set_import_path_text(shared_string(path));
    auto started = context.importer.start(context.service.active_database(), std::filesystem::path {path});
    if (!started) {
        publish_import_start_error(context.window, started.error());
        return false;
    }
    publish_import_state(context.window, context.importer);
    context.poll_timer.start(slint::TimerMode::Repeated,
                             import_poll_interval,
                             [&context]() -> void
                             {
                                 if (publish_import_state(context.window, context.importer)) {
                                     return;
                                 }
                                 context.poll_timer.stop();
                                 context.browser.load_initial();
                                 context.search.invalidate_position_cache();
                                 context.search.refresh_if_auto(context.board.current_hash(), context.browser.active_filter());
                                 abandon_launch_queue_after_unsuccessful_import(context);
                             });
    return true;
}

// Runs on a repeating timer while launch paths are pending: waits until a
// database is open, every background task is idle and no replace confirmation
// is open (it may be about to close or replace that database), then starts the
// next import.
void pump_launch_queue(import_context& context, slint::Timer& launch_timer)
{
    if (context.launch_queue.empty()) {
        launch_timer.stop();
        return;
    }
    if (!motif::slint_app::may_start_launch_import(
            motif::slint_app::launch_import_state {.activity = context.activity(),
                                                   .completion_pending = context.poll_timer.running(),
                                                   .confirmation_pending = context.confirmation.pending(),
                                                   .database_open = context.service.has_active()}))
    {
        return;
    }
    auto const next = context.launch_queue.take_next();
    publish_launch_queue(context.window, context.launch_queue);
    if (next) {
        begin_import(context, next->string());
    }
}

void register_import_callbacks(import_context& context)
{
    auto& window = context.window;
    window.on_import_requested(
        [&context](slint::SharedString const& path) -> void
        {
            // A running import is deliberately not gated here: begin_import
            // answers it with "An import is already running." instead of
            // ignoring the click. Every other activity waits.
            auto activity = context.activity();
            activity.import_active = false;
            if (!activity.idle()) {
                return;
            }
            context.window.set_error_text("");
            begin_import(context, trim_whitespace(std::string_view {path.data(), path.size()}));
        });
    window.on_cancel_import_requested([&context]() -> void { context.importer.request_cancel(); });
}

auto launch_arguments(std::span<char* const> const raw) -> std::vector<std::string>
{
    auto arguments = std::vector<std::string> {};
    if (raw.size() > 1) {
        arguments.assign(raw.begin() + 1, raw.end());
    }
    return arguments;
}

}  // namespace

auto main(int const argc, char** const argv) -> int
{
    auto service = motif::slint_app::workspace_service {motif::slint_app::config_path()};
    auto importer = motif::slint_app::import_service {};
    auto window = WorkspaceWindow::create();
    auto browser = async_browser_runner {window, motif::slint_app::make_event_loop_environment()};
    auto board = async_board_runner {window};
    auto search = motif::slint_app::async_search_runner {window, motif::slint_app::make_event_loop_environment()};
    auto analysis = motif::slint_app::async_analysis_runner {window,
                                                             service,
                                                             motif::slint_app::make_event_loop_environment(),
                                                             [&board]() -> std::optional<std::string> { return board.current_fen(); }};
    auto picker = motif::slint_app::async_picker_runner {
        window, motif::slint_app::make_event_loop_environment(), motif::slint_app::make_default_file_picker()};
    auto activation = motif::slint_app::make_board_activation_handlers(slint::ComponentWeakHandle<WorkspaceWindow> {window}, board);
    browser.set_activation_handlers(activation.loaded, activation.failed);
    browser.set_external_activation_handlers(activation.loaded_at, activation.failed_at);
    board.add_position_changed_handler([&search, &browser](motif::db::zobrist_hash const hash) -> void
                                       { motif::slint_app::follow_board_position(search, browser, hash); });
    board.add_position_changed_handler([&analysis](motif::db::zobrist_hash const) -> void { analysis.position_changed(); });
    board.add_position_changed_handler([&browser](motif::db::zobrist_hash const hash) -> void { browser.position_changed(hash); });
    auto runner = motif::slint_app::async_workspace_runner {
        service, window, browser, board, search, analysis, motif::slint_app::make_event_loop_environment()};
    search.set_retry_on_database_failure([&importer]() -> bool { return importer.active(); });
    auto import_timer = slint::Timer {};
    auto launch_arguments_list = launch_arguments(std::span<char* const> {argv, static_cast<std::size_t>(argc)});
    auto launch_queue = motif::slint_app::pgn_launch_queue::from_arguments(launch_arguments_list);
    auto confirmation = motif::slint_app::make_replace_confirmation(window, board);
    auto const activity = motif::slint_app::activity_probe {
        [&]() -> motif::slint_app::workspace_activity { return motif::slint_app::current_activity(importer, runner, browser, search); }};
    auto import_context_value = import_context {
        .window = *window,
        .service = service,
        .importer = importer,
        .browser = browser,
        .board = board,
        .search = search,
        .launch_queue = launch_queue,
        .poll_timer = import_timer,
        .activity = activity,
        .confirmation = confirmation,
    };
    auto launch_timer = slint::Timer {};

    motif::slint_app::clear_browser(*window);
    motif::slint_app::clear_board(*window);
    motif::slint_app::clear_search(*window);
    motif::slint_app::publish_workspace(*window, service);
    analysis.load();
    motif::slint_app::register_workspace_callbacks(*window, service, runner, browser, board, search);
    motif::slint_app::register_database_callbacks(
        *window,
        activity,
        confirmation,
        motif::slint_app::database_actions {
            .create = [&runner](std::string const& dir_path, std::string const& name) -> bool
            { return runner.run_create(dir_path, name) == async_workspace_runner::start_outcome::accepted; },
            .open = [&runner](std::string const& path) -> bool
            { return runner.run_open(path) == async_workspace_runner::start_outcome::accepted; },
            .scratch = [&runner]() -> bool { return runner.run_scratch() == async_workspace_runner::start_outcome::accepted; },
            .close = [&]() -> void
            {
                analysis.workspace_changed();
                browser.detach();
                board.detach();
                search.detach();
                service.close_active();
                window->set_error_text("");
                motif::slint_app::publish_workspace(*window, service);
            },
        });
    motif::slint_app::register_analysis_callbacks(*window, analysis, activity);
    motif::slint_app::register_browser_callbacks(
        *window,
        browser,
        activity,
        confirmation,
        motif::slint_app::browser_hooks {
            .filters_changed = [&search, &board, &browser]() -> void
            { search.refresh_if_auto(board.current_hash(), browser.active_filter()); },
            .board_position = [&board]() -> std::optional<motif::db::zobrist_hash> { return board.current_hash(); },
        });
    motif::slint_app::register_board_callbacks(*window, board, activity);
    motif::slint_app::register_play_callbacks(*window, board, activity, confirmation);
    motif::slint_app::register_search_callbacks(*window, search, browser, board, activity, confirmation);
    motif::slint_app::register_explorer_callbacks(
        *window, board, activity, [&board, &search]() -> bool { return motif::slint_app::explorer_rows_current(board, search); });
    register_import_callbacks(import_context_value);
    motif::slint_app::register_picker_callbacks(*window, picker);

    // Launch arguments (command line or a file manager's %F): invalid ones are
    // reported in the error bar and on stderr; valid ones wait for a database
    // and then follow the normal import flow.
    publish_launch_queue(*window, launch_queue);
    if (auto const rejection = launch_queue.rejection_message(); !rejection.empty()) {
        fmt::print(stderr, "warning: {}\n", rejection);
        window->set_error_text(shared_string(rejection));
    }
    if (!launch_queue.empty()) {
        launch_timer.start(slint::TimerMode::Repeated,
                           import_poll_interval,
                           [&import_context_value, &launch_timer]() -> void { pump_launch_queue(import_context_value, launch_timer); });
    }

    motif::slint_app::install_close_guard(*window, board, confirmation);

    window->run();
    // The window is closed: a file dialog still open is given a moment, then left behind.
    picker.shutdown();
    // Stop and release the engine before anything else is torn down.
    analysis.shutdown();
    return 0;
}
