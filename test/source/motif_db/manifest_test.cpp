#include <filesystem>
#include <string>

#include "motif/db/manifest.hpp"

#include <catch2/catch_test_macros.hpp>

#include "motif/db/error.hpp"

namespace
{

struct tmp_file
{
    std::filesystem::path path;

    explicit tmp_file(std::string const& suffix)
    {
        path = std::filesystem::temp_directory_path() / ("motif_manifest_test_" + suffix + ".json");
    }

    ~tmp_file() { std::filesystem::remove(path); }

    tmp_file(tmp_file const&) = delete;
    auto operator=(tmp_file const&) -> tmp_file& = delete;
    tmp_file(tmp_file&&) = delete;
    auto operator=(tmp_file&&) -> tmp_file& = delete;
};

}  // namespace

TEST_CASE("manifest: glaze round-trip preserves all fields", "[motif-db][manifest]")
{
    motif::db::db_manifest const original {
        .name = "round-trip-db",
        .schema_version = 1,
        .game_count = 42,
        .created_at = "2026-04-18T12:00:00Z",
    };

    tmp_file const tmp {"roundtrip"};
    auto write_res = motif::db::write_manifest(tmp.path, original);
    REQUIRE(write_res.has_value());

    auto read_res = motif::db::read_manifest(tmp.path);
    REQUIRE(read_res.has_value());

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const& loaded = *read_res;
    CHECK(loaded.name == original.name);
    CHECK(loaded.schema_version == original.schema_version);
    CHECK(loaded.game_count == original.game_count);
    CHECK(loaded.created_at == original.created_at);
}

TEST_CASE("manifest: read_manifest returns not_found for missing file", "[motif-db][manifest]")
{
    auto const missing_path = std::filesystem::temp_directory_path() / "motif_manifest_does_not_exist_xyz.json";
    auto res = motif::db::read_manifest(missing_path);
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error() == motif::db::error_code::not_found);
}

TEST_CASE("manifest: make_manifest populates all fields with non-empty values", "[motif-db][manifest]")
{
    auto const manifest = motif::db::make_manifest("test-db");
    CHECK(manifest.name == "test-db");
    CHECK(manifest.schema_version == 1);
    CHECK(manifest.game_count == 0);
    CHECK_FALSE(manifest.created_at.empty());
}

TEST_CASE("manifest: round-trip with zero game_count and empty extra fields", "[motif-db][manifest]")
{
    motif::db::db_manifest const original {
        .name = "empty-db",
        .schema_version = 1,
        .game_count = 0,
        .created_at = "2026-01-01T00:00:00Z",
    };

    tmp_file const tmp {"zeros"};
    auto write_res = motif::db::write_manifest(tmp.path, original);
    REQUIRE(write_res.has_value());

    auto read_res = motif::db::read_manifest(tmp.path);
    REQUIRE(read_res.has_value());

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const& loaded = *read_res;
    CHECK(loaded.name == original.name);
    CHECK(loaded.schema_version == original.schema_version);
    CHECK(loaded.game_count == original.game_count);
    CHECK(loaded.created_at == original.created_at);
}
