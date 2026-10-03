#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

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
    std::int64_t count {0};
    // Share of the position's games, clamped to 0..100. `share_text` shows
    // "<1%" for a move played in fewer than half a percent of the games.
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
// is the denominator of every share. Row order is kept as received.
[[nodiscard]] auto make_explorer_table(std::span<motif::search::opening_stats::continuation const> continuations, std::int64_t total_games)
    -> explorer_table;

}  // namespace motif::slint_app
