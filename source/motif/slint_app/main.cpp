#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <slint.h>

#include "motif/slint_app/import_service.hpp"
#include "motif/slint_app/workspace_service.hpp"
#include "workspace.h"

namespace
{

using workspace_result = motif::slint_app::result<void>;

auto shared_string(std::string_view value) -> slint::SharedString
{
    return slint::SharedString {value};
}

void publish_state(WorkspaceWindow& window, motif::slint_app::workspace_service const& service)
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
    return "Unknown";
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
    return "Unknown";
}

auto to_ui_count(std::size_t count) -> int
{
    constexpr auto maximum = static_cast<std::size_t>(std::numeric_limits<int>::max());
    return static_cast<int>(std::min(count, maximum));
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

class async_workspace_runner
{
  public:
    enum class start_outcome : std::uint8_t
    {
        accepted,
        busy,
    };

    async_workspace_runner(motif::slint_app::workspace_service& service, slint::ComponentWeakHandle<WorkspaceWindow> window)
        : service_(service)
        , window_(std::move(window))
    {
    }

    ~async_workspace_runner()
    {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    async_workspace_runner(async_workspace_runner const&) = delete;
    auto operator=(async_workspace_runner const&) -> async_workspace_runner& = delete;
    async_workspace_runner(async_workspace_runner&&) = delete;
    auto operator=(async_workspace_runner&&) -> async_workspace_runner& = delete;

    [[nodiscard]] auto run_create(std::string dir_path, std::string name) -> start_outcome
    {
        return start([this, dir_path = std::move(dir_path), name = std::move(name)]() mutable -> workspace_result
                     { return service_.create_database(dir_path, name); });
    }

    [[nodiscard]] auto run_open(std::string dir_path) -> start_outcome
    {
        return start([this, dir_path = std::move(dir_path)]() mutable -> workspace_result { return service_.open_database(dir_path); });
    }

    [[nodiscard]] auto is_busy() const noexcept -> bool { return busy_.load(std::memory_order_acquire); }

  private:
    motif::slint_app::workspace_service& service_;
    slint::ComponentWeakHandle<WorkspaceWindow> window_;
    std::thread worker_;
    std::atomic<bool> busy_ {false};

    template<typename Operation>
    auto start(Operation operation) -> start_outcome
    {
        if (busy_.exchange(true, std::memory_order_acq_rel)) {
            return start_outcome::busy;
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        worker_ = std::thread(
            [this, operation = std::move(operation)]() mutable -> void
            {
                auto result = operation();
                slint::invoke_from_event_loop(
                    [this, result = std::move(result)]() mutable -> void
                    {
                        auto locked = window_.lock();
                        if (locked) {
                            auto& window = *locked;
                            window->set_error_text(shared_string(result ? std::string_view {} : service_.error_message()));
                            publish_state(*window, service_);
                            window->set_busy(false);
                        }
                        busy_.store(false, std::memory_order_release);
                    });
            });
        return start_outcome::accepted;
    }
};

void register_workspace_callbacks(WorkspaceWindow& window,
                                  motif::slint_app::workspace_service& service,
                                  motif::slint_app::import_service const& importer,
                                  async_workspace_runner& runner)
{
    window.on_create_requested(
        [&](slint::SharedString const& dir_path, slint::SharedString const& name) -> void
        {
            if (importer.active()) {
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
            if (importer.active()) {
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
            if (importer.active()) {
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
            if (runner.is_busy() || importer.active()) {
                return;
            }
            window.set_error_text("");
            if (auto result = service.activate_scratch(); !result) {
                window.set_error_text(shared_string(service.error_message()));
            }
            publish_state(window, service);
        });
    window.on_close_requested(
        [&]() -> void
        {
            if (runner.is_busy() || importer.active()) {
                return;
            }
            service.close_active();
            window.set_error_text("");
            publish_state(window, service);
        });
    window.on_dismiss_error([&]() -> void { window.set_error_text(""); });
}

void publish_import_start_error(WorkspaceWindow& window, motif::slint_app::import_error_code error)
{
    switch (error) {
        case motif::slint_app::import_error_code::invalid_argument:
            window.set_error_text("A PGN file path is required.");
            break;
        case motif::slint_app::import_error_code::no_database:
            window.set_error_text("Open a database before importing.");
            break;
        case motif::slint_app::import_error_code::busy:
            window.set_error_text("An import is already running.");
            break;
    }
}

void register_import_callbacks(WorkspaceWindow& window,
                               motif::slint_app::workspace_service& service,
                               motif::slint_app::import_service& importer,
                               async_workspace_runner const& runner,
                               slint::Timer& timer)
{
    constexpr auto poll_interval = std::chrono::milliseconds {200};
    window.on_import_requested(
        [&](slint::SharedString const& path) -> void
        {
            if (runner.is_busy()) {
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
    auto runner = async_workspace_runner {service, window};
    auto import_timer = slint::Timer {};

    publish_state(*window, service);
    register_workspace_callbacks(*window, service, importer, runner);
    register_import_callbacks(*window, service, importer, runner, import_timer);

    window->run();
    return 0;
}
