#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <slint.h>

#include "motif/slint_app/game_browser_presenter.hpp"
#include "motif/slint_app/import_service.hpp"
#include "motif/slint_app/workspace_service.hpp"
#include "workspace.h"

namespace
{

using workspace_result = motif::slint_app::result<void>;
using browser_query_result = motif::slint_app::browser_result<motif::slint_app::browser_query>;
using activation_result = motif::slint_app::browser_result<motif::slint_app::activation_request>;
using browser_page_result = motif::slint_app::browser_result<motif::slint_app::browser_page>;
using loaded_game_result = motif::slint_app::browser_result<motif::slint_app::loaded_game>;

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

void publish_workspace(WorkspaceWindow& window, motif::slint_app::workspace_service const& service)
{
    window.set_has_database(service.has_active());
    window.set_is_scratch(service.is_temporary());
    window.set_display_name(shared_string(service.display_name()));
    window.set_active_path(shared_string(service.active_path()));

    auto rows = std::vector<RecentEntry> {};
    rows.reserve(service.recent_databases().size());
    for (auto const& entry : service.recent_databases()) {
        rows.push_back(RecentEntry {.name = shared_string(entry.name), .path = shared_string(entry.path)});
    }
    window.set_recent_databases(std::make_shared<slint::VectorModel<RecentEntry>>(std::move(rows)));
}

auto import_state_text(motif::slint_app::import_state state) -> std::string_view
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

auto import_phase_text(motif::import::import_phase phase) -> std::string_view
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
    window.set_browser_busy(false);
}

void publish_browser(WorkspaceWindow& window, motif::slint_app::game_browser_state const& state)
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
    window.set_error_text(shared_string(state.error_text));
    auto column_widths = std::vector<std::int32_t> {state.column_widths.begin(), state.column_widths.end()};
    window.set_game_column_widths(std::make_shared<slint::VectorModel<std::int32_t>>(std::move(column_widths)));

    if (state.active_game) {
        auto const title = state.active_game->white.name + " – " + state.active_game->black.name + "  " + state.active_game->result;
        window.set_game_active_title(shared_string(title));
    } else {
        window.set_game_active_title("");
    }
}

auto component_matches(slint::ComponentWeakHandle<WorkspaceWindow> const& window,
                       std::shared_ptr<std::atomic<std::uint64_t>> const& lifetime,
                       std::uint64_t lifetime_token) -> bool
{
    return lifetime->load(std::memory_order_acquire) == lifetime_token && window.lock().has_value();
}

class async_browser_runner
{
  public:
    explicit async_browser_runner(slint::ComponentWeakHandle<WorkspaceWindow> window)
        : state_(std::make_shared<state>(std::move(window)))
    {
    }

    ~async_browser_runner()
    {
        state_->lifetime->fetch_add(1, std::memory_order_acq_rel);
        std::scoped_lock const lock {state_->worker_mutex};
        if (state_->worker.joinable()) {
            state_->worker.join();
        }
    }

    async_browser_runner(async_browser_runner const&) = delete;
    auto operator=(async_browser_runner const&) -> async_browser_runner& = delete;
    async_browser_runner(async_browser_runner&&) = delete;
    auto operator=(async_browser_runner&&) -> async_browser_runner& = delete;

    void attach(motif::db::database_manager& database)
    {
        ++state_->epoch;
        state_->presenter = std::make_shared<motif::slint_app::game_browser_presenter>(database);
        state_->pending_query.reset();
        state_->pending_activation.reset();
    }

    void detach()
    {
        ++state_->epoch;
        state_->presenter.reset();
        state_->pending_query.reset();
        state_->pending_activation.reset();
        if (auto locked = state_->window.lock()) {
            clear_browser(**locked);
        }
    }

    [[nodiscard]] auto is_busy() const noexcept -> bool { return state_->busy.load(std::memory_order_acquire); }

    void load_initial()
    {
        prepare_query([](auto& presenter) -> auto { return presenter.prepare_initial_load(); });
    }

