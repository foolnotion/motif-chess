#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "motif/db/position_prefix_postings.hpp"

#include <catch2/catch_test_macros.hpp>

#include "motif/db/error.hpp"
#include "motif/db/types.hpp"

namespace
{

class temporary_file
{
  public:
    explicit temporary_file(char const* name)
        : path_ {std::filesystem::temp_directory_path() / name}
    {
        std::filesystem::remove(path_);
    }

    ~temporary_file() { std::filesystem::remove(path_); }

    temporary_file(temporary_file const&) = delete;
    auto operator=(temporary_file const&) -> temporary_file& = delete;
    temporary_file(temporary_file&&) = delete;
    auto operator=(temporary_file&&) -> temporary_file& = delete;

    [[nodiscard]] auto path() const -> std::filesystem::path const& { return path_; }

  private:
    std::filesystem::path path_;
};

auto row(std::uint64_t hash, std::uint32_t game_key) -> motif::db::position_row
{
    return {.zobrist_hash = motif::db::zobrist_hash {hash},
            .game_id = motif::db::game_id {game_key},
            .ply = 0U,
            .encoded_move = 0U,
            .result = 0,
            .white_elo = {},
            .black_elo = {}};
}

constexpr auto prefix_bits = std::uint8_t {4};
constexpr auto first_prefix_hash = std::uint64_t {0x1000'0000'0000'0000};
constexpr auto first_prefix_same_game_hash = std::uint64_t {0x1000'0000'0000'0001};
constexpr auto first_prefix_second_game_hash = std::uint64_t {0x1000'0000'0000'0002};
constexpr auto first_prefix_second_batch_hash = std::uint64_t {0x1fff'0000'0000'0000};
constexpr auto second_prefix_hash = std::uint64_t {0x2000'0000'0000'0000};
constexpr auto first_prefix_lookup_hash = std::uint64_t {0x1abc'0000'0000'0000};
constexpr auto first_prefix_collision_hash = std::uint64_t {0x1eee'0000'0000'0000};
constexpr auto empty_prefix_hash = std::uint64_t {0x3000'0000'0000'0000};
constexpr auto first_game_key = std::uint32_t {8};
constexpr auto second_game_key = std::uint32_t {3};
constexpr auto third_game_key = std::uint32_t {7};

}  // namespace

TEST_CASE("position_prefix_postings: persists sorted unique candidates per prefix", "[motif-db][position_prefix_postings]")
{
    temporary_file const file {"motif-position-prefix-postings-test.bin"};
    auto postings = motif::db::position_prefix_postings {file.path(), prefix_bits};
    auto first_batch = std::vector<motif::db::position_row> {
        row(first_prefix_hash, first_game_key),
        row(first_prefix_same_game_hash, first_game_key),
        row(first_prefix_second_game_hash, second_game_key),
    };
    auto second_batch = std::vector<motif::db::position_row> {
        row(first_prefix_second_batch_hash, second_game_key),
        row(second_prefix_hash, third_game_key),
    };

    REQUIRE(postings.append(first_batch).has_value());
    REQUIRE(postings.append(second_batch).has_value());
    REQUIRE(postings.finalize().has_value());

    auto reopened = motif::db::position_prefix_postings {file.path(), prefix_bits};
    REQUIRE(reopened.open().has_value());

    auto const first_prefix = reopened.candidates(motif::db::zobrist_hash {first_prefix_lookup_hash});
    REQUIRE(first_prefix.has_value());
    REQUIRE(*first_prefix == std::vector<motif::db::game_id> {{3U}, {8U}});

    auto const colliding_prefix = reopened.candidates(motif::db::zobrist_hash {first_prefix_collision_hash});
    REQUIRE(colliding_prefix.has_value());
    REQUIRE(*colliding_prefix == std::vector<motif::db::game_id> {{3U}, {8U}});

    auto const empty_prefix = reopened.candidates(motif::db::zobrist_hash {empty_prefix_hash});
    REQUIRE(empty_prefix.has_value());
    CHECK(empty_prefix->empty());
}

TEST_CASE("position_prefix_postings: external merge preserves sorted unique candidates when spilling",
          "[motif-db][position_prefix_postings]")
{
    temporary_file const file {"motif-position-prefix-postings-spill-test.bin"};
    constexpr auto small_spill_threshold = std::size_t {2};
    auto postings = motif::db::position_prefix_postings {file.path(), prefix_bits, small_spill_threshold};
    auto first_batch = std::vector<motif::db::position_row> {
        row(first_prefix_hash, first_game_key),
        row(first_prefix_same_game_hash, first_game_key),
        row(first_prefix_second_game_hash, second_game_key),
    };
    auto second_batch = std::vector<motif::db::position_row> {
        row(first_prefix_second_batch_hash, second_game_key),
        row(second_prefix_hash, third_game_key),
    };

    REQUIRE(postings.append(first_batch).has_value());
    REQUIRE(postings.append(second_batch).has_value());
    REQUIRE(postings.finalize().has_value());

    auto reopened = motif::db::position_prefix_postings {file.path(), prefix_bits};
    REQUIRE(reopened.open().has_value());

    auto const first_prefix = reopened.candidates(motif::db::zobrist_hash {first_prefix_lookup_hash});
    REQUIRE(first_prefix.has_value());
    REQUIRE(*first_prefix == std::vector<motif::db::game_id> {{3U}, {8U}});

    auto const colliding_prefix = reopened.candidates(motif::db::zobrist_hash {first_prefix_collision_hash});
    REQUIRE(colliding_prefix.has_value());
    REQUIRE(*colliding_prefix == std::vector<motif::db::game_id> {{3U}, {8U}});

    auto const second_prefix = reopened.candidates(motif::db::zobrist_hash {second_prefix_hash});
    REQUIRE(second_prefix.has_value());
    REQUIRE(*second_prefix == std::vector<motif::db::game_id> {{7U}});

    auto const empty_prefix = reopened.candidates(motif::db::zobrist_hash {empty_prefix_hash});
    REQUIRE(empty_prefix.has_value());
    CHECK(empty_prefix->empty());
}

TEST_CASE("position_prefix_postings: rejects invalid format configuration", "[motif-db][position_prefix_postings]")
{
    temporary_file const file {"motif-position-prefix-postings-invalid-test.bin"};
    auto postings = motif::db::position_prefix_postings {file.path(), 0U};
    auto const append_result = postings.append(std::span<motif::db::position_row const> {});
    REQUIRE_FALSE(append_result.has_value());
    CHECK(append_result.error() == motif::db::error_code::invalid_argument);
}
