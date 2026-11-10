// Scenarios `mutate`, `segments` and `patch_chain`: cost of changing a large
// bundle and of getting its derived indexes (start-position stats,
// opening-tree navigation) answerable again. Every mutation kind runs on a
// fresh copy of the cached reference bundle.
//
// Insert and removal still invalidate the derived indexes, so queries fail
// until they are rebuilt; await_ready() (bench_support.hpp) detects that and
// calls the manager's rebuild entry points inside the timed window, recording
// them separately. A result/Elo correction (patch_elo_1, patch_elo_imported_1)
// is incremental since PR 6: one metadata run and one signed tree run are
// published in the edit's own FULL commit, so await_ready() finds nothing to
// rebuild, `incremental` is 1 and rebuild_*_ms are 0. When insert/removal
// become incremental the same code reports them the same way, with no change here.
//
// Metrics per kind K in {insert_1, insert_100, remove_1, patch_elo_1,
// patch_elo_imported_1, patch_meta_1}: K.mutate_ms (the call itself;
// patch_elo_1.patch_ms is the same number), K.ready_ms (mutation start until
// the start-position query and a full 512-position navigation pass answer
// again), K.rebuild_ms / rebuild_postings_ms / rebuild_tree_ms, K.incremental,
// K.nav_p50_us / nav_p99_us (warm, afterwards), K.postings_bytes / tree_bytes
// (derived index files afterwards), K.tree_patch_bytes / metadata_bytes (the
// per-edit chain files) and K.peak_rss_mib (this kind only, where
// /proc/self/clear_refs is available).
//
// `patch_chain` applies N = 1, 4, 8, 16 and 17 sequential Elo corrections and
// reports nav_p50_us after each count, patch_ms for each edit, compact_ms (the
// edit that crossed the 16-run hard cap and folded the chains) and open_median_ms
// for a bundle holding a full chain.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "motif_bench/bench_support.hpp"

using motif_bench::await_ready;
using motif_bench::base_bundle;
using motif_bench::clock_type;
using motif_bench::copy_bundle;
using motif_bench::emit;
using motif_bench::emit_latency_us;
using motif_bench::emit_readiness;
using motif_bench::ensure_base_bundle;
using motif_bench::env_string;
using motif_bench::load_games;
using motif_bench::make_unique_game;
using motif_bench::measure_bundle;
using motif_bench::median_quantile;
using motif_bench::ms_since;
using motif_bench::navigation_pass_us;
using motif_bench::peak_rss_mib;
using motif_bench::percentile;
using motif_bench::reset_peak_rss;
using motif_bench::sample_game_ids;
using motif_bench::sample_navigation_hashes;
using motif_bench::scoped_work_dir;
using motif_bench::skip_unless_benchmarkable;
using motif_bench::start_hash;
using motif_bench::tree_in_service;
using motif_bench::warm_navigation_us;
using motif_bench::warm_start_us;

