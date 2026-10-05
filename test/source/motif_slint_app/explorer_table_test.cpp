#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "motif/slint_app/explorer_table.hpp"

#include <catch2/catch_test_macros.hpp>

#include "motif/search/opening_stats.hpp"

// Sample figures are literal on purpose: each test asserts the exact rounded result.
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
namespace
{

using motif::search::opening_stats::continuation;
using motif::slint_app::make_explorer_table;
using motif::slint_app::split_results;

// The three result counts are positional because that is how a PGN tally reads.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto move(std::string san, std::uint32_t const frequency, std::uint32_t const white, std::uint32_t const draws, std::uint32_t const black)
    -> continuation
{
    auto result = continuation {};
    result.san = std::move(san);
    result.frequency = frequency;
    result.direct_frequency = frequency;
    result.white_wins = white;
    result.draws = draws;
    result.black_wins = black;
    return result;
}

}  // namespace

TEST_CASE("explorer table: result percentages always sum to 100", "[motif-slint-app][explorer]")
{
    auto const even = split_results(1, 1, 1);
    CHECK(even.white + even.draw + even.black == 100);
    CHECK(even.white == 34);  // the remainder goes to the first of equal fractions
    CHECK(even.draw == 33);
    CHECK(even.black == 33);

    auto const lopsided = split_results(999, 1, 0);
    CHECK(lopsided.white + lopsided.draw + lopsided.black == 100);
    CHECK(lopsided.black == 0);

    auto const rounded = split_results(5, 3, 2);  // 50 / 30 / 20 exactly
    CHECK(rounded.white == 50);
    CHECK(rounded.draw == 30);
    CHECK(rounded.black == 20);

    auto const thirds = split_results(2, 1, 0);  // 66.67 / 33.33: the larger fraction rounds up
    CHECK(thirds.white == 67);
    CHECK(thirds.draw == 33);
    CHECK(thirds.black == 0);
}

TEST_CASE("explorer table: a move with no counted result has an empty bar", "[motif-slint-app][explorer]")
{
    auto const none = split_results(0, 0, 0);
    CHECK(none.white == 0);
    CHECK(none.draw == 0);
    CHECK(none.black == 0);
}

TEST_CASE("explorer table: shares come from the move count over the position's games", "[motif-slint-app][explorer]")
{
    auto const moves = std::vector<continuation> {move("e4", 3, 3, 0, 0), move("d4", 1, 0, 1, 0), move("a3", 1, 0, 0, 1)};
    auto const table = make_explorer_table(moves, 1000);
    REQUIRE(table.rows.size() == 3);
    CHECK(table.rows[0].share_text == "<1%");  // 3 of 1000 is below half a percent
    CHECK(table.rows[0].share_percent == 0);
    CHECK(table.rows[0].count == 3);

    auto const half = make_explorer_table(std::vector<continuation> {move("e4", 1, 1, 0, 0)}, 2);
    CHECK(half.rows[0].share_text == "50%");
    auto const whole = make_explorer_table(std::vector<continuation> {move("e4", 2, 1, 1, 0)}, 3);
    CHECK(whole.rows[0].share_text == "67%");  // 66.67 rounds up
}

TEST_CASE("explorer table: a malformed direct count cannot draw a share above 100%", "[motif-slint-app][explorer]")
{
    auto const table = make_explorer_table(std::vector<continuation> {move("e4", 12, 12, 0, 0)}, 10);
    CHECK(table.rows[0].share_percent == 100);
    CHECK(table.rows[0].share_text == "100%");
}

TEST_CASE("explorer table: a transposed child counts only the games that played the move from here", "[motif-slint-app][explorer]")
{
    // Mirrors opening_stats_test: one game at the position, its child also reached by a
    // second game through another move order (frequency 2), but played from here once.
    auto transposed = move("Nf6", 1, 1, 0, 0);
    transposed.frequency = 2;
    transposed.direct_frequency = 1;
    auto const table = make_explorer_table(std::vector<continuation> {transposed}, 1);
    REQUIRE(table.rows.size() == 1);
    CHECK(table.rows[0].count == 1);
    CHECK(table.rows[0].share_text == "100%");
    CHECK(table.rows[0].accessible_label.starts_with("Nf6, 1 games, 100% of games"));
}

TEST_CASE("explorer table: rows are current only for a finished search on the board's position and filter", "[motif-slint-app][explorer]")
{
    auto const here = motif::db::zobrist_hash {1};
    auto const elsewhere = motif::db::zobrist_hash {2};
    using motif::slint_app::rows_are_current;
    using motif::slint_app::rows_are_stale;

    CHECK(rows_are_current(false, here, here, false));
    CHECK_FALSE(rows_are_stale(false, here, here, false));

    // Auto-search off: the board moved on and the finished rows did not.
    CHECK_FALSE(rows_are_current(false, here, elsewhere, false));
    CHECK(rows_are_stale(false, here, elsewhere, false));

    // Auto-search off: the browser filter changed and nothing re-queried.
    CHECK_FALSE(rows_are_current(false, here, here, true));
    CHECK(rows_are_stale(false, here, here, true));

    // A running search replaces the rows, so they are neither current nor flagged stale.
    CHECK_FALSE(rows_are_current(true, here, here, false));
    CHECK_FALSE(rows_are_stale(true, here, elsewhere, true));

    // No board: nothing to be stale against and nothing to play on.
    CHECK_FALSE(rows_are_current(false, here, std::nullopt, false));
    CHECK_FALSE(rows_are_stale(false, here, std::nullopt, false));
}

