// Scenario `multi_open`: the intended deployment -- one large reference bundle
// plus several small personal bundles open in one process. Reports what the
// small bundles add to process RSS and to the SQLite page-cache lease sum
// (resource_governor), and whether queries on the big bundle slow down while
// they are open.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "motif/db/resource_governor.hpp"
#include "motif_bench/bench_support.hpp"

using motif_bench::clock_type;
using motif_bench::current_rss_mib;
using motif_bench::emit;
using motif_bench::ensure_base_bundle;
using motif_bench::load_games;
using motif_bench::median_quantile;
using motif_bench::ms_since;
using motif_bench::peak_rss_mib;
using motif_bench::percentile;
using motif_bench::reset_peak_rss;
using motif_bench::sample_game_ids;
using motif_bench::sample_navigation_hashes;
using motif_bench::scoped_work_dir;
using motif_bench::skip_unless_benchmarkable;
using motif_bench::start_hash;
using motif_bench::tail_quantile;
using motif_bench::warm_navigation_us;
using motif_bench::warm_start_us;

namespace
{
constexpr std::size_t small_bundle_count = 8;
constexpr std::size_t games_per_small_bundle = 200;
constexpr std::uint64_t donor_seed = 23;
}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one measured scenario, reported linearly
TEST_CASE("bench: multi_open", "[performance][bench]")
{
    skip_unless_benchmarkable();
    constexpr auto scenario = "multi_open";

    auto const base = ensure_base_bundle();
    scoped_work_dir const work {"multi_open"};

    // Preparation (untimed): the small bundles, each with its own games,
    // postings and opening tree, closed again before measurement starts.
    std::vector<std::filesystem::path> small_dirs;
    {
        auto source = motif::db::database_manager::open(base.dir);
        REQUIRE(source.has_value());
        auto donors = load_games(*source, sample_game_ids(*source, small_bundle_count * games_per_small_bundle, donor_seed));
        source->close();
        REQUIRE(donors.size() == small_bundle_count * games_per_small_bundle);

        for (std::size_t bundle = 0; bundle < small_bundle_count; ++bundle) {
            auto dir = work.path() / fmt::format("small-{}", bundle);
            auto manager = motif::db::database_manager::create(dir, fmt::format("small-{}", bundle));
            REQUIRE(manager.has_value());
            for (std::size_t i = 0; i < games_per_small_bundle; ++i) {
                REQUIRE(manager->insert_game(donors[(bundle * games_per_small_bundle) + i]).has_value());
            }
            REQUIRE(manager->rebuild_position_postings().has_value());
            REQUIRE(manager->rebuild_opening_tree_index().has_value());
            manager->close();
            small_dirs.push_back(std::move(dir));
        }
    }

    auto& governor = motif::db::resource_governor::process();
    emit(scenario, "sqlite_cache_in_use_start_bytes", governor.sqlite_cache_in_use());
    auto const rss_start = current_rss_mib();
    reset_peak_rss();

    // The big bundle alone.
    auto big = motif::db::database_manager::open(base.dir);
    REQUIRE(big.has_value());
    auto const sample = sample_navigation_hashes(*big, start_hash());
    REQUIRE_FALSE(sample.empty());
    auto const start_alone = percentile(warm_start_us(*big), median_quantile);
    auto const nav_alone = warm_navigation_us(*big, sample);
    auto const rss_alone = current_rss_mib();
    auto const cache_alone = governor.sqlite_cache_in_use();

    // Open all small bundles next to it and use each once.
    auto const open_begin = clock_type::now();
    std::vector<motif::db::database_manager> smalls;
    smalls.reserve(small_dirs.size());
    for (auto const& dir : small_dirs) {
        auto small = motif::db::database_manager::open(dir);
        REQUIRE(small.has_value());
        smalls.push_back(std::move(*small));
    }
    emit(scenario, "open_small_total_ms", ms_since(open_begin));

    std::vector<double> small_start_p50;
    small_start_p50.reserve(smalls.size());
    for (auto const& small : smalls) {
        small_start_p50.push_back(percentile(warm_start_us(small), median_quantile));
    }
    auto const start_with_small = percentile(warm_start_us(*big), median_quantile);
    auto const nav_with_small = warm_navigation_us(*big, sample);
    auto const rss_multi = current_rss_mib();
    auto const cache_multi = governor.sqlite_cache_in_use();

    emit(scenario, "small_bundles", smalls.size());
    emit(scenario, "rss_big_alone_mib", rss_alone - rss_start);
    emit(scenario, "rss_multi_mib", rss_multi - rss_start);
    emit(scenario, "rss_delta_mib", rss_multi - rss_alone);
    emit(scenario, "sqlite_cache_big_alone_bytes", cache_alone);
    emit(scenario, "sqlite_cache_in_use_bytes", cache_multi);
    emit(scenario, "big_start_p50_alone_us", start_alone);
    emit(scenario, "big_start_p50_with_small_us", start_with_small);
    emit(scenario, "big_nav_p50_alone_us", percentile(nav_alone, median_quantile));
    emit(scenario, "big_nav_p50_with_small_us", percentile(nav_with_small, median_quantile));
    emit(scenario, "big_nav_p99_with_small_us", percentile(nav_with_small, tail_quantile));
    emit(scenario, "small_start_p50_us", percentile(small_start_p50, median_quantile));
    emit(scenario, "peak_rss_mib", peak_rss_mib());

    for (auto& small : smalls) {
        small.close();
    }
    big->close();
}
