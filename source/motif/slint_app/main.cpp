#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <slint.h>

#include "motif/slint_app/activity_gate.hpp"
#include "motif/slint_app/board_host.hpp"
#include "motif/slint_app/board_presenter.hpp"
#include "motif/slint_app/game_browser_presenter.hpp"
#include "motif/slint_app/import_service.hpp"
#include "motif/slint_app/pgn_launch_queue.hpp"
#include "motif/slint_app/replace_controller.hpp"
#include "motif/slint_app/replace_gate.hpp"
#include "motif/slint_app/search_presenter.hpp"
#include "motif/slint_app/workspace_service.hpp"
#include "workspace.h"

namespace
{

using motif::slint_app::async_board_runner;
using replace_confirmation = motif::slint_app::replace_controller;
using workspace_result = motif::slint_app::result<void>;
using browser_query_result = motif::slint_app::browser_result<motif::slint_app::browser_query>;
using activation_result = motif::slint_app::browser_result<motif::slint_app::activation_request>;
using browser_page_result = motif::slint_app::browser_result<motif::slint_app::browser_page>;
using loaded_game_result = motif::slint_app::browser_result<motif::slint_app::loaded_game>;
using search_query_result = motif::slint_app::search_result<motif::slint_app::search_query>;
using search_page_result = motif::slint_app::search_result<motif::slint_app::search_page>;

auto shared_string(std::string_view value) -> slint::SharedString
{
    return slint::SharedString {value};
}

auto trim_whitespace(std::string_view value) -> std::string_view
{
    constexpr auto whitespace = " \t\n\r\f\v";
    auto const begin = value.find_first_not_of(whitespace);
    if (begin == std::string_view::npos) {
        return {};
    }
    return value.substr(begin, value.find_last_not_of(whitespace) - begin + 1);
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

auto to_ui_preset(motif::slint_app::workspace_preset_kind const kind) -> WorkspacePreset
{
    switch (kind) {
        case motif::slint_app::workspace_preset_kind::explorer:
            return WorkspacePreset::Explorer;
        case motif::slint_app::workspace_preset_kind::analysis:
            return WorkspacePreset::Analysis;
        case motif::slint_app::workspace_preset_kind::review:
            break;
    }
    return WorkspacePreset::Review;
}

auto from_ui_preset(WorkspacePreset const preset) -> motif::slint_app::workspace_preset_kind
{
    switch (preset) {
        case WorkspacePreset::Explorer:
            return motif::slint_app::workspace_preset_kind::explorer;
        case WorkspacePreset::Analysis:
            return motif::slint_app::workspace_preset_kind::analysis;
        case WorkspacePreset::Review:
            break;
    }
    return motif::slint_app::workspace_preset_kind::review;
}

void publish_workspace(WorkspaceWindow& window, motif::slint_app::workspace_service const& service)
{
    // Restored before the workspace becomes visible, so the first rendered
    // frame already shows the saved preset.
    window.set_workspace_preset(to_ui_preset(service.preset()));
    window.set_has_database(service.has_active());
    window.set_is_scratch(service.is_temporary());
    window.set_display_name(shared_string(service.display_name()));
    window.set_active_path(shared_string(service.active_path()));
    window.set_notation_below(service.notation_below());

    auto rows = std::vector<RecentEntry> {};
    rows.reserve(service.recent_databases().size());
    for (auto const& entry : service.recent_database_states()) {
        rows.push_back(RecentEntry {.name = shared_string(entry.name), .path = shared_string(entry.path), .available = entry.available});
    }
    window.set_recent_databases(std::make_shared<slint::VectorModel<RecentEntry>>(std::move(rows)));
}

void publish_launch_queue(WorkspaceWindow& window, motif::slint_app::pgn_launch_queue const& queue)
{
    window.set_queued_import_count(to_ui_count(queue.pending()));
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

void clear_search(WorkspaceWindow& window)
{
    window.set_search_matches(std::make_shared<slint::VectorModel<SearchMatchRow>>());
    window.set_search_total_matches(0);
    window.set_search_page_index(0);
    window.set_search_has_previous_page(false);
    window.set_search_has_next_page(false);
    window.set_search_continuations(std::make_shared<slint::VectorModel<ContinuationRow>>());
    window.set_search_total_games(0);
    window.set_search_active_filter_summary("");
    window.set_search_searching(false);
    window.set_search_busy(false);
}

auto elo_text(std::optional<std::int32_t> const elo) -> std::string
{
    return elo ? fmt::format("{}", *elo) : std::string {};
}

auto elo_text(std::optional<double> const elo) -> std::string
{
    if (!elo) {
        return {};
    }
    constexpr auto rounding_offset = 0.5;
    return fmt::format("{}", static_cast<int>(*elo + rounding_offset));
}

// Renders the metadata filters currently applied to a search query as a
// short human-readable summary for AC3's "active filters" display. Reads
// only the raw filter fields the browser already owns; builds no
// independent filter model.
auto filter_summary(motif::db::search_filter const& filter) -> std::string
{
    if (filter.player_name && !filter.player_name->empty() && filter.result && !filter.result->empty()) {
        return fmt::format("player \"{}\", result {}", *filter.player_name, *filter.result);
    }
    if (filter.player_name && !filter.player_name->empty()) {
        return fmt::format("player \"{}\"", *filter.player_name);
    }
    if (filter.result && !filter.result->empty()) {
        return fmt::format("result {}", *filter.result);
    }
    return {};
}

void publish_search(WorkspaceWindow& window, motif::slint_app::search_state const& state)
{
    auto matches = std::vector<SearchMatchRow> {};
    matches.reserve(state.matches.size());
    for (auto const& match : state.matches) {
        matches.push_back(SearchMatchRow {
            .white = shared_string(match.game.white),
            .black = shared_string(match.game.black),
            .result = shared_string(match.game.result),
            .event = shared_string(match.game.event),
            .date = shared_string(match.game.date),
            .eco = shared_string(match.game.eco),
            .white_elo = shared_string(elo_text(match.game.white_elo)),
            .black_elo = shared_string(elo_text(match.game.black_elo)),
            .ply = to_ui_count(static_cast<std::size_t>(match.ply)),
        });
    }
    window.set_search_matches(std::make_shared<slint::VectorModel<SearchMatchRow>>(std::move(matches)));
    window.set_search_total_matches(to_ui_count(state.total_matches));

    auto continuations = std::vector<ContinuationRow> {};
    continuations.reserve(state.continuations.size());
    window.set_search_page_index(to_ui_count(state.page_index));
    window.set_search_has_previous_page(state.has_previous_page);
    window.set_search_has_next_page(state.has_next_page);
    for (auto const& continuation : state.continuations) {
        continuations.push_back(ContinuationRow {
            .san = shared_string(continuation.san),
            .frequency = to_ui_count(static_cast<std::size_t>(continuation.frequency)),
            .direct_frequency = to_ui_count(static_cast<std::size_t>(continuation.direct_frequency)),
            .white_wins = to_ui_count(static_cast<std::size_t>(continuation.white_wins)),
            .draws = to_ui_count(static_cast<std::size_t>(continuation.draws)),
            .black_wins = to_ui_count(static_cast<std::size_t>(continuation.black_wins)),
            .white_elo = shared_string(elo_text(continuation.average_white_elo)),
            .black_elo = shared_string(elo_text(continuation.average_black_elo)),
            .eco = shared_string(continuation.eco.value_or(std::string {})),
            .opening_name = shared_string(continuation.opening_name.value_or(std::string {})),
        });
    }
    window.set_search_continuations(std::make_shared<slint::VectorModel<ContinuationRow>>(std::move(continuations)));
    window.set_search_total_games(to_ui_count(state.total_games));
    window.set_search_active_filter_summary(shared_string(filter_summary(state.active_filter)));
    window.set_search_searching(state.searching);
    window.set_search_auto_search(state.auto_search);
    window.set_search_error_text(shared_string(state.error_text));
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
        state_->pending_external_activation.reset();
    }

    void detach()
    {
        ++state_->epoch;
        state_->presenter.reset();
        state_->pending_query.reset();
        state_->pending_activation.reset();
        state_->pending_external_activation.reset();
        if (auto locked = state_->window.lock()) {
            clear_browser(**locked);
        }
    }

    using activation_success_handler = std::function<void(motif::db::game_id, motif::db::game const&)>;
    using activation_failure_handler = std::function<void()>;

    void set_activation_handlers(activation_success_handler success, activation_failure_handler failure)
    {
        state_->activation_succeeded = std::move(success);
        state_->activation_failed = std::move(failure);
    }

    // Handlers for activations that originate outside this runner's own row
    // selection (e.g. a search-result row): reuses the identical
    // single-read pipeline as row activation via activate_by_id(), so the
    // completion carries the caller-supplied ply alongside the loaded game.
    using external_activation_success_handler = std::function<void(motif::db::game_id, motif::db::game const&, std::size_t)>;

    void set_external_activation_handlers(external_activation_success_handler success, activation_failure_handler failure)
    {
        state_->external_activation_succeeded = std::move(success);
        state_->external_activation_failed = std::move(failure);
    }

    [[nodiscard]] auto is_busy() const noexcept -> bool { return state_->busy.load(std::memory_order_acquire); }

    // Snapshot of the browser's current player/result filters, reused as-is
    // by search queries so the two presenters share one filter model
    // without coupling to each other directly.
    [[nodiscard]] auto active_filter() const -> motif::db::search_filter
    {
        auto filter = motif::db::search_filter {};
        if (!state_->presenter) {
            return filter;
        }
        auto const& browser_state = state_->presenter->state();
        if (!browser_state.player_filter.empty()) {
            filter.player_name = browser_state.player_filter;
        }
        if (!browser_state.result_filter.empty()) {
            filter.result = browser_state.result_filter;
        }
        return filter;
    }

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

    // Activates an arbitrary game by id at a specific ply (e.g. a search
    // match row). Reuses this runner's existing single-read activation
    // pipeline -- busy gating, worker thread, lifetime token,
    // generation-guarded completion -- rather than opening a second
    // concurrent database read, and does not touch browser row selection.
    void activate_by_id(motif::db::game_id game_id, std::size_t ply)
    {
        if (!state_->presenter) {
            return;
        }
        if (state_->busy.load(std::memory_order_acquire)) {
            state_->pending_external_activation = pending_external_activation {.game_id = game_id, .ply = ply, .epoch = state_->epoch};
            return;
        }
        run_external_activation(state_, state_->presenter->prepare_external_activation(game_id), ply, state_->epoch);
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
            if (auto locked = state_->window.lock()) {
                publish_browser(**locked, state_->presenter->state());
            }
        }
    }

  private:
    struct pending_external_activation
    {
        motif::db::game_id game_id;
        std::size_t ply;
        std::uint64_t epoch;
    };

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
        std::optional<pending_external_activation> pending_external_activation;
        std::uint64_t epoch {};
        std::shared_ptr<std::atomic<std::uint64_t>> lifetime {std::make_shared<std::atomic<std::uint64_t>>(0)};
        activation_success_handler activation_succeeded;
        activation_failure_handler activation_failed;
        external_activation_success_handler external_activation_succeeded;
        activation_failure_handler external_activation_failed;
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
            return;
        }
        if (shared->pending_external_activation && shared->presenter) {
            auto request = *shared->pending_external_activation;
            shared->pending_external_activation.reset();
            run_external_activation(shared, shared->presenter->prepare_external_activation(request.game_id), request.ply, request.epoch);
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
            shared->pending_external_activation.reset();
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
            shared->pending_external_activation.reset();
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

    static void complete_external_activation(std::shared_ptr<state> const& shared,
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

    static void run_external_activation(std::shared_ptr<state> const& shared,
                                        activation_result request,
                                        std::size_t ply,
                                        std::uint64_t epoch)
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
            shared->pending_external_activation = pending_external_activation {.game_id = request->game_id, .ply = ply, .epoch = epoch};
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
            [shared, presenter = std::move(presenter), request = *request, ply, epoch, lifetime, lifetime_token, window]() mutable -> void
            {
                auto result = presenter->execute_activation(request);
                slint::invoke_from_event_loop(
                    [shared,
                     epoch,
                     generation = request.generation,
                     result = std::move(result),
                     ply,
                     lifetime,
                     lifetime_token,
                     window]() mutable -> void
                    {
                        if (!component_matches(window, lifetime, lifetime_token)) {
                            return;
                        }
                        complete_external_activation(
                            shared, completion_key {.epoch = epoch, .generation = generation}, std::move(result), ply);
                    });
            });
    }
};

