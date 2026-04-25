#include <cstddef>
#include <cstdint>
#include <filesystem>

#include "motif/import/checkpoint.hpp"

#include <catch2/catch_test_macros.hpp>

#include "motif/import/error.hpp"

TEST_CASE("checkpoint: glaze round-trip all fields", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "cp_test";
    std::filesystem::create_directories(tmp);

    constexpr std::size_t k_offset = 123456789UZ;
    constexpr std::int64_t k_committed = 42'000;
    constexpr std::int64_t k_last_id = 41'999;

    motif::import::import_checkpoint orig {
        .source_path = "/data/games.pgn",
        .byte_offset = k_offset,
        .games_committed = k_committed,
        .last_game_id = k_last_id,
    };

    REQUIRE(motif::import::write_checkpoint(tmp, orig).has_value());

    auto result = motif::import::read_checkpoint(tmp);
    REQUIRE(result.has_value());
    CHECK(result->source_path == orig.source_path);
    CHECK(result->byte_offset == orig.byte_offset);
    CHECK(result->games_committed == orig.games_committed);
    CHECK(result->last_game_id == orig.last_game_id);

    std::filesystem::remove_all(tmp);
}

TEST_CASE("checkpoint: read_checkpoint returns not_found when absent",
          "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "cp_absent";
    std::filesystem::create_directories(tmp);

    auto result = motif::import::read_checkpoint(tmp);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == motif::import::error_code::not_found);

    std::filesystem::remove_all(tmp);
}

TEST_CASE("checkpoint: delete_checkpoint is idempotent", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "cp_del";
    std::filesystem::create_directories(tmp);

    CHECK_NOTHROW(motif::import::delete_checkpoint(tmp));

    motif::import::import_checkpoint const chk {.source_path = "x",
                                                .byte_offset = 1};
    REQUIRE(motif::import::write_checkpoint(tmp, chk).has_value());
    motif::import::delete_checkpoint(tmp);
    CHECK_FALSE(std::filesystem::exists(motif::import::checkpoint_path(tmp)));

    std::filesystem::remove_all(tmp);
}
