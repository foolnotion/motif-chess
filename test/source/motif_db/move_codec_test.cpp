#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include "motif/db/move_codec.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chesslib/board/move_codec.hpp>
#include <chesslib/core/types.hpp>

#include "motif/db/error.hpp"

struct move_test_support
{
    static constexpr auto move_field_count = std::size_t {7};

    static auto make_move(chesslib::u8 source_square,
                          chesslib::u8 target_square,
                          chesslib::u8 promotion = 0,
                          chesslib::u8 capture = 0,
                          chesslib::u8 double_pawn = 0,
                          chesslib::u8 enpassant = 0,
                          chesslib::u8 castling = 0) -> chesslib::move
    {
        return chesslib::move {
            .source_square = source_square,
            .target_square = target_square,
            .promotion = promotion,
            .capture = capture,
            .double_pawn = double_pawn,
            .enpassant = enpassant,
            .castling = castling,
        };
    }

    static auto move_fields(chesslib::move move) -> std::array<int, move_field_count>
    {
        return {
            move.source_square,
            move.target_square,
            move.promotion,
            move.capture,
            move.double_pawn,
            move.enpassant,
            move.castling,
        };
    }

    static auto require_same_move(chesslib::move lhs, chesslib::move rhs) -> void { REQUIRE(move_fields(lhs) == move_fields(rhs)); }
};

struct move_case
{
    std::string_view name;
    chesslib::move value;
};

TEST_CASE("error_code: string conversion", "[motif_db][error_code]")
{
    CHECK(motif::db::to_string(motif::db::error_code::ok) == "ok");
    CHECK(motif::db::to_string(motif::db::error_code::not_found) == "not_found");
    CHECK(motif::db::to_string(motif::db::error_code::schema_mismatch) == "schema_mismatch");
    CHECK(motif::db::to_string(motif::db::error_code::io_failure) == "io_failure");
    CHECK(motif::db::to_string(motif::db::error_code::duplicate) == "duplicate");
}

TEST_CASE("move_codec: encode returns encoded move", "[motif_db][move_codec]")
{
    auto const source = move_test_support::make_move(chesslib::square::g1, chesslib::square::f3);

    auto const encoded = motif::db::encode_move(source);

    REQUIRE(encoded.has_value());
    CHECK(encoded.value() == chesslib::codec::encode(source));
}

TEST_CASE("move_codec: decode returns decoded move", "[motif_db][move_codec]")
{
    auto const source =
        move_test_support::make_move(chesslib::square::a7, chesslib::square::a8, static_cast<chesslib::u8>(chesslib::piece::queen));

    auto const decoded = motif::db::decode_move(chesslib::codec::encode(source));

    REQUIRE(decoded.has_value());
    move_test_support::require_same_move(decoded.value(), source);
}

TEST_CASE("move_codec: round trips supported move types", "[motif_db][move_codec]")
{
    auto const test_cases = std::array {
        move_case {
            .name = "quiet",
            .value = move_test_support::make_move(chesslib::square::g1, chesslib::square::f3),
        },
        move_case {
            .name = "capture",
            .value = move_test_support::make_move(chesslib::square::e4, chesslib::square::d5, 0, 1),
        },
        move_case {
            .name = "promotion",
            .value =
                move_test_support::make_move(chesslib::square::a7, chesslib::square::a8, static_cast<chesslib::u8>(chesslib::piece::queen)),
        },
        move_case {
            .name = "castling",
            .value = move_test_support::make_move(chesslib::square::e1, chesslib::square::g1, 0, 0, 0, 0, 1),
        },
        move_case {
            .name = "en_passant",
            .value = move_test_support::make_move(chesslib::square::e5, chesslib::square::d6, 0, 1, 0, 1),
        },
    };

    for (auto const& test_case : test_cases) {
        SECTION(std::string {test_case.name})
        {
            auto const encoded = motif::db::encode_move(test_case.value);
            REQUIRE(encoded.has_value());

            auto const decoded = motif::db::decode_move(encoded.value());
            REQUIRE(decoded.has_value());

            move_test_support::require_same_move(decoded.value(), test_case.value);
        }
    }
}