    void set_filters(std::string player, std::string result)
    {
        prepare_query([player = std::move(player), result = std::move(result)](auto& presenter) mutable -> auto
                      { return presenter.set_filters(std::move(player), std::move(result)); });
    }

    void set_page(std::size_t page_index)
    {
        prepare_query([page_index](auto& presenter) -> auto { return presenter.set_page(page_index); });
    }

    void sort(std::size_t column, bool ascending)
    {
        prepare_query([column, ascending](auto& presenter) -> auto { return presenter.sort_games(column, ascending); });
    }

    void select(std::size_t row)
    {
        if (!state_->presenter) {
            return;
        }
        (void)state_->presenter->select_game(row);
        publish();
    }

    void move_selection(int delta)
    {
        if (!state_->presenter) {
            return;
        }
        (void)state_->presenter->move_selection(delta);
        publish();
    }

    void activate()
    {
        if (!state_->presenter) {
            return;
        }
        run_activation(state_, state_->presenter->prepare_activation(), state_->epoch);
    }

    void resize_column(std::size_t column, std::int32_t width)
    {
        if (!state_->presenter) {
            return;
        }
        (void)state_->presenter->resize_column(column, width);
        publish();
    }

    void dismiss_error()
    {
        if (state_->presenter) {
            state_->presenter->dismiss_error();
        }
    }

  private:
    struct state
    {
        explicit state(slint::ComponentWeakHandle<WorkspaceWindow> window_value)
            : window(std::move(window_value))
        {
        }

        slint::ComponentWeakHandle<WorkspaceWindow> window;
        std::shared_ptr<motif::slint_app::game_browser_presenter> presenter;
        std::mutex worker_mutex;
        std::thread worker;
        std::atomic<bool> busy {false};
        std::optional<browser_query_result> pending_query;
        std::optional<activation_result> pending_activation;
        std::uint64_t epoch {};
        std::shared_ptr<std::atomic<std::uint64_t>> lifetime {std::make_shared<std::atomic<std::uint64_t>>(0)};
    };

    std::shared_ptr<state> state_;

    template<typename Prepare>
    void prepare_query(Prepare prepare)
    {
        if (!state_->presenter) {
            return;
        }
        run_query(state_, prepare(*state_->presenter), state_->epoch);
    }

    void publish()
    {
        if (state_->presenter) {
            if (auto locked = state_->window.lock()) {
                publish_browser(**locked, state_->presenter->state());
            }
        }
    }