namespace
{

constexpr std::uint64_t donor_seed = 11;
constexpr std::size_t donor_count = 128;
constexpr std::size_t large_insert_count = 100;
constexpr std::size_t segment_inserts = 10;
constexpr std::int32_t fallback_elo = 2000;
constexpr std::int32_t elo_patch_delta = 37;
constexpr std::size_t full_chain_count = 16;

struct fixture
{
    base_bundle base;
    std::vector<motif::db::zobrist_hash> sample;
    std::vector<motif::db::game> donors;
    motif::db::game_id imported_id;
    motif::db::game_patch manual_patch;
    motif::db::game_patch imported_patch;
};

// The reference bundle's manual game and one imported game each get a rating
// correction (the manual one also flips its result), chosen from the unmutated
// bundle.
void prepare_patches(motif::db::database_manager const& source, fixture& data)
{
    auto const ids = sample_game_ids(source, 1, donor_seed + 2U);
    REQUIRE(ids.size() == 1U);
    data.imported_id = ids.front();
    auto const imported = source.store().get(data.imported_id);
    auto const manual = source.store().get(data.base.manual_game);
    REQUIRE(imported.has_value());
    REQUIRE(imported->provenance.source_type == "imported");
    REQUIRE(manual.has_value());
    data.imported_patch.white_elo = motif::db::rating_patch::set(imported->white.elo.value_or(fallback_elo) + elo_patch_delta);
    data.manual_patch.white_elo = motif::db::rating_patch::set(manual->white.elo.value_or(fallback_elo) + elo_patch_delta);
    data.manual_patch.result = manual->result == "1-0" ? "0-1" : "1-0";
}

// Sampling and donor games come from the unmutated reference bundle, opened
// read-only, so every copy is measured against the same positions.
auto make_fixture() -> fixture
{
    fixture data;
    data.base = ensure_base_bundle();
    auto source = motif::db::database_manager::open(data.base.dir);
    REQUIRE(source.has_value());
    data.sample = sample_navigation_hashes(*source, start_hash());
    REQUIRE_FALSE(data.sample.empty());
    data.donors = load_games(*source, sample_game_ids(*source, donor_count, donor_seed));
    REQUIRE(data.donors.size() == donor_count);
    prepare_patches(*source, data);
    source->close();
    return data;
}

// MOTIF_BENCH_KINDS=insert_1,remove_1 restricts the mutation kinds (default:
// all), e.g. to save a 1M-game index rebuild per skipped kind.
auto kind_selected(std::string const& kind) -> bool
{
    auto const wanted = env_string("MOTIF_BENCH_KINDS", "");
    if (wanted.empty()) {
        return true;
    }
    return ("," + wanted + ",").find("," + kind + ",") != std::string::npos;
}

void run_kind(fixture const& data,
              std::string_view const scenario,
              std::string const& kind,
              std::function<void(motif::db::database_manager&)> const& mutation)
{
    if (!kind_selected(kind)) {
        return;
    }
    scoped_work_dir const work {kind};
    auto const copy = work.path() / "bundle";
    copy_bundle(data.base.dir, copy);

    auto manager = motif::db::database_manager::open(copy);
    REQUIRE(manager.has_value());
    bool const tree_expected = tree_in_service(*manager);

    // The bundle has served queries before it is edited, as in real use.
    (void)navigation_pass_us(*manager, data.sample);

    reset_peak_rss();
    auto const begin = clock_type::now();
    mutation(*manager);
    auto const mutate_ms = ms_since(begin);
    auto const report = await_ready(*manager, tree_expected, data.sample, begin);

    auto const prefix = kind + ".";
    emit(scenario, prefix + "mutate_ms", mutate_ms);
    if (kind.starts_with("patch_")) {
        emit(scenario, prefix + "patch_ms", mutate_ms);  // the edit call itself, by its patch name
    }
    emit_readiness(scenario, prefix, report);
    emit_latency_us(scenario, prefix + "nav", warm_navigation_us(*manager, data.sample));
    auto const bytes = measure_bundle(copy);
    emit(scenario, prefix + "postings_bytes", bytes.postings);
    emit(scenario, prefix + "tree_bytes", bytes.tree);
    emit(scenario, prefix + "tree_patch_bytes", bytes.tree_patches);
    emit(scenario, prefix + "metadata_bytes", bytes.metadata);
    emit(scenario, prefix + "peak_rss_mib", peak_rss_mib());
    manager->close();
}

void insert_games(motif::db::database_manager& manager, fixture const& data, std::string_view const tag, std::size_t const count)
{
    for (std::size_t i = 0; i < count; ++i) {
        auto game = make_unique_game(data.donors[i % data.donors.size()], tag, i);
        auto const inserted = manager.insert_game(game);
        REQUIRE(inserted.has_value());
    }
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one measured scenario, reported linearly
TEST_CASE("bench: mutate", "[performance][bench]")
{
    skip_unless_benchmarkable();
    constexpr auto scenario = "mutate";
    auto const data = make_fixture();
    auto const manual = data.base.manual_game;

    run_kind(data, scenario, "insert_1", [&](motif::db::database_manager& manager) -> void { insert_games(manager, data, "insert_1", 1); });

    run_kind(data,
             scenario,
             "insert_100",
             [&](motif::db::database_manager& manager) -> void { insert_games(manager, data, "insert_100", large_insert_count); });

    // The reference bundle holds one user-added game so removal and patches
    // target a game that editability rules allow.
    run_kind(data,
             scenario,
             "remove_1",
             [&](motif::db::database_manager& manager) -> void { REQUIRE(manager.remove_user_game(manual).has_value()); });

    run_kind(data,
             scenario,
             "patch_elo_1",
             [&](motif::db::database_manager& manager) -> void
             { REQUIRE(manager.patch_game_metadata(manual, data.manual_patch).has_value()); });

    // The same correction on a real imported game (not the manual fixture): imported
    // games accept result and rating corrections, and it must be just as incremental.
    run_kind(data,
             scenario,
             "patch_elo_imported_1",
             [&](motif::db::database_manager& manager) -> void
             { REQUIRE(manager.patch_game_metadata(data.imported_id, data.imported_patch).has_value()); });

    run_kind(data,
             scenario,
             "patch_meta_1",
             [&](motif::db::database_manager& manager) -> void
             {
                 motif::db::game_patch patch;
                 patch.event = "motif-bench patched event";
                 patch.site = "motif-bench patched site";
                 REQUIRE(manager.patch_game_metadata(manual, patch).has_value());
             });
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one measured scenario, reported linearly
TEST_CASE("bench: patch_chain", "[performance][bench]")
{
    // Sequential single-game Elo corrections on a copy of the base bundle, each a
    // separate incremental edit (one metadata run + one tree run per edit, then a
    // synchronous fold when the hard cap of 16 runs would be exceeded).
    //
    // Metrics: patch_<k>_ms (the k-th edit), n<N>.nav_p50_us / n<N>.nav_p99_us
    // (warm navigation after N edits, N in {1, 4, 8, 16, 17}), compact_ms (the
    // edit that folded the chain: the 17th with the default caps), tree_runs /
    // metadata_runs after each count, open_median_ms (opening a bundle that holds
    // a FULL 16-run chain) next to open_baseline_median_ms (the unedited base).
    skip_unless_benchmarkable();
    constexpr auto scenario = "patch_chain";
    constexpr std::array<std::size_t, 5> counts {1, 4, 8, 16, 17};
    constexpr std::size_t open_repeats = 5;
    auto const data = make_fixture();

    scoped_work_dir const work {"patch_chain"};
    auto const copy = work.path() / "bundle";
    copy_bundle(data.base.dir, copy);
    auto manager = motif::db::database_manager::open(copy);
    REQUIRE(manager.has_value());
    REQUIRE(tree_in_service(*manager));
    (void)navigation_pass_us(*manager, data.sample);

    // One edit per game, so every run carries different content; sampled from the
    // bundle itself (imported games: only result and ratings are editable on them).
    auto const targets = sample_game_ids(*manager, counts.back(), donor_seed + 1U);
    REQUIRE(targets.size() == counts.back());
    emit(scenario, "baseline_nav_p50_us", percentile(warm_navigation_us(*manager, data.sample), median_quantile));

    auto median_open_ms = [](std::filesystem::path const& dir) -> double
    {
        std::vector<double> samples;
        for (std::size_t i = 0; i < open_repeats; ++i) {
            auto const begin = clock_type::now();
            auto opened = motif::db::database_manager::open(dir);
            samples.push_back(ms_since(begin));
            REQUIRE(opened.has_value());
            opened->close();
        }
        return percentile(samples, median_quantile);
    };

    auto applied = std::size_t {0};
    auto compact_ms = 0.0;
    for (auto const count : counts) {
        while (applied < count) {
            auto const game_key = targets[applied];
            auto current = manager->store().get(game_key);
            REQUIRE(current.has_value());
            motif::db::game_patch patch;
            patch.white_elo =
                motif::db::rating_patch::set(current->white.elo.value_or(fallback_elo) + 1 + static_cast<std::int32_t>(applied));
            auto const begin = clock_type::now();
            REQUIRE(manager->patch_game_metadata(game_key, patch).has_value());
            auto const patch_ms = ms_since(begin);
            ++applied;
            emit(scenario, fmt::format("patch_{}_ms", applied), patch_ms);
            auto const& report = manager->last_patch_report();
            REQUIRE(report.published);
            if (report.tree_folded && compact_ms == 0.0) {
                compact_ms = patch_ms;  // the fold cost rides inside this edit
            }
        }
        auto const prefix = fmt::format("n{}", count);
        emit_latency_us(scenario, prefix + ".nav", warm_navigation_us(*manager, data.sample));
        emit(scenario, prefix + ".tree_runs", manager->last_patch_report().tree_runs);
        emit(scenario, prefix + ".metadata_runs", manager->last_patch_report().metadata_runs);
        if (count == full_chain_count) {
            // The chain is as long as it may get: a bundle that holds it is what
            // open() must be able to load. Copy it while the source is closed.
            manager->close();
            auto const full_chain = work.path() / "full_chain";
            copy_bundle(copy, full_chain);
            emit(scenario, "open_median_ms", median_open_ms(full_chain));
            emit(scenario, "open_baseline_median_ms", median_open_ms(data.base.dir));
            auto const bytes = measure_bundle(full_chain);
            emit(scenario, "chain_tree_patch_bytes", bytes.tree_patches);
            emit(scenario, "chain_metadata_bytes", bytes.metadata);
            manager = motif::db::database_manager::open(copy);
            REQUIRE(manager.has_value());
            (void)navigation_pass_us(*manager, data.sample);
        }
    }
    emit(scenario, "compact_ms", compact_ms);
    emit(scenario, "peak_rss_mib", peak_rss_mib());
    manager->close();
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one measured scenario, reported linearly
TEST_CASE("bench: segments", "[performance][bench]")
{
    skip_unless_benchmarkable();
    constexpr auto scenario = "segments";
    auto const data = make_fixture();

    scoped_work_dir const work {"segments"};
    auto const copy = work.path() / "bundle";
    copy_bundle(data.base.dir, copy);
    auto manager = motif::db::database_manager::open(copy);
    REQUIRE(manager.has_value());
    bool const tree_expected = tree_in_service(*manager);

    // Baseline on the unmutated copy: pooled warm latencies.
    auto const baseline_start = percentile(warm_start_us(*manager), median_quantile);
    auto const baseline_nav = percentile(warm_navigation_us(*manager, data.sample), median_quantile);
    emit(scenario, "baseline_start_p50_us", baseline_start);
    emit(scenario, "baseline_nav_p50_us", baseline_nav);

    // K sequential single-game inserts, each a separate mutation.
    reset_peak_rss();
    std::vector<double> insert_ms;
    auto const begin = clock_type::now();
    for (std::size_t i = 0; i < segment_inserts; ++i) {
        auto game = make_unique_game(data.donors[i % data.donors.size()], "segments", i);
        auto const insert_begin = clock_type::now();
        auto const inserted = manager->insert_game(game);
        insert_ms.push_back(ms_since(insert_begin));
        REQUIRE(inserted.has_value());
    }
    emit(scenario, "inserts", segment_inserts);
    emit(scenario, "insert_total_ms", ms_since(begin));
    emit(scenario, "insert_p50_ms", percentile(insert_ms, median_quantile));
    emit(scenario, "insert_max_ms", *std::ranges::max_element(insert_ms));

    auto const report = await_ready(*manager, tree_expected, data.sample, begin);
    emit_readiness(scenario, "", report);

    auto const after_start = percentile(warm_start_us(*manager), median_quantile);
    auto const after_nav = percentile(warm_navigation_us(*manager, data.sample), median_quantile);
    emit(scenario, "after_start_p50_us", after_start);
    emit(scenario, "after_nav_p50_us", after_nav);
    auto const start_ratio = baseline_start > 0.0 ? after_start / baseline_start : 0.0;
    auto const nav_ratio = baseline_nav > 0.0 ? after_nav / baseline_nav : 0.0;
    emit(scenario, "start_degradation_ratio", start_ratio);
    emit(scenario, "nav_degradation_ratio", nav_ratio);
    emit(scenario, "degradation_ratio", std::max(start_ratio, nav_ratio));

    auto const bytes = measure_bundle(copy);
    emit(scenario, "postings_files", bytes.postings_files);
    emit(scenario, "tree_files", bytes.tree_files);
    emit(scenario, "postings_bytes", bytes.postings);
    emit(scenario, "tree_bytes", bytes.tree);
    emit(scenario, "peak_rss_mib", peak_rss_mib());
    manager->close();
}
