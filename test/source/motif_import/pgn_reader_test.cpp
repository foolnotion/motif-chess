#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>

#include "motif/import/pgn_reader.hpp"

#include <catch2/catch_test_macros.hpp>
#include <pgnlib/pgnlib.hpp>  // NOLINT(misc-include-cleaner)
#include <pgnlib/types.hpp>  // NOLINT(misc-include-cleaner)

#include "motif/import/error.hpp"

namespace
{

constexpr std::string_view two_games = R"pgn(
[Event "Game1"]
[White "Alice"]
[Black "Bob"]
[Result "1-0"]

1. e4 e5 2. Nf3 Nc6 1-0

[Event "Game2"]
[White "Charlie"]
[Black "Dave"]
[Result "0-1"]

1. d4 d5 0-1
)pgn";

constexpr std::string_view three_games_second_bad = R"pgn(
[Event "E1"]
[White "Alice"]
[Black "Bob"]
[Result "1-0"]

1. e4 e5 2. Nf3 Nc6 1-0

[Event "E2"]
[White "Broken"]
[Black "Game"
[Result "1-0"]

1. XXXX_invalid_san 1-0

[Event "E3"]
[White "Charlie"]
[Black "Dave"]
[Result "0-1"]

1. d4 d5 0-1
)pgn";

constexpr std::string_view two_games_with_event_in_comment = R"pgn(
[Event "CommentGame1"]
[White "Alice"]
[Black "Bob"]
[Result "1-0"]

1. e4 {
[Event "Fake"]
still comment
} e5 1-0

[Event "CommentGame2"]
[White "Charlie"]
[Black "Dave"]
[Result "0-1"]

1. d4 d5 0-1
)pgn";

auto find_tag(pgn::game const& game, std::string_view key) -> std::string
{
    for (auto const& tag : game.tags) {
        if (tag.key == key) {
            return tag.value;
        }
    }
    return {};
}

auto write_temp_pgn(std::string_view content, std::string const& filename)
    -> std::filesystem::path
{
    auto tmp = std::filesystem::temp_directory_path() / filename;
    std::ofstream out(tmp, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return tmp;
}

}  // namespace

TEST_CASE("pgn_reader: streams all games from two-game PGN", "[motif-import]")
{
    auto tmp = write_temp_pgn(two_games, "pgn_reader_two_games.pgn");

    motif::import::pgn_reader reader {tmp};

    auto game1 = reader.next();
    REQUIRE(game1.has_value());
    CHECK(find_tag(*game1, "Event") == "Game1");
    CHECK(reader.game_number() == 1);

    auto game2 = reader.next();
    REQUIRE(game2.has_value());
    CHECK(find_tag(*game2, "Event") == "Game2");
    CHECK(reader.game_number() == 2);

    auto game3 = reader.next();
    REQUIRE_FALSE(game3.has_value());
    CHECK(game3.error() == motif::import::error_code::eof);

    std::filesystem::remove(tmp);
}

TEST_CASE("pgn_reader: empty PGN returns eof on first call", "[motif-import]")
{
    auto tmp = write_temp_pgn("", "pgn_reader_empty.pgn");

    motif::import::pgn_reader reader {tmp};
    auto result = reader.next();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == motif::import::error_code::eof);

    std::filesystem::remove(tmp);
}

TEST_CASE(
    "pgn_reader: malformed middle game returns parse_error then continues",
    "[motif-import]")
{
    auto tmp =
        write_temp_pgn(three_games_second_bad, "pgn_reader_bad_middle.pgn");

    motif::import::pgn_reader reader {tmp};

    auto game1 = reader.next();
    REQUIRE(game1.has_value());
    CHECK(find_tag(*game1, "Event") == "E1");

    auto game2 = reader.next();
    REQUIRE_FALSE(game2.has_value());
    CHECK(game2.error() == motif::import::error_code::parse_error);

    auto game3 = reader.next();
    REQUIRE(game3.has_value());
    CHECK(find_tag(*game3, "Event") == "E3");

    auto game4 = reader.next();
    REQUIRE_FALSE(game4.has_value());
    CHECK(game4.error() == motif::import::error_code::eof);

    std::filesystem::remove(tmp);
}

TEST_CASE("pgn_reader: seek_to_offset resumes from correct game",
          "[motif-import]")
{
    auto tmp = write_temp_pgn(two_games, "pgn_reader_seek_test.pgn");

    std::size_t game2_offset = 0;
    {
        pgn::game_stream stream {tmp};
        auto iter = stream.begin();
        REQUIRE(iter != std::default_sentinel);
        ++iter;
        REQUIRE(iter != std::default_sentinel);
        game2_offset = iter.byte_offset();
    }

    motif::import::pgn_reader reader {tmp};
    auto seek_result = reader.seek_to_offset(game2_offset);
    REQUIRE(seek_result.has_value());

    auto game = reader.next();
    REQUIRE(game.has_value());
    CHECK(find_tag(*game, "Event") == "Game2");

    std::filesystem::remove(tmp);
}

TEST_CASE("pgn_reader: byte_offset stays absolute across seek_to_offset",
          "[motif-import]")
{
    auto tmp = write_temp_pgn(two_games, "pgn_reader_absolute_offset_test.pgn");

    const auto game1_offset = two_games.find("[Event \"Game1\"]");
    REQUIRE(game1_offset != std::string_view::npos);

    const auto game2_offset = two_games.find("[Event \"Game2\"]");
    REQUIRE(game2_offset != std::string_view::npos);

    motif::import::pgn_reader reader {tmp};
    CHECK(reader.byte_offset() == game1_offset);

    auto seek_result = reader.seek_to_offset(game2_offset);
    REQUIRE(seek_result.has_value());
    CHECK(reader.byte_offset() == game2_offset);

    auto game = reader.next();
    REQUIRE(game.has_value());
    CHECK(find_tag(*game, "Event") == "Game2");
    CHECK(reader.byte_offset() == 0);

    std::filesystem::remove(tmp);
}

TEST_CASE("pgn_reader: ignores [Event] lines inside brace comments",
          "[motif-import]")
{
    auto tmp = write_temp_pgn(two_games_with_event_in_comment,
                              "pgn_reader_event_comment_test.pgn");

    motif::import::pgn_reader reader {tmp};

    auto game1 = reader.next();
    REQUIRE(game1.has_value());
    CHECK(find_tag(*game1, "Event") == "CommentGame1");

    auto game2 = reader.next();
    REQUIRE(game2.has_value());
    CHECK(find_tag(*game2, "Event") == "CommentGame2");

    auto resume_offset =
        two_games_with_event_in_comment.find("[Event \"Fake\"]");
    REQUIRE(resume_offset != std::string_view::npos);

    auto real_game2_offset =
        two_games_with_event_in_comment.find("[Event \"CommentGame2\"]");
    REQUIRE(real_game2_offset != std::string_view::npos);

    motif::import::pgn_reader resumed_reader {tmp};
    auto seek_result = resumed_reader.seek_to_offset(resume_offset);
    REQUIRE(seek_result.has_value());
    CHECK(resumed_reader.byte_offset() == real_game2_offset);

    auto resumed_game = resumed_reader.next();
    REQUIRE(resumed_game.has_value());
    CHECK(find_tag(*resumed_game, "Event") == "CommentGame2");

    std::filesystem::remove(tmp);
}

TEST_CASE("pgn_reader: nonexistent file returns io_failure", "[motif-import]")
{
    motif::import::pgn_reader reader {
        std::filesystem::path {"/nonexistent/path/file.pgn"}};
    auto result = reader.next();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == motif::import::error_code::io_failure);
}