    static void dispatch_pending(std::shared_ptr<state> const& shared)
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
        }
    }

    struct completion_key
    {
        std::uint64_t epoch;
        std::uint64_t generation;
    };

    static void complete_query(std::shared_ptr<state> const& shared, completion_key const key, browser_page_result result)
    {
        if (shared->epoch == key.epoch && shared->presenter) {
            if (result) {
                (void)shared->presenter->apply_query(std::move(*result));
            } else {
                (void)shared->presenter->apply_query_error(key.generation, result.error());
            }
            if (auto locked = shared->window.lock()) {
                publish_browser(**locked, shared->presenter->state());
            }
        }
        shared->busy.store(false, std::memory_order_release);
        if (auto locked = shared->window.lock()) {
            (*locked)->set_browser_busy(false);
        }
        dispatch_pending(shared);
    }

    static void run_query(std::shared_ptr<state> const& shared, browser_query_result query, std::uint64_t epoch)
    {
        if (!query) {
            if (shared->presenter) {
                if (auto locked = shared->window.lock()) {
                    publish_browser(**locked, shared->presenter->state());
                }
            }
            return;
        }
        if (shared->busy.exchange(true, std::memory_order_acq_rel)) {
            shared->pending_query = std::move(query);
            shared->pending_activation.reset();
            return;
        }
        if (auto locked = shared->window.lock()) {
            (*locked)->set_browser_busy(true);
        }
        auto presenter = shared->presenter;
        auto const lifetime = shared->lifetime;
        auto const lifetime_token = lifetime->load(std::memory_order_acquire);
        auto const window = shared->window;
        std::scoped_lock const lock {shared->worker_mutex};
        if (shared->worker.joinable()) {
            shared->worker.join();
        }
        shared->worker = std::thread(
            [shared, presenter = std::move(presenter), query = std::move(*query), epoch, lifetime, lifetime_token, window]() mutable -> void
            {
                auto result = presenter->execute_query(query);
                slint::invoke_from_event_loop(
                    [shared, epoch, generation = query.generation, result = std::move(result), lifetime, lifetime_token, window]() mutable
                        -> void
                    {
                        if (!component_matches(window, lifetime, lifetime_token)) {
                            return;
                        }
                        complete_query(shared, completion_key {.epoch = epoch, .generation = generation}, std::move(result));
                    });
            });
    }

    static void complete_activation(std::shared_ptr<state> const& shared, completion_key const key, loaded_game_result result)
    {
        if (shared->epoch == key.epoch && shared->presenter) {
            if (result) {
                (void)shared->presenter->apply_activation(std::move(*result));
            } else {
                (void)shared->presenter->apply_activation_error(key.generation, result.error());
            }
            if (auto locked = shared->window.lock()) {
                publish_browser(**locked, shared->presenter->state());
            }
        }
        shared->busy.store(false, std::memory_order_release);
        if (auto locked = shared->window.lock()) {
            (*locked)->set_browser_busy(false);
        }
        dispatch_pending(shared);
    }

    static void run_activation(std::shared_ptr<state> const& shared, activation_result request, std::uint64_t epoch)
    {
        if (!request) {
            if (shared->presenter) {
                if (auto locked = shared->window.lock()) {
                    publish_browser(**locked, shared->presenter->state());
                }
            }
            return;
        }
        if (shared->busy.exchange(true, std::memory_order_acq_rel)) {
            shared->pending_activation = std::move(request);
            return;
        }
        if (auto locked = shared->window.lock()) {
            (*locked)->set_browser_busy(true);
        }
        auto presenter = shared->presenter;
        auto const lifetime = shared->lifetime;
        auto const lifetime_token = lifetime->load(std::memory_order_acquire);
        auto const window = shared->window;
        std::scoped_lock const lock {shared->worker_mutex};
        if (shared->worker.joinable()) {
            shared->worker.join();
        }
        shared->worker = std::thread(
            [shared, presenter = std::move(presenter), request = *request, epoch, lifetime, lifetime_token, window]() mutable -> void
            {
                auto result = presenter->execute_activation(request);
                slint::invoke_from_event_loop(
                    [shared, epoch, generation = request.generation, result = std::move(result), lifetime, lifetime_token, window]() mutable
                        -> void
                    {
                        if (!component_matches(window, lifetime, lifetime_token)) {
                            return;
                        }
                        complete_activation(shared, completion_key {.epoch = epoch, .generation = generation}, std::move(result));
                    });
            });
    }
};

class async_workspace_runner
{
  public:
    enum class start_outcome : std::uint8_t
    {
        accepted,
        busy,
    };

    async_workspace_runner(motif::slint_app::workspace_service& service,
                           slint::ComponentWeakHandle<WorkspaceWindow> window,
                           async_browser_runner& browser)
        : state_(std::make_shared<state>(service, std::move(window), browser))
    {
    }

    ~async_workspace_runner()
    {
        state_->lifetime->fetch_add(1, std::memory_order_acq_rel);
        if (state_->worker.joinable()) {
            state_->worker.join();
        }
    }

    async_workspace_runner(async_workspace_runner const&) = delete;
    auto operator=(async_workspace_runner const&) -> async_workspace_runner& = delete;
    async_workspace_runner(async_workspace_runner&&) = delete;
    auto operator=(async_workspace_runner&&) -> async_workspace_runner& = delete;

    [[nodiscard]] auto run_create(std::string dir_path, std::string name) -> start_outcome
    {
        return start([service = &state_->service, dir_path = std::move(dir_path), name = std::move(name)]() mutable -> workspace_result
                     { return service->create_database(dir_path, name); });
    }

