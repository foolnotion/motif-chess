#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "motif/slint_app/picker_host.hpp"

#include <slint.h>

#include "motif/slint_app/event_loop.hpp"
#include "motif/slint_app/file_picker.hpp"
#include "workspace.h"

namespace motif::slint_app
{

namespace
{

auto request_for(BrowseTarget const target) -> picker_request
{
    switch (target) {
        case BrowseTarget::DatabaseOpen:
        case BrowseTarget::DatabaseCreate:
            return picker_request {.kind = picker_kind::folder};
        case BrowseTarget::ImportPgn:
            return picker_request {.kind = picker_kind::file, .filters = {{.name = "PGN files", .spec = "pgn"}}};
        case BrowseTarget::Engine:
            break;
    }
    return picker_request {.kind = picker_kind::file};
}

void store(slint::ComponentHandle<WorkspaceWindow> const& window, BrowseTarget const target, std::string const& path)
{
    auto const text = slint::SharedString {path};
    switch (target) {
        case BrowseTarget::DatabaseOpen:
            window->set_open_path_text(text);
            break;
        case BrowseTarget::DatabaseCreate:
            window->set_create_path_text(text);
            break;
        case BrowseTarget::ImportPgn:
            window->set_import_path_text(text);
            break;
        case BrowseTarget::Engine:
            window->set_analysis_engine_path_text(text);
            break;
    }
}

auto field_text(WorkspaceWindow& window, BrowseTarget const target) -> std::string
{
    auto const text = [&window, target]() -> slint::SharedString
    {
        switch (target) {
            case BrowseTarget::DatabaseOpen:
                return window.get_open_path_text();
            case BrowseTarget::DatabaseCreate:
                return window.get_create_path_text();
            case BrowseTarget::ImportPgn:
                return window.get_import_path_text();
            case BrowseTarget::Engine:
                break;
        }
        return window.get_analysis_engine_path_text();
    }();
    return std::string {std::string_view {text.data(), text.size()}};
}

}  // namespace

auto picker_failure_text(std::string_view message) -> std::string
{
    constexpr auto trailing = std::string_view {" \t\r\n."};
    if (auto const last = message.find_last_not_of(trailing); last == std::string_view::npos) {
        message = {};
    } else {
        message = message.substr(0, last + 1);
    }
    if (message.empty()) {
        return "The file dialog is not available. Type the path instead.";
    }
    return "The file dialog is not available: " + std::string {message} + ". Type the path instead.";
}

// ---------------------------------------------------------------------------
// async_picker_runner
// ---------------------------------------------------------------------------

struct async_picker_runner::impl : std::enable_shared_from_this<impl>
{
    // What the event loop and a dialog thread share. The thread holds it after
    // the runner is gone, so it never reaches back into the runner or the window.
    struct dialog
    {
        std::mutex mutex;
        std::condition_variable finished;
        // The thread has delivered, or decided not to, and is about to return.
        bool done {false};
        // Shutdown began: the thread delivers nothing.
        bool stopping {false};
        // Shutdown gave up waiting: the thread, once its dialog returns, does nothing at all.
        bool abandoned {false};
    };

    impl(slint::ComponentWeakHandle<WorkspaceWindow> window_value,
         event_loop_environment environment_value,
         std::shared_ptr<file_picker> picker_value)
        : window(std::move(window_value))
        , environment(std::move(environment_value))
        , picker(std::move(picker_value))
    {
    }

    slint::ComponentWeakHandle<WorkspaceWindow> window;
    event_loop_environment environment;
    std::shared_ptr<file_picker> picker;
    // Bumped when the runner is shut down: every task scheduled before that is dropped.
    std::shared_ptr<std::atomic<std::uint64_t>> lifetime {std::make_shared<std::atomic<std::uint64_t>>(0)};
    bool is_open {false};
    std::shared_ptr<dialog> current;
    std::thread worker;

    void publish_open(bool const open) const
    {
        if (auto const locked = window.lock()) {
            (*locked)->set_picker_open(open);
        }
    }

    void reap()
    {
        if (worker.joinable()) {
            worker.join();
        }
    }

