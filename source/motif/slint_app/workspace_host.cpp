#include <chrono>
#include <thread>
#include <utility>
#include <vector>

#include "motif/slint_app/workspace_host.hpp"

#include "motif/slint_app/analysis_host.hpp"
#include "motif/slint_app/board_host.hpp"
#include "motif/slint_app/browser_host.hpp"
#include "motif/slint_app/import_service.hpp"
#include "motif/slint_app/search_host.hpp"

namespace motif::slint_app
{
namespace
{

auto shared_string(std::string_view value) -> slint::SharedString
{
    return slint::SharedString {value};
}

auto to_ui_preset(workspace_preset_kind const kind) -> WorkspacePreset
{
    switch (kind) {
        case workspace_preset_kind::explorer:
            return WorkspacePreset::Explorer;
        case workspace_preset_kind::analysis:
            return WorkspacePreset::Analysis;
        case workspace_preset_kind::review:
            break;
    }
    return WorkspacePreset::Review;
}

auto from_ui_preset(WorkspacePreset const preset) -> workspace_preset_kind
{
    switch (preset) {
        case WorkspacePreset::Explorer:
            return workspace_preset_kind::explorer;
        case WorkspacePreset::Analysis:
            return workspace_preset_kind::analysis;
        case WorkspacePreset::Review:
            break;
    }
    return workspace_preset_kind::review;
}

auto component_matches(slint::ComponentWeakHandle<WorkspaceWindow> const& window,
                       std::shared_ptr<std::atomic<std::uint64_t>> const& lifetime,
                       std::uint64_t lifetime_token) -> bool
{
    return lifetime->load(std::memory_order_acquire) == lifetime_token && window.lock().has_value();
}

}  // namespace

auto current_activity(import_service const& importer,
                      async_workspace_runner const& workspace,
                      async_browser_runner const& browser,
                      async_search_runner const& search) -> workspace_activity
{
    return workspace_activity {.import_active = importer.active(),
                               .workspace_busy = workspace.is_busy(),
                               .browser_busy = browser.is_busy(),
                               .search_busy = search.is_busy()};
}

void publish_workspace(WorkspaceWindow& window, workspace_service const& service)
{
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

struct async_workspace_runner::state
{
    state(workspace_service& service_value,
          slint::ComponentWeakHandle<WorkspaceWindow> window_value,
          async_browser_runner& browser_value,
          async_board_runner& board_value,
          async_search_runner& search_value,
          async_analysis_runner& analysis_value,
          event_loop_environment environment_value)
        : service(service_value)
        , window(std::move(window_value))
        , browser(browser_value)
        , board(board_value)
        , search(search_value)
        , analysis(analysis_value)
        , environment(std::move(environment_value))
    {
    }

    workspace_service& service;
    slint::ComponentWeakHandle<WorkspaceWindow> window;
    async_browser_runner& browser;
    async_board_runner& board;
    async_search_runner& search;
    async_analysis_runner& analysis;
    event_loop_environment environment;
    attach_observer observer;
    std::thread worker;
    std::atomic<bool> busy {false};
    std::shared_ptr<std::atomic<std::uint64_t>> lifetime {std::make_shared<std::atomic<std::uint64_t>>(0)};
};

async_workspace_runner::async_workspace_runner(workspace_service& service,
                                               slint::ComponentWeakHandle<WorkspaceWindow> window,
                                               async_browser_runner& browser,
                                               async_board_runner& board,
                                               async_search_runner& search,
                                               async_analysis_runner& analysis,
                                               event_loop_environment environment,
                                               attach_observer observer)
    : state_(std::make_shared<state>(service, std::move(window), browser, board, search, analysis, std::move(environment)))
{
    state_->observer = std::move(observer);
}

async_workspace_runner::~async_workspace_runner()
{
    state_->lifetime->fetch_add(1, std::memory_order_acq_rel);
    if (state_->worker.joinable()) {
        state_->worker.join();
    }
}

auto async_workspace_runner::run_create(std::string dir_path, std::string name) -> start_outcome
{
    return start([service = &state_->service, dir_path = std::move(dir_path), name = std::move(name)]() mutable -> result<void>
                 { return service->create_database(dir_path, name); });
}

auto async_workspace_runner::run_open(std::string dir_path) -> start_outcome
{
    return start([service = &state_->service, dir_path = std::move(dir_path)]() mutable -> result<void>
                 { return service->open_database(dir_path); });
}

auto async_workspace_runner::run_scratch() -> start_outcome
{
    return start([service = &state_->service]() -> result<void> { return service->activate_scratch(); });
}

auto async_workspace_runner::is_busy() const noexcept -> bool
{
    return state_->busy.load(std::memory_order_acquire);
}

void async_workspace_runner::notify_attach(state& shared, workspace_attach_step const step)
{
    if (shared.observer) {
        shared.observer(step);
    }
}

void async_workspace_runner::attach_hosts(state& shared)
{
    auto* database = shared.service.active_database();
    if (database == nullptr) {
        return;
    }
    shared.analysis.workspace_changed();
    notify_attach(shared, workspace_attach_step::analysis_changed);
    shared.browser.attach(*database);
    notify_attach(shared, workspace_attach_step::browser_attached);
    shared.browser.load_initial();
    notify_attach(shared, workspace_attach_step::browser_loaded);
    shared.search.attach(*database);
    notify_attach(shared, workspace_attach_step::search_attached);
    shared.board.attach();
    notify_attach(shared, workspace_attach_step::board_attached);
}

auto async_workspace_runner::start(std::function<result<void>()> operation) -> start_outcome
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
    auto const environment = state_->environment;
    state_->worker = std::thread(
        [shared = state_, operation = std::move(operation), lifetime, lifetime_token, window, environment]() mutable -> void
        {
            auto result = operation();
            environment.schedule(
                std::chrono::milliseconds {0},
                [shared, result, lifetime, lifetime_token, window]() mutable -> void
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
                            attach_hosts(*shared);
                        }
                    }
                    shared->busy.store(false, std::memory_order_release);
                });
        });
    return start_outcome::accepted;
}

void register_workspace_callbacks(WorkspaceWindow& window,
                                  workspace_service& service,
                                  async_workspace_runner const& runner,
                                  async_browser_runner& browser,
                                  async_board_runner& board,
                                  async_search_runner& search)
{
    window.on_recent_removed(
        [&](slint::SharedString const& path) -> void
        {
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

}  // namespace motif::slint_app
