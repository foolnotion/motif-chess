#include <cstdint>
#include <vector>

#include "motif/chess/chess.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("motif::chess parse_fen rejects invalid FEN", "[motif-chess]")
{
    auto parsed = motif::chess::parse_fen("not a valid fen");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error() == motif::chess::error_code::invalid_fen);
}

TEST_CASE("motif::chess apply_san encodes and updates board", "[motif-chess]")
{
    auto board = motif::chess::board {};

    auto move = motif::chess::apply_san(board, "e4");
    REQUIRE(move.has_value());
    CHECK(*move != 0);
    CHECK(motif::chess::write_fen(board) == "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1");
}

TEST_CASE("motif::chess apply_uci returns move metadata", "[motif-chess]")
{
    auto board = motif::chess::board {};

    auto move = motif::chess::apply_uci(board, "e2e4");
    REQUIRE(move.has_value());
    CHECK(move->uci == "e2e4");
    CHECK(move->san == "e4");
    CHECK(move->from == "e2");
    CHECK(move->to == "e4");
    CHECK_FALSE(move->promotion.has_value());
}

TEST_CASE("motif::chess replay rejects invalid ply", "[motif-chess]")
{
    auto replayed = motif::chess::replay(std::vector<std::uint16_t> {}, 1);
    REQUIRE_FALSE(replayed.has_value());
    CHECK(replayed.error() == motif::chess::error_code::invalid_ply);
}

TEST_CASE("motif::chess board copy from moved-from value stays valid", "[motif-chess]")
{
    auto original = motif::chess::board {};
    REQUIRE(motif::chess::apply_san(original, "e4").has_value());

    auto moved = std::move(original);
    auto copied = original;
    auto assigned = motif::chess::board {};
    assigned = original;

    CHECK(motif::chess::write_fen(copied) == motif::chess::write_fen(motif::chess::board {}));
    CHECK(motif::chess::write_fen(assigned) == motif::chess::write_fen(motif::chess::board {}));
    CHECK(motif::chess::write_fen(moved) == "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1");
}

TEST_CASE("motif::chess legal_moves returns promotion metadata", "[motif-chess]")
{
    auto board = motif::chess::parse_fen("4k3/P7/8/8/8/8/8/4K3 w - - 0 1");
    REQUIRE(board.has_value());

    auto moves = motif::chess::legal_moves(*board);
    auto found_promotion = false;
    for (auto const& move : moves) {
        if (move.uci == "a7a8q") {
            found_promotion = true;
            CHECK(move.san == "a8=Q+");
            REQUIRE(move.promotion.has_value());
            CHECK(*move.promotion == "q");  // NOLINT(bugprone-unchecked-optional-access)
        }
    }

    CHECK(found_promotion);
}
