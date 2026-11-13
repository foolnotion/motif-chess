#include <cstdint>
#include <utility>
#include <vector>

#include "motif/db/resource_governor.hpp"

#include <catch2/catch_test_macros.hpp>

namespace
{
constexpr auto mib = std::uint64_t {1} << 20U;
constexpr auto total_budget = 100 * mib;
constexpr auto bundle_share = 40 * mib;
constexpr auto bundle_floor = 10 * mib;
constexpr auto lease_count = 5;

auto small_limits() -> motif::db::resource_limits
{
    return {.total_sqlite_cache_bytes = total_budget,
            .max_bundle_sqlite_cache_bytes = bundle_share,
            .min_bundle_sqlite_cache_bytes = bundle_floor};
}
}  // namespace

TEST_CASE("resource_governor grants the full per-bundle share while the budget lasts", "[motif-db][resource_governor]")
{
    motif::db::resource_governor governor {small_limits()};
    auto first = governor.acquire_sqlite_cache();
    auto second = governor.acquire_sqlite_cache();
    CHECK(first.bytes() == 40 * mib);
    CHECK(second.bytes() == 40 * mib);
    CHECK(governor.sqlite_cache_in_use() == 80 * mib);
}

TEST_CASE("resource_governor shrinks later grants and never drops below the floor", "[motif-db][resource_governor]")
{
    motif::db::resource_governor governor {small_limits()};
    std::vector<motif::db::sqlite_cache_lease> leases;
    leases.reserve(lease_count);
    for (auto index = 0; index < lease_count; ++index) {
        leases.push_back(governor.acquire_sqlite_cache());
    }
    // 40 + 40 + 20 + floor + floor
    CHECK(leases[2].bytes() == 20 * mib);
    CHECK(leases[3].bytes() == 10 * mib);
    CHECK(leases[4].bytes() == 10 * mib);
    CHECK(governor.sqlite_cache_in_use() == 120 * mib);
}

TEST_CASE("resource_governor returns a released share to the pool", "[motif-db][resource_governor]")
{
    motif::db::resource_governor governor {small_limits()};
    auto first = governor.acquire_sqlite_cache();
    {
        auto second = governor.acquire_sqlite_cache();
        auto third = governor.acquire_sqlite_cache();
        CHECK(third.bytes() == 20 * mib);
    }
    CHECK(governor.sqlite_cache_in_use() == 40 * mib);
    CHECK(governor.acquire_sqlite_cache().bytes() == 40 * mib);
}

TEST_CASE("resource_governor lease releases exactly once across moves", "[motif-db][resource_governor]")
{
    motif::db::resource_governor governor {small_limits()};
    auto lease = governor.acquire_sqlite_cache();
    auto moved = std::move(lease);
    CHECK(governor.sqlite_cache_in_use() == 40 * mib);
    moved = motif::db::sqlite_cache_lease {};
    CHECK(governor.sqlite_cache_in_use() == 0U);
}
