#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "motif/slint_app/browser_host.hpp"

#include <slint.h>

#include "motif/slint_app/activity_gate.hpp"
#include "motif/slint_app/game_browser_presenter.hpp"
#include "motif/slint_app/replace_controller.hpp"
#include "workspace.h"

namespace motif::slint_app
{

namespace
{

using browser_query_result = browser_result<browser_query>;
using activation_result = browser_result<activation_request>;
using browser_page_result = browser_result<browser_page>;
using loaded_game_result = browser_result<loaded_game>;

auto shared_string(std::string_view value) -> slint::SharedString
{
    return slint::SharedString {value};
}

auto to_ui_count(std::size_t const count) -> int
{
    constexpr auto maximum = static_cast<std::size_t>(std::numeric_limits<int>::max());
    return static_cast<int>(std::min(count, maximum));
}

auto to_ui_count(std::int64_t const count) -> int
{
    if (count <= 0) {
        return 0;
    }
    constexpr auto maximum = static_cast<std::int64_t>(std::numeric_limits<int>::max());
    return static_cast<int>(std::min(count, maximum));
}

// The window property is an int; the serial only has to change, so it wraps.
auto to_ui_serial(std::uint64_t const serial) -> int
{
    return static_cast<int>(serial & static_cast<std::uint64_t>(std::numeric_limits<int>::max()));
}

auto component_matches(slint::ComponentWeakHandle<WorkspaceWindow> const& window,
                       std::shared_ptr<std::atomic<std::uint64_t>> const& lifetime,
                       std::uint64_t const lifetime_token) -> bool
{
    return lifetime->load(std::memory_order_acquire) == lifetime_token && window.lock().has_value();
}

}  // namespace

void clear_browser(WorkspaceWindow& window)
{
    window.set_game_rows(std::make_shared<slint::VectorModel<GameRow>>());
    window.set_game_total_count(0);
    window.set_game_page_index(0);
    window.set_game_has_previous_page(false);
    window.set_game_has_next_page(false);
    window.set_game_selected_row(-1);
    window.set_game_player_filter("");
    window.set_game_result_filter("");
    window.set_game_sort_column(0);
    window.set_game_sort_ascending(true);
    window.set_game_active_title("");
    window.set_game_column_widths(std::make_shared<slint::VectorModel<std::int32_t>>());
    window.set_game_following(false);
    window.set_game_refreshing(false);
    window.set_game_progress_visible(false);
    window.set_game_status_text("");
    window.set_browser_error_text("");
    window.set_browser_busy(false);
}

void publish_browser(WorkspaceWindow& window, game_browser_state const& state, std::uint64_t const result_serial_base)
{
    auto rows = std::vector<GameRow> {};
    rows.reserve(state.games.size());
    for (auto const& game : state.games) {
        rows.push_back(GameRow {
            .white = shared_string(game.white),
            .black = shared_string(game.black),
            .result = shared_string(game.result),
            .event = shared_string(game.event),
            .date = shared_string(game.date),
            .eco = shared_string(game.eco),
        });
    }
    window.set_game_rows(std::make_shared<slint::VectorModel<GameRow>>(std::move(rows)));
    window.set_game_total_count(to_ui_count(state.total_count));
    window.set_game_page_index(to_ui_count(state.page_index));
    window.set_game_has_previous_page(state.has_previous_page);
    window.set_game_has_next_page(state.has_next_page);
    window.set_game_selected_row(state.selected_row ? to_ui_count(*state.selected_row) : -1);
    window.set_game_player_filter(shared_string(state.player_filter));
    window.set_game_result_filter(shared_string(state.result_filter));
    window.set_game_sort_column(static_cast<std::int32_t>(state.sort_column));
    window.set_game_sort_ascending(state.sort_ascending);
    window.set_game_following(state.following);
    window.set_game_refreshing(state.loading);
    window.set_game_status_text(shared_string(browser_status_text(state)));
    window.set_game_result_serial(to_ui_serial(result_serial_base + state.result_serial));
    window.set_browser_error_text(shared_string(state.error_text));
    auto column_widths = std::vector<std::int32_t> {state.column_widths.begin(), state.column_widths.end()};
    window.set_game_column_widths(std::make_shared<slint::VectorModel<std::int32_t>>(std::move(column_widths)));

    if (state.active_game) {
        auto const title = state.active_game->white.name + " – " + state.active_game->black.name + "  " + state.active_game->result;
        window.set_game_active_title(shared_string(title));
    } else {
        window.set_game_active_title("");
    }
}

// ---------------------------------------------------------------------------
// async_browser_runner
// ---------------------------------------------------------------------------

struct async_browser_runner::impl
{
    struct pending_external_activation
    {
        motif::db::game_id game_id;
        std::size_t ply;
        std::uint64_t epoch;
    };

