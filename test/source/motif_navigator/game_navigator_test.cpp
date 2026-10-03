#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "motif/navigator/game_navigator.hpp"

#include <catch2/catch_test_macros.hpp>

#include "motif/chess/chess.hpp"
#include "motif/db/types.hpp"

namespace
{

using motif::navigator::game_navigator;
using outcome = game_navigator::move_outcome;

constexpr std::size_t beyond_the_end {99};
constexpr std::string_view start_fen {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"};

auto make_game(std::vector<std::string> const& sans) -> motif::db::game
{
    auto board = motif::chess::board {};
    auto game = motif::db::game {};
    for (auto const& san : sans) {
        auto move = motif::chess::apply_san(board, san);
        REQUIRE(move);
        game.moves.push_back(*move);
    }
    return game;
}

auto loaded(std::vector<std::string> const& sans) -> game_navigator
{
    auto navigator = game_navigator {};
    navigator.load(make_game(sans));
    return navigator;
}

// Plays a line of SAN moves from the cursor, requiring each to apply.
void play(game_navigator& navigator, std::vector<std::string> const& sans)
{
    for (auto const& san : sans) {
        REQUIRE(navigator.apply_san(san));
    }
}

}  // namespace

TEST_CASE("game_navigator: a loaded game keeps the flat-list semantics", "[motif-navigator]")
{
    auto navigator = loaded({"e4", "e5", "Nf3"});

    CHECK(navigator.has_game());
    CHECK(navigator.total_plies() == 3);
    CHECK(navigator.current_ply() == 0);
    CHECK(navigator.current_fen() == start_fen);
    CHECK(navigator.move_list() == std::vector<std::string> {"e4", "e5", "Nf3"});

    navigator.advance();
    CHECK(navigator.current_ply() == 1);
    CHECK(navigator.current_san() == "e4");
    CHECK(navigator.last_move().value_or(motif::chess::move_info {}).from == "e2");

    navigator.jump_to_end();
    CHECK(navigator.current_ply() == 3);
    navigator.advance();
    CHECK(navigator.current_ply() == 3);

    navigator.navigate_to(2);
    CHECK(navigator.current_san() == "e5");
    navigator.navigate_to(beyond_the_end);
    CHECK(navigator.current_ply() == 3);
    navigator.jump_to_start();
    navigator.retreat();
    CHECK(navigator.current_ply() == 0);
    CHECK_FALSE(navigator.last_move().has_value());
    CHECK(navigator.current_san().empty());
}

TEST_CASE("game_navigator: an empty navigator reports no game and the starting position", "[motif-navigator]")
{
    auto navigator = game_navigator {};

    CHECK_FALSE(navigator.has_game());
    CHECK(navigator.total_plies() == 0);
    CHECK(navigator.current_fen() == start_fen);
    CHECK(navigator.current_hash().value_or(motif::db::zobrist_hash {}).value == motif::chess::board {}.hash());
}

TEST_CASE("game_navigator: the first move on an empty navigator starts the main line", "[motif-navigator]")
{
    auto navigator = game_navigator {};

    auto applied = navigator.apply_move("e2", "e4");
    REQUIRE(applied);
    CHECK(applied->san == "e4");
    CHECK(applied->outcome == outcome::extended_line);
    CHECK(navigator.has_game());
    CHECK(navigator.total_plies() == 1);
    CHECK(navigator.current_ply() == 1);
    CHECK(navigator.move_list() == std::vector<std::string> {"e4"});
    CHECK(navigator.last_move().value_or(motif::chess::move_info {}).to == "e4");
}

TEST_CASE("game_navigator: a move equal to the main continuation only advances", "[motif-navigator]")
{
    auto navigator = loaded({"e4", "e5"});
    auto const nodes_before = navigator.node_count();

    auto applied = navigator.apply_move("e2", "e4");
    REQUIRE(applied);
    CHECK(applied->outcome == outcome::followed_line);
    CHECK(navigator.node_count() == nodes_before);
    CHECK(navigator.current_ply() == 1);
    CHECK(navigator.is_main_line(navigator.current_node()));
}

TEST_CASE("game_navigator: an off-line move creates a variation and leaves the main line alone", "[motif-navigator]")
{
    auto navigator = loaded({"e4", "e5", "Nf3"});
    navigator.navigate_to(1);

    auto applied = navigator.apply_move("c7", "c5");
    REQUIRE(applied);
    CHECK(applied->outcome == outcome::created_variation);
    CHECK(applied->san == "c5");
    CHECK(navigator.current_ply() == 2);
    CHECK_FALSE(navigator.is_main_line(navigator.current_node()));
    CHECK(navigator.total_plies() == 3);
    CHECK(navigator.move_list() == std::vector<std::string> {"e4", "e5", "Nf3"});
    CHECK(navigator.current_fen() == "rnbqkbnr/pp1ppppp/8/2p5/4P3/8/PPPP1PPP/RNBQKBNR w KQkq c6 0 2");

    auto const branch_point = navigator.parent(navigator.current_node());
    CHECK(navigator.children(branch_point).size() == 2);
}

TEST_CASE("game_navigator: a variation nests under a variation", "[motif-navigator]")
{
    auto navigator = loaded({"e4", "e5", "Nf3", "Nc6"});
    navigator.navigate_to(1);
    play(navigator, {"c5", "Nf3", "d6"});  // variation c5 with its own continuation
    navigator.retreat();
    auto const inner_branch = navigator.current_node();

    auto nested = navigator.apply_san("e6");
    REQUIRE(nested);
    CHECK(nested->outcome == outcome::created_variation);
    CHECK(navigator.parent(nested->node) == inner_branch);
    CHECK_FALSE(navigator.is_main_line(nested->node));
    CHECK(navigator.current_ply() == 4);
    CHECK(navigator.total_plies() == 4);
    CHECK(navigator.move_list() == std::vector<std::string> {"e4", "e5", "Nf3", "Nc6"});
}

TEST_CASE("game_navigator: replaying a variation's first move re-enters it without a duplicate", "[motif-navigator]")
{
    auto navigator = loaded({"e4", "e5"});
    navigator.navigate_to(1);
    auto created = navigator.apply_san("c5");
    REQUIRE(created);
    auto const nodes_after_creation = navigator.node_count();

    navigator.navigate_to(1);
    auto again = navigator.apply_move("c7", "c5");
    REQUIRE(again);
    CHECK(again->outcome == outcome::entered_variation);
    CHECK(again->node == created->node);
    CHECK(navigator.node_count() == nodes_after_creation);
}

TEST_CASE("game_navigator: a move into an existing node reached by SAN or squares is the same node", "[motif-navigator]")
{
    auto navigator = game_navigator {};
    auto by_squares = navigator.apply_move("g1", "f3");
    REQUIRE(by_squares);
    navigator.jump_to_start();
    auto by_san = navigator.apply_san("Nf3");
    REQUIRE(by_san);
    CHECK(by_san->node == by_squares->node);
    CHECK(by_san->outcome == outcome::followed_line);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("game_navigator: navigation reaches every node of a tree with variations", "[motif-navigator]")
{
    auto navigator = loaded({"e4", "e5", "Nf3"});
    navigator.navigate_to(1);
    play(navigator, {"c5", "Nf3"});  // variation at ply 2
    navigator.navigate_to(2);
    play(navigator, {"Nc3"});  // second alternative at ply 3
    REQUIRE(navigator.node_count() == 7);

    for (auto id = std::size_t {0}; id < navigator.node_count(); ++id) {
        REQUIRE(navigator.navigate_to_node(static_cast<game_navigator::node_id>(id)));
        CHECK(navigator.current_node() == id);
        CHECK(navigator.current_ply() == navigator.depth(static_cast<game_navigator::node_id>(id)));
        auto replayed = motif::chess::board {};
        auto path = std::vector<std::string> {};
        for (auto walker = static_cast<game_navigator::node_id>(id); walker != game_navigator::root_node; walker = navigator.parent(walker))
        {
            path.insert(path.begin(), navigator.san(walker));
        }
        for (auto const& san : path) {
            REQUIRE(motif::chess::apply_san(replayed, san));
        }
        CHECK(navigator.current_fen() == motif::chess::write_fen(replayed));
    }
    CHECK_FALSE(navigator.navigate_to_node(static_cast<game_navigator::node_id>(navigator.node_count())));
}

TEST_CASE("game_navigator: controls follow the line the cursor is on", "[motif-navigator]")
{
    auto navigator = loaded({"e4", "e5", "Nf3"});
    navigator.navigate_to(1);
    auto const branch_point = navigator.current_node();
    auto const variation_start = navigator.apply_san("c5");
    REQUIRE(variation_start);
    play(navigator, {"Nf3", "d6"});
    auto const variation_end = navigator.current_node();

    // Left from a variation's first move returns to the branch point.
    navigator.navigate_to_node(variation_start->node);
    navigator.retreat();
    CHECK(navigator.current_node() == branch_point);

    // From the branch point Right and End stay on the main line.
    navigator.advance();
    CHECK(navigator.san(navigator.current_node()) == "e5");
    navigator.navigate_to_node(branch_point);
    navigator.jump_to_end();
    CHECK(navigator.current_ply() == 3);
    CHECK(navigator.is_main_line(navigator.current_node()));

    // Inside the variation they follow the variation.
    navigator.navigate_to_node(variation_start->node);
    navigator.advance();
    CHECK(navigator.san(navigator.current_node()) == "Nf3");
    CHECK_FALSE(navigator.is_main_line(navigator.current_node()));
    navigator.jump_to_end();
    CHECK(navigator.current_node() == variation_end);
    navigator.advance();
    CHECK(navigator.current_node() == variation_end);

    navigator.retreat();
    navigator.retreat();
    navigator.retreat();
    CHECK(navigator.current_node() == branch_point);
}

TEST_CASE("game_navigator: an illegal move changes nothing", "[motif-navigator]")
{
    auto navigator = loaded({"e4", "e5"});
    navigator.navigate_to(1);
    auto const fen = navigator.current_fen();
    auto const nodes = navigator.node_count();

    auto not_reachable = navigator.apply_move("e7", "e3");
    REQUIRE_FALSE(not_reachable);
    CHECK(not_reachable.error() == game_navigator::move_error::illegal_move);
    auto off_board = navigator.apply_move("z9", "e4");
    REQUIRE_FALSE(off_board);
    CHECK(off_board.error() == game_navigator::move_error::illegal_move);
    auto wrong_side = navigator.apply_move("e2", "e3");
    REQUIRE_FALSE(wrong_side);
    auto stray_promotion = navigator.apply_move("e7", "e5", 'q');
    REQUIRE_FALSE(stray_promotion);
    CHECK(stray_promotion.error() == game_navigator::move_error::illegal_move);

    CHECK(navigator.current_fen() == fen);
    CHECK(navigator.node_count() == nodes);
    CHECK(navigator.current_ply() == 1);
    CHECK(navigator.total_plies() == 2);
}

TEST_CASE("game_navigator: promotion needs a piece and each piece is its own move", "[motif-navigator]")
{
    auto navigator = game_navigator {};
    play(navigator, {"h4", "g5", "hxg5", "a6", "g6", "a5", "gxh7", "a4"});
    auto const fen = navigator.current_fen();
    auto const nodes = navigator.node_count();

    auto missing = navigator.apply_move("h7", "g8");
    REQUIRE_FALSE(missing);
    CHECK(missing.error() == game_navigator::move_error::promotion_required);
    CHECK(navigator.current_fen() == fen);
    CHECK(navigator.node_count() == nodes);

    auto bad_piece = navigator.apply_move("h7", "g8", 'k');
    REQUIRE_FALSE(bad_piece);
    CHECK(bad_piece.error() == game_navigator::move_error::illegal_move);

    auto queen = navigator.apply_move("h7", "g8", 'Q');
    REQUIRE(queen);
    CHECK(queen->san.starts_with("hxg8=Q"));
    CHECK(navigator.current_fen().starts_with("rnbqkbQr/1ppppp2/8/8/p7/8/PPPPPPP1/RNBQKBNR b"));

    navigator.retreat();
    auto knight = navigator.apply_move("h7", "g8", 'n');
    REQUIRE(knight);
    CHECK(knight->outcome == outcome::created_variation);
    CHECK(knight->san.starts_with("hxg8=N"));
    CHECK(knight->node != queen->node);
}

TEST_CASE("game_navigator: castling and en passant are played by squares", "[motif-navigator]")
{
    auto navigator = game_navigator {};
    play(navigator, {"e4", "e5", "Nf3", "Nc6", "Bc4", "Bc5"});
    auto castle = navigator.apply_move("e1", "g1");
    REQUIRE(castle);
    CHECK(castle->san == "O-O");
    CHECK(navigator.current_fen().starts_with("r1bqk1nr/pppp1ppp/2n5/2b1p3/2B1P3/5N2/PPPP1PPP/RNBQ1RK1 b"));

    auto passant = game_navigator {};
    play(passant, {"e4", "a6", "e5", "d5"});
    auto capture = passant.apply_move("e5", "d6");
    REQUIRE(capture);
    CHECK(capture->san == "exd6");
    CHECK(passant.current_fen().starts_with("rnbqkbnr/1pp1pppp/p2P4/8/8/8/PPPP1PPP/RNBQKBNR b"));
}

TEST_CASE("game_navigator: SAN entry acts as board input and rejects bad or ambiguous SAN", "[motif-navigator]")
{
    auto navigator = game_navigator {};
    auto applied = navigator.apply_san("Nf3");
    REQUIRE(applied);
    CHECK(applied->san == "Nf3");
    CHECK(applied->outcome == outcome::extended_line);

    play(navigator, {"a6", "d3", "a5"});
    auto const fen = navigator.current_fen();
    auto const nodes = navigator.node_count();

    auto garbage = navigator.apply_san("Zz9");
    REQUIRE_FALSE(garbage);
    CHECK(garbage.error() == game_navigator::move_error::invalid_san);
    CHECK_FALSE(navigator.apply_san("").has_value());
    CHECK_FALSE(navigator.apply_san("   ").has_value());
    auto illegal = navigator.apply_san("Ke4");
    REQUIRE_FALSE(illegal);
    auto ambiguous = navigator.apply_san("Nd2");  // knights on b1 and f3 both reach d2 once the d-pawn has moved
    REQUIRE_FALSE(ambiguous);
    CHECK(ambiguous.error() == game_navigator::move_error::invalid_san);
    CHECK(navigator.current_fen() == fen);
    CHECK(navigator.node_count() == nodes);

    auto disambiguated = navigator.apply_san("Nbd2");
    REQUIRE(disambiguated);
    CHECK(disambiguated->san == "Nbd2");
}

TEST_CASE("game_navigator: load and clear discard variations", "[motif-navigator]")
{
    auto navigator = loaded({"e4", "e5"});
    navigator.navigate_to(1);
    REQUIRE(navigator.apply_san("c5"));
    REQUIRE(navigator.node_count() == 4);

    navigator.load(make_game({"d4"}));
    CHECK(navigator.node_count() == 2);
    CHECK(navigator.current_ply() == 0);
    CHECK(navigator.move_list() == std::vector<std::string> {"d4"});

    navigator.clear();
    CHECK_FALSE(navigator.has_game());
    CHECK(navigator.node_count() == 1);
    CHECK(navigator.current_fen() == start_fen);
}

TEST_CASE("game_navigator: playing on a long line stays within the interaction budget", "[motif-navigator][performance]")
{
    auto navigator = game_navigator {};
    constexpr auto cycle_count = std::size_t {150};
    for (std::size_t cycle = 0; cycle < cycle_count; ++cycle) {
        play(navigator, {"Nf3", "Nf6", "Ng1", "Ng8"});
    }
    REQUIRE(navigator.current_ply() == cycle_count * 4);

    // The spec's 100 ms interaction budget for one move on a long line. The
    // bound is generous so it holds in debug and sanitizer builds; it guards
    // against a pathological cost per move (for instance rebuilding the whole
    // tree), not against the cached board specifically.
    auto const started = std::chrono::steady_clock::now();
    REQUIRE(navigator.apply_san("e4"));
    auto const elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed < std::chrono::milliseconds {100});
}