    // Posts `task` to the event loop unless the runner is shut down by the time
    // it runs. Callable from any thread: it reads only what it was given.
    static void post(std::weak_ptr<impl> const& self,
                     std::shared_ptr<std::atomic<std::uint64_t>> const& guard,
                     event_loop_environment const& environment,
                     std::function<void(impl&)> task)
    {
        auto const token = guard->load(std::memory_order_acquire);
        environment.schedule(std::chrono::milliseconds {0},
                             [self, guard, token, task = std::move(task)]() -> void
                             {
                                 if (guard->load(std::memory_order_acquire) != token) {
                                     return;
                                 }
                                 if (auto const locked = self.lock()) {
                                     task(*locked);
                                 }
                             });
    }

    // The answer reached the event loop: apply it and let the next dialog open.
    void complete(BrowseTarget const target, picker_outcome const& outcome)
    {
        is_open = false;
        reap();
        auto const locked = window.lock();
        if (!locked) {
            return;
        }
        (*locked)->set_picker_open(false);
        switch (outcome.result) {
            case picker_result::chosen:
                store(*locked, target, outcome.path);
                break;
            case picker_result::cancelled:
                break;
            case picker_result::failed:
                (*locked)->set_error_text(slint::SharedString {picker_failure_text(outcome.message)});
                break;
        }
    }

    void request(BrowseTarget const target, std::string_view const text)
    {
        if (is_open) {
            return;
        }
        is_open = true;
        publish_open(/*open=*/true);
        reap();

        auto const self = weak_from_this();
        auto picked = request_for(target);
        auto seed = std::string {text};

        if (picker->thread() == picker_thread::event_loop) {
            post(self,
                 lifetime,
                 environment,
                 [target, picked = std::move(picked), seed = std::move(seed)](impl& runner) mutable -> void
                 {
                     picked.start_path = picker_start_path(seed);
                     runner.complete(target, runner.picker->pick(picked));
                 });
            return;
        }

        current = std::make_shared<dialog>();
        worker = std::thread {[picker = picker,
                               state = current,
                               deliver = [self, guard = lifetime, environment = environment, target](picker_outcome outcome) -> void
                               {
                                   post(self,
                                        guard,
                                        environment,
                                        [target, outcome = std::move(outcome)](impl& runner) -> void { runner.complete(target, outcome); });
                               },
                               picked = std::move(picked),
                               seed = std::move(seed)]() mutable -> void
                              {
                                  picked.start_path = picker_start_path(seed);
                                  auto outcome = picker->pick(picked);
                                  std::scoped_lock const lock {state->mutex};
                                  if (state->abandoned) {
                                      return;
                                  }
                                  if (!state->stopping) {
                                      deliver(std::move(outcome));
                                  }
                                  state->done = true;
                                  state->finished.notify_all();
                              }};
    }

    void shutdown()
    {
        lifetime->fetch_add(1, std::memory_order_acq_rel);
        if (!worker.joinable()) {
            return;
        }
        auto const state = current;
        {
            std::scoped_lock const lock {state->mutex};
            state->stopping = true;
        }
        if (picker->abortable()) {
            picker->abort();
        }
        auto lock = std::unique_lock {state->mutex};
        auto const finished = state->finished.wait_for(lock, picker_shutdown_wait, [&state]() -> bool { return state->done; });
        if (!finished) {
            state->abandoned = true;
        }
        lock.unlock();
        if (finished) {
            worker.join();
        } else {
            worker.detach();
        }
    }
};

async_picker_runner::async_picker_runner(slint::ComponentWeakHandle<WorkspaceWindow> window,
                                         event_loop_environment environment,
                                         std::shared_ptr<file_picker> picker)
    : impl_ {std::make_shared<impl>(std::move(window), std::move(environment), std::move(picker))}
{
}

async_picker_runner::~async_picker_runner()
{
    shutdown();
}

void async_picker_runner::request(BrowseTarget const target, std::string_view const field_text)
{
    impl_->request(target, field_text);
}

auto async_picker_runner::open() const -> bool
{
    return impl_->is_open;
}

void async_picker_runner::shutdown()
{
    impl_->shutdown();
}

void register_picker_callbacks(WorkspaceWindow& window, async_picker_runner& picker)
{
    window.on_browse_requested([&window, &picker](BrowseTarget const target) -> void
                               { picker.request(target, field_text(window, target)); });
}

}  // namespace motif::slint_app