    struct completion_key
    {
        std::uint64_t epoch;
        std::uint64_t generation;
    };

    impl(slint::ComponentWeakHandle<WorkspaceWindow> window_value, event_loop_environment environment_value)
        : window(std::move(window_value))
        , environment(std::move(environment_value))
    {
    }

    slint::ComponentWeakHandle<WorkspaceWindow> window;
    event_loop_environment environment;
    std::shared_ptr<game_browser_presenter> presenter;
    std::mutex worker_mutex;
    std::thread worker;
    std::atomic<bool> busy {false};
    std::optional<browser_query_result> pending_query;
    std::optional<activation_result> pending_activation;
    std::optional<pending_external_activation> pending_external;
    std::uint64_t epoch {};
    // Added to the presenter's result serial, so replacing the presenter (a
    // workspace change) still moves the published serial forward.
    std::uint64_t result_serial_base {};
    // A list query is in flight, and which one the progress timer belongs to.
    bool query_running {false};
    std::uint64_t progress_serial {};
    std::shared_ptr<std::atomic<std::uint64_t>> lifetime {std::make_shared<std::atomic<std::uint64_t>>(0)};
    activation_success_handler activation_succeeded;
    activation_failure_handler activation_failed;
    external_activation_success_handler external_activation_succeeded;
    activation_failure_handler external_activation_failed;

    void publish() const
    {
        if (presenter) {
            if (auto locked = window.lock()) {
                publish_browser(**locked, presenter->state(), result_serial_base);
            }
        }
    }

    // The list query is over, or the workspace it belonged to is gone.
    void end_progress()
    {
        query_running = false;
        ++progress_serial;
        if (auto locked = window.lock()) {
            (*locked)->set_game_progress_visible(false);
        }
    }

    // Moves the published serial past everything the retiring presenter used.
    void retire_presenter()
    {
        result_serial_base += presenter ? presenter->state().result_serial + 1 : 1;
        presenter.reset();
        end_progress();
    }

    void set_busy(bool const value) const
    {
        if (auto locked = window.lock()) {
            (*locked)->set_browser_busy(value);
        }
    }

    static void dispatch_pending(std::shared_ptr<impl> const& shared)
    {
        if (shared->pending_query) {
            auto query = std::move(*shared->pending_query);
            shared->pending_query.reset();
            run_query(shared, std::move(query), shared->epoch);
            return;
        }
        if (shared->pending_activation) {
            auto request = std::move(*shared->pending_activation);
            shared->pending_activation.reset();
            run_activation(shared, std::move(request), shared->epoch);
            return;
        }
        if (shared->pending_external && shared->presenter) {
            auto request = *shared->pending_external;
            shared->pending_external.reset();
            run_external_activation(shared, shared->presenter->prepare_external_activation(request.game_id), request.ply, request.epoch);
        }
    }

    static void finish(std::shared_ptr<impl> const& shared)
    {
        shared->busy.store(false, std::memory_order_release);
        shared->set_busy(/*value=*/false);
        dispatch_pending(shared);
    }

    // Marks the runner busy and runs `work` on the worker thread. The caller
    // has checked that nothing else runs.
    template<typename Work>
    static void launch(std::shared_ptr<impl> const& shared, Work work)
    {
        shared->busy.store(true, std::memory_order_release);
        shared->set_busy(/*value=*/true);
        std::scoped_lock const lock {shared->worker_mutex};
        if (shared->worker.joinable()) {
            shared->worker.join();
        }
        shared->worker = std::thread(std::move(work));
    }