// Off-event-loop adapter around the toolkit-neutral search presenter,
// mirroring async_browser_runner's worker-thread + invoke_from_event_loop +
// lifetime-token + generation shape (search hits the database and must not
// block the UI thread). Deliberately a fourth, locally-duplicated runner
// rather than a shared base with async_browser_runner/async_board_runner --
// see Story 9.3's Dev Notes on premature abstraction of this pattern.
class async_search_runner
{
  public:
    explicit async_search_runner(slint::ComponentWeakHandle<WorkspaceWindow> window)
        : state_(std::make_shared<state>(std::move(window)))
    {
    }

    ~async_search_runner()
    {
        state_->lifetime->fetch_add(1, std::memory_order_acq_rel);
        std::scoped_lock const lock {state_->worker_mutex};
        if (state_->worker.joinable()) {
            state_->worker.join();
        }
    }

    async_search_runner(async_search_runner const&) = delete;
    auto operator=(async_search_runner const&) -> async_search_runner& = delete;
    async_search_runner(async_search_runner&&) = delete;
    auto operator=(async_search_runner&&) -> async_search_runner& = delete;

    void set_retry_on_database_failure(std::function<bool()> retry_condition)
    {
        state_->retry_on_database_failure = std::move(retry_condition);
    }

    void attach(motif::db::database_manager& database)
    {
        ++state_->epoch;
        state_->presenter = std::make_shared<motif::slint_app::search_presenter>(database);
        state_->pending_query.reset();
        state_->last_hash.reset();
        publish();
    }

