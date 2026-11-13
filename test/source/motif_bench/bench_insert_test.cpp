// Scenario `insert_single`: write throughput into an empty small bundle.
//  - standalone database_manager::insert_game, one game at a time (per-insert
//    latency: tracks the durable ID allocator's one UPDATE per insert);
//  - the raw batch path (drop identity index, insert_raw, commit every
//    batch_size games, deduplicate), as the import pipeline uses it (tracks the
//    one UPDATE per batch commit).
// Games are sampled from the cached reference bundle so their shape is
// realistic. Env: MOTIF_BENCH_INSERT_N (default 20000: large enough that
// several WAL checkpoints fall inside the run, so max/total are stable) and
// MOTIF_BENCH_BATCH_N (default 10000 games, so the batch path commits ten
// times).

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
using motif_bench::env_size;
using motif_bench::load_games;
using motif_bench::median_quantile;
using motif_bench::ms_per_second;
using motif_bench::ms_since;
using motif_bench::peak_rss_mib;
using motif_bench::percentile;
using motif_bench::reset_peak_rss;
using motif_bench::sample_game_ids;
using motif_bench::scoped_work_dir;
using motif_bench::skip_unless_benchmarkable;
using motif_bench::us_since;

namespace
{
constexpr std::size_t default_single_games = 20000;
constexpr std::size_t default_batch_games = 10000;
constexpr std::size_t batch_size = 1000;
constexpr std::uint64_t sample_seed = 7;
}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one measured scenario, reported linearly
TEST_CASE("bench: insert_single", "[performance][bench]")
{
    skip_unless_benchmarkable();
    constexpr auto scenario = "insert_single";

    auto const single_n = env_size("MOTIF_BENCH_INSERT_N", default_single_games);
    auto const batch_n = env_size("MOTIF_BENCH_BATCH_N", default_batch_games);

    auto const base = ensure_base_bundle();
    std::vector<motif::db::game> games;
    {
        auto source = motif::db::database_manager::open(base.dir);
        REQUIRE(source.has_value());
        games = load_games(*source, sample_game_ids(*source, std::max(single_n, batch_n), sample_seed));
        source->close();
    }
    REQUIRE(games.size() >= std::min(single_n, batch_n));

    scoped_work_dir const work {"insert"};
    reset_peak_rss();

    // Standalone inserts through the manager.
    {
        auto manager = motif::db::database_manager::create(work.path() / "single", "bench-single");
        REQUIRE(manager.has_value());
        auto const count = std::min(single_n, games.size());
        std::vector<double> insert_us;
        insert_us.reserve(count);
        auto const begin = clock_type::now();
        for (std::size_t i = 0; i < count; ++i) {
            auto const insert_begin = clock_type::now();
            auto const inserted = manager->insert_game(games[i]);
            insert_us.push_back(us_since(insert_begin));
            REQUIRE(inserted.has_value());
        }
        auto const total_ms = ms_since(begin);
        emit(scenario, "single_games", count);
        emit(scenario, "single_total_ms", total_ms);
        emit(scenario, "single_per_s", static_cast<double>(count) / (total_ms / ms_per_second));
        emit_latency_us(scenario, "single", insert_us);
        emit(scenario, "single_max_us", *std::ranges::max_element(insert_us));
        manager->close();
    }

    // Raw batches: the import pipeline's write path.
    {
        auto manager = motif::db::database_manager::create(work.path() / "batch", "bench-batch");
        REQUIRE(manager.has_value());
        auto const count = std::min(batch_n, games.size());
        std::vector<double> commit_ms;
        auto const scope = manager->lock_generation();
        REQUIRE(manager->writer().drop_identity_index().has_value());

        auto const begin = clock_type::now();
        for (std::size_t first = 0; first < count; first += batch_size) {
            auto const last = std::min(count, first + batch_size);
            REQUIRE(manager->writer().begin_transaction().has_value());
            for (std::size_t i = first; i < last; ++i) {
                REQUIRE(manager->writer().insert_raw(games[i]).has_value());
            }
            auto const commit_begin = clock_type::now();
            REQUIRE(manager->writer().commit_transaction().has_value());
            commit_ms.push_back(ms_since(commit_begin));
        }
        auto const insert_ms = ms_since(begin);
        auto const dedup_begin = clock_type::now();
        REQUIRE(manager->writer().deduplicate().has_value());
        auto const dedup_ms = ms_since(dedup_begin);

        emit(scenario, "batch_games", count);
        emit(scenario, "batch_size", batch_size);
        emit(scenario, "batch_insert_ms", insert_ms);
        emit(scenario, "batch_games_per_s", static_cast<double>(count) / (insert_ms / ms_per_second));
        emit(scenario, "batch_commit_p50_ms", percentile(commit_ms, median_quantile));
        emit(scenario, "batch_dedup_ms", dedup_ms);
        emit(scenario, "batch_with_dedup_games_per_s", static_cast<double>(count) / ((insert_ms + dedup_ms) / ms_per_second));
        manager->close();
    }

    emit(scenario, "peak_rss_mib", peak_rss_mib());
}