    // Hands a finished result to the event loop, unless the runner or the
    // window is gone by then.
    static void post(std::shared_ptr<impl> const& shared,
                     std::shared_ptr<std::atomic<std::uint64_t>> lifetime,
                     std::uint64_t const lifetime_token,
                     std::function<void()> task)
    {
        shared->environment.schedule(
            std::chrono::milliseconds {0},
            [window = shared->window, lifetime = std::move(lifetime), lifetime_token, task = std::move(task)]() -> void
            {
                if (component_matches(window, lifetime, lifetime_token)) {
                    task();
                }
            });
    }

    static void complete_query(std::shared_ptr<impl> const& shared, completion_key const key, browser_page_result result)
    {
        shared->end_progress();
        if (shared->epoch == key.epoch && shared->presenter) {
            if (result) {
                (void)shared->presenter->apply_query(std::move(*result));
            } else {
                (void)shared->presenter->apply_query_error(key.generation, result.error());
            }
            shared->publish();
        }
        finish(shared);
    }

    static void run_query(std::shared_ptr<impl> const& shared, browser_query_result query, std::uint64_t const epoch)
    {
        if (!query) {
            shared->publish();
            return;
        }
        // Publishes the rows as about to be replaced (dimmed) from the moment
        // the query exists, whether it runs now or waits its turn.
        shared->publish();
        if (shared->busy.load(std::memory_order_acquire)) {
            shared->pending_query = std::move(query);
            shared->pending_activation.reset();
            shared->pending_external.reset();
            return;
        }
        auto presenter = shared->presenter;
        auto const lifetime = shared->lifetime;
        auto const lifetime_token = lifetime->load(std::memory_order_acquire);
        // The bar appears only if the query outlasts the delay.
        shared->query_running = true;
        shared->environment.schedule(browser_progress_delay,
                                     [shared, serial = ++shared->progress_serial, lifetime, lifetime_token]() -> void
                                     {
                                         if (component_matches(shared->window, lifetime, lifetime_token) && shared->query_running
                                             && shared->progress_serial == serial)
                                         {
                                             if (auto locked = shared->window.lock()) {
                                                 (*locked)->set_game_progress_visible(true);
                                             }
                                         }
                                     });
        launch(shared,
               [shared, presenter = std::move(presenter), query = std::move(*query), epoch, lifetime, lifetime_token]() mutable -> void
               {
                   auto result = presenter->execute_query(query);
                   post(shared,
                        lifetime,
                        lifetime_token,
                        [shared, epoch, generation = query.generation, result = std::move(result)]() mutable -> void
                        { complete_query(shared, completion_key {.epoch = epoch, .generation = generation}, std::move(result)); });
               });
    }

    static void complete_activation(std::shared_ptr<impl> const& shared, completion_key const key, loaded_game_result result)
    {
        if (shared->epoch == key.epoch && shared->presenter) {
            if (result) {
                auto applied = shared->presenter->apply_activation(std::move(*result));
                auto const& presenter_state = shared->presenter->state();
                if (applied && *applied && shared->activation_succeeded && presenter_state.active_game) {
                    shared->activation_succeeded(presenter_state.active_game_id, *presenter_state.active_game);
                } else if (!applied && shared->activation_failed) {
                    shared->activation_failed();
                }
            } else {
                auto applied = shared->presenter->apply_activation_error(key.generation, result.error());
                if (applied && *applied && shared->activation_failed) {
                    shared->activation_failed();
                }
            }
            shared->publish();
        }
        finish(shared);
    }

    static void run_activation(std::shared_ptr<impl> const& shared, activation_result request, std::uint64_t const epoch)
    {
        if (!request) {
            shared->publish();
            return;
        }
        if (shared->busy.load(std::memory_order_acquire)) {
            shared->pending_activation = std::move(request);
            shared->pending_external.reset();
            return;
        }
        auto presenter = shared->presenter;
        auto const lifetime = shared->lifetime;
        auto const lifetime_token = lifetime->load(std::memory_order_acquire);
        launch(shared,
               [shared, presenter = std::move(presenter), request = *request, epoch, lifetime, lifetime_token]() mutable -> void
               {
                   auto result = presenter->execute_activation(request);
                   post(shared,
                        lifetime,
                        lifetime_token,
                        [shared, epoch, generation = request.generation, result = std::move(result)]() mutable -> void
                        { complete_activation(shared, completion_key {.epoch = epoch, .generation = generation}, std::move(result)); });
               });
    }

