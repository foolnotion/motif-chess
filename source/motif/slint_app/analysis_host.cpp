#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "motif/slint_app/analysis_host.hpp"

#include <slint.h>

#include "motif/engine/engine_manager.hpp"
#include "motif/slint_app/analysis_presenter.hpp"
#include "motif/slint_app/analysis_service.hpp"
#include "motif/slint_app/config.hpp"
#include "motif/slint_app/workspace_service.hpp"
#include "workspace.h"

namespace motif::slint_app
{

namespace
{

auto shared_string(std::string_view value) -> slint::SharedString
{
    return slint::SharedString {value};
}

constexpr auto limit_kind_index(analysis_limit_kind const kind) -> int
{
    return kind == analysis_limit_kind::movetime ? 1 : 0;
}

constexpr auto limit_kind_from_index(int const index) -> analysis_limit_kind
{
    return index == 1 ? analysis_limit_kind::movetime : analysis_limit_kind::depth;
}

// Collects engine events from foreign threads until the event loop takes them.
// A newer info line for the same MultiPV rank replaces an older one that was
// never drawn, and events of older generations are discarded, so a burst of
// engine output costs one redraw. Exactly one flush is scheduled at a time.
class analysis_inbox
{
  public:
    void set_flush(std::function<void()> flush) { flush_ = std::move(flush); }

    void push(analysis_event event)
    {
        {
            std::scoped_lock const lock {mutex_};
            std::erase_if(pending_,
                          [&event](analysis_event const& queued) -> bool
                          {
                              if (queued.epoch != event.epoch) {
                                  return queued.epoch < event.epoch;
                              }
                              if (queued.generation != event.generation) {
                                  return queued.generation < event.generation;
                              }
                              auto const* const newer = std::get_if<motif::engine::info_event>(&event.payload);
                              auto const* const older = std::get_if<motif::engine::info_event>(&queued.payload);
                              return newer != nullptr && older != nullptr && newer->multipv == older->multipv;
                          });
            pending_.push_back(std::move(event));
            if (scheduled_) {
                return;
            }
            scheduled_ = true;
        }
        flush_();
    }

    auto take() -> std::vector<analysis_event>
    {
        std::scoped_lock const lock {mutex_};
        scheduled_ = false;
        return std::exchange(pending_, {});
    }

  private:
    std::function<void()> flush_;
    std::mutex mutex_;
    std::vector<analysis_event> pending_;
    bool scheduled_ {false};
};

}  // namespace

void publish_analysis(WorkspaceWindow& window, analysis_state const& state)
{
    auto names = std::vector<slint::SharedString> {};
    names.reserve(state.engines.size());
    for (auto const& engine : state.engines) {
        names.push_back(shared_string(engine.name));
    }
    window.set_analysis_engine_names(std::make_shared<slint::VectorModel<slint::SharedString>>(std::move(names)));
    window.set_analysis_selected_engine(state.selected_engine);
    window.set_analysis_multipv(state.multipv);
    window.set_analysis_limit_kind(limit_kind_index(state.limit));
    window.set_analysis_limit_value(state.limit == analysis_limit_kind::movetime ? state.movetime_ms : state.depth);
    window.set_analysis_active(state.active);
    window.set_analysis_status_text(shared_string(state.status_text));
    window.set_analysis_notice(shared_string(state.notice));
    window.set_analysis_lines_stale(state.lines_stale);
    window.set_analysis_can_start(state.can_start);

    auto lines = std::vector<AnalysisLine> {};
    lines.reserve(state.lines.size());
    for (auto const& line : state.lines) {
        lines.push_back(AnalysisLine {
            .rank = line.rank,
            .depth = line.depth,
            .score = shared_string(line.score),
            .moves = shared_string(line.moves),
            .accessible_label = shared_string(line.accessible_label),
        });
    }
    window.set_analysis_lines(std::make_shared<slint::VectorModel<AnalysisLine>>(std::move(lines)));
}

// ---------------------------------------------------------------------------
// async_analysis_runner
// ---------------------------------------------------------------------------

struct async_analysis_runner::impl
{
    impl(slint::ComponentWeakHandle<WorkspaceWindow> window_value,
         workspace_service& config_value,
         event_loop_environment environment_value,
         position_provider position_value)
        : window(std::move(window_value))
        , config(&config_value)
        , environment(std::move(environment_value))
        , position(std::move(position_value))
    {
    }

    slint::ComponentWeakHandle<WorkspaceWindow> window;
    workspace_service* config;
    event_loop_environment environment;
    position_provider position;
    analysis_presenter presenter;
    std::shared_ptr<analysis_inbox> inbox {std::make_shared<analysis_inbox>()};
    // Bumped when the runner is shut down: every task scheduled before that is dropped.
    std::shared_ptr<std::atomic<std::uint64_t>> lifetime {std::make_shared<std::atomic<std::uint64_t>>(0)};
    // Bumped when the workspace changes: events of the old workspace are dropped.
    std::uint64_t epoch {1};
    bool follow_scheduled {false};
    // Declared last: its destructor ends the engine threads that call into `inbox`.
    std::unique_ptr<analysis_service> service;