    void detach()
    {
        state_->lifetime->fetch_add(1, std::memory_order_acq_rel);

        ++state_->epoch;
        state_->pending_query.reset();
        {
            std::scoped_lock const lock {state_->worker_mutex};
            if (state_->worker.joinable()) {
                state_->worker.join();
            }
        }
        state_->busy.store(false, std::memory_order_release);
        state_->presenter.reset();
        state_->last_hash.reset();
        if (auto locked = state_->window.lock()) {
            (*locked)->set_search_busy(false);
            clear_search(**locked);
        }
    }

    // Must run before a forced refresh (e.g. after an import commits new
    // games) so the refresh does not serve the starting-position cache's
    // now-stale data.
    void invalidate_position_cache()
    {
        if (state_->presenter) {
            state_->presenter->invalidate_starting_position_cache();
        }
    }

    [[nodiscard]] auto is_busy() const noexcept -> bool { return state_->busy.load(std::memory_order_acquire); }

    void search(motif::db::zobrist_hash const hash, motif::db::search_filter const& filter)
    {
        if (!state_->presenter) {
            return;
        }
        state_->last_hash = hash;
        auto query = state_->presenter->search(hash, filter);
        publish();
        if (!query) {
            run_query(state_, search_query_result {tl::unexpected {query.error()}}, state_->epoch);
            return;
        }
        auto& dispatchable = *query;
        if (!dispatchable) {
            // Served synchronously from the starting-position cache;
            // state() already reflects it via publish() above.
            return;
        }
        run_query(state_, search_query_result {std::move(*dispatchable)}, state_->epoch);
    }