    static void complete_external_activation(std::shared_ptr<impl> const& shared,
                                             completion_key const key,
                                             loaded_game_result result,
                                             std::size_t const ply)
    {
        if (shared->epoch == key.epoch && shared->presenter) {
            if (result) {
                auto applied = shared->presenter->apply_external_activation(std::move(*result));
                auto const& presenter_state = shared->presenter->state();
                if (applied && *applied && shared->external_activation_succeeded && presenter_state.active_game) {
                    shared->external_activation_succeeded(presenter_state.active_game_id, *presenter_state.active_game, ply);
                } else if (!applied && shared->external_activation_failed) {
                    shared->external_activation_failed();
                }
            } else {
                auto applied = shared->presenter->apply_activation_error(key.generation, result.error());
                if (applied && *applied && shared->external_activation_failed) {
                    shared->external_activation_failed();
                }
            }
            shared->publish();
        }
        finish(shared);
    }

    static void run_external_activation(std::shared_ptr<impl> const& shared,
                                        activation_result request,
                                        std::size_t const ply,
                                        std::uint64_t const epoch)
    {
        if (!request) {
            shared->publish();
            return;
        }
        if (shared->busy.load(std::memory_order_acquire)) {
            shared->pending_external = pending_external_activation {.game_id = request->game_id, .ply = ply, .epoch = epoch};
            return;
        }
        auto presenter = shared->presenter;
        auto const lifetime = shared->lifetime;
        auto const lifetime_token = lifetime->load(std::memory_order_acquire);
        launch(shared,
               [shared, presenter = std::move(presenter), request = *request, ply, epoch, lifetime, lifetime_token]() mutable -> void
               {
                   auto result = presenter->execute_activation(request);
                   post(shared,
                        lifetime,
                        lifetime_token,
                        [shared, epoch, generation = request.generation, result = std::move(result), ply]() mutable -> void
                        {
                            complete_external_activation(
                                shared, completion_key {.epoch = epoch, .generation = generation}, std::move(result), ply);
                        });
               });
    }

