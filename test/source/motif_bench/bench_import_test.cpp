// Scenario `import`: fresh import of the benchmark corpus with postings and the
// opening tree. Run one case per process (peak RSS is the process high-water
// mark) from a Release build; see scripts/bench_compare.py.

#include <cstdint>
#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "motif_bench/bench_support.hpp"

using motif_bench::corpus_path;
using motif_bench::emit;
using motif_bench::import_corpus;
using motif_bench::install_base_bundle;
using motif_bench::load_base_bundle;
using motif_bench::measure_bundle;
using motif_bench::ms_per_second;
using motif_bench::peak_rss_mib;
using motif_bench::reset_peak_rss;
using motif_bench::scoped_work_dir;
using motif_bench::skip_unless_benchmarkable;

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one measured scenario, reported linearly
TEST_CASE("bench: import", "[performance][bench]")
{
    skip_unless_benchmarkable();
    constexpr auto scenario = "import";

    auto const pgn = corpus_path();
    scoped_work_dir const work {"import"};
    auto const bundle = work.path() / "bundle";

    reset_peak_rss();
    auto const summary = import_corpus(pgn, bundle, work.path() / "logs");
    auto const peak_after_import = peak_rss_mib();

    auto const wall_ms = static_cast<double>(summary.elapsed.count());
    emit(scenario, "games_attempted", summary.total_attempted);
    emit(scenario, "games_committed", summary.committed);
    emit(scenario, "wall_ms", wall_ms);
    emit(scenario, "games_per_s", wall_ms > 0.0 ? static_cast<double>(summary.committed) / (wall_ms / ms_per_second) : 0.0);
    emit(scenario, "ingest_ms", summary.ingest_elapsed.count());
    emit(scenario, "dedup_ms", summary.dedup_elapsed.count());

    if (auto const& metrics = summary.position_postings_metrics; metrics.has_value()) {
        auto const total = metrics->replay_elapsed + metrics->spill_elapsed + metrics->merge_elapsed + metrics->final_write_elapsed;
        emit(scenario, "postings_ms", total.count());
        emit(scenario, "postings_replay_ms", metrics->replay_elapsed.count());
        emit(scenario, "postings_spill_ms", metrics->spill_elapsed.count());
        emit(scenario, "postings_merge_ms", metrics->merge_elapsed.count());
        emit(scenario, "postings_final_write_ms", metrics->final_write_elapsed.count());
        emit(scenario, "postings_peak_temp_bytes", metrics->peak_temp_bytes);
        emit(scenario, "postings_occurrences", metrics->occurrence_count);
        emit(scenario, "postings_distinct_hashes", metrics->distinct_hash_count);
    }
    if (auto const& metrics = summary.opening_tree_index_metrics; metrics.has_value()) {
        auto const total = metrics->replay_elapsed + metrics->root_merge_elapsed + metrics->edge_merge_elapsed
            + metrics->child_merge_elapsed + metrics->index_write_elapsed;
        emit(scenario, "tree_ms", total.count());
        emit(scenario, "tree_replay_ms", metrics->replay_elapsed.count());
        emit(scenario, "tree_root_merge_ms", metrics->root_merge_elapsed.count());
        emit(scenario, "tree_edge_merge_ms", metrics->edge_merge_elapsed.count());
        emit(scenario, "tree_child_merge_ms", metrics->child_merge_elapsed.count());
        emit(scenario, "tree_index_write_ms", metrics->index_write_elapsed.count());
        emit(scenario, "tree_edge_records", metrics->edge_record_count);
    }

    // The bundle was closed by import_corpus, so SQLite has checkpointed its
    // WAL and the sizes describe the retained bundle.
    auto const bytes = measure_bundle(bundle);
    emit(scenario, "size_games_db_bytes", bytes.games_db);
    emit(scenario, "size_postings_bytes", bytes.postings);
    emit(scenario, "size_tree_bytes", bytes.tree);
    emit(scenario, "size_other_bytes", bytes.other);
    emit(scenario, "size_total_bytes", bytes.total());
    emit(scenario, "peak_rss_mib", peak_after_import);

    // Seed the shared reference bundle for the other scenarios (untimed).
    if (!load_base_bundle(pgn).has_value()) {
        (void)install_base_bundle(bundle, pgn);
    }
}
