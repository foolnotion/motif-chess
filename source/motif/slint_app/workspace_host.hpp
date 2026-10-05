#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include <slint.h>

#include "motif/slint_app/event_loop.hpp"
#include "motif/slint_app/workspace_service.hpp"
#include "workspace.h"

namespace motif::slint_app
{

class async_analysis_runner;
class async_board_runner;
class async_browser_runner;
class async_search_runner;

void publish_workspace(WorkspaceWindow& window, workspace_service const& service);

class async_workspace_runner
{
  public:
    enum class start_outcome : std::uint8_t
    {
        accepted,
        busy,
    };

    async_workspace_runner(workspace_service& service,
                           slint::ComponentWeakHandle<WorkspaceWindow> window,
                           async_browser_runner& browser,
                           async_board_runner& board,
                           async_search_runner& search,
                           async_analysis_runner& analysis,
                           event_loop_environment environment);
    ~async_workspace_runner();

    async_workspace_runner(async_workspace_runner const&) = delete;
    auto operator=(async_workspace_runner const&) -> async_workspace_runner& = delete;
    async_workspace_runner(async_workspace_runner&&) = delete;
    auto operator=(async_workspace_runner&&) -> async_workspace_runner& = delete;

    [[nodiscard]] auto run_create(std::string dir_path, std::string name) -> start_outcome;
    [[nodiscard]] auto run_open(std::string dir_path) -> start_outcome;
    [[nodiscard]] auto run_scratch() -> start_outcome;
    [[nodiscard]] auto is_busy() const noexcept -> bool;

  private:
    struct state;
    std::shared_ptr<state> state_;
    [[nodiscard]] auto start(std::function<result<void>()> operation) -> start_outcome;
};

void register_workspace_callbacks(WorkspaceWindow& window,
                                  workspace_service& service,
                                  async_workspace_runner const& runner,
                                  async_browser_runner& browser,
                                  async_board_runner& board,
                                  async_search_runner& search);

}  // namespace motif::slint_app
