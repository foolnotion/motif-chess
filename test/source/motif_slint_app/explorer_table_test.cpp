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

TEST_CASE("explorer table: a share never exceeds 100% even when transpositions inflate a count", "[motif-slint-app][explorer]")
{
    auto const table = make_explorer_table(std::vector<continuation> {move("e4", 12, 12, 0, 0)}, 10);
    CHECK(table.rows[0].share_percent == 100);
    CHECK(table.rows[0].share_text == "100%");
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

TEST_CASE("explorer table: rows keep the received order and the total sums the listed moves", "[motif-slint-app][explorer]")
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
    CHECK(table.rows[0].accessible_label == "Nf3, 20 games, 50% of games, average Elo white 2500 black 2480, white 50%, draws 25%, black 25%");
    CHECK(table.total.accessible_label == "Total, 40 games, white 50%, draws 25%, black 25%");
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
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
