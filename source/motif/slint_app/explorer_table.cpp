#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>

#include "motif/slint_app/explorer_table.hpp"

#include <fmt/format.h>

namespace motif::slint_app
{

namespace
{

constexpr std::int64_t percent_scale {100};
constexpr double elo_rounding_offset {0.5};
constexpr auto missing_elo = "—";

auto elo_text(std::optional<double> const elo) -> std::string
{
    if (!elo) {
        return missing_elo;
    }
    return fmt::format("{}", static_cast<int>(*elo + elo_rounding_offset));
}

// Direct counts never exceed the position's games for a well-formed index; the
// clamp only keeps a malformed one from drawing a share above 100%.
auto share_of(std::int64_t const count, std::int64_t const total) noexcept -> std::int32_t
{
    if (total <= 0 || count <= 0) {
        return 0;
    }
    auto const rounded = (count * percent_scale * 2 + total) / (total * 2);
    return static_cast<std::int32_t>(std::min<std::int64_t>(rounded, percent_scale));
}

auto share_label(std::int64_t const count, std::int32_t const percent) -> std::string
{
    if (count > 0 && percent == 0) {
        return "<1%";
    }
    return fmt::format("{}%", percent);
}

auto split_label(result_split const& split) -> std::string
{
    return fmt::format("white {}%, draws {}%, black {}%", split.white, split.draw, split.black);
}

}  // namespace

auto split_results(std::uint64_t const white, std::uint64_t const draw, std::uint64_t const black) noexcept -> result_split
{
    auto const sum = white + draw + black;
    if (sum == 0) {
        return {};
    }
    auto const counts = std::array<std::uint64_t, 3> {white, draw, black};
    auto floors = std::array<std::int32_t, 3> {};
    auto remainders = std::array<std::uint64_t, 3> {};
    std::int32_t assigned = 0;
    for (std::size_t index = 0; index < counts.size(); ++index) {
        auto const scaled = counts.at(index) * percent_scale;
        floors.at(index) = static_cast<std::int32_t>(scaled / sum);
        remainders.at(index) = scaled % sum;
        assigned += floors.at(index);
    }
    auto order = std::array<std::size_t, 3> {0, 1, 2};
    std::ranges::stable_sort(order,
                             [&remainders](std::size_t const left, std::size_t const right) -> bool
                             { return remainders.at(left) > remainders.at(right); });
    for (std::size_t slot = 0; assigned < percent_scale; ++slot, ++assigned) {
        ++floors.at(order.at(slot));
    }
    return result_split {.white = floors.at(0), .draw = floors.at(1), .black = floors.at(2)};
}

auto make_explorer_table(std::span<motif::search::opening_stats::continuation const> const continuations, std::int64_t const total_games)
    -> explorer_table
{
    auto table = explorer_table {};
    table.rows.reserve(continuations.size());
    std::uint64_t white_total = 0;
    std::uint64_t draw_total = 0;
    std::uint64_t black_total = 0;
    for (auto const& continuation : continuations) {
        white_total += continuation.white_wins;
        draw_total += continuation.draws;
        black_total += continuation.black_wins;

        auto row = explorer_row {};
        row.san = continuation.san;
        row.count = continuation.direct_frequency;
        row.share_percent = share_of(row.count, total_games);
        row.share_text = share_label(row.count, row.share_percent);
        row.split = split_results(continuation.white_wins, continuation.draws, continuation.black_wins);
        row.white_elo = elo_text(continuation.average_white_elo);
        row.black_elo = elo_text(continuation.average_black_elo);
        row.eco = continuation.eco.value_or(std::string {});
        row.opening_name = continuation.opening_name.value_or(std::string {});
        row.accessible_label = fmt::format("{}, {} games, {} of games, average Elo white {} black {}, {}",
                                           row.san,
                                           row.count,
                                           row.share_text,
                                           row.white_elo,
                                           row.black_elo,
                                           split_label(row.split));
        table.rows.push_back(std::move(row));
    }
    // The backend orders by transposition popularity; the table lists moves played
    // from here, most played first, ties in the backend's order.
    std::ranges::stable_sort(table.rows,
                             [](explorer_row const& left, explorer_row const& right) -> bool { return left.count > right.count; });
    table.total.count = total_games;
    table.total.split = split_results(white_total, draw_total, black_total);
    table.total.accessible_label =
        fmt::format("Total, {} games, results of the games that continued: {}", total_games, split_label(table.total.split));
    return table;
}

auto rows_are_current(bool const searching,
                      motif::db::zobrist_hash const shown,
                      std::optional<motif::db::zobrist_hash> const board,
                      bool const outdated) noexcept -> bool
{
    return !searching && !outdated && board && *board == shown;
}

auto rows_are_stale(bool const searching,
                    motif::db::zobrist_hash const shown,
                    std::optional<motif::db::zobrist_hash> const board,
                    bool const outdated) noexcept -> bool
{
    return !searching && (outdated || (board && *board != shown));
}

void explorer_rows_tracker::database_attached() noexcept
{
    database_detached();
}

void explorer_rows_tracker::database_detached() noexcept
{
    last_hash.reset();
    board_hash.reset();
    outdated = false;
}

auto explorer_rows_tracker::current(bool const searching,
                                    motif::db::zobrist_hash const shown,
                                    motif::db::zobrist_hash const board) const noexcept -> bool
{
    return rows_are_current(searching, shown, board, outdated);
}

auto explorer_rows_tracker::stale(bool const searching, motif::db::zobrist_hash const shown) const noexcept -> bool
{
    return rows_are_stale(searching, shown, board_hash, outdated);
}

}  // namespace motif::slint_app