    template<typename Prepare>
    void prepare_query(std::shared_ptr<impl> const& shared, Prepare prepare)
    {
        if (!presenter) {
            return;
        }
        run_query(shared, prepare(*presenter), epoch);
    }
};

async_browser_runner::async_browser_runner(slint::ComponentWeakHandle<WorkspaceWindow> window, event_loop_environment environment)
    : impl_(std::make_shared<impl>(std::move(window), std::move(environment)))
{
}

async_browser_runner::~async_browser_runner()
{
    impl_->lifetime->fetch_add(1, std::memory_order_acq_rel);
    wait_for_worker();
}

void async_browser_runner::wait_for_worker()
{
    std::scoped_lock const lock {impl_->worker_mutex};
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
}

void async_browser_runner::attach(motif::db::database_manager& database)
{
    ++impl_->epoch;
    impl_->retire_presenter();
    impl_->presenter = std::make_shared<game_browser_presenter>(database);
    impl_->pending_query.reset();
    impl_->pending_activation.reset();
    impl_->pending_external.reset();
}

void async_browser_runner::detach()
{
    ++impl_->epoch;
    impl_->retire_presenter();
    impl_->pending_query.reset();
    impl_->pending_activation.reset();
    impl_->pending_external.reset();
    if (auto locked = impl_->window.lock()) {
        clear_browser(**locked);
    }
}

void async_browser_runner::set_activation_handlers(activation_success_handler success, activation_failure_handler failure)
{
    impl_->activation_succeeded = std::move(success);
    impl_->activation_failed = std::move(failure);
}

void async_browser_runner::set_external_activation_handlers(external_activation_success_handler success, activation_failure_handler failure)
{
    impl_->external_activation_succeeded = std::move(success);
    impl_->external_activation_failed = std::move(failure);
}

auto async_browser_runner::is_busy() const noexcept -> bool
{
    return impl_->busy.load(std::memory_order_acquire);
}

auto async_browser_runner::active_filter() const -> motif::db::search_filter
{
    auto filter = motif::db::search_filter {};
    if (!impl_->presenter) {
        return filter;
    }
    auto const& browser_state = impl_->presenter->state();
    if (!browser_state.player_filter.empty()) {
        filter.player_name = browser_state.player_filter;
    }
    if (!browser_state.result_filter.empty()) {
        filter.result = browser_state.result_filter;
    }
    return filter;
}

void async_browser_runner::load_initial()
{
    impl_->prepare_query(impl_, [](auto& presenter) -> auto { return presenter.prepare_initial_load(); });
}

void async_browser_runner::set_filters(std::string player, std::string result)
{
    impl_->prepare_query(impl_,
                         [player = std::move(player), result = std::move(result)](auto& presenter) mutable -> auto
                         { return presenter.set_filters(std::move(player), std::move(result)); });
}

void async_browser_runner::set_page(std::size_t const page_index)
{
    impl_->prepare_query(impl_, [page_index](auto& presenter) -> auto { return presenter.set_page(page_index); });
}

void async_browser_runner::sort(std::size_t const column, bool const ascending)
{
    impl_->prepare_query(impl_, [column, ascending](auto& presenter) -> auto { return presenter.sort_games(column, ascending); });
}

void async_browser_runner::select(std::size_t const row)
{
    if (!impl_->presenter) {
        return;
    }
    (void)impl_->presenter->select_game(row);
    impl_->publish();
}

void async_browser_runner::move_selection(int const delta)
{
    if (!impl_->presenter) {
        return;
    }
    (void)impl_->presenter->move_selection(delta);
    impl_->publish();
}

auto async_browser_runner::can_activate() const -> bool
{
    if (!impl_->presenter) {
        return false;
    }
    auto const& state = impl_->presenter->state();
    return state.has_selection && (!state.rows_restricted || state.selected_row.has_value());
}

void async_browser_runner::activate()
{
    if (!impl_->presenter) {
        return;
    }
    auto request = impl_->presenter->prepare_activation();
    // A followed list opens the game at the ply where it reaches the position:
    // the board then shows the position the list follows, and nothing refreshes.
    auto const ply = request ? request->ply : std::nullopt;
    if (ply) {
        impl::run_external_activation(impl_, std::move(request), *ply, impl_->epoch);
    } else {
        impl::run_activation(impl_, std::move(request), impl_->epoch);
    }
}

void async_browser_runner::activate_by_id(motif::db::game_id const game_id, std::size_t const ply)
{
    if (!impl_->presenter) {
        return;
    }
    if (impl_->busy.load(std::memory_order_acquire)) {
        impl_->pending_external = impl::pending_external_activation {.game_id = game_id, .ply = ply, .epoch = impl_->epoch};
        return;
    }
    impl::run_external_activation(impl_, impl_->presenter->prepare_external_activation(game_id), ply, impl_->epoch);
}

void async_browser_runner::resize_column(std::size_t const column, std::int32_t const width)
{
    if (!impl_->presenter) {
        return;
    }
    (void)impl_->presenter->resize_column(column, width);
    impl_->publish();
}

void async_browser_runner::dismiss_error()
{
    if (impl_->presenter) {
        impl_->presenter->dismiss_error();
        impl_->publish();
    }
}

void async_browser_runner::republish()
{
    impl_->publish();
}

void async_browser_runner::set_follow(bool const enabled, std::optional<motif::db::zobrist_hash> const board_position)
{
    if (!impl_->presenter || impl_->presenter->state().following == enabled) {
        impl_->publish();
        return;
    }
    impl::run_query(impl_, impl_->presenter->set_follow(enabled, board_position), impl_->epoch);
}

void async_browser_runner::position_changed(motif::db::zobrist_hash const position)
{
    // The Explorer preset has no game browser: its list is brought up to date
    // when the browser comes back.
    if (auto locked = impl_->window.lock(); !locked || (*locked)->get_workspace_preset() == WorkspacePreset::Explorer) {
        return;
    }
    follow_board_position(position);
}

void async_browser_runner::shown(std::optional<motif::db::zobrist_hash> const board_position)
{
    if (board_position) {
        follow_board_position(*board_position);
    }
}

void async_browser_runner::follow_board_position(motif::db::zobrist_hash const position)
{
    if (!impl_->presenter) {
        return;
    }
    auto query = impl_->presenter->follow_position(position);
    if (!query) {
        impl_->publish();
        return;
    }
    auto refresh = std::move(*query);
    if (refresh) {
        impl::run_query(impl_, browser_query_result {std::move(*refresh)}, impl_->epoch);
    }
}

// ---------------------------------------------------------------------------
// Registrar
// ---------------------------------------------------------------------------

void register_browser_callbacks(WorkspaceWindow& window,
                                async_browser_runner& browser,
                                activity_probe const& activity,
                                replace_controller& confirmation,
                                browser_hooks hooks)
{
    window.on_game_filters_changed(
        [&browser, activity, filters_changed = hooks.filters_changed](slint::SharedString const& player, slint::SharedString const& result) -> void
        {
            (void)run_when_idle(activity(),
                                [&]() -> void
                                {
                                    browser.set_filters(std::string {std::string_view {player.data(), player.size()}},
                                                        std::string {std::string_view {result.data(), result.size()}});
                                    // Filter refresh follows the auto-search preference: with
                                    // auto-search off, only a manual Search re-queries.
                                    if (filters_changed) {
                                        filters_changed();
                                    }
                                });
        });
    window.on_game_page_requested(
        [&browser, activity](std::int32_t const page) -> void
        {
            if (page >= 0) {
                (void)run_when_idle(activity(), [&]() -> void { browser.set_page(static_cast<std::size_t>(page)); });
            }
        });
    window.on_game_selected(
        [&browser, activity](std::int32_t const row) -> void
        {
            if (row >= 0) {
                (void)run_when_idle(activity(), [&]() -> void { browser.select(static_cast<std::size_t>(row)); });
            }
        });
    window.on_game_move_selection([&browser, activity](std::int32_t const delta) -> void
                                  { (void)run_when_idle(activity(), [&]() -> void { browser.move_selection(delta); }); });
    window.on_game_activate_requested(
        [&browser, &confirmation, activity]() -> void
        {
            (void)run_when_idle(activity(),
                                [&]() -> void
                                {
                                    // With nothing to open there is nothing to discard the board for:
                                    // report why, without asking.
                                    if (!browser.can_activate()) {
                                        browser.activate();
                                        return;
                                    }
                                    confirmation.request("Load game",
                                                         [&browser, activity]() -> void
                                                         { (void)run_when_idle(activity(), [&]() -> void { browser.activate(); }); });
                                });
        });
    window.on_game_sort_requested(
        [&browser, activity](std::int32_t const column, bool const ascending) -> void
        {
            if (column >= 0) {
                (void)run_when_idle(activity(), [&]() -> void { browser.sort(static_cast<std::size_t>(column), ascending); });
            }
        });
    window.on_game_column_resized(
        [&browser, activity](std::int32_t const column, std::int32_t const width) -> void
        {
            if (column >= 0) {
                (void)run_when_idle(activity(), [&]() -> void { browser.resize_column(static_cast<std::size_t>(column), width); });
            }
        });
    // The toggle never moves by itself: whatever the host decides is published,
    // so a refused press leaves it as it was.
    window.on_game_follow_toggled(
        [&browser, &confirmation, activity, board_position = hooks.board_position](bool const enabled) -> void
        {
            // A confirmation waiting for its answer is as busy as a running query.
            auto const ran = !confirmation.pending()
                && run_when_idle(activity(),
                                 [&]() -> void
                                 {
                                     auto const position = board_position ? board_position() : std::nullopt;
                                     browser.set_follow(enabled, position);
                                 });
            if (!ran) {
                browser.republish();
            }
        });
    // Mounting the browser catches the list up with the board, unless the workspace is
    // busy (an import, say, leaves the position index unusable until it ends).
    window.on_game_browser_shown(
        [&browser, activity, board_position = std::move(hooks.board_position)]() -> void
        {
            (void)run_when_idle(activity(), [&]() -> void { browser.shown(board_position ? board_position() : std::nullopt); });
        });
}

}  // namespace motif::slint_app