    void publish() const
    {
        if (auto locked = window.lock()) {
            publish_analysis(**locked, presenter.state());
        }
    }

    // Runs `task` on the event loop after `delay`, unless the runner was shut
    // down or destroyed by then.
    void schedule(std::weak_ptr<impl> const& self, std::chrono::milliseconds const delay, void (impl::*task)()) const
    {
        auto const guard = lifetime;
        auto const token = guard->load(std::memory_order_acquire);
        environment.schedule(delay,
                             [self, guard, token, task]() -> void
                             {
                                 if (guard->load(std::memory_order_acquire) != token) {
                                     return;
                                 }
                                 if (auto const locked = self.lock()) {
                                     ((*locked).*task)();
                                 }
                             });
    }

    void flush()
    {
        auto changed = false;
        auto ended = false;
        for (auto const& event : inbox->take()) {
            if (event.epoch != epoch) {
                continue;
            }
            auto const generation = event.generation;
            auto const applied = std::visit(
                [this, generation](auto const& payload) -> bool
                {
                    using payload_type = std::decay_t<decltype(payload)>;
                    if constexpr (std::is_same_v<payload_type, motif::engine::info_event>) {
                        return presenter.apply_info(generation, payload);
                    } else if constexpr (std::is_same_v<payload_type, motif::engine::complete_event>) {
                        return presenter.apply_complete(generation);
                    } else if constexpr (std::is_same_v<payload_type, motif::engine::error_event>) {
                        return presenter.apply_error(generation, payload.message);
                    } else {
                        return presenter.apply_start_failure(generation);
                    }
                },
                event.payload);
            changed = changed || applied;
            ended = ended || (applied && !std::holds_alternative<motif::engine::info_event>(event.payload));
        }
        if (ended) {
            // A finished, crashed or never started session has nothing left to
            // report: free its engine process now rather than at the next restart.
            service->stop();
        }
        if (changed) {
            publish();
        }
    }

    void follow()
    {
        follow_scheduled = false;
        if (!presenter.restart_pending()) {
            return;
        }
        auto const fen = position();
        if (!fen) {
            return;
        }
        if (auto request = presenter.restart(*fen)) {
            request->epoch = epoch;
            service->start(std::move(*request));
        }
        publish();
    }

    void persist_options()
    {
        auto const& state = presenter.state();
        if (!config->set_engine_options(
                engine_options {.multipv = state.multipv, .limit = state.limit, .depth = state.depth, .movetime_ms = state.movetime_ms}))
        {
            reload_after_failure();
        }
    }

