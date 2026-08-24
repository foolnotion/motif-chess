#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>

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
    constexpr std::uint64_t k_source_size = 987'654;
    constexpr std::int64_t k_source_mtime_ns = 1'700'000'000'000'000'000LL;
    constexpr std::uint64_t source_content_hash = 0x123456789abcdef0ULL;

    motif::import::import_checkpoint orig {
        .source_path = "/data/games.pgn",
        .byte_offset = k_offset,
        .games_committed = k_committed,
        .last_game_id = k_last_id,
        .source_size = k_source_size,
        .source_mtime_ns = k_source_mtime_ns,
        .source_content_hash = source_content_hash,
    };

    REQUIRE(motif::import::write_checkpoint(tmp, orig).has_value());

    auto result = motif::import::read_checkpoint(tmp);
    REQUIRE(result.has_value());
    CHECK(result->source_path == orig.source_path);
    CHECK(result->byte_offset == orig.byte_offset);
    CHECK(result->games_committed == orig.games_committed);
    CHECK(result->last_game_id == orig.last_game_id);
    CHECK(result->source_size == orig.source_size);
    CHECK(result->source_mtime_ns == orig.source_mtime_ns);
    CHECK(result->source_content_hash == orig.source_content_hash);

    std::filesystem::remove_all(tmp);
}

TEST_CASE("checkpoint: stat_source reports current size and changes after mutation", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "cp_stat_source";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto const file_path = tmp / "source.pgn";
    {
        std::ofstream out {file_path};
        out << "abc";
    }

    auto const first_stat = motif::import::stat_source(file_path);
    REQUIRE(first_stat.has_value());
    CHECK(first_stat->size == 3);
    auto const first_hash = motif::import::hash_source(file_path);
    REQUIRE(first_hash.has_value());

    {
        std::ofstream out {file_path, std::ios::trunc};
        out << "abcdef";
    }

    auto const second_stat = motif::import::stat_source(file_path);
    REQUIRE(second_stat.has_value());
    CHECK(second_stat->size == 6);
    auto const second_hash = motif::import::hash_source(file_path);
    REQUIRE(second_hash.has_value());
    CHECK(*second_hash != *first_hash);

    auto const missing_stat = motif::import::stat_source(tmp / "does_not_exist.pgn");
    REQUIRE_FALSE(missing_stat.has_value());
    CHECK(missing_stat.error() == motif::import::error_code::io_failure);
    CHECK_FALSE(motif::import::hash_source(tmp / "does_not_exist.pgn").has_value());

    std::filesystem::remove_all(tmp);
}

TEST_CASE("checkpoint: read_checkpoint returns not_found when absent", "[motif-import]")
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

    motif::import::import_checkpoint const chk {.source_path = "x", .byte_offset = 1};
    REQUIRE(motif::import::write_checkpoint(tmp, chk).has_value());
    motif::import::delete_checkpoint(tmp);
    CHECK_FALSE(std::filesystem::exists(motif::import::checkpoint_path(tmp)));

    std::filesystem::remove_all(tmp);
}

TEST_CASE("checkpoint: replacement leaves the latest complete checkpoint", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "cp_replace";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    REQUIRE(motif::import::write_checkpoint(tmp, {.source_path = "first", .byte_offset = 1}).has_value());
    REQUIRE(motif::import::write_checkpoint(tmp, {.source_path = "second", .byte_offset = 2}).has_value());

    auto const checkpoint = motif::import::read_checkpoint(tmp);
    REQUIRE(checkpoint.has_value());
    CHECK(checkpoint->source_path == "second");
    CHECK(checkpoint->byte_offset == 2);
    CHECK_FALSE(std::filesystem::exists(motif::import::checkpoint_path(tmp).string() + ".tmp"));

    std::filesystem::remove_all(tmp);
}
