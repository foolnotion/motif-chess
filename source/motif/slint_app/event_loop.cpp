#include <chrono>
#include <functional>
#include <utility>

#include "motif/slint_app/event_loop.hpp"

#include <slint.h>

namespace motif::slint_app
{

auto make_event_loop_environment() -> event_loop_environment
{
    return event_loop_environment {
        .schedule = [](std::chrono::milliseconds const delay, std::function<void()> task) -> void
        {
            slint::invoke_from_event_loop(
                [delay, task = std::move(task)]() mutable -> void
                {
                    if (delay.count() <= 0) {
                        task();
                    } else {
                        slint::Timer::single_shot(delay, std::move(task));
                    }
                });
        },
    };
}

}  // namespace motif::slint_app
