#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "motif/slint_app/search_host.hpp"

#include <fmt/format.h>

#include "motif/db/query_connection.hpp"
#include "motif/slint_app/board_host.hpp"
#include "motif/slint_app/browser_host.hpp"
#include "motif/slint_app/explorer_table.hpp"

namespace motif::slint_app
{
namespace
{

auto shared_string(std::string_view value) -> slint::SharedString
{
    return slint::SharedString {value};
}

auto to_ui_count(std::size_t count) -> int
{
    constexpr auto maximum = static_cast<std::size_t>(std::numeric_limits<int>::max());
    return static_cast<int>(std::min(count, maximum));
}

auto elo_text(std::optional<std::int32_t> const elo) -> std::string
{
    return elo ? fmt::format("{}", *elo) : std::string {};
}

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

auto component_matches(slint::ComponentWeakHandle<WorkspaceWindow> const& window,
                       std::shared_ptr<std::atomic<std::uint64_t>> const& lifetime,
                       std::uint64_t lifetime_token) -> bool
{
    return lifetime->load(std::memory_order_acquire) == lifetime_token && window.lock().has_value();
}

}  // namespace

void clear_search(WorkspaceWindow& window)
{
    window.set_search_matches(std::make_shared<slint::VectorModel<SearchMatchRow>>());
    window.set_search_total_matches(0);
    window.set_search_page_index(0);
    window.set_search_has_previous_page(false);
    window.set_search_has_next_page(false);
    publish_explorer(window, explorer_table {});
    window.set_search_rows_stale(false);
    window.set_search_total_games(0);
    window.set_search_active_filter_summary("");
    window.set_search_searching(false);
    window.set_search_busy(false);
}

void publish_search(WorkspaceWindow& window, search_state const& state, bool const rows_stale)
{
    auto matches = std::vector<SearchMatchRow> {};
    matches.reserve(state.matches.size());
    for (auto const& match : state.matches) {
        matches.push_back(SearchMatchRow {.white = shared_string(match.game.white),
                                          .black = shared_string(match.game.black),
                                          .result = shared_string(match.game.result),
                                          .event = shared_string(match.game.event),
                                          .date = shared_string(match.game.date),
                                          .eco = shared_string(match.game.eco),
                                          .white_elo = shared_string(elo_text(match.game.white_elo)),
                                          .black_elo = shared_string(elo_text(match.game.black_elo)),
                                          .ply = to_ui_count(static_cast<std::size_t>(match.ply))});
    }
    window.set_search_matches(std::make_shared<slint::VectorModel<SearchMatchRow>>(std::move(matches)));
    window.set_search_total_matches(to_ui_count(state.total_matches));
    window.set_search_page_index(to_ui_count(state.page_index));
    window.set_search_has_previous_page(state.has_previous_page);
    window.set_search_has_next_page(state.has_next_page);
    publish_explorer(window, make_explorer_table(state.continuations, state.total_games));
    window.set_search_rows_stale(rows_stale);
    window.set_search_total_games(to_ui_count(state.total_games));
    window.set_search_active_filter_summary(shared_string(filter_summary(state.active_filter)));
    window.set_search_searching(state.searching);
    window.set_search_auto_search(state.auto_search);
    window.set_search_error_text(shared_string(state.error_text));
}

struct async_search_runner::state
{
    state(slint::ComponentWeakHandle<WorkspaceWindow> window_value, event_loop_environment environment_value)
        : window(std::move(window_value))
        , environment(std::move(environment_value))
    {
    }

    slint::ComponentWeakHandle<WorkspaceWindow> window;
    event_loop_environment environment;
    std::shared_ptr<search_presenter> presenter;
    std::mutex worker_mutex;
    std::mutex connection_mutex;
    std::thread worker;
    std::shared_ptr<motif::db::query_connection> active_connection;
    std::shared_ptr<std::atomic<bool>> active_cancellation;
    std::atomic<bool> busy {false};
    std::optional<search_query_result> pending_query;
    explorer_rows_tracker rows;
    std::uint64_t epoch {};
    std::shared_ptr<std::atomic<std::uint64_t>> lifetime {std::make_shared<std::atomic<std::uint64_t>>(0)};
    std::function<bool()> retry_on_database_failure;

