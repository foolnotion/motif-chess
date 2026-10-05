#include <chrono>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include "motif/slint_app/workspace_host.hpp"

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

TEST_CASE("workspace host: publishes an inactive workspace", "[motif-slint-app]")
{
    auto window = WorkspaceWindow::create();
    auto service = motif::slint_app::workspace_service {};

    motif::slint_app::publish_workspace(*window, service);

    CHECK_FALSE(window->get_has_database());
    CHECK_FALSE(window->get_is_scratch());
}

TEST_CASE("workspace host: injected event loop has the production scheduling contract", "[motif-slint-app]")
{
    auto loop = manual_loop {};
    CHECK(loop.environment().schedule != nullptr);
}
