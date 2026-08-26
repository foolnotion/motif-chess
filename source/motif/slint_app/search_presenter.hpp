#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <tl/expected.hpp>

#include "motif/db/database_manager.hpp"
#include "motif/db/types.hpp"
#include "motif/search/opening_stats.hpp"
#include "motif/slint_app/error.hpp"

namespace motif::slint_app
{

struct search_error
{
    error_code code;
    std::string message;
};

template<typename T>
using search_result = tl::expected<T, search_error>;

// One matching-game row: find_games() metadata joined with the ply at which
// that game reached the queried position. motif::db::game_list_entry has no
// ply field, so this pairs it with the value looked up separately from
// query_position_matches().
struct search_match
{
    motif::db::game_list_entry game;
    std::uint16_t ply {};
};

struct search_query
{
    std::uint64_t generation {};
    motif::db::zobrist_hash hash {};
    motif::db::search_filter filter;
};

struct search_page
{
    std::uint64_t generation {};
    motif::db::zobrist_hash hash {};
    std::vector<search_match> matches;
    std::int64_t total_matches {};
    motif::search::opening_stats::stats continuations;
    motif::db::search_filter filter;
};

// Toolkit-neutral position-search/opening-explorer state. Mirrors
// game_browser_state's generation/error shape (this presenter issues async
// queries), not board_state's synchronous shape. Deliberately has no
// player/result filter fields of its own -- it always renders whatever
// filter accompanied the most recently applied query, reusing the browser's
// filter model rather than building a second one.
struct search_state
{
    motif::db::zobrist_hash current_hash {};
    std::vector<search_match> matches;
    std::int64_t total_matches {0};
    std::size_t page_index {0};
    bool has_previous_page {false};
    bool has_next_page {false};
    std::vector<motif::search::opening_stats::continuation> continuations;
    std::int64_t total_games {0};
    motif::db::search_filter active_filter;
    bool searching {false};
    bool auto_search {true};
    std::string error_text;
    std::uint64_t query_generation {0};
};

class search_presenter
{
  public:
    explicit search_presenter(motif::db::database_manager& database) noexcept;

    // Async query pipeline: search() runs on the caller's thread and is
    // cheap; execute_query() performs the (potentially blocking) database
    // reads and should run off the UI event loop; apply_query()/
    // apply_query_error() must run back on the UI thread and are guarded by
    // generation so a stale completion can never replace newer state.
    auto search(motif::db::zobrist_hash hash, motif::db::search_filter filter) -> search_result<search_query>;
    [[nodiscard]] auto execute_query(search_query const& query) const -> search_result<search_page>;
    auto apply_query(search_page page) -> search_result<bool>;
    auto apply_query_error(std::uint64_t generation, search_error query_error) -> search_result<bool>;

    void set_auto_search(bool enabled) noexcept;
    void dismiss_error() noexcept;

    [[nodiscard]] auto state() const noexcept -> search_state const&;

  private:
    motif::db::database_manager* database_;
    search_state state_;

    static auto from_database_error(motif::db::error const& database_error) -> search_error;
    static auto from_search_error(motif::search::error const& search_error_value) -> search_error;
};

}  // namespace motif::slint_app
