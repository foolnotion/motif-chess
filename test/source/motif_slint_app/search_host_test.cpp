#include <chrono>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include "motif/slint_app/search_host.hpp"

#include <catch2/catch_test_macros.hpp>
#include <slint.h>

#include "workspace.h"

namespace
{

class manual_loop
{
  public:
    [[nodiscard]] auto environment() -> motif::slint_app::event_loop_environment
    {
        return {.schedule = [this](std::chrono::milliseconds const delay, std::function<void()> task) -> void
                {
                    std::scoped_lock const lock {mutex_};
                    tasks_.push_back({delay, std::move(task)});
                }};
    }

  private:
    struct task
    {
        std::chrono::milliseconds delay;
        std::function<void()> callback;
    };

    std::mutex mutex_;
    std::vector<task> tasks_;
};

}  // namespace

TEST_CASE("search host: clear search resets all published search state", "[motif-slint-app]")
{
    auto window = WorkspaceWindow::create();
    motif::slint_app::clear_search(*window);

    CHECK(window->get_search_total_matches() == 0);
    CHECK_FALSE(window->get_search_busy());
    CHECK_FALSE(window->get_search_rows_stale());
}

TEST_CASE("search host: injected event loop is accepted by the production runner", "[motif-slint-app]")
{
    auto window = WorkspaceWindow::create();
    auto loop = manual_loop {};
    auto runner = motif::slint_app::async_search_runner {window, loop.environment()};

    CHECK_FALSE(runner.is_busy());
}