    void search(std::optional<motif::db::zobrist_hash> const& hash, motif::db::search_filter const& filter)
    {
        if (hash) {
            search(*hash, filter);
        }
    }

    // Re-issues a search only if the position actually changed and
    // auto-search is enabled, avoiding duplicate queries on every board
    // render tick.
    void search_if_hash_changed(motif::db::zobrist_hash const hash, motif::db::search_filter const& filter)
    {
        if (!state_->presenter || !state_->presenter->state().auto_search) {
            return;
        }
        if (state_->last_hash && *state_->last_hash == hash) {
            return;
        }
        search(hash, filter);
    }

    void search_if_hash_changed(std::optional<motif::db::zobrist_hash> const& hash, motif::db::search_filter const& filter)
    {
        if (hash) {
            search_if_hash_changed(*hash, filter);
        }
    }

    void set_page(std::size_t const page_index)
    {
        if (!state_->presenter || state_->presenter->state().searching) {
            return;
        }
        auto filter = state_->presenter->state().active_filter;
        filter.offset = page_index * filter.limit;
        search(state_->presenter->state().current_hash, filter);
    }

    void refresh_if_auto(std::optional<motif::db::zobrist_hash> const& hash, motif::db::search_filter const& filter)
    {
        if (state_->presenter && state_->presenter->state().auto_search && hash) {
            search(*hash, filter);
        }
    }

    void set_auto_search(bool const enabled)
    {
        if (!state_->presenter) {
            return;
        }
        state_->presenter->set_auto_search(enabled);
        publish();
    }

    void publish_auto_search() { publish(); }

    [[nodiscard]] auto match_at(std::size_t const row) const -> std::optional<std::pair<motif::db::game_id, std::size_t>>
    {
        if (!state_->presenter || row >= state_->presenter->state().matches.size()) {
            return std::nullopt;
        }
        auto const& match = state_->presenter->state().matches[row];
        return std::make_pair(match.game.id, static_cast<std::size_t>(match.ply));
    }

