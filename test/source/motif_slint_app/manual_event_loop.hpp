#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <utility>
#include <vector>

#include "motif/slint_app/event_loop.hpp"

namespace test_support
{

constexpr auto task_wait_timeout = std::chrono::seconds {30};

class manual_event_loop
{
  public:
    [[nodiscard]] auto environment() -> motif::slint_app::event_loop_environment
    {
        return {.schedule = [this](std::chrono::milliseconds const delay, std::function<void()> task) -> void
                {
                    {
                        std::scoped_lock const lock {mutex_};
                        tasks_.push_back(entry {.delay = delay, .task = std::move(task)});
                    }
                    ready_.notify_all();
                }};
    }

    [[nodiscard]] auto pending(std::chrono::milliseconds const delay) const -> std::size_t
    {
        std::scoped_lock const lock {mutex_};
        return static_cast<std::size_t>(std::ranges::count_if(tasks_, [delay](entry const& item) -> bool { return item.delay == delay; }));
    }

    void wait_for_tasks(std::chrono::milliseconds const delay, std::size_t const count)
    {
        auto lock = std::unique_lock {mutex_};
        if (!ready_.wait_for(lock, task_wait_timeout, [this, delay, count]() -> bool { return matching_count(delay) >= count; })) {
            throw std::runtime_error {"manual event loop did not receive the scheduled task"};
        }
    }

    auto run(std::chrono::milliseconds const delay) -> std::size_t
    {
        auto due = std::vector<entry> {};
        {
            std::scoped_lock const lock {mutex_};
            auto const split = std::ranges::stable_partition(tasks_, [delay](entry const& item) -> bool { return item.delay != delay; });
            due.assign(std::make_move_iterator(split.begin()), std::make_move_iterator(split.end()));
            tasks_.erase(split.begin(), tasks_.end());
        }
        for (auto& item : due) {
            item.task();
        }
        return due.size();
    }

  private:
    struct entry
    {
        std::chrono::milliseconds delay;
        std::function<void()> task;
    };

    [[nodiscard]] auto matching_count(std::chrono::milliseconds const delay) const -> std::size_t
    {
        return static_cast<std::size_t>(std::ranges::count_if(tasks_, [delay](entry const& item) -> bool { return item.delay == delay; }));
    }

    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::vector<entry> tasks_;
};

}  // namespace test_support