    void cancel_worker() noexcept
    {
        std::scoped_lock const lock {connection_mutex};
        if (active_cancellation) {
            active_cancellation->store(true, std::memory_order_release);
        }
        if (active_connection) {
            active_connection->interrupt();
        }
        motif::db::notify_query_connection_open_gate_cancellation_for_tests();
    }

    static auto publish_connection(std::shared_ptr<state> const& shared,
                                   std::shared_ptr<std::atomic<bool>> const& cancellation,
                                   std::shared_ptr<motif::db::query_connection> const& connection) -> bool
    {
        std::scoped_lock const lock {shared->connection_mutex};
        shared->active_cancellation = cancellation;
        shared->active_connection = connection;
        if (cancellation->load(std::memory_order_acquire)) {
            connection->interrupt();
            return false;
        }
        return true;
    }

    static void clear_connection(std::shared_ptr<state> const& shared, std::shared_ptr<std::atomic<bool>> const& cancellation)
    {
        std::scoped_lock const lock {shared->connection_mutex};
        if (shared->active_cancellation == cancellation) {
            shared->active_connection.reset();
            shared->active_cancellation.reset();
        }
    }
};

struct async_search_runner::completion_key
{
    std::uint64_t epoch;
    std::uint64_t generation;
};

async_search_runner::async_search_runner(slint::ComponentWeakHandle<WorkspaceWindow> window, event_loop_environment environment)
    : state_(std::make_shared<state>(std::move(window), std::move(environment)))
{
}

async_search_runner::~async_search_runner()
{
    state_->lifetime->fetch_add(1, std::memory_order_acq_rel);
    state_->cancel_worker();
    std::scoped_lock const lock {state_->worker_mutex};
    if (state_->worker.joinable()) {
        state_->worker.join();
    }
}

void async_search_runner::set_retry_on_database_failure(std::function<bool()> retry_condition)
{
    state_->retry_on_database_failure = std::move(retry_condition);
}

void async_search_runner::attach(motif::db::database_manager& database)
{
    ++state_->epoch;
    state_->presenter = std::make_shared<search_presenter>(database);
    state_->pending_query.reset();
    state_->rows.database_attached();
    publish();
}

void async_search_runner::detach()
{
    state_->lifetime->fetch_add(1, std::memory_order_acq_rel);
    state_->cancel_worker();
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
    state_->rows.database_detached();
    if (auto locked = state_->window.lock()) {
        (*locked)->set_search_busy(false);
        clear_search(**locked);
    }
}

void async_search_runner::invalidate_position_cache()
{
    if (state_->presenter) {
        state_->presenter->invalidate_starting_position_cache();
    }
}

auto async_search_runner::is_busy() const noexcept -> bool
{
    return state_->busy.load(std::memory_order_acquire);
}

auto async_search_runner::shows_position(motif::db::zobrist_hash const hash) const -> bool
{
    return state_->presenter && state_->rows.current(state_->presenter->state().searching, state_->presenter->state().current_hash, hash);
}

void async_search_runner::note_board_position(motif::db::zobrist_hash const hash)
{
    state_->rows.board_hash = hash;
    if (auto locked = state_->window.lock()) {
        (*locked)->set_search_rows_stale(rows_stale(*state_));
    }
}

void async_search_runner::search(motif::db::zobrist_hash const hash, motif::db::search_filter const& filter)
{
    if (!state_->presenter) {
        return;
    }
    state_->rows.last_hash = hash;
    state_->rows.outdated = false;
    auto query = state_->presenter->search(hash, filter);
    publish();
    if (!query) {
        run_query(state_, search_query_result {tl::unexpected {query.error()}}, state_->epoch);
        return;
    }
    auto& dispatchable = *query;
    if (!dispatchable) {
        return;
    }
    run_query(state_, search_query_result {std::move(*dispatchable)}, state_->epoch);
}

void async_search_runner::search(std::optional<motif::db::zobrist_hash> const& hash, motif::db::search_filter const& filter)
{
    if (hash) {
        search(*hash, filter);
    }
}

void async_search_runner::search_if_hash_changed(motif::db::zobrist_hash const hash, motif::db::search_filter const& filter)
{
    if (!state_->presenter || !state_->presenter->state().auto_search) {
        return;
    }
    if (state_->rows.last_hash && *state_->rows.last_hash == hash && !state_->rows.outdated) {
        return;
    }
    search(hash, filter);
}

void async_search_runner::search_if_hash_changed(std::optional<motif::db::zobrist_hash> const& hash, motif::db::search_filter const& filter)
{
    if (hash) {
        search_if_hash_changed(*hash, filter);
    }
}

void async_search_runner::set_page(std::size_t const page_index)
{
    if (!state_->presenter || state_->presenter->state().searching) {
        return;
    }
    auto filter = state_->presenter->state().active_filter;
    filter.offset = page_index * filter.limit;
    search(state_->presenter->state().current_hash, filter);
}

void async_search_runner::refresh_if_auto(std::optional<motif::db::zobrist_hash> const& hash, motif::db::search_filter const& filter)
{
    if (!state_->presenter || !hash) {
        return;
    }
    if (state_->presenter->state().auto_search) {
        search(*hash, filter);
        return;
    }
    state_->rows.outdated = true;
    if (auto locked = state_->window.lock()) {
        (*locked)->set_search_rows_stale(rows_stale(*state_));
    }
}

void async_search_runner::set_auto_search(bool const enabled)
{
    if (state_->presenter) {
        state_->presenter->set_auto_search(enabled);
        publish();
    }
}

void async_search_runner::publish_auto_search()
{
    publish();
}

auto async_search_runner::match_at(std::size_t const row) const -> std::optional<std::pair<motif::db::game_id, std::size_t>>
{
    if (!state_->presenter || row >= state_->presenter->state().matches.size()) {
        return std::nullopt;
    }
    auto const& match = state_->presenter->state().matches[row];
    return std::make_pair(match.game.id, static_cast<std::size_t>(match.ply));
}

void async_search_runner::dismiss_error()
{
    if (state_->presenter) {
        state_->presenter->dismiss_error();
        publish();
    }
}

auto async_search_runner::rows_stale(state const& shared) -> bool
{
    return shared.presenter && shared.rows.stale(shared.presenter->state().searching, shared.presenter->state().current_hash);
}

void async_search_runner::publish()
{
    if (state_->presenter) {
        if (auto locked = state_->window.lock()) {
            publish_search(**locked, state_->presenter->state(), rows_stale(*state_));
        }
    }
}

void async_search_runner::dispatch_pending(std::shared_ptr<state> const& shared)
{
    if (shared->pending_query) {
        auto query = std::move(*shared->pending_query);
        shared->pending_query.reset();
        run_query(shared, std::move(query), shared->epoch);
    }
}

void async_search_runner::complete_query(std::shared_ptr<state> const& shared, completion_key const key, search_page_result result)
{
    if (shared->epoch == key.epoch && shared->presenter) {
        if (result) {
            (void)shared->presenter->apply_query(std::move(*result));
        } else {
            (void)shared->presenter->apply_query_error(key.generation, result.error());
        }
        if (auto locked = shared->window.lock()) {
            publish_search(**locked, shared->presenter->state(), rows_stale(*shared));
        }
    }
    shared->busy.store(false, std::memory_order_release);
    if (auto locked = shared->window.lock()) {
        (*locked)->set_search_busy(false);
    }
    dispatch_pending(shared);
}

void async_search_runner::run_query(std::shared_ptr<state> const& shared, search_query_result query, std::uint64_t const epoch)
{
    if (!query) {
        if (shared->presenter) {
            if (auto locked = shared->window.lock()) {
                publish_search(**locked, shared->presenter->state(), rows_stale(*shared));
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
    auto const environment = shared->environment;
    auto cancellation = std::make_shared<std::atomic<bool>>(false);
    {
        std::scoped_lock const connection_lock {shared->connection_mutex};
        shared->active_cancellation = cancellation;
    }
    std::scoped_lock const lock {shared->worker_mutex};
    if (shared->worker.joinable()) {
        shared->worker.join();
    }
    shared->worker = std::thread(
        [shared,
         presenter = std::move(presenter),
         query = std::move(*query),
         epoch,
         lifetime,
         lifetime_token,
         window,
         environment,
         cancellation]() mutable -> void
        {
            auto connection_result = motif::db::query_connection::open(presenter->database_dir(), cancellation);
            if (!connection_result) {
                auto result = search_page_result {
                    tl::unexpected {search_error {.code = error_code::database_failure, .message = "Could not open search metadata"}}};
                state::clear_connection(shared, cancellation);
                environment.schedule(
                    std::chrono::milliseconds {0},
                    [shared, epoch, generation = query.generation, result = std::move(result), lifetime, lifetime_token, window]() mutable
                        -> void
                    {
                        if (component_matches(window, lifetime, lifetime_token)) {
                            async_search_runner::complete_query(
                                shared, completion_key {.epoch = epoch, .generation = generation}, std::move(result));
                        }
                    });
                return;
            }
            auto connection = std::make_shared<motif::db::query_connection>(std::move(*connection_result));
            if (!state::publish_connection(shared, cancellation, connection)) {
                state::clear_connection(shared, cancellation);
                return;
            }
            auto result = presenter->execute_query(query, *connection);
            if (!connection->cancelled() && !result && result.error().code == error_code::database_failure
                && shared->retry_on_database_failure && shared->retry_on_database_failure())
            {
                result = presenter->execute_query(query, *connection);
            }
            state::clear_connection(shared, cancellation);
            environment.schedule(
                std::chrono::milliseconds {0},
                [shared, epoch, generation = query.generation, result = std::move(result), lifetime, lifetime_token, window]() mutable
                    -> void
                {
                    if (!component_matches(window, lifetime, lifetime_token)) {
                        return;
                    }
                    async_search_runner::complete_query(
                        shared, completion_key {.epoch = epoch, .generation = generation}, std::move(result));
                });
        });
}

void register_search_callbacks(WorkspaceWindow& window,
                               async_search_runner& search,
                               async_browser_runner& browser,
                               async_board_runner const& board,
                               activity_probe const& activity,
                               replace_controller& confirmation)
{
    window.on_search_requested(
        [&, activity]() -> void
        { (void)run_when_idle(activity(), [&]() -> void { search.search(board.current_hash(), browser.active_filter()); }); });
    window.on_search_auto_search_toggled(
        [&, activity](bool const enabled) -> void
        {
            if (!run_when_idle(activity(),
                               [&]() -> void
                               {
                                   search.set_auto_search(enabled);
                                   if (enabled) {
                                       search.search_if_hash_changed(board.current_hash(), browser.active_filter());
                                   }
                               }))
            {
                search.publish_auto_search();
            }
        });
    window.on_search_page_requested(
        [&, activity](std::int32_t const page) -> void
        {
            if (page >= 0) {
                (void)run_when_idle(activity(), [&]() -> void { search.set_page(static_cast<std::size_t>(page)); });
            }
        });
    window.on_search_match_activated(
        [&, activity](std::int32_t const row) -> void
        {
            if (row < 0) {
                return;
            }
            (void)run_when_idle(
                activity(),
                [&]() -> void
                {
                    if (auto const match = search.match_at(static_cast<std::size_t>(row))) {
                        confirmation.request(
                            "Load game",
                            [&, activity, match = *match]() -> void
                            { (void)run_when_idle(activity(), [&]() -> void { browser.activate_by_id(match.first, match.second); }); });
                    }
                });
        });
}

void follow_board_position(async_search_runner& search, async_browser_runner const& browser, motif::db::zobrist_hash const hash)
{
    search.note_board_position(hash);
    search.search_if_hash_changed(hash, browser.active_filter());
}

auto explorer_rows_current(async_board_runner const& board, async_search_runner const& search) -> bool
{
    auto const hash = board.current_hash();
    return hash && search.shows_position(*hash);
}

}  // namespace motif::slint_app
