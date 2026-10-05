#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <slint.h>

#include "motif/db/database_manager.hpp"
#include "motif/db/types.hpp"
#include "motif/slint_app/activity_gate.hpp"
#include "motif/slint_app/event_loop.hpp"
#include "motif/slint_app/replace_controller.hpp"
#include "motif/slint_app/search_presenter.hpp"
#include "workspace.h"

namespace motif::slint_app
{

class async_board_runner;
class async_browser_runner;

using search_query_result = search_result<search_query>;
using search_page_result = search_result<search_page>;
void clear_search(WorkspaceWindow& window);
void publish_search(WorkspaceWindow& window, search_state const& state, bool rows_stale);

class async_search_runner
{
  public:
    explicit async_search_runner(slint::ComponentWeakHandle<WorkspaceWindow> window, event_loop_environment environment);
    ~async_search_runner();

    async_search_runner(async_search_runner const&) = delete;
    auto operator=(async_search_runner const&) -> async_search_runner& = delete;
    async_search_runner(async_search_runner&&) = delete;
    auto operator=(async_search_runner&&) -> async_search_runner& = delete;

    void set_retry_on_database_failure(std::function<bool()> retry_condition);
    void attach(motif::db::database_manager& database);
    void detach();
    void invalidate_position_cache();
    [[nodiscard]] auto is_busy() const noexcept -> bool;
    [[nodiscard]] auto shows_position(motif::db::zobrist_hash hash) const -> bool;
    void note_board_position(motif::db::zobrist_hash hash);
    void search(motif::db::zobrist_hash hash, motif::db::search_filter const& filter);
    void search(std::optional<motif::db::zobrist_hash> const& hash, motif::db::search_filter const& filter);
    void search_if_hash_changed(motif::db::zobrist_hash hash, motif::db::search_filter const& filter);
    void search_if_hash_changed(std::optional<motif::db::zobrist_hash> const& hash, motif::db::search_filter const& filter);
    void set_page(std::size_t page_index);
    void refresh_if_auto(std::optional<motif::db::zobrist_hash> const& hash, motif::db::search_filter const& filter);
    void set_auto_search(bool enabled);
    void publish_auto_search();
    [[nodiscard]] auto match_at(std::size_t row) const -> std::optional<std::pair<motif::db::game_id, std::size_t>>;
    void dismiss_error();

  private:
    struct state;
    struct completion_key;
    std::shared_ptr<state> state_;

    [[nodiscard]] static auto rows_stale(state const& shared) -> bool;
    void publish();
    static void dispatch_pending(std::shared_ptr<state> const& shared);
    static void complete_query(std::shared_ptr<state> const& shared, completion_key key, search_page_result result);
    static void run_query(std::shared_ptr<state> const& shared, search_query_result query, std::uint64_t epoch);
};

void register_search_callbacks(WorkspaceWindow& window,
                               async_search_runner& search,
                               async_browser_runner& browser,
                               async_board_runner const& board,
                               activity_probe const& activity,
                               replace_controller& confirmation);

void follow_board_position(async_search_runner& search, async_browser_runner const& browser, motif::db::zobrist_hash hash);
[[nodiscard]] auto explorer_rows_current(async_board_runner const& board, async_search_runner const& search) -> bool;

}  // namespace motif::slint_app
