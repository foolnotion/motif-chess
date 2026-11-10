// Scenario `open`: time database_manager::open on the cached reference bundle
// and the first queries after it. The OS page cache is left alone (nothing is
// dropped), so "cold" means cold within this process: first query after open,
// first pass over the sample.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "motif_bench/bench_support.hpp"

using motif_bench::clock_type;
using motif_bench::emit;
using motif_bench::emit_latency_us;
using motif_bench::ensure_base_bundle;
using motif_bench::median_quantile;
using motif_bench::ms_since;
using motif_bench::navigation_pass_us;
using motif_bench::peak_rss_mib;
using motif_bench::percentile;
using motif_bench::reset_peak_rss;
using motif_bench::sample_navigation_hashes;
using motif_bench::skip_unless_benchmarkable;
using motif_bench::start_hash;
using motif_bench::time_query_us;
using motif_bench::tree_in_service;
using motif_bench::us_per_ms;
using motif_bench::warm_start_us;

namespace
{
constexpr std::size_t open_repeats = 5;
}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one measured scenario, reported linearly
TEST_CASE("bench: open", "[performance][bench]")
{
    skip_unless_benchmarkable();
    constexpr auto scenario = "open";

    auto const base = ensure_base_bundle();
    emit(scenario, "games", base.game_count);

    reset_peak_rss();
    std::vector<double> open_ms;
    for (std::size_t i = 0; i < open_repeats; ++i) {
        auto const begin = clock_type::now();
        auto manager = motif::db::database_manager::open(base.dir);
        open_ms.push_back(ms_since(begin));
        REQUIRE(manager.has_value());
        manager->close();
    }
    emit(scenario, "open_first_ms", open_ms.front());
    emit(scenario, "open_min_ms", *std::ranges::min_element(open_ms));
    emit(scenario, "open_median_ms", percentile(open_ms, median_quantile));

    // One more open, kept for the query measurements.
    auto const open_begin = clock_type::now();
    auto manager = motif::db::database_manager::open(base.dir);
    auto const kept_open_ms = ms_since(open_begin);
    REQUIRE(manager.has_value());
    // Presence of a descriptor is not enough: the tree must cover the current
    // mutation sequence, or this scenario would time the postings fallback.
    REQUIRE(tree_in_service(*manager));

    auto const first = time_query_us(*manager, start_hash());
    REQUIRE(first.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access) -- REQUIREd just above
    emit(scenario, "start_first_us", *first);
    emit(scenario, "open_to_first_query_ms", kept_open_ms + (*first / us_per_ms));
    // NOLINTEND(bugprone-unchecked-optional-access)
    emit(scenario, "start_warm_p50_us", percentile(warm_start_us(*manager), median_quantile));

    auto const sample = sample_navigation_hashes(*manager, start_hash());
    REQUIRE_FALSE(sample.empty());
    emit(scenario, "nav_sample_size", sample.size());
    // First pass is cold within the process; second is warm.
    emit_latency_us(scenario, "nav_cold", navigation_pass_us(*manager, sample));
    emit_latency_us(scenario, "nav_warm", navigation_pass_us(*manager, sample));

    emit(scenario, "peak_rss_mib", peak_rss_mib());
    manager->close();
}
