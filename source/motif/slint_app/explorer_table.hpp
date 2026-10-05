#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "motif/db/types.hpp"
#include "motif/search/opening_stats.hpp"

namespace motif::slint_app
{

// Whole-percent split of a set of results. The three values always sum to 100
// when any game was counted, and are all zero otherwise.
struct result_split
{
    std::int32_t white {0};
    std::int32_t draw {0};
    std::int32_t black {0};
};

// One move of the opening-explorer table, ready to render. Every figure and
// string is derived here so the UI only indexes and draws it.
struct explorer_row
{
    std::string san;
    // Games that played this move from the position (the direct edge count, the
    // same population as the result split), not games that reached the child
    // position by another move order.
    std::int64_t count {0};
    // Share of the position's games that chose this move, 0..100. `share_text`
    // shows "<1%" for a move played in fewer than half a percent of the games.
    std::int32_t share_percent {0};
    std::string share_text;
    result_split split;
    // Average Elo, or "—" when the move's games carry none.
    std::string white_elo;
    std::string black_elo;
    std::string eco;
    std::string opening_name;
    std::string accessible_label;
};

// The Σ row: the position's total games and the combined result split of the
// listed moves.
struct explorer_total
{
    std::int64_t count {0};
    result_split split;
    std::string accessible_label;
};

struct explorer_table
{
    std::vector<explorer_row> rows;
    explorer_total total;
};

// Splits `white + draw + black` into whole percents by the largest-remainder
// method so the parts always add up to 100.
[[nodiscard]] auto split_results(std::uint64_t white, std::uint64_t draw, std::uint64_t black) noexcept -> result_split;

// Builds the table from the existing position-search continuations. `total_games`
// is the denominator of every share. Rows are ordered by direct count, most played
// first; equal counts keep the order received.
[[nodiscard]] auto make_explorer_table(std::span<motif::search::opening_stats::continuation const> continuations, std::int64_t total_games)
    -> explorer_table;

// The rows on screen are only actionable for the position and filter they were
// searched for. `shown` is the searched position, `board` the one on the board
// (none: no board); `outdated` means the browser filter or the database changed
// since the search and nothing re-queried (auto-search off). `current` is false
// while a search runs (its rows are replaced anyway); `stale` is true when a
// finished result no longer matches, until the next search finishes.
[[nodiscard]] auto rows_are_current(bool searching,
                                    motif::db::zobrist_hash shown,
                                    std::optional<motif::db::zobrist_hash> board,
                                    bool outdated) noexcept -> bool;
[[nodiscard]] auto rows_are_stale(bool searching,
                                  motif::db::zobrist_hash shown,
                                  std::optional<motif::db::zobrist_hash> board,
                                  bool outdated) noexcept -> bool;

// What the search runner remembers about the rows on screen: the position the
// last search started for, the position last reported by the board, and
// whether the rows predate a filter or data change nothing re-queried.
// A database switch ends the rows' relevance, so attach and detach both reset
// all of it; the next search starts from a clean slate.
struct explorer_rows_tracker
{
    std::optional<motif::db::zobrist_hash> last_hash;
    std::optional<motif::db::zobrist_hash> board_hash;
    bool outdated {false};

    void database_attached() noexcept;
    void database_detached() noexcept;

    [[nodiscard]] auto current(bool searching, motif::db::zobrist_hash shown, motif::db::zobrist_hash board) const noexcept -> bool;
    [[nodiscard]] auto stale(bool searching, motif::db::zobrist_hash shown) const noexcept -> bool;
};

}  // namespace motif::slint_app