    void dismiss_error()
    {
        if (state_->presenter) {
            state_->presenter->dismiss_error();
            publish();
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
        std::shared_ptr<motif::slint_app::search_presenter> presenter;
        std::mutex worker_mutex;
        std::thread worker;
        std::atomic<bool> busy {false};
        std::optional<search_query_result> pending_query;
        std::optional<motif::db::zobrist_hash> last_hash;
        std::uint64_t epoch {};
        std::shared_ptr<std::atomic<std::uint64_t>> lifetime {std::make_shared<std::atomic<std::uint64_t>>(0)};
        std::function<bool()> retry_on_database_failure;
    };

    std::shared_ptr<state> state_;

    void publish()
    {
        if (state_->presenter) {
            if (auto locked = state_->window.lock()) {
                publish_search(**locked, state_->presenter->state());
            }
        }
    }

    static void dispatch_pending(std::shared_ptr<state> const& shared)
    {
        if (shared->pending_query) {
            auto query = std::move(*shared->pending_query);
            shared->pending_query.reset();
            run_query(shared, std::move(query), shared->epoch);
        }
    }

    struct completion_key
    {
        std::uint64_t epoch;
        std::uint64_t generation;
    };

    static void complete_query(std::shared_ptr<state> const& shared, completion_key const key, search_page_result result)
    {
        if (shared->epoch == key.epoch && shared->presenter) {
            if (result) {
                (void)shared->presenter->apply_query(std::move(*result));
            } else {
                (void)shared->presenter->apply_query_error(key.generation, result.error());
            }
            if (auto locked = shared->window.lock()) {
                publish_search(**locked, shared->presenter->state());
            }
        }
        shared->busy.store(false, std::memory_order_release);
        if (auto locked = shared->window.lock()) {
            (*locked)->set_search_busy(false);
        }
        dispatch_pending(shared);
    }

    static void run_query(std::shared_ptr<state> const& shared, search_query_result query, std::uint64_t epoch)
    {
        if (!query) {
            if (shared->presenter) {
                if (auto locked = shared->window.lock()) {
                    publish_search(**locked, shared->presenter->state());
                }
            }
            return;
        }
        if (shared->busy.exchange(true, std::memory_order_acq_rel)) {
            shared->pending_query = std::move(query);
            return;
        }
        if (auto locked = shared->window.lock()) {
            (*locked)->set_search_busy(true);
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
                if (!result && result.error().code == motif::slint_app::error_code::database_failure && shared->retry_on_database_failure
                    && shared->retry_on_database_failure())
                {
                    result = presenter->execute_query(query);
                }
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
                           async_browser_runner& browser,
                           async_board_runner& board,
                           async_search_runner& search)
        : state_(std::make_shared<state>(service, std::move(window), browser, board, search))
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
              async_browser_runner& browser_value,
              async_board_runner& board_value,
              async_search_runner& search_value)
            : service(service_value)
            , window(std::move(window_value))
            , browser(browser_value)
            , board(board_value)
            , search(search_value)
        {
        }

        motif::slint_app::workspace_service& service;
        slint::ComponentWeakHandle<WorkspaceWindow> window;
        async_browser_runner& browser;
        async_board_runner& board;
        async_search_runner& search;
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
                                    shared->search.attach(*database);
                                    shared->board.attach();
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

// What the workspace is busy with right now: import, workspace change, game
// load, board action or search. Every board, play, New Game and replace
// callback decides on this one snapshot through run_when_idle.
auto current_activity(motif::slint_app::import_service const& importer,
                      async_workspace_runner const& runner,
                      async_browser_runner const& browser,
                      async_board_runner const& board,
                      async_search_runner const& search) -> motif::slint_app::workspace_activity
{
    return motif::slint_app::workspace_activity {.import_active = importer.active(),
                                                 .workspace_busy = runner.is_busy(),
                                                 .browser_busy = browser.is_busy(),
                                                 .board_busy = board.is_busy(),
                                                 .search_busy = search.is_busy()};
}

// Every way of leaving the current database or opening another. Each waits for
// the replace confirmation when an edited or unsaved game is on the board.
void register_database_open_callbacks(WorkspaceWindow& window,
                                      motif::slint_app::import_service const& importer,
                                      async_workspace_runner& runner,
                                      async_browser_runner& browser,
                                      async_board_runner& board,
                                      async_search_runner& search,
                                      replace_confirmation& confirmation)
{
    window.on_create_requested(
        [&](slint::SharedString const& dir_path, slint::SharedString const& name) -> void
        {
            if (importer.active() || browser.is_busy() || board.is_busy() || search.is_busy()) {
                return;
            }
            confirmation.request("Create database",
                                 [&, dir_path, name]() -> void
                                 {
                                     window.set_error_text("");
                                     auto const trimmed_path = trim_whitespace(std::string_view {dir_path.data(), dir_path.size()});
                                     auto const trimmed_name = trim_whitespace(std::string_view {name.data(), name.size()});
                                     window.set_create_path_text(shared_string(trimmed_path));
                                     window.set_create_name_text(shared_string(trimmed_name));
                                     if (runner.run_create(std::string {trimmed_path}, std::string {trimmed_name})
                                         == async_workspace_runner::start_outcome::accepted)
                                     {
                                         window.set_busy(true);
                                     }
                                 });
        });
    window.on_open_requested(
        [&](slint::SharedString const& dir_path) -> void
        {
            if (importer.active() || browser.is_busy() || board.is_busy() || search.is_busy()) {
                return;
            }
            confirmation.request("Open database",
                                 [&, dir_path]() -> void
                                 {
                                     window.set_error_text("");
                                     auto const trimmed_path = trim_whitespace(std::string_view {dir_path.data(), dir_path.size()});
                                     window.set_open_path_text(shared_string(trimmed_path));
                                     if (runner.run_open(std::string {trimmed_path}) == async_workspace_runner::start_outcome::accepted) {
                                         window.set_busy(true);
                                     }
                                 });
        });
    window.on_recent_activated(
        [&](slint::SharedString const& path) -> void
        {
            if (importer.active() || browser.is_busy() || board.is_busy() || search.is_busy()) {
                return;
            }
            confirmation.request("Open database",
                                 [&, path]() -> void
                                 {
                                     window.set_error_text("");
                                     auto const trimmed_path = trim_whitespace(std::string_view {path.data(), path.size()});
                                     if (runner.run_open(std::string {trimmed_path}) == async_workspace_runner::start_outcome::accepted) {
                                         window.set_busy(true);
                                     }
                                 });
        });
}

// Scratch and close: the other ways of leaving the current database.
void register_database_exit_callbacks(WorkspaceWindow& window,
                                      motif::slint_app::workspace_service& service,
                                      motif::slint_app::import_service const& importer,
                                      async_workspace_runner& runner,
                                      async_browser_runner& browser,
                                      async_board_runner& board,
                                      async_search_runner& search,
                                      replace_confirmation& confirmation)
{
    window.on_scratch_requested(
        [&]() -> void
        {
            if (runner.is_busy() || browser.is_busy() || board.is_busy() || search.is_busy() || importer.active()) {
                return;
            }
            confirmation.request("Open scratch database",
                                 [&]() -> void
                                 {
                                     window.set_error_text("");
                                     if (runner.run_scratch() == async_workspace_runner::start_outcome::accepted) {
                                         window.set_busy(true);
                                     }
                                 });
        });
    window.on_close_requested(
        [&]() -> void
        {
            if (runner.is_busy() || browser.is_busy() || board.is_busy() || search.is_busy() || importer.active()) {
                return;
            }
            confirmation.request("Close database",
                                 [&]() -> void
                                 {
                                     browser.detach();
                                     board.detach();
                                     search.detach();
                                     service.close_active();
                                     window.set_error_text("");
                                     publish_workspace(window, service);
                                 });
        });
}

void register_workspace_callbacks(WorkspaceWindow& window,
                                  motif::slint_app::workspace_service& service,
                                  async_workspace_runner const& runner,
                                  async_browser_runner& browser,
                                  async_board_runner& board,
                                  async_search_runner& search)
{
    window.on_recent_removed(
        [&](slint::SharedString const& path) -> void
        {
            // The worker thread mutates the same config; wait until it is done.
            if (runner.is_busy()) {
                return;
            }
            if (auto removed = service.remove_recent(std::string {std::string_view {path.data(), path.size()}}); !removed) {
                window.set_error_text(shared_string(service.error_message()));
                return;
            }
            window.set_error_text("");
            publish_workspace(window, service);
        });
    window.on_dismiss_error(
        [&]() -> void
        {
            browser.dismiss_error();
            board.dismiss_error();
            search.dismiss_error();
            window.set_error_text("");
        });
    window.on_preset_selected(
        [&window, &service](WorkspacePreset const preset) -> void
        {
            // The toolbar already switched the layout. If the choice cannot be
            // saved, roll back to the stored preset so the window never shows a
            // state that a later publish_workspace would silently revert.
            if (auto saved = service.set_preset(from_ui_preset(preset)); !saved) {
                window.set_workspace_preset(to_ui_preset(service.preset()));
                window.set_error_text("Could not save workspace preset preference.");
            }
        });
    window.on_notation_layout_toggled(
        [&window, &service]() -> void
        {
            auto const notation_below = !window.get_notation_below();
            if (auto saved = service.set_notation_below(notation_below); !saved) {
                window.set_error_text("Could not save workspace layout preference.");
                return;
            }
            window.set_notation_below(notation_below);
        });
}

void register_browser_callbacks(WorkspaceWindow& window,
                                async_browser_runner& browser,
                                motif::slint_app::import_service const& importer,
                                async_workspace_runner const& runner,
                                async_board_runner& board,
                                async_search_runner& search,
                                replace_confirmation& confirmation)
{
    window.on_game_filters_changed(
        [&](slint::SharedString const& player, slint::SharedString const& result) -> void
        {
            if (importer.active() || runner.is_busy() || browser.is_busy() || board.is_busy() || search.is_busy()) {
                return;
            }
            browser.set_filters(std::string {player}, std::string {result});
            // Filter refresh follows the auto-search preference: with
            // auto-search off, only a manual Search re-queries.
            search.refresh_if_auto(board.current_hash(), browser.active_filter());
        });
    window.on_game_page_requested(
        [&](std::int32_t page) -> void
        {
            if (importer.active() || runner.is_busy() || browser.is_busy() || board.is_busy() || search.is_busy() || page < 0) {
                return;
            }
            browser.set_page(static_cast<std::size_t>(page));
        });
    window.on_game_selected(
        [&](std::int32_t row) -> void
        {
            if (importer.active() || runner.is_busy() || browser.is_busy() || board.is_busy() || search.is_busy() || row < 0) {
                return;
            }
            browser.select(static_cast<std::size_t>(row));
        });
    window.on_game_move_selection(
        [&](std::int32_t delta) -> void
        {
            if (importer.active() || runner.is_busy() || browser.is_busy() || board.is_busy() || search.is_busy()) {
                return;
            }
            browser.move_selection(delta);
        });
    window.on_game_activate_requested(
        [&]() -> void
        {
            (void)motif::slint_app::run_when_idle(current_activity(importer, runner, browser, board, search),
                                                  [&]() -> void
                                                  {
                                                      confirmation.request(
                                                          "Load game",
                                                          [&]() -> void
                                                          {
                                                              (void)motif::slint_app::run_when_idle(
                                                                  current_activity(importer, runner, browser, board, search),
                                                                  [&]() -> void { browser.activate(); });
                                                          });
                                                  });
        });
    window.on_game_sort_requested(
        [&](std::int32_t column, bool ascending) -> void
        {
            if (importer.active() || runner.is_busy() || browser.is_busy() || board.is_busy() || search.is_busy() || column < 0) {
                return;
            }
            browser.sort(static_cast<std::size_t>(column), ascending);
        });
    window.on_game_column_resized(
        [&](std::int32_t column, std::int32_t width) -> void
        {
            if (importer.active() || runner.is_busy() || browser.is_busy() || board.is_busy() || search.is_busy() || column < 0) {
                return;
            }
            browser.resize_column(static_cast<std::size_t>(column), width);
        });
}

void register_search_callbacks(WorkspaceWindow& window,
                               async_search_runner& search,
                               async_browser_runner& browser,
                               motif::slint_app::import_service const& importer,
                               async_workspace_runner const& runner,
                               async_board_runner const& board,
                               replace_confirmation& confirmation)
{
    window.on_search_requested(
        [&]() -> void
        {
            if (importer.active() || runner.is_busy() || browser.is_busy() || board.is_busy() || search.is_busy()) {
                return;
            }
            search.search(board.current_hash(), browser.active_filter());
        });
    window.on_search_auto_search_toggled(
        [&](bool enabled) -> void
        {
            if (importer.active() || runner.is_busy() || browser.is_busy() || board.is_busy() || search.is_busy()) {
                // Republish so the checkbox snaps back to the stored
                // preference instead of visually desyncing until the next
                // unrelated publish.
                search.publish_auto_search();
                return;
            }
            search.set_auto_search(enabled);
            if (enabled) {
                search.search_if_hash_changed(board.current_hash(), browser.active_filter());
            }
        });
    window.on_search_page_requested(
        [&](std::int32_t page) -> void
        {
            if (importer.active() || runner.is_busy() || browser.is_busy() || board.is_busy() || search.is_busy() || page < 0) {
                return;
            }
            search.set_page(static_cast<std::size_t>(page));
        });
    window.on_search_match_activated(
        [&](std::int32_t row) -> void
        {
            if (row < 0) {
                return;
            }
            (void)motif::slint_app::run_when_idle(current_activity(importer, runner, browser, board, search),
                                                  [&]() -> void
                                                  {
                                                      if (auto const match = search.match_at(static_cast<std::size_t>(row))) {
                                                          confirmation.request(
                                                              "Load game",
                                                              [&, match = *match]() -> void
                                                              {
                                                                  (void)motif::slint_app::run_when_idle(
                                                                      current_activity(importer, runner, browser, board, search),
                                                                      [&]() -> void { browser.activate_by_id(match.first, match.second); });
                                                              });
                                                      }
                                                  });
        });
}

struct import_context
{
    WorkspaceWindow& window;
    motif::slint_app::workspace_service& service;
    motif::slint_app::import_service& importer;
    async_workspace_runner const& runner;
    async_browser_runner& browser;
    async_board_runner const& board;
    async_search_runner& search;
    motif::slint_app::pgn_launch_queue& launch_queue;
    slint::Timer& poll_timer;
};

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
// database is open and every background task is idle, then starts the next
// import. This is the same state the Import PGN button requires.
void pump_launch_queue(import_context& context, slint::Timer& launch_timer)
{
    if (context.launch_queue.empty()) {
        launch_timer.stop();
        return;
    }
    if (context.runner.is_busy() || context.browser.is_busy() || context.board.is_busy() || context.search.is_busy()
        || context.importer.active() || context.poll_timer.running() || !context.service.has_active())
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
            if (context.runner.is_busy() || context.browser.is_busy() || context.board.is_busy() || context.search.is_busy()) {
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
    auto browser = async_browser_runner {window};
    auto board = async_board_runner {window};
    auto search = async_search_runner {window};
    auto activation = motif::slint_app::make_board_activation_handlers(slint::ComponentWeakHandle<WorkspaceWindow> {window}, board);
    browser.set_activation_handlers(activation.loaded, activation.failed);
    browser.set_external_activation_handlers(activation.loaded_at, activation.failed_at);
    board.set_position_changed_handler([&search, &browser](motif::db::zobrist_hash const hash) -> void
                                       { search.search_if_hash_changed(hash, browser.active_filter()); });
    auto runner = async_workspace_runner {service, window, browser, board, search};
    search.set_retry_on_database_failure([&importer]() -> bool { return importer.active(); });
    auto import_timer = slint::Timer {};
    auto launch_arguments_list = launch_arguments(std::span<char* const> {argv, static_cast<std::size_t>(argc)});
    auto launch_queue = motif::slint_app::pgn_launch_queue::from_arguments(launch_arguments_list);
    auto import_context_value = import_context {*window, service, importer, runner, browser, board, search, launch_queue, import_timer};
    auto launch_timer = slint::Timer {};
    auto confirmation = motif::slint_app::make_replace_confirmation(window, board);
    auto const activity = motif::slint_app::activity_probe {[&]() -> motif::slint_app::workspace_activity
                                                            { return current_activity(importer, runner, browser, board, search); }};

    clear_browser(*window);
    motif::slint_app::clear_board(*window);
    clear_search(*window);
    publish_workspace(*window, service);
    register_workspace_callbacks(*window, service, runner, browser, board, search);
    register_database_open_callbacks(*window, importer, runner, browser, board, search, confirmation);
    register_database_exit_callbacks(*window, service, importer, runner, browser, board, search, confirmation);
    register_browser_callbacks(*window, browser, importer, runner, board, search, confirmation);
    motif::slint_app::register_board_callbacks(*window, board, activity);
    motif::slint_app::register_play_callbacks(*window, board, activity, confirmation);
    register_search_callbacks(*window, search, browser, importer, runner, board, confirmation);
    register_import_callbacks(import_context_value);

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
    return 0;
}
