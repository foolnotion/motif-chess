#pragma once

#include <chrono>
#include <functional>

namespace motif::slint_app
{

// How a host reaches the event loop. `schedule` may be called from any thread
// and runs the task on the event-loop thread after the delay. The analysis and
// browser hosts take this instead of calling Slint directly, so their tests can
// stand in for the loop.
struct event_loop_environment
{
    std::function<void(std::chrono::milliseconds, std::function<void()>)> schedule;
};

// Slint's event loop: invoke_from_event_loop, and a single-shot timer for delays.
[[nodiscard]] auto make_event_loop_environment() -> event_loop_environment;

}  // namespace motif::slint_app
