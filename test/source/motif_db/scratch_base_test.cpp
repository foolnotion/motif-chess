#include "motif/db/scratch_base.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("scratch_base::instance returns a valid object",
          "[motif-db][scratch_base]")
{
    auto& scratch = motif::db::scratch_base::instance();
    (void)scratch;
}

TEST_CASE("scratch_base::instance returns the same address on repeated calls",
          "[motif-db][scratch_base]")
{
    auto* first = &motif::db::scratch_base::instance();
    auto* second = &motif::db::scratch_base::instance();
    CHECK(first == second);
}

TEST_CASE("scratch_base::store().create_schema() succeeds",
          "[motif-db][scratch_base]")
{
    auto& scratch = motif::db::scratch_base::instance();
    auto res = scratch.store().create_schema();
    // create_schema is idempotent — may already exist from previous test
    CHECK(res.has_value());
}

TEST_CASE("scratch_base::positions().initialize_schema() succeeds",
          "[motif-db][scratch_base]")
{
    auto& scratch = motif::db::scratch_base::instance();
    auto res = scratch.positions().initialize_schema();
    CHECK(res.has_value());
}