    [[nodiscard]] auto run_open(std::string dir_path) -> start_outcome
    {
        return start([service = &state_->service, dir_path = std::move(dir_path)]() mutable -> workspace_result
                     { return service->open_database(dir_path); });
    }

    [[nodiscard]] auto run_scratch() -> start_outcome
    {
        return start([service = &state_->service]() -> workspace_result { return service->activate_scratch(); });
    }

    [[nodiscard]] auto is_busy() const noexcept -> bool { return state_->busy.load(std::memory_order_acquire); }

  private:
    struct state
    {
        state(motif::slint_app::workspace_service& service_value,
              slint::ComponentWeakHandle<WorkspaceWindow> window_value,
              async_browser_runner& browser_value)
            : service(service_value)
            , window(std::move(window_value))
            , browser(browser_value)
        {
        }

        motif::slint_app::workspace_service& service;
        slint::ComponentWeakHandle<WorkspaceWindow> window;
        async_browser_runner& browser;
        std::thread worker;
        std::atomic<bool> busy {false};
        std::shared_ptr<std::atomic<std::uint64_t>> lifetime {std::make_shared<std::atomic<std::uint64_t>>(0)};
    };

    std::shared_ptr<state> state_;

    template<typename Operation>
    auto start(Operation operation) -> start_outcome
    {
        if (state_->busy.exchange(true, std::memory_order_acq_rel)) {
            return start_outcome::busy;
        }
        if (state_->worker.joinable()) {
            state_->worker.join();
        }
        auto const lifetime = state_->lifetime;
        auto const lifetime_token = lifetime->load(std::memory_order_acquire);
        auto const window = state_->window;
        state_->worker = std::thread(
            [shared = state_, operation = std::move(operation), lifetime, lifetime_token, window]() mutable -> void
            {
                auto result = operation();
                slint::invoke_from_event_loop(
                    [shared, result = std::move(result), lifetime, lifetime_token, window]() mutable -> void
                    {
                        if (!component_matches(window, lifetime, lifetime_token)) {
                            return;
                        }
                        if (auto locked = shared->window.lock()) {
                            auto& workspace_window = **locked;
                            workspace_window.set_error_text(shared_string(result ? std::string_view {} : shared->service.error_message()));
                            publish_workspace(workspace_window, shared->service);
                            workspace_window.set_busy(false);
                            if (result) {
                                if (auto* database = shared->service.active_database(); database != nullptr) {
                                    shared->browser.attach(*database);
                                    shared->browser.load_initial();
                                }
                            }
                        }
                        shared->busy.store(false, std::memory_order_release);
                    });
            });
        return start_outcome::accepted;
    }
};

void publish_import_start_error(WorkspaceWindow& window, motif::slint_app::import_error_code error)
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

void register_workspace_callbacks(WorkspaceWindow& window,
                                  motif::slint_app::workspace_service& service,
                                  motif::slint_app::import_service const& importer,
                                  async_workspace_runner& runner,
                                  async_browser_runner& browser)
{
    window.on_create_requested(
        [&](slint::SharedString const& dir_path, slint::SharedString const& name) -> void
        {
            if (importer.active() || browser.is_busy()) {
                return;
            }
            window.set_error_text("");
            if (runner.run_create(std::string {dir_path}, std::string {name}) == async_workspace_runner::start_outcome::accepted) {
                window.set_busy(true);
            }
        });
    window.on_open_requested(
        [&](slint::SharedString const& dir_path) -> void
        {
            if (importer.active() || browser.is_busy()) {
                return;
            }
            window.set_error_text("");
            if (runner.run_open(std::string {dir_path}) == async_workspace_runner::start_outcome::accepted) {
                window.set_busy(true);
            }
        });
    window.on_recent_activated(
        [&](slint::SharedString const& path) -> void
        {
            if (importer.active() || browser.is_busy()) {
                return;
            }
            window.set_error_text("");
            if (runner.run_open(std::string {path}) == async_workspace_runner::start_outcome::accepted) {
                window.set_busy(true);
            }
        });
    window.on_scratch_requested(
        [&]() -> void
        {
            if (runner.is_busy() || browser.is_busy() || importer.active()) {
                return;
            }
            window.set_error_text("");
            if (runner.run_scratch() == async_workspace_runner::start_outcome::accepted) {
                window.set_busy(true);
            }
        });
    window.on_close_requested(
        [&]() -> void
        {
            if (runner.is_busy() || browser.is_busy() || importer.active()) {
                return;
            }
            browser.detach();
            service.close_active();
            window.set_error_text("");
            publish_workspace(window, service);
        });
    window.on_dismiss_error(
        [&]() -> void
        {
            browser.dismiss_error();
            window.set_error_text("");
        });
}