TEST_CASE("explorer table: zero games gives no division and an empty total", "[motif-slint-app][explorer]")
{
    auto const table = make_explorer_table(std::vector<continuation> {move("e4", 5, 5, 0, 0)}, 0);
    REQUIRE(table.rows.size() == 1);
    CHECK(table.rows[0].share_percent == 0);
    CHECK(table.total.count == 0);

    auto const empty = make_explorer_table(std::vector<continuation> {}, 0);
    CHECK(empty.rows.empty());
    CHECK(empty.total.count == 0);
    CHECK(empty.total.split.white + empty.total.split.draw + empty.total.split.black == 0);
}

TEST_CASE("explorer table: rows are ordered by moves played from here, ties keep the received order", "[motif-slint-app][explorer]")
{
    // The backend sorts by transposition popularity, so a move reached mostly by other
    // move orders (frequency 9, played from here once) arrives ahead of a move played
    // three times from here.
    auto popular_by_transposition = move("Nc3", 1, 1, 0, 0);
    popular_by_transposition.frequency = 9;
    auto played_here = move("e4", 3, 3, 0, 0);
    played_here.frequency = 4;
    auto tied_first = move("d4", 1, 0, 1, 0);
    auto tied_second = move("c4", 1, 0, 0, 1);
    auto const table = make_explorer_table(std::vector<continuation> {popular_by_transposition, tied_first, played_here, tied_second}, 10);
    REQUIRE(table.rows.size() == 4);
    CHECK(table.rows[0].san == "e4");
    CHECK(table.rows[1].san == "Nc3");
    CHECK(table.rows[2].san == "d4");
    CHECK(table.rows[3].san == "c4");
}

TEST_CASE("explorer table: the total sums the results of the listed moves", "[motif-slint-app][explorer]")
{
    auto const moves = std::vector<continuation> {move("e4", 60, 30, 20, 10), move("d4", 40, 0, 20, 20)};
    auto const table = make_explorer_table(moves, 100);
    REQUIRE(table.rows.size() == 2);
    CHECK(table.rows[0].san == "e4");
    CHECK(table.rows[1].san == "d4");
    CHECK(table.rows[0].share_text == "60%");
    CHECK(table.rows[1].split.draw == 50);
    CHECK(table.total.count == 100);
    // 30 / 40 / 30 games across both moves.
    CHECK(table.total.split.white == 30);
    CHECK(table.total.split.draw == 40);
    CHECK(table.total.split.black == 30);
}

TEST_CASE("explorer table: average Elo is rounded and a missing Elo is a dash", "[motif-slint-app][explorer]")
{
    auto with_elo = move("e4", 1, 1, 0, 0);
    with_elo.average_white_elo = 2412.6;
    auto const table = make_explorer_table(std::vector<continuation> {with_elo, move("d4", 1, 0, 1, 0)}, 2);
    CHECK(table.rows[0].white_elo == "2413");
    CHECK(table.rows[0].black_elo == "—");
    CHECK(table.rows[1].white_elo == "—");
}

TEST_CASE("explorer table: every row's accessible label carries move, games, share, Elo and the three percentages",
          "[motif-slint-app][explorer]")
{
    auto row = move("Nf3", 20, 10, 5, 5);
    row.average_white_elo = 2500.0;
    row.average_black_elo = 2480.0;
    auto const table = make_explorer_table(std::vector<continuation> {row}, 40);
    CHECK(table.rows[0].accessible_label
          == "Nf3, 20 games, 50% of games, average Elo white 2500 black 2480, white 50%, draws 25%, black 25%");
    CHECK(table.total.accessible_label == "Total, 40 games, results of the games that continued: white 50%, draws 25%, black 25%");
}

TEST_CASE("explorer table: ECO and opening name pass through, absent values are empty", "[motif-slint-app][explorer]")
{
    auto named = move("e4", 1, 1, 0, 0);
    named.eco = "B00";
    named.opening_name = "King's Pawn";
    auto const table = make_explorer_table(std::vector<continuation> {named, move("d4", 1, 0, 1, 0)}, 2);
    CHECK(table.rows[0].eco == "B00");
    CHECK(table.rows[0].opening_name == "King's Pawn");
    CHECK(table.rows[1].eco.empty());
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("explorer rows tracker: a database switch leaves no rows flagged stale", "[motif-slint-app][explorer]")
{
    // A fresh presenter shows no rows; its searched position is the default hash.
    auto const fresh = motif::db::zobrist_hash {};
    auto const old_board = motif::db::zobrist_hash {7};
    using motif::slint_app::explorer_rows_tracker;

    // The state a manual-search user leaves behind: the board moved on and a
    // filter change marked the rows outdated, then the database is replaced.
    auto const dirty = []() -> explorer_rows_tracker
    {
        auto tracker = explorer_rows_tracker {};
        tracker.last_hash = motif::db::zobrist_hash {3};
        tracker.board_hash = motif::db::zobrist_hash {7};
        tracker.outdated = true;
        return tracker;
    };

    auto attached = dirty();
    attached.database_attached();
    CHECK_FALSE(attached.stale(false, fresh));
    CHECK_FALSE(attached.outdated);
    CHECK_FALSE(attached.last_hash);
    CHECK_FALSE(attached.board_hash);
    CHECK_FALSE(attached.current(false, fresh, old_board));

    auto detached = dirty();
    detached.database_detached();
    CHECK_FALSE(detached.stale(false, fresh));
    CHECK_FALSE(detached.outdated);
    CHECK_FALSE(detached.last_hash);
    CHECK_FALSE(detached.board_hash);
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
