#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>

#include "motif/db/schema.hpp"

namespace {

// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto open_memory() -> sqlite3*
{
    sqlite3* conn = nullptr;  // NOLINT(misc-const-correctness) — sqlite3 API requires mutable ptr
    sqlite3_open(":memory:", &conn);
    return conn;
}

// Opens an on-disk SQLite connection in a temp file.
struct disk_db {
    std::filesystem::path path;
    sqlite3*              conn{nullptr};

    explicit disk_db(std::string const& suffix)
    {
        path = std::filesystem::temp_directory_path()
               / ("motif_schema_test_" + suffix + ".db");
        sqlite3_open(path.c_str(), &conn);
    }
    ~disk_db()
    {
        if (conn != nullptr) {
            sqlite3_close(conn);
        }
        std::filesystem::remove(path);
    }
    disk_db(disk_db const&)                    = delete;
    auto operator=(disk_db const&) -> disk_db& = delete;
    disk_db(disk_db&&)                         = delete;
    auto operator=(disk_db&&) -> disk_db&      = delete;
};

} // namespace

TEST_CASE("schema::initialize on a fresh on-disk database succeeds",
          "[motif-db][schema]")
{
    disk_db ddb{"init"};
    REQUIRE(ddb.conn != nullptr);

    auto res = motif::db::schema::initialize(ddb.conn);
    REQUIRE(res.has_value());
}

TEST_CASE("schema::initialize is idempotent — second call also succeeds",
          "[motif-db][schema]")
{
    disk_db ddb{"idempotent"};
    REQUIRE(ddb.conn != nullptr);

    auto first = motif::db::schema::initialize(ddb.conn);
    REQUIRE(first.has_value());

    auto second = motif::db::schema::initialize(ddb.conn);
    REQUIRE(second.has_value());
}

TEST_CASE("schema::version returns k_version after initialize",
          "[motif-db][schema]")
{
    disk_db ddb{"version"};
    REQUIRE(ddb.conn != nullptr);

    auto init = motif::db::schema::initialize(ddb.conn);
    REQUIRE(init.has_value());

    auto ver = motif::db::schema::version(ddb.conn);
    REQUIRE(ver.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(*ver == motif::db::schema::current_version);
}

TEST_CASE("schema::version on fresh connection returns 0 (not yet initialised)",
          "[motif-db][schema]")
{
    disk_db ddb{"zero"};
    REQUIRE(ddb.conn != nullptr);

    auto ver = motif::db::schema::version(ddb.conn);
    REQUIRE(ver.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(*ver == 0U);
}

TEST_CASE("schema::initialize on :memory: succeeds (WAL falls back to memory mode)",
          "[motif-db][schema]")
{
    sqlite3* conn = open_memory();
    REQUIRE(conn != nullptr);

    auto res = motif::db::schema::initialize(conn);
    // WAL is not applicable to :memory: but initialize must not fail.
    REQUIRE(res.has_value());

    sqlite3_close(conn);
}