void register_browser_callbacks(WorkspaceWindow& window,
                                async_browser_runner& browser,
                                motif::slint_app::import_service const& importer,
                                async_workspace_runner const& runner)
{
    window.on_game_filters_changed(
        [&](slint::SharedString const& player, slint::SharedString const& result) -> void
        {
            if (importer.active() || runner.is_busy()) {
                return;
            }
            browser.set_filters(std::string {player}, std::string {result});
        });
    window.on_game_page_requested(
        [&](std::int32_t page) -> void
        {
            if (importer.active() || runner.is_busy() || page < 0) {
                return;
            }
            browser.set_page(static_cast<std::size_t>(page));
        });
    window.on_game_selected(
        [&](std::int32_t row) -> void
        {
            if (importer.active() || runner.is_busy() || row < 0) {
                return;
            }
            browser.select(static_cast<std::size_t>(row));
        });
    window.on_game_move_selection(
        [&](std::int32_t delta) -> void
        {
            if (importer.active() || runner.is_busy()) {
                return;
            }
            browser.move_selection(delta);
        });
    window.on_game_activate_requested(
        [&]() -> void
        {
            if (importer.active() || runner.is_busy()) {
                return;
            }
            browser.activate();
        });
    window.on_game_sort_requested(
        [&](std::int32_t column, bool ascending) -> void
        {
            if (importer.active() || runner.is_busy() || column < 0) {
                return;
            }
            browser.sort(static_cast<std::size_t>(column), ascending);
        });
    window.on_game_column_resized(
        [&](std::int32_t column, std::int32_t width) -> void
        {
            if (importer.active() || runner.is_busy() || column < 0) {
                return;
            }
            browser.resize_column(static_cast<std::size_t>(column), width);
        });
}

void register_import_callbacks(WorkspaceWindow& window,
                               motif::slint_app::workspace_service& service,
                               motif::slint_app::import_service& importer,
                               async_workspace_runner const& runner,
                               async_browser_runner const& browser,
                               slint::Timer& timer)
{
    constexpr auto poll_interval = std::chrono::milliseconds {200};
    window.on_import_requested(
        [&](slint::SharedString const& path) -> void
        {
            if (runner.is_busy() || browser.is_busy()) {
                return;
            }
            window.set_error_text("");
            auto started = importer.start(service.active_database(), std::filesystem::path {std::string {path}});
            if (!started) {
                publish_import_start_error(window, started.error());
                return;
            }
            publish_import_state(window, importer);
            timer.start(slint::TimerMode::Repeated,
                        poll_interval,
                        [&]() -> void
                        {
                            if (!publish_import_state(window, importer)) {
                                timer.stop();
                            }
                        });
        });
    window.on_cancel_import_requested([&]() -> void { importer.request_cancel(); });
}

}  // namespace

auto main() -> int
{
    auto service = motif::slint_app::workspace_service {};
    auto importer = motif::slint_app::import_service {};
    auto window = WorkspaceWindow::create();
    auto browser = async_browser_runner {window};
    auto runner = async_workspace_runner {service, window, browser};
    auto import_timer = slint::Timer {};

    publish_workspace(*window, service);
    clear_browser(*window);
    register_workspace_callbacks(*window, service, importer, runner, browser);
    register_browser_callbacks(*window, browser, importer, runner);
    register_import_callbacks(*window, service, importer, runner, browser, import_timer);

    window->run();
    return 0;
}