    // A change that could not be saved is not adopted: show the stored state again.
    void reload_after_failure()
    {
        presenter.configure(config->engines());
        presenter.set_notice(std::string {config->error_message()});
    }
};

async_analysis_runner::async_analysis_runner(slint::ComponentWeakHandle<WorkspaceWindow> window,
                                             workspace_service& config,
                                             event_loop_environment environment,
                                             position_provider position)
    : impl_ {std::make_shared<impl>(std::move(window), config, std::move(environment), std::move(position))}
{
    auto const self = std::weak_ptr<impl> {impl_};
    impl_->inbox->set_flush(
        [self]() -> void
        {
            if (auto const locked = self.lock()) {
                locked->schedule(self, analysis_flush_interval, &impl::flush);
            }
        });
    impl_->service =
        std::make_unique<analysis_service>([inbox = impl_->inbox](analysis_event event) -> void { inbox->push(std::move(event)); });
}

async_analysis_runner::~async_analysis_runner()
{
    shutdown();
}

void async_analysis_runner::load()
{
    impl_->presenter.configure(impl_->config->engines());
    impl_->publish();
}

void async_analysis_runner::start()
{
    auto const fen = impl_->position();
    if (!fen) {
        impl_->presenter.set_notice("There is no position to analyse.");
        impl_->publish();
        return;
    }
    if (auto request = impl_->presenter.begin(*fen)) {
        request->epoch = impl_->epoch;
        impl_->service->start(std::move(*request));
    }
    impl_->publish();
}

void async_analysis_runner::stop()
{
    impl_->presenter.stop();
    impl_->service->stop();
    impl_->publish();
}

void async_analysis_runner::position_changed()
{
    auto const& presenter = impl_->presenter;
    if (!presenter.state().active) {
        return;
    }
    auto const fen = impl_->position();
    // The board publishes after every action, selections and flips included;
    // only a different position restarts the engine.
    if (!fen || (*fen == presenter.analysed_fen() && !presenter.restart_pending())) {
        return;
    }
    if (impl_->presenter.position_changed()) {
        impl_->service->stop();
        impl_->publish();
        if (!impl_->follow_scheduled) {
            impl_->follow_scheduled = true;
            impl_->schedule(impl_, analysis_follow_delay, &impl::follow);
        }
    }
}

void async_analysis_runner::workspace_changed()
{
    ++impl_->epoch;
    impl_->presenter.invalidate();
    impl_->service->stop();
    impl_->publish();
}

void async_analysis_runner::select_engine(int const index)
{
    if (auto const path = impl_->presenter.select_engine(index)) {
        if (!impl_->config->select_engine(*path)) {
            impl_->reload_after_failure();
        }
    }
    impl_->publish();
}

void async_analysis_runner::add_engine(std::string_view const path)
{
    if (impl_->presenter.locked()) {
        return;
    }
    if (!impl_->config->add_engine(path)) {
        impl_->presenter.set_notice(std::string {impl_->config->error_message()});
        impl_->publish();
        return;
    }
    impl_->presenter.configure(impl_->config->engines());
    if (auto const locked = impl_->window.lock()) {
        (*locked)->set_analysis_engine_path_text("");
    }
    impl_->publish();
}

void async_analysis_runner::remove_engine()
{
    auto const& state = impl_->presenter.state();
    if (impl_->presenter.locked() || state.selected_engine < 0) {
        return;
    }
    auto const path = state.engines[static_cast<std::size_t>(state.selected_engine)].path;
    if (!impl_->config->remove_engine(path)) {
        impl_->reload_after_failure();
    } else {
        impl_->presenter.configure(impl_->config->engines());
    }
    impl_->publish();
}

void async_analysis_runner::set_multipv(int const value)
{
    if (!impl_->presenter.locked()) {
        impl_->presenter.set_multipv(value);
        impl_->persist_options();
    }
    impl_->publish();
}

void async_analysis_runner::set_limit_kind(int const kind)
{
    if (!impl_->presenter.locked()) {
        impl_->presenter.set_limit(limit_kind_from_index(kind));
        impl_->persist_options();
    }
    impl_->publish();
}

void async_analysis_runner::set_limit_value(int const value)
{
    if (!impl_->presenter.locked()) {
        impl_->presenter.set_limit_value(value);
        impl_->persist_options();
    }
    impl_->publish();
}

void async_analysis_runner::notify(std::string message)
{
    impl_->presenter.set_notice(std::move(message));
    impl_->publish();
}

void async_analysis_runner::shutdown()
{
    impl_->lifetime->fetch_add(1, std::memory_order_acq_rel);
    impl_->service->shutdown();
}

void async_analysis_runner::wait_until_idle()
{
    impl_->service->wait_until_idle();
}

auto async_analysis_runner::state() const -> analysis_state const&
{
    return impl_->presenter.state();
}

void register_analysis_callbacks(WorkspaceWindow& window, async_analysis_runner& analysis, activity_probe const& activity)
{
    window.on_analysis_start_requested([&analysis]() -> void { analysis.start(); });
    window.on_analysis_stop_requested([&analysis]() -> void { analysis.stop(); });
    window.on_analysis_panel_left([&analysis]() -> void { analysis.stop(); });

    // The stored engine list and options belong to a running workspace
    // operation's worker thread until it finishes.
    window.on_analysis_engine_selected(
        [&analysis, activity](std::int32_t const index) -> void
        {
            if (activity().workspace_busy) {
                analysis.notify("Wait for the workspace to finish before changing engines.");
                return;
            }
            analysis.select_engine(index);
        });
    window.on_analysis_engine_added(
        [&analysis, activity](slint::SharedString const& path) -> void
        {
            if (activity().workspace_busy) {
                analysis.notify("Wait for the workspace to finish before changing engines.");
                return;
            }
            analysis.add_engine(std::string_view {path.data(), path.size()});
        });
    window.on_analysis_engine_removed(
        [&analysis, activity]() -> void
        {
            if (activity().workspace_busy) {
                analysis.notify("Wait for the workspace to finish before changing engines.");
                return;
            }
            analysis.remove_engine();
        });
    window.on_analysis_multipv_changed(
        [&analysis, activity](std::int32_t const value) -> void
        {
            if (activity().workspace_busy) {
                analysis.notify("Wait for the workspace to finish before changing engines.");
                return;
            }
            analysis.set_multipv(value);
        });
    window.on_analysis_limit_kind_changed(
        [&analysis, activity](std::int32_t const kind) -> void
        {
            if (activity().workspace_busy) {
                analysis.notify("Wait for the workspace to finish before changing engines.");
                return;
            }
            analysis.set_limit_kind(kind);
        });
    window.on_analysis_limit_value_changed(
        [&analysis, activity](std::int32_t const value) -> void
        {
            if (activity().workspace_busy) {
                analysis.notify("Wait for the workspace to finish before changing engines.");
                return;
            }
            analysis.set_limit_value(value);
        });
}

}  // namespace motif::slint_app
