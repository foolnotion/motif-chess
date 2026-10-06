#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>

#include "motif/slint_app/analysis_service.hpp"

#include <fmt/base.h>

#include "motif/engine/engine_manager.hpp"
#include "motif/engine/error.hpp"

namespace motif::slint_app
{

namespace
{

// What the worker is asked to do next. Only the latest ask matters, so a single
// slot is enough: a stop is a command without a request.
struct command
{
    std::optional<analysis_request> request;
};

}  // namespace

struct analysis_service::impl
{
    explicit impl(event_sink sink_value)
        : sink(std::move(sink_value))
    {
    }

    event_sink sink;
    motif::engine::engine_manager manager;

    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable idle;
    std::optional<command> pending;
    bool busy {false};
    bool stopping {false};
    // Cancels the launch the worker is running, if any. Replaced by the worker
    // when it takes a command, so it is only ever touched under the mutex: a
    // caller can then cancel a launch without waiting for the worker, and the
    // request cannot land on the next command's launch.
    std::stop_source launch_stop;

    // The generation of the session whose events may still be delivered; 0 when
    // none. Written under the mutex by the caller, read by reader threads.
    std::atomic<std::uint64_t> active_generation {0};

    // Touched by the worker thread only.
    std::optional<std::string> session_id;

    std::thread worker;

    void emit(std::uint64_t const epoch, std::uint64_t const generation, auto payload) const
    {
        if (active_generation.load(std::memory_order_acquire) != generation) {
            return;
        }
        try {
            sink(analysis_event {.epoch = epoch, .generation = generation, .payload = std::move(payload)});
        } catch (...) {
            fmt::print(stderr, "analysis_service: event sink threw\n");
        }
    }

    void release_session()
    {
        if (!session_id) {
            return;
        }
        // Blocks until the engine process has quit; the manager lock is not held.
        static_cast<void>(manager.release_analysis(*session_id));
        session_id.reset();
    }

    void launch(analysis_request const& request, std::stop_token const& stop_token)
    {
        if (active_generation.load(std::memory_order_acquire) != request.generation) {
            return;  // superseded while it waited in the queue
        }
        auto const fail = [this, &request]() -> void
        { emit(request.epoch, request.generation, analysis_start_failure {.code = motif::engine::error_code::engine_start_failed}); };

        if (!manager.configure_engine(motif::engine::engine_config {.name = request.engine_name, .path = request.engine_path}).has_value())
        {
            fail();
            return;
        }
        auto const started = manager.start_analysis(
            motif::engine::analysis_params {
                .fen = request.fen,
                .engine = request.engine_name,
                .multipv = request.multipv,
                .depth = request.depth,
                .movetime_ms = request.movetime_ms,
            },
            stop_token);
        if (!started) {
            // A launch cancelled by a stop, a newer start or shutdown is not a failure.
            if (started.error().code != motif::engine::error_code::engine_start_cancelled) {
                fail();
            }
            return;
        }
        session_id = *started;
        if (active_generation.load(std::memory_order_acquire) != request.generation) {
            return;  // superseded while the engine launched; the next command releases it
        }

        auto const epoch = request.epoch;
        auto const generation = request.generation;
        // Events replayed by subscribe() arrive on this thread, later ones on the
        // engine's reader thread.
        auto const subscribed = manager.subscribe(
            *started,
            [this, epoch, generation](motif::engine::info_event const& event) -> void { emit(epoch, generation, event); },
            [this, epoch, generation](motif::engine::complete_event const& event) -> void { emit(epoch, generation, event); },
            [this, epoch, generation](motif::engine::error_event const& event) -> void { emit(epoch, generation, event); });
        if (!subscribed) {
            fail();
        }
    }

    void run()
    {
        for (;;) {
            auto next = command {};
            auto stop_token = std::stop_token {};
            {
                std::unique_lock lock {mutex};
                wake.wait(lock, [this]() -> bool { return pending.has_value() || stopping; });
                if (stopping || !pending) {
                    break;
                }
                next = std::move(*pending);
                pending.reset();
                busy = true;
                launch_stop = std::stop_source {};
                stop_token = launch_stop.get_token();
            }
            release_session();
            if (next.request) {
                launch(*next.request, stop_token);
            }
            {
                std::scoped_lock const lock {mutex};
                busy = false;
            }
            idle.notify_all();
        }
        release_session();
    }
};

analysis_service::analysis_service(event_sink sink)
    : impl_ {std::make_unique<impl>(std::move(sink))}
{
    impl_->worker = std::thread([state = impl_.get()]() -> void { state->run(); });
}

analysis_service::~analysis_service()
{
    shutdown();
}

void analysis_service::start(analysis_request request)
{
    {
        std::scoped_lock const lock {impl_->mutex};
        if (impl_->stopping) {
            return;
        }
        impl_->active_generation.store(request.generation, std::memory_order_release);
        impl_->launch_stop.request_stop();
        impl_->pending = command {.request = std::move(request)};
    }
    impl_->wake.notify_one();
}

void analysis_service::stop()
{
    {
        std::scoped_lock const lock {impl_->mutex};
        if (impl_->stopping) {
            return;
        }
        impl_->active_generation.store(0, std::memory_order_release);
        impl_->launch_stop.request_stop();
        impl_->pending = command {};
    }
    impl_->wake.notify_one();
}

void analysis_service::wait_until_idle()
{
    std::unique_lock lock {impl_->mutex};
    impl_->idle.wait(lock, [this]() -> bool { return (!impl_->pending.has_value() && !impl_->busy) || impl_->stopping; });
}

void analysis_service::shutdown()
{
    {
        std::scoped_lock const lock {impl_->mutex};
        impl_->stopping = true;
        impl_->pending.reset();
        impl_->active_generation.store(0, std::memory_order_release);
        impl_->launch_stop.request_stop();
    }
    impl_->wake.notify_all();
    impl_->idle.notify_all();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
}

}  // namespace motif::slint_app
