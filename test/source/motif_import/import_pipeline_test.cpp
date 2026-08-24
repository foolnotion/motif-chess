#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "motif/import/import_pipeline.hpp"

#include <catch2/catch_test_macros.hpp>
#include <duckdb.h>
#include <fmt/base.h>
#include <pgnlib/pgnlib.hpp>
#include <pgnlib/types.hpp>
#include <tl/expected.hpp>

#include "motif/chess/chess.hpp"
#include "motif/db/database_manager.hpp"
#include "motif/db/types.hpp"
#include "motif/import/checkpoint.hpp"
#include "motif/import/error.hpp"
#include "motif/import/logger.hpp"
#include "motif/import/pgn_helpers.hpp"
#include "motif/import/pgn_reader.hpp"
#include "test_helpers.hpp"

namespace
{

using test_helpers::is_sanitized_build;

constexpr auto k_three_game_pgn = R"pgn(
[Event "Test"]
[Site "?"]
[Date "2024.01.01"]
[Round "1"]
[White "A"]
[Black "B"]
[Result "1-0"]
[WhiteElo "2000"]
[BlackElo "1900"]

1. e4 e5 2. Nf3 Nc6 3. Bb5 1-0

[Event "Test"]
[Site "?"]
[Date "2024.01.02"]
[Round "1"]
[White "C"]
[Black "D"]
[Result "0-1"]
[WhiteElo "2100"]
[BlackElo "2200"]

1. d4 d5 2. c4 c6 0-1

[Event "Test"]
[Site "?"]
[Date "2024.01.03"]
[Round "1"]
[White "E"]
[Black "F"]
[Result "1/2-1/2"]

1. Nf3 Nf6 2. g3 g6 1/2-1/2
)pgn";

constexpr auto k_invalid_san_pgn = R"pgn(
[Event "Broken"]
[Site "?"]
[Date "2024.01.01"]
[Round "1"]
[White "A"]
[Black "B"]
[Result "1-0"]

1. NotAMove 1-0
)pgn";

constexpr auto k_valid_then_invalid_pgn = R"pgn(
[Event "Valid Event"]
[Site "?"]
[Date "2024.01.01"]
[Round "1"]
[White "Valid White"]
[Black "Valid Black"]
[Result "1-0"]

1. e4 e5 2. Nf3 Nc6 1-0

[Event "Broken Event"]
[Site "?"]
[Date "2024.01.02"]
[Round "2"]
[White "Broken White"]
[Black "Broken Black"]
[Result "0-1"]

1. NotAMove 0-1
)pgn";

constexpr auto k_all_invalid_pgn = R"pgn(
[Event "Broken Event 1"]
[Site "?"]
[Date "2024.01.01"]
[Round "1"]
[White "Broken White 1"]
[Black "Broken Black 1"]
[Result "1-0"]

1. NotAMove 1-0

[Event "Broken Event 2"]
[Site "?"]
[Date "2024.01.02"]
[Round "2"]
[White "Broken White 2"]
[Black "Broken Black 2"]
[Result "0-1"]

1. StillNotAMove 0-1
)pgn";

constexpr auto k_duplicate_and_invalid_pgn = R"pgn(
[Event "Repeat Event"]
[Site "?"]
[Date "2024.01.01"]
[Round "1"]
[White "Repeat White"]
[Black "Repeat Black"]
[Result "1-0"]

1. e4 e5 2. Nf3 Nc6 1-0

[Event "Repeat Event"]
[Site "?"]
[Date "2024.01.01"]
[Round "1"]
[White "Repeat White"]
[Black "Repeat Black"]
[Result "1-0"]

1. e4 e5 2. Nf3 Nc6 1-0

[Event "Broken Event"]
[Site "?"]
[Date "2024.01.02"]
[Round "2"]
[White "Broken White"]
[Black "Broken Black"]
[Result "0-1"]

1. NotAMove 0-1
)pgn";

constexpr motif::import::import_config k_single_worker {
    .num_workers = 1,
    .num_lines = 4,
    .batch_size = 2,
};
constexpr auto import_perf_limit_ms = std::int64_t {120'000};
constexpr std::size_t perf_batch_size {10'000};
constexpr double us_per_ms {1'000.0};
constexpr std::uint64_t query_sample_seed {42};

auto perf_pgn_path() -> std::filesystem::path
{
    // NOLINTNEXTLINE(concurrency-mt-unsafe) -- test-only env override read once
    auto const* const perf_pgn = std::getenv("MOTIF_IMPORT_PERF_PGN");
    if (perf_pgn != nullptr) {
        return std::filesystem::path {perf_pgn};
    }

    auto repo_local = std::filesystem::path {MOTIF_PROJECT_SOURCE_DIR} / "bench" / "data" / "twic-bench.pgn";
    if (std::filesystem::exists(repo_local)) {
        return repo_local;
    }

    repo_local = std::filesystem::path {MOTIF_PROJECT_SOURCE_DIR} / "bench" / "data" / "twic-1m.pgn";
    if (std::filesystem::exists(repo_local)) {
        return repo_local;
    }

    return "/data/chess/1m_games.pgn";
}

auto keep_perf_bundle() -> bool
{
    // NOLINTNEXTLINE(concurrency-mt-unsafe) -- test-only environment read
    return std::getenv("MOTIF_IMPORT_KEEP_DB") != nullptr;
}

auto perf_log_dir() -> std::filesystem::path
{
    return std::filesystem::temp_directory_path() / "motif-import-perf-logs";
}

auto make_temp_log_dir() -> std::filesystem::path
{
    static std::atomic_uint64_t counter {0};

    auto const suffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + std::to_string(counter.fetch_add(1));
    auto const dir = std::filesystem::temp_directory_path() / ("motif-import-test-" + suffix);
    std::filesystem::create_directories(dir);
    return dir;
}

auto run_perf_import(motif::import::import_config const& config) -> motif::import::result<motif::import::import_summary>
{
    auto const pgn_file = perf_pgn_path();
    if (!std::filesystem::exists(pgn_file)) {
        return tl::unexpected {motif::import::error_code::io_failure};
    }

    auto const tmp = std::filesystem::temp_directory_path() / "ipl_perf";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto mgr = motif::db::database_manager::create(tmp / "db", "perf");
    if (!mgr.has_value()) {
        std::filesystem::remove_all(tmp);
        return tl::unexpected {motif::import::error_code::io_failure};
    }

    std::filesystem::remove_all(perf_log_dir());
    auto init_log = motif::import::initialize_logging({.log_dir = perf_log_dir()});
    if (!init_log) {
        mgr->close();
        std::filesystem::remove_all(tmp);
        return tl::unexpected {motif::import::error_code::io_failure};
    }

    motif::import::import_pipeline pipeline {*mgr};
    auto summary = pipeline.run(pgn_file, config);

    auto const shutdown_result = motif::import::shutdown_logging();
    (void)shutdown_result;
    mgr->close();
    if (!keep_perf_bundle()) {
        std::filesystem::remove_all(tmp);
    }
    return summary;
}

void print_import_perf_summary(std::string_view const label, motif::import::import_summary const& summary)
{
    fmt::print("\n=== {} ===\n"
               "  attempted:    {}\n"
               "  committed:    {}\n"
               "  skipped:      {}\n"
               "  errors:       {}\n"
               "  ingest:       {} ms\n"
               "  replay:       {} ms\n"
               "  sort:         {} ms\n"
               "  rollup:       {} ms\n"
               "  elapsed:      {} ms\n",
               label, summary.total_attempted, summary.committed, summary.skipped,
               summary.errors, summary.ingest_elapsed.count(), summary.position_replay_elapsed.count(),
               summary.sort_elapsed.count(), summary.rollup_elapsed.count(), summary.elapsed.count());
}

void print_duration_result(std::string_view const label, std::chrono::milliseconds const elapsed)
{
    fmt::print("\n=== {} ===\n" "  elapsed:      {} ms\n", label, elapsed.count());
}

void print_position_rebuild_timing(motif::db::position_rebuild_timing const& timing)
{
    fmt::print("  position replay: {} ms\n"
               "  position rows:   {} ms\n"
               "  position insert: {} ms\n"
               "  sort:            {} ms\n"
               "  rollup:          {} ms\n",
               timing.position_replay_elapsed.count(), timing.position_row_build_elapsed.count(), timing.position_insert_elapsed.count(),
               timing.sort_elapsed.count(), timing.rollup_elapsed.count());
}

void check_release_calibrated_perf(std::int64_t const elapsed_ms)
{
    CHECK(elapsed_ms < import_perf_limit_ms);
}

void skip_perf_unless_release_build()
{
    if (is_sanitized_build) {
        SKIP("performance checks are skipped in sanitize builds");
    }

#ifndef NDEBUG
    SKIP("performance checks run only in release builds");
#endif
}

auto serial_fast_path_candidate_config() -> motif::import::import_config
{
    return motif::import::import_config {
        .num_workers = 1,
        .num_lines = 1,
        .rebuild_positions_after_import = true,
        .sort_positions_by_zobrist_after_rebuild = true,
        .batch_size = motif::import::import_config::default_batch_size,
    };
}

constexpr std::size_t wide_parallel_lines_per_worker = 16;

auto wide_parallel_config(std::size_t const workers) -> motif::import::import_config
{
    return motif::import::import_config {
        .num_workers = workers,
        .num_lines = workers * wide_parallel_lines_per_worker,
        .rebuild_positions_after_import = true,
        .sort_positions_by_zobrist_after_rebuild = true,
        .batch_size = motif::import::import_config::default_batch_size,
    };
}

auto sqlite_only_serial_perf_config() -> motif::import::import_config
{
    return motif::import::import_config {
        .num_workers = 1,
        .num_lines = 1,
        .rebuild_positions_after_import = false,
        .batch_size = perf_batch_size,
    };
}

auto sqlite_rebuild_no_index_perf_config() -> motif::import::import_config
{
    return motif::import::import_config {
        .num_workers = 1,
        .num_lines = 1,
        .rebuild_positions_after_import = true,
        .sort_positions_by_zobrist_after_rebuild = false,
        .batch_size = perf_batch_size,
    };
}

auto sqlite_rebuild_sorted_no_index_perf_config() -> motif::import::import_config
{
    return motif::import::import_config {
        .num_workers = 1,
        .num_lines = 1,
        .rebuild_positions_after_import = true,
        .sort_positions_by_zobrist_after_rebuild = true,
        .batch_size = perf_batch_size,
    };
}

auto run_sqlite_then_rebuild_perf() -> motif::import::result<std::chrono::milliseconds>
{
    auto const pgn_file = perf_pgn_path();
    if (!std::filesystem::exists(pgn_file)) {
        return tl::unexpected {motif::import::error_code::io_failure};
    }

    auto const tmp = std::filesystem::temp_directory_path() / "ipl_perf";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto mgr = motif::db::database_manager::create(tmp / "db", "perf");
    if (!mgr.has_value()) {
        std::filesystem::remove_all(tmp);
        return tl::unexpected {motif::import::error_code::io_failure};
    }

    std::filesystem::remove_all(perf_log_dir());
    auto init_log = motif::import::initialize_logging({.log_dir = perf_log_dir()});
    if (!init_log) {
        mgr->close();
        std::filesystem::remove_all(tmp);
        return tl::unexpected {motif::import::error_code::io_failure};
    }

    motif::import::import_pipeline pipeline {*mgr};
    auto import_summary = pipeline.run(pgn_file, sqlite_only_serial_perf_config());
    if (!import_summary.has_value()) {
        auto const shutdown_result = motif::import::shutdown_logging();
        (void)shutdown_result;
        mgr->close();
        std::filesystem::remove_all(tmp);
        return tl::unexpected {import_summary.error()};
    }

    auto const rebuild_start = std::chrono::steady_clock::now();
    auto rebuild_timing = motif::db::position_rebuild_timing {};
    auto rebuild_res = mgr->rebuild_position_store(/*sort_by_zobrist=*/true, &rebuild_timing);
    auto const rebuild_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - rebuild_start);
    if (!rebuild_res.has_value()) {
        auto const shutdown_result = motif::import::shutdown_logging();
        (void)shutdown_result;
        mgr->close();
        std::filesystem::remove_all(tmp);
        return tl::unexpected {motif::import::error_code::io_failure};
    }
    print_position_rebuild_timing(rebuild_timing);

    auto const shutdown_result = motif::import::shutdown_logging();
    (void)shutdown_result;
    mgr->close();
    if (!keep_perf_bundle()) {
        std::filesystem::remove_all(tmp);
    }
    return import_summary->elapsed + rebuild_elapsed;
}

auto run_rebuild_only_perf() -> motif::import::result<std::chrono::milliseconds>
{
    auto const pgn_file = perf_pgn_path();
    if (!std::filesystem::exists(pgn_file)) {
        return tl::unexpected {motif::import::error_code::io_failure};
    }

    auto const tmp = std::filesystem::temp_directory_path() / "ipl_perf";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto mgr = motif::db::database_manager::create(tmp / "db", "perf");
    if (!mgr.has_value()) {
        std::filesystem::remove_all(tmp);
        return tl::unexpected {motif::import::error_code::io_failure};
    }

    std::filesystem::remove_all(perf_log_dir());
    auto init_log = motif::import::initialize_logging({.log_dir = perf_log_dir()});
    if (!init_log) {
        mgr->close();
        std::filesystem::remove_all(tmp);
        return tl::unexpected {motif::import::error_code::io_failure};
    }

    motif::import::import_pipeline pipeline {*mgr};
    auto import_summary = pipeline.run(pgn_file, sqlite_only_serial_perf_config());
    if (!import_summary.has_value()) {
        auto const shutdown_result = motif::import::shutdown_logging();
        (void)shutdown_result;
        mgr->close();
        std::filesystem::remove_all(tmp);
        return tl::unexpected {import_summary.error()};
    }

    auto const rebuild_start = std::chrono::steady_clock::now();
    auto rebuild_timing = motif::db::position_rebuild_timing {};
    auto rebuild_res = mgr->rebuild_position_store(/*sort_by_zobrist=*/true, &rebuild_timing);
    auto const rebuild_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - rebuild_start);
    if (!rebuild_res.has_value()) {
        auto const shutdown_result = motif::import::shutdown_logging();
        (void)shutdown_result;
        mgr->close();
        std::filesystem::remove_all(tmp);
        return tl::unexpected {motif::import::error_code::io_failure};
    }
    print_position_rebuild_timing(rebuild_timing);

    auto const shutdown_result = motif::import::shutdown_logging();
    (void)shutdown_result;
    mgr->close();
    if (!keep_perf_bundle()) {
        std::filesystem::remove_all(tmp);
    }
    return rebuild_elapsed;
}

}  // namespace

TEST_CASE("import_pipeline: run imports games and deletes checkpoint on success", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "ipl_run";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto const pgn_file = tmp / "games.pgn";
    {
        std::ofstream out {pgn_file};
        out << k_three_game_pgn;
    }

    auto mgr = motif::db::database_manager::create(tmp / "db", "test");
    REQUIRE(mgr.has_value());

    motif::import::import_pipeline pipeline {*mgr};
    auto summary = pipeline.run(pgn_file, k_single_worker);
    REQUIRE(summary.has_value());
    CHECK(summary->committed == 3);
    CHECK(summary->skipped == 0);
    CHECK_FALSE(std::filesystem::exists(motif::import::checkpoint_path(mgr->dir())));
    auto row_count = mgr->positions().row_count();
    REQUIRE(row_count.has_value());
    CHECK(*row_count == 16);

    mgr->close();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: inline path row count matches rebuild path", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "ipl_parity";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto const pgn_file = tmp / "games.pgn";
    {
        std::ofstream out {pgn_file};
        out << k_three_game_pgn;
    }

    auto mgr_a = motif::db::database_manager::create(tmp / "db_a", "test");
    REQUIRE(mgr_a.has_value());
    motif::import::import_pipeline pipeline_a {*mgr_a};
    auto summary_a = pipeline_a.run(pgn_file, motif::import::import_config {});
    REQUIRE(summary_a.has_value());
    auto const row_count_a = mgr_a->positions().row_count();
    REQUIRE(row_count_a.has_value());
    mgr_a->close();

    constexpr motif::import::import_config rebuild_config {
        .num_workers = 1,
        .num_lines = 4,
        .rebuild_positions_after_import = false,
        .batch_size = 2,
    };
    auto mgr_b = motif::db::database_manager::create(tmp / "db_b", "test");
    REQUIRE(mgr_b.has_value());
    motif::import::import_pipeline pipeline_b {*mgr_b};
    auto summary_b = pipeline_b.run(pgn_file, rebuild_config);
    REQUIRE(summary_b.has_value());
    REQUIRE(mgr_b->rebuild_position_store().has_value());
    auto const row_count_b = mgr_b->positions().row_count();
    REQUIRE(row_count_b.has_value());
    mgr_b->close();

    CHECK(summary_a->committed == summary_b->committed);
    CHECK(*row_count_a == *row_count_b);

    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: inline import builds opening rollups without sorting", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "ipl_inline_rollups_unsorted";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto const pgn_file = tmp / "games.pgn";
    {
        std::ofstream out {pgn_file};
        out << k_three_game_pgn;
    }

    auto mgr = motif::db::database_manager::create(tmp / "db", "test");
    REQUIRE(mgr.has_value());

    constexpr motif::import::import_config config {
        .num_workers = 1,
        .num_lines = 4,
        .rebuild_positions_after_import = true,
        .sort_positions_by_zobrist_after_rebuild = false,
        .batch_size = 2,
    };
    motif::import::import_pipeline pipeline {*mgr};
    auto const summary = pipeline.run(pgn_file, config);
    REQUIRE(summary.has_value());
    REQUIRE(summary->committed == 3);

    mgr->close();

    duckdb_database database {};
    auto const db_path = (tmp / "db" / "positions.duckdb").string();
    REQUIRE(duckdb_open(db_path.c_str(), &database) == DuckDBSuccess);
    duckdb_connection con {};
    REQUIRE(duckdb_connect(database, &con) == DuckDBSuccess);
    constexpr auto rollup_count_sql = R"sql(
        SELECT COUNT(*) = 1
        FROM information_schema.tables
        WHERE table_schema = 'main'
          AND table_name = 'opening_continuation'
    )sql";
    duckdb_result result {};
    REQUIRE(duckdb_query(con, rollup_count_sql, &result) == DuckDBSuccess);
    CHECK(duckdb_value_boolean(&result, 0, 0));
    duckdb_destroy_result(&result);
    duckdb_disconnect(&con);
    duckdb_close(&database);

    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: resume skips already-committed games (duplicate policy)", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "ipl_resume";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto const pgn_file = tmp / "games.pgn";
    {
        std::ofstream out {pgn_file};
        out << k_three_game_pgn;
    }

    auto mgr = motif::db::database_manager::create(tmp / "db", "test");
    REQUIRE(mgr.has_value());

    motif::import::import_pipeline pipeline {*mgr};

    constexpr motif::import::import_config big_batch {
        .num_workers = 1,
        .num_lines = 4,
        .batch_size = 500,
    };

    auto first_run = pipeline.run(pgn_file, big_batch);
    REQUIRE(first_run.has_value());
    CHECK(first_run->committed == 3);

    // Write a checkpoint at offset 0 (forces resume to re-read from start)
    auto const source_stat = motif::import::stat_source(pgn_file);
    REQUIRE(source_stat.has_value());
    auto const source_hash = motif::import::hash_source(pgn_file);
    REQUIRE(source_hash.has_value());
    motif::import::import_checkpoint const fake_chk {
        .source_path = pgn_file.string(),
        .byte_offset = 0,
        .games_committed = 0,
        .last_game_id = 0,
        .source_size = source_stat->size,
        .source_mtime_ns = source_stat->mtime_ns,
        .source_content_hash = *source_hash,
    };
    REQUIRE(motif::import::write_checkpoint(mgr->dir(), fake_chk).has_value());

    // Resume: all 3 games are duplicates → none newly committed
    auto second_run = pipeline.resume(pgn_file, big_batch);
    REQUIRE(second_run.has_value());
    CHECK(second_run->committed == 0);
    CHECK(second_run->skipped == 3);

    auto const row_count = mgr->positions().row_count();
    REQUIRE(row_count.has_value());
    CHECK(*row_count == 16);

    mgr->close();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: run resumes a matching checkpoint", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "ipl_run_resume";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto const pgn_file = tmp / "games.pgn";
    {
        std::ofstream out {pgn_file};
        out << k_three_game_pgn;
    }

    auto mgr = motif::db::database_manager::create(tmp / "db", "test");
    REQUIRE(mgr.has_value());
    motif::import::import_pipeline pipeline {*mgr};
    REQUIRE(pipeline.run(pgn_file, k_single_worker).has_value());
    auto const source_stat = motif::import::stat_source(pgn_file);
    REQUIRE(source_stat.has_value());
    auto const source_hash = motif::import::hash_source(pgn_file);
    REQUIRE(source_hash.has_value());
    REQUIRE(motif::import::write_checkpoint(mgr->dir(),
                                            {.source_path = pgn_file.string(),
                                             .byte_offset = 0,
                                             .games_committed = 0,
                                             .last_game_id = 0,
                                             .source_size = source_stat->size,
                                             .source_mtime_ns = source_stat->mtime_ns,
                                             .source_content_hash = *source_hash})
                .has_value());

    auto const summary = pipeline.run(pgn_file, k_single_worker);
    REQUIRE(summary.has_value());
    CHECK(summary->committed == 0);
    CHECK(summary->skipped == 3);

    mgr->close();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: resume repairs an interrupted raw import", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "ipl_resume_raw_recovery";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto const pgn_file = tmp / "games.pgn";
    {
        std::ofstream out {pgn_file};
        out << k_three_game_pgn;
    }

    auto mgr = motif::db::database_manager::create(tmp / "db", "test");
    REQUIRE(mgr.has_value());

    // Simulate a power loss after a raw batch commits: the game survives but
    // the identity index has not yet been recreated by deduplicate().
    motif::db::game const interrupted_game {
        .white = {.name = "Interrupted White", .elo = std::nullopt, .title = std::nullopt, .country = std::nullopt},
        .black = {.name = "Interrupted Black", .elo = std::nullopt, .title = std::nullopt, .country = std::nullopt},
        .event_details = std::nullopt,
        .date = std::nullopt,
        .result = "1-0",
        .eco = std::nullopt,
        .moves = {},
        .extra_tags = {},
        .provenance = {},
    };
    REQUIRE(mgr->writer().drop_identity_index().has_value());
    auto const interrupted_id = mgr->writer().insert_raw(interrupted_game);
    REQUIRE(interrupted_id.has_value());
    REQUIRE_FALSE(*mgr->writer().identity_index_exists());

    auto const source_stat = motif::import::stat_source(pgn_file);
    REQUIRE(source_stat.has_value());
    auto const source_hash = motif::import::hash_source(pgn_file);
    REQUIRE(source_hash.has_value());
    motif::import::import_checkpoint const checkpoint {
        .source_path = pgn_file.string(),
        .byte_offset = 0,
        .games_committed = 1,
        .last_game_id = static_cast<std::int64_t>(interrupted_id->value),
        .source_size = source_stat->size,
        .source_mtime_ns = source_stat->mtime_ns,
        .source_content_hash = *source_hash,
    };
    REQUIRE(motif::import::write_checkpoint(mgr->dir(), checkpoint).has_value());

    motif::import::import_pipeline pipeline {*mgr};
    auto const summary = pipeline.resume(pgn_file, k_single_worker);
    REQUIRE(summary.has_value());
    CHECK(summary->committed == 3);

    auto const game_count = mgr->store().count_games();
    REQUIRE(game_count.has_value());
    CHECK(*game_count == 4);
    REQUIRE(mgr->writer().identity_index_exists().has_value());
    CHECK(*mgr->writer().identity_index_exists());

    auto const position_count = mgr->positions().row_count();
    REQUIRE(position_count.has_value());
    CHECK(*position_count == 16);

    mgr->close();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: resume rejects a checkpoint offset strictly beyond EOF", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "ipl_resume_past_eof";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto const pgn_file = tmp / "games.pgn";
    {
        std::ofstream out {pgn_file};
        out << k_three_game_pgn;
    }

    auto mgr = motif::db::database_manager::create(tmp / "db", "test");
    REQUIRE(mgr.has_value());

    auto const source_stat = motif::import::stat_source(pgn_file);
    REQUIRE(source_stat.has_value());
    auto const source_hash = motif::import::hash_source(pgn_file);
    REQUIRE(source_hash.has_value());

    // Exact EOF remains a valid resume point (nothing left to read).
    motif::import::import_checkpoint const at_eof {
        .source_path = pgn_file.string(),
        .byte_offset = source_stat->size,
        .games_committed = 3,
        .last_game_id = 3,
        .source_size = source_stat->size,
        .source_mtime_ns = source_stat->mtime_ns,
        .source_content_hash = *source_hash,
    };
    REQUIRE(motif::import::write_checkpoint(mgr->dir(), at_eof).has_value());
    motif::import::import_pipeline at_eof_pipeline {*mgr};
    auto const at_eof_result = at_eof_pipeline.resume(pgn_file, k_single_worker);
    REQUIRE(at_eof_result.has_value());
    CHECK(at_eof_result->committed == 0);

    // Strictly beyond EOF must be rejected outright.
    motif::import::import_checkpoint const past_eof {
        .source_path = pgn_file.string(),
        .byte_offset = source_stat->size + 1,
        .games_committed = 3,
        .last_game_id = 3,
        .source_size = source_stat->size,
        .source_mtime_ns = source_stat->mtime_ns,
        .source_content_hash = *source_hash,
    };
    REQUIRE(motif::import::write_checkpoint(mgr->dir(), past_eof).has_value());
    motif::import::import_pipeline past_eof_pipeline {*mgr};
    auto const past_eof_result = past_eof_pipeline.resume(pgn_file, k_single_worker);
    REQUIRE_FALSE(past_eof_result.has_value());
    CHECK(past_eof_result.error() == motif::import::error_code::invalid_state);

    mgr->close();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: resume rejects a source file mutated at the same path", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "ipl_resume_mutated_source";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto const pgn_file = tmp / "games.pgn";
    {
        std::ofstream out {pgn_file};
        out << k_three_game_pgn;
    }

    auto mgr = motif::db::database_manager::create(tmp / "db", "test");
    REQUIRE(mgr.has_value());

    // Checkpoint the source's identity as of an (imagined) earlier partial
    // import, matching its current size/mtime.
    auto const original_stat = motif::import::stat_source(pgn_file);
    REQUIRE(original_stat.has_value());
    auto const original_hash = motif::import::hash_source(pgn_file);
    REQUIRE(original_hash.has_value());
    motif::import::import_checkpoint const checkpoint {
        .source_path = pgn_file.string(),
        .byte_offset = 0,
        .games_committed = 0,
        .last_game_id = 0,
        .source_size = original_stat->size,
        .source_mtime_ns = original_stat->mtime_ns,
        .source_content_hash = *original_hash,
    };
    REQUIRE(motif::import::write_checkpoint(mgr->dir(), checkpoint).has_value());

    // Simulate the source being edited/replaced at the same path after the
    // checkpoint was written -- e.g. a user re-exporting an edited PGN to the
    // same location before resuming.
    {
        std::ofstream out {pgn_file, std::ios::trunc};
        out << k_three_game_pgn;
        out << k_three_game_pgn;  // different size and content than before
    }

    motif::import::import_pipeline pipeline {*mgr};
    auto result = pipeline.resume(pgn_file, k_single_worker);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == motif::import::error_code::invalid_state);

    mgr->close();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: resume rejects same-size source mutation with preserved mtime", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "ipl_resume_same_size_mutation";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto const pgn_file = tmp / "games.pgn";
    {
        std::ofstream out {pgn_file};
        out << k_three_game_pgn;
    }

    auto mgr = motif::db::database_manager::create(tmp / "db", "test");
    REQUIRE(mgr.has_value());
    auto const source_stat = motif::import::stat_source(pgn_file);
    auto const source_hash = motif::import::hash_source(pgn_file);
    REQUIRE(source_stat.has_value());
    REQUIRE(source_hash.has_value());
    REQUIRE(motif::import::write_checkpoint(mgr->dir(),
                                            {.source_path = pgn_file.string(),
                                             .source_size = source_stat->size,
                                             .source_mtime_ns = source_stat->mtime_ns,
                                             .source_content_hash = *source_hash})
                .has_value());

    auto replacement = std::string {k_three_game_pgn};
    auto const marker = replacement.find("[White \"A\"]");
    REQUIRE(marker != std::string::npos);
    replacement.replace(marker, std::string_view {"[White \"A\"]"}.size(), "[White \"Z\"]");
    {
        std::ofstream out {pgn_file, std::ios::trunc};
        out << replacement;
    }
    REQUIRE(std::filesystem::file_size(pgn_file) == source_stat->size);
    std::filesystem::last_write_time(pgn_file, std::filesystem::file_time_type {std::chrono::nanoseconds {source_stat->mtime_ns}});

    motif::import::import_pipeline pipeline {*mgr};
    auto const result = pipeline.resume(pgn_file, k_single_worker);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == motif::import::error_code::invalid_state);

    mgr->close();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: resume returns io_failure when no checkpoint exists", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "ipl_nochk";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto const pgn_file = tmp / "games.pgn";
    {
        std::ofstream out {pgn_file};
        out << k_three_game_pgn;
    }

    auto mgr = motif::db::database_manager::create(tmp / "db", "test");
    REQUIRE(mgr.has_value());

    motif::import::import_pipeline pipeline {*mgr};
    auto result = pipeline.resume(pgn_file);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == motif::import::error_code::io_failure);

    mgr->close();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: resume rejects checkpoints for a different source file", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "ipl_wrong_source";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto const first_pgn = tmp / "first.pgn";
    {
        std::ofstream out {first_pgn};
        out << k_three_game_pgn;
    }

    auto const second_pgn = tmp / "second.pgn";
    {
        std::ofstream out {second_pgn};
        out << k_three_game_pgn;
    }

    auto mgr = motif::db::database_manager::create(tmp / "db", "test");
    REQUIRE(mgr.has_value());

    motif::import::import_checkpoint const checkpoint {
        .source_path = first_pgn.string(),
        .byte_offset = 0,
        .games_committed = 0,
        .last_game_id = 0,
    };
    REQUIRE(motif::import::write_checkpoint(mgr->dir(), checkpoint).has_value());

    motif::import::import_pipeline pipeline {*mgr};
    auto result = pipeline.resume(second_pgn, k_single_worker);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == motif::import::error_code::invalid_state);

    mgr->close();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: progress reflects committed count after run", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "ipl_prog";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto const pgn_file = tmp / "games.pgn";
    {
        std::ofstream out {pgn_file};
        out << k_three_game_pgn;
    }

    auto mgr = motif::db::database_manager::create(tmp / "db", "test");
    REQUIRE(mgr.has_value());

    motif::import::import_pipeline pipeline {*mgr};
    auto summary = pipeline.run(pgn_file, k_single_worker);
    REQUIRE(summary.has_value());

    auto prog = pipeline.progress();
    CHECK(prog.games_committed == summary->committed);
    CHECK(prog.games_processed >= prog.games_committed);
    CHECK(prog.total_games == 3);

    mgr->close();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: progress is empty before the first run", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "ipl_prog_init";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto mgr = motif::db::database_manager::create(tmp / "db", "test");
    REQUIRE(mgr.has_value());

    motif::import::import_pipeline const pipeline {*mgr};
    auto const prog = pipeline.progress();
    CHECK(prog.games_processed == 0);
    CHECK(prog.games_committed == 0);
    CHECK(prog.games_skipped == 0);
    CHECK(prog.errors == 0);
    CHECK(prog.total_games == 0);
    CHECK(prog.elapsed == std::chrono::milliseconds {0});

    mgr->close();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: parse errors count as one attempted game", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "ipl_parse_once";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto const pgn_file = tmp / "games.pgn";
    {
        std::ofstream out {pgn_file};
        out << k_invalid_san_pgn;
    }

    auto mgr = motif::db::database_manager::create(tmp / "db", "test");
    REQUIRE(mgr.has_value());

    motif::import::import_pipeline pipeline {*mgr};
    auto summary = pipeline.run(pgn_file, k_single_worker);
    REQUIRE(summary.has_value());
    CHECK(summary->total_attempted == 1);
    CHECK(summary->committed == 0);
    CHECK(summary->skipped == 1);
    CHECK(summary->errors == 1);

    mgr->close();
    std::filesystem::remove_all(tmp);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("import_pipeline: malformed game is skipped and logged with headers", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "ipl_malformed_log";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto const pgn_file = tmp / "games.pgn";
    {
        std::ofstream out {pgn_file};
        out << k_valid_then_invalid_pgn;
    }

    auto mgr = motif::db::database_manager::create(tmp / "db", "test");
    REQUIRE(mgr.has_value());

    auto const log_dir = make_temp_log_dir();
    auto init_log = motif::import::initialize_logging({.log_dir = log_dir});
    REQUIRE(init_log.has_value());

    motif::import::import_pipeline pipeline {*mgr};
    auto summary = pipeline.run(pgn_file, k_single_worker);
    REQUIRE(summary.has_value());
    CHECK(summary->total_attempted == 2);
    CHECK(summary->committed == 1);
    CHECK(summary->skipped == 1);
    CHECK(summary->errors == 1);

    auto const shutdown_log = motif::import::shutdown_logging();
    REQUIRE(shutdown_log.has_value());

    auto log_file = std::ifstream {log_dir / "motif-chess.log"};
    REQUIRE(log_file.is_open());

    auto log_contents = std::string {};
    for (auto line = std::string {}; std::getline(log_file, line);) {
        log_contents += line;
        log_contents.push_back('\n');
    }

    CHECK(log_contents.contains("Skipped game at offset"));
    CHECK(log_contents.contains("parse_error"));
    CHECK(log_contents.contains("Broken White"));
    CHECK(log_contents.contains("Broken Black"));
    CHECK(log_contents.contains("Broken Event"));

    mgr->close();
    std::filesystem::remove_all(log_dir);
    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: all malformed games produce zero committed", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "ipl_all_malformed";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto const pgn_file = tmp / "games.pgn";
    {
        std::ofstream out {pgn_file};
        out << k_all_invalid_pgn;
    }

    auto mgr = motif::db::database_manager::create(tmp / "db", "test");
    REQUIRE(mgr.has_value());

    motif::import::import_pipeline pipeline {*mgr};
    auto summary = pipeline.run(pgn_file, k_single_worker);
    REQUIRE(summary.has_value());
    CHECK(summary->total_attempted == 2);
    CHECK(summary->committed == 0);
    CHECK(summary->skipped == 2);
    CHECK(summary->errors == 2);

    mgr->close();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: summary errors count malformed but not duplicates", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "ipl_summary_errors";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto const pgn_file = tmp / "games.pgn";
    {
        std::ofstream out {pgn_file};
        out << k_duplicate_and_invalid_pgn;
    }

    auto mgr = motif::db::database_manager::create(tmp / "db", "test");
    REQUIRE(mgr.has_value());

    motif::import::import_pipeline pipeline {*mgr};
    auto summary = pipeline.run(pgn_file, k_single_worker);
    REQUIRE(summary.has_value());
    CHECK(summary->total_attempted == 3);
    CHECK(summary->committed == 1);
    CHECK(summary->skipped == 2);
    CHECK(summary->errors == 1);
    CHECK(summary->duplicates_removed == 1);

    mgr->close();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: raw ingest + dedup leaves no dangling position rows", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "ipl_raw_dedup_positions";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto const pgn_file = tmp / "games.pgn";
    {
        std::ofstream out {pgn_file};
        out << k_duplicate_and_invalid_pgn;
    }

    auto mgr = motif::db::database_manager::create(tmp / "db", "test");
    REQUIRE(mgr.has_value());

    motif::import::import_pipeline pipeline {*mgr};
    auto summary = pipeline.run(pgn_file, k_single_worker);
    REQUIRE(summary.has_value());
    CHECK(summary->committed == 1);
    CHECK(summary->duplicates_removed == 1);

    // The duplicate pair collapses to one surviving game; position rows must
    // all reference it, not the raw-inserted-then-deleted duplicate.
    auto const games = mgr->find_games(motif::db::search_filter {});
    REQUIRE(games.has_value());
    REQUIRE(games->games.size() == 1);

    auto row_count = mgr->positions().row_count();
    REQUIRE(row_count.has_value());
    CHECK(*row_count > 0);

    mgr->close();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: failed runs preserve existing checkpoints", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "ipl_preserve_cp";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto mgr = motif::db::database_manager::create(tmp / "db", "test");
    REQUIRE(mgr.has_value());

    motif::import::import_checkpoint const checkpoint {
        .source_path = "missing.pgn",
        .byte_offset = 17,
        .games_committed = 2,
        .last_game_id = 9,
    };
    REQUIRE(motif::import::write_checkpoint(mgr->dir(), checkpoint).has_value());

    motif::import::import_pipeline pipeline {*mgr};
    auto const missing_pgn = tmp / "missing.pgn";
    auto result = pipeline.run(missing_pgn, k_single_worker);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == motif::import::error_code::io_failure);

    auto saved = motif::import::read_checkpoint(mgr->dir());
    REQUIRE(saved.has_value());
    CHECK(saved->source_path == checkpoint.source_path);
    CHECK(saved->byte_offset == checkpoint.byte_offset);
    CHECK(saved->games_committed == checkpoint.games_committed);
    CHECK(saved->last_game_id == checkpoint.last_game_id);

    mgr->close();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: run can skip position writes", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "ipl_no_positions";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto const pgn_file = tmp / "games.pgn";
    {
        std::ofstream out {pgn_file};
        out << k_three_game_pgn;
    }

    auto mgr = motif::db::database_manager::create(tmp / "db", "test");
    REQUIRE(mgr.has_value());

    motif::import::import_pipeline pipeline {*mgr};
    auto summary = pipeline.run(pgn_file, sqlite_only_serial_perf_config());
    REQUIRE(summary.has_value());
    CHECK(summary->committed == 3);
    CHECK(summary->position_replay_elapsed == std::chrono::milliseconds {0});
    CHECK(summary->sort_elapsed == std::chrono::milliseconds {0});
    CHECK(summary->rollup_elapsed == std::chrono::milliseconds {0});

    auto row_count = mgr->positions().row_count();
    REQUIRE(row_count.has_value());
    CHECK(*row_count == 0);

    mgr->close();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: zero num_workers is rejected", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "ipl_zero_workers";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto const pgn_file = tmp / "games.pgn";
    {
        std::ofstream out {pgn_file};
        out << k_three_game_pgn;
    }

    auto mgr = motif::db::database_manager::create(tmp / "db", "test");
    REQUIRE(mgr.has_value());

    motif::import::import_pipeline pipeline {*mgr};

    motif::import::import_config const zero_workers {
        .num_workers = 0,
        .num_lines = 4,
        .batch_size = 2,
    };

    auto result = pipeline.run(pgn_file, zero_workers);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == motif::import::error_code::invalid_state);

    mgr->close();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: zero num_lines is rejected", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "ipl_zero_lines";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto const pgn_file = tmp / "games.pgn";
    {
        std::ofstream out {pgn_file};
        out << k_three_game_pgn;
    }

    auto mgr = motif::db::database_manager::create(tmp / "db", "test");
    REQUIRE(mgr.has_value());

    motif::import::import_pipeline pipeline {*mgr};

    motif::import::import_config const zero_lines {
        .num_workers = 1,
        .num_lines = 0,
        .batch_size = 2,
    };

    auto result = pipeline.run(pgn_file, zero_lines);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == motif::import::error_code::invalid_state);

    mgr->close();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: rebuild_positions_after_import=false leaves position " "store empty", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "ipl_no_pos_rows";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto const pgn_file = tmp / "games.pgn";
    {
        std::ofstream out {pgn_file};
        out << k_three_game_pgn;
    }

    auto mgr = motif::db::database_manager::create(tmp / "db", "test");
    REQUIRE(mgr.has_value());

    motif::import::import_config const no_pos_config {
        .num_workers = 1,
        .num_lines = 4,
        .rebuild_positions_after_import = false,
        .batch_size = 2,
    };

    motif::import::import_pipeline pipeline {*mgr};
    auto summary = pipeline.run(pgn_file, no_pos_config);
    REQUIRE(summary.has_value());
    CHECK(summary->committed == 3);

    auto row_count = mgr->positions().row_count();
    REQUIRE(row_count.has_value());
    CHECK(*row_count == 0);

    mgr->close();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("pgn_helpers preserve PGN helper behavior", "[motif-import]")
{
    std::vector<pgn::tag> const tags {
        {.key = "White", .value = "Alpha"},
        {.key = "WhiteElo", .value = "2015"},
        {.key = "Event", .value = "Test Event"},
    };

    CHECK(motif::import::find_tag(tags, "White") == "Alpha");
    CHECK(motif::import::find_tag(tags, "Black").empty());

    auto white_elo = motif::import::parse_elo("2015");
    CHECK(white_elo == std::optional<std::int16_t> {2015});
    CHECK_FALSE(motif::import::parse_elo("?").has_value());
    CHECK_FALSE(motif::import::parse_elo("40000").has_value());

    CHECK(motif::import::pgn_result_to_string(pgn::result::white) == "1-0");
    CHECK(motif::import::pgn_result_to_string(pgn::result::black) == "0-1");
    CHECK(motif::import::pgn_result_to_string(pgn::result::draw) == "1/2-1/2");
    CHECK(motif::import::pgn_result_to_string(pgn::result::unknown) == "*");

    CHECK(motif::import::pgn_result_to_int8(pgn::result::white) == 1);
    CHECK(motif::import::pgn_result_to_int8(pgn::result::black) == -1);
    CHECK(motif::import::pgn_result_to_int8(pgn::result::draw) == 0);
    CHECK(motif::import::pgn_result_to_int8(pgn::result::unknown) == 0);
}

TEST_CASE("import_config defaults preserve measured fast path", "[motif-import]")
{
    auto const config = motif::import::import_config {};

    // num_workers is derived from hardware_concurrency() (see
    // default_worker_count()), so it is machine-dependent; only its
    // invariants -- at least one worker, and num_lines scaled to match --
    // are part of the contract this test protects.
    CHECK(config.num_workers >= 1);
    CHECK(config.num_lines == config.num_workers * motif::import::import_config::default_lines_per_worker);
    CHECK(config.rebuild_positions_after_import);
    CHECK(config.sort_positions_by_zobrist_after_rebuild);
    CHECK(config.batch_size == motif::import::import_config::default_batch_size);
}

TEST_CASE("default_worker_count uses half the detected hardware threads, floor of one", "[motif-import]")
{
    auto const detected = static_cast<std::size_t>(std::thread::hardware_concurrency());
    constexpr std::size_t half_divisor = 2;
    constexpr std::size_t min_workers = 1;
    auto const expected = std::max(min_workers, detected / half_divisor);

    CHECK(motif::import::default_worker_count() == expected);
}

TEST_CASE("diagnostic: raw-ingest+dedup throughput curve on 1M-game corpus", "[performance][motif-import]")
{
    skip_perf_unless_release_build();

    auto const pgn_file = perf_pgn_path();
    if (!std::filesystem::exists(pgn_file)) {
        SKIP("1M-game PGN not available");
    }

    auto const tmp = std::filesystem::temp_directory_path() / "ipl_throughput_diag";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto mgr = motif::db::database_manager::create(tmp / "db", "diag");
    REQUIRE(mgr.has_value());

    motif::import::import_pipeline pipeline {*mgr};

    std::atomic<bool> done {false};
    motif::import::result<motif::import::import_summary> summary_result {tl::unexpected {motif::import::error_code::io_failure}};
    std::thread worker {[&]() -> void
                        {
                            summary_result = pipeline.run(pgn_file, motif::import::import_config {});
                            done.store(true, std::memory_order_relaxed);
                        }};

    constexpr auto poll_interval = std::chrono::milliseconds {2000};
    std::size_t last_processed = 0;
    auto const started = std::chrono::steady_clock::now();
    auto last_time = started;

    fmt::print("\n=== raw-ingest+dedup throughput curve ===\n");
    while (!done.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(poll_interval);
        auto const prog = pipeline.progress();
        auto const now = std::chrono::steady_clock::now();
        auto const window_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_time).count();
        auto const delta = prog.games_processed - last_processed;
        double const rate = window_ms > 0 ? static_cast<double>(delta) * 1000.0 / static_cast<double>(window_ms) : 0.0;
        char const* phase_str = "idle";
        switch (prog.phase) {
            case motif::import::import_phase::ingesting:
                phase_str = "ingesting";
                break;
            case motif::import::import_phase::deduplicating:
                phase_str = "deduplicating";
                break;
            case motif::import::import_phase::rebuilding:
                phase_str = "rebuilding";
                break;
            default:
                break;
        }
        fmt::print("  t={:6.1f}s  processed={:>8}  window_rate={:>8.0f} games/s  phase={}\n",
                   std::chrono::duration<double>(now - started).count(),
                   prog.games_processed,
                   rate,
                   phase_str);
        last_processed = prog.games_processed;
        last_time = now;
    }
    worker.join();

    REQUIRE(summary_result.has_value());
    print_import_perf_summary("diagnostic: raw-ingest+dedup throughput curve", *summary_result);

    mgr->close();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: default fast path perf", "[performance][motif-import]")
{
    skip_perf_unless_release_build();

    auto const pgn_file = perf_pgn_path();
    if (!std::filesystem::exists(pgn_file)) {
        SKIP("1M-game PGN not available");
    }

    auto summary = run_perf_import(motif::import::import_config {});
    REQUIRE(summary.has_value());
    print_import_perf_summary("import_pipeline: default fast path perf", *summary);
    check_release_calibrated_perf(summary->elapsed.count());
}

TEST_CASE("diagnostic: game_stream vs import_stream acceptance divergence", "[performance][motif-import]")
{
    skip_perf_unless_release_build();

    auto const pgn_file = perf_pgn_path();
    if (!std::filesystem::exists(pgn_file)) {
        SKIP("PGN corpus not available");
    }

    // Walk both streams in lockstep and report the first several games
    // where acceptance disagrees, printing enough of the game to identify
    // it (tags + move count).
    pgn::game_stream old_stream {pgn_file};
    pgn::import_stream new_stream {pgn_file};
    auto old_it = old_stream.begin();
    auto new_it = new_stream.begin();
    std::size_t game_idx = 0;
    std::size_t divergences = 0;

    constexpr std::size_t max_divergences_to_report = 15;
    while (old_it != std::default_sentinel && new_it != std::default_sentinel && divergences < max_divergences_to_report) {
        ++game_idx;
        bool const old_ok = old_it->has_value();
        bool const new_ok = new_it->has_value();
        if (old_ok != new_ok) {
            ++divergences;
            fmt::print("divergence #{} at game {}: old_ok={} new_ok={}\n", divergences, game_idx, old_ok, new_ok);
            if (new_ok) {
                auto const& tags = new_it->value().tags;
                for (auto const& tag : tags) {
                    fmt::print("  new tag: {}=\"{}\"\n", tag.key, tag.value);
                }
                fmt::print("  new moves: {}\n", new_it->value().moves.size());
            }
        }
        ++old_it;
        ++new_it;
    }
    fmt::print("total divergences found (capped at 15): {}\n", divergences);
    CHECK(game_idx > 0);
}

TEST_CASE("import_pipeline: raw PGN read only, no SAN, no writes", "[performance][motif-import]")
{
    skip_perf_unless_release_build();

    auto const pgn_file = perf_pgn_path();
    if (!std::filesystem::exists(pgn_file)) {
        SKIP("PGN corpus not available");
    }

    motif::import::pgn_reader reader {pgn_file};
    std::size_t games_parsed = 0;
    std::size_t games_skipped = 0;
    std::size_t moves_seen = 0;

    auto const started = std::chrono::steady_clock::now();
    while (true) {
        auto game_res = reader.next();
        if (!game_res) {
            if (game_res.error() == motif::import::error_code::parse_error) {
                ++games_skipped;
                continue;
            }
            break;
        }
        ++games_parsed;
        moves_seen += game_res->moves.size();
    }
    auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);

    fmt::print("\n=== import_pipeline: raw PGN read only, no SAN, no writes ===\n"
               "  games:        {}\n"
               "  skipped:      {}\n"
               "  moves:        {}\n"
               "  elapsed:      {} ms\n",
               games_parsed, games_skipped, moves_seen, elapsed.count());

    CHECK(games_parsed > 0);
}

TEST_CASE("import_pipeline: raw PGN read + SAN parse only, no writes", "[performance][motif-import]")
{
    skip_perf_unless_release_build();

    auto const pgn_file = perf_pgn_path();
    if (!std::filesystem::exists(pgn_file)) {
        SKIP("PGN corpus not available");
    }

    motif::import::pgn_reader reader {pgn_file};
    std::size_t games_parsed = 0;
    std::size_t games_skipped = 0;
    std::size_t moves_applied = 0;

    auto const started = std::chrono::steady_clock::now();
    while (true) {
        auto game_res = reader.next();
        if (!game_res) {
            if (game_res.error() == motif::import::error_code::parse_error) {
                ++games_skipped;
                continue;
            }
            break;
        }
        ++games_parsed;
        auto board = motif::chess::board {};
        for (auto const& node : game_res->moves) {
            if (auto move_res = motif::chess::apply_san(board, node.san); move_res.has_value()) {
                ++moves_applied;
            }
        }
    }
    auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);

    fmt::print("\n=== import_pipeline: raw PGN read + SAN parse only, no writes ===\n"
               "  games:        {}\n"
               "  skipped:      {}\n"
               "  moves:        {}\n"
               "  elapsed:      {} ms\n",
               games_parsed, games_skipped, moves_applied, elapsed.count());

    CHECK(games_parsed > 0);
}

TEST_CASE("import_pipeline: serial fast path candidate perf", "[performance][motif-import]")
{
    skip_perf_unless_release_build();

    auto const pgn_file = perf_pgn_path();
    if (!std::filesystem::exists(pgn_file)) {
        SKIP("1M-game PGN not available");
    }

    auto summary = run_perf_import(serial_fast_path_candidate_config());
    REQUIRE(summary.has_value());
    print_import_perf_summary("import_pipeline: serial fast path candidate perf", *summary);
    check_release_calibrated_perf(summary->elapsed.count());
}

TEST_CASE("import_pipeline: wide parallel (8 workers) perf", "[performance][motif-import]")
{
    skip_perf_unless_release_build();

    auto const pgn_file = perf_pgn_path();
    if (!std::filesystem::exists(pgn_file)) {
        SKIP("1M-game PGN not available");
    }

    constexpr std::size_t wide_parallel_workers_8 = 8;
    auto summary = run_perf_import(wide_parallel_config(wide_parallel_workers_8));
    REQUIRE(summary.has_value());
    print_import_perf_summary("import_pipeline: wide parallel (8 workers) perf", *summary);
    check_release_calibrated_perf(summary->elapsed.count());
}

TEST_CASE("import_pipeline: wide parallel (16 workers) perf", "[performance][motif-import]")
{
    skip_perf_unless_release_build();

    auto const pgn_file = perf_pgn_path();
    if (!std::filesystem::exists(pgn_file)) {
        SKIP("1M-game PGN not available");
    }

    constexpr std::size_t wide_parallel_workers_16 = 16;
    auto summary = run_perf_import(wide_parallel_config(wide_parallel_workers_16));
    REQUIRE(summary.has_value());
    print_import_perf_summary("import_pipeline: wide parallel (16 workers) perf", *summary);
    check_release_calibrated_perf(summary->elapsed.count());
}

TEST_CASE("import_pipeline: wide parallel (24 workers) perf", "[performance][motif-import]")
{
    skip_perf_unless_release_build();

    auto const pgn_file = perf_pgn_path();
    if (!std::filesystem::exists(pgn_file)) {
        SKIP("1M-game PGN not available");
    }

    constexpr std::size_t wide_parallel_workers_24 = 24;
    auto summary = run_perf_import(wide_parallel_config(wide_parallel_workers_24));
    REQUIRE(summary.has_value());
    print_import_perf_summary("import_pipeline: wide parallel (24 workers) perf", *summary);
    check_release_calibrated_perf(summary->elapsed.count());
}

TEST_CASE("import_pipeline: wide parallel (32 workers) perf", "[performance][motif-import]")
{
    skip_perf_unless_release_build();

    auto const pgn_file = perf_pgn_path();
    if (!std::filesystem::exists(pgn_file)) {
        SKIP("1M-game PGN not available");
    }

    constexpr std::size_t wide_parallel_workers_32 = 32;
    auto summary = run_perf_import(wide_parallel_config(wide_parallel_workers_32));
    REQUIRE(summary.has_value());
    print_import_perf_summary("import_pipeline: wide parallel (32 workers) perf", *summary);
    check_release_calibrated_perf(summary->elapsed.count());
}

TEST_CASE("import_pipeline: sqlite-only serial perf", "[performance][motif-import]")
{
    skip_perf_unless_release_build();

    auto const pgn_file = perf_pgn_path();
    if (!std::filesystem::exists(pgn_file)) {
        SKIP("1M-game PGN not available");
    }

    auto summary = run_perf_import(sqlite_only_serial_perf_config());
    REQUIRE(summary.has_value());
    print_import_perf_summary("import_pipeline: sqlite-only serial perf", *summary);
    check_release_calibrated_perf(summary->elapsed.count());
}

TEST_CASE("import_pipeline: sqlite-import plus rebuild perf", "[performance][motif-import]")
{
    skip_perf_unless_release_build();

    auto const pgn_file = perf_pgn_path();
    if (!std::filesystem::exists(pgn_file)) {
        SKIP("1M-game PGN not available");
    }

    auto total_elapsed = run_sqlite_then_rebuild_perf();
    REQUIRE(total_elapsed.has_value());
    print_duration_result("import_pipeline: sqlite-import plus rebuild perf", *total_elapsed);
    check_release_calibrated_perf(total_elapsed->count());
}

TEST_CASE("import_pipeline: rebuild-only perf", "[performance][motif-import]")
{
    skip_perf_unless_release_build();

    auto const pgn_file = perf_pgn_path();
    if (!std::filesystem::exists(pgn_file)) {
        SKIP("1M-game PGN not available");
    }

    auto rebuild_elapsed = run_rebuild_only_perf();
    REQUIRE(rebuild_elapsed.has_value());
    print_duration_result("import_pipeline: rebuild-only perf", *rebuild_elapsed);
    check_release_calibrated_perf(rebuild_elapsed->count());
}

TEST_CASE("import_pipeline: sqlite-import plus rebuild perf (no index)", "[performance][motif-import]")
{
    skip_perf_unless_release_build();

    auto const pgn_file = perf_pgn_path();
    if (!std::filesystem::exists(pgn_file)) {
        SKIP("1M-game PGN not available");
    }

    auto summary = run_perf_import(sqlite_rebuild_no_index_perf_config());
    REQUIRE(summary.has_value());
    print_import_perf_summary("import_pipeline: sqlite-import plus rebuild perf (no index)", *summary);
    check_release_calibrated_perf(summary->elapsed.count());
}

TEST_CASE("import_pipeline: sqlite-import plus rebuild perf (sorted no index)", "[performance][motif-import]")
{
    skip_perf_unless_release_build();

    auto const pgn_file = perf_pgn_path();
    if (!std::filesystem::exists(pgn_file)) {
        SKIP("1M-game PGN not available");
    }

    auto summary = run_perf_import(sqlite_rebuild_sorted_no_index_perf_config());
    REQUIRE(summary.has_value());
    print_import_perf_summary("import_pipeline: sqlite-import plus rebuild perf (sorted no index)", *summary);
    check_release_calibrated_perf(summary->elapsed.count());
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("import_pipeline: 10k diagnostic summary", "[motif-import][diagnostic]")
{
    auto const pgn_file = std::filesystem::path {"/home/bogdb/scid/twic/10k_games.pgn"};
    if (!std::filesystem::exists(pgn_file)) {
        SKIP("10k-game PGN not available");
    }

    auto const tmp = std::filesystem::temp_directory_path() / "ipl_diag";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto mgr = motif::db::database_manager::create(tmp / "db", "diag");
    REQUIRE(mgr.has_value());

    motif::import::import_pipeline pipeline {*mgr};
    auto summary = pipeline.run(pgn_file);
    REQUIRE(summary.has_value());

    fmt::print("attempted={} committed={} skipped={} errors={}\n",
               summary->total_attempted,
               summary->committed,
               summary->skipped,
               summary->errors);

    CHECK(summary->total_attempted == 10'000);
    CHECK(summary->committed + summary->skipped == summary->total_attempted);
    CHECK(summary->errors <= summary->skipped);

    mgr->close();
    std::filesystem::remove_all(tmp);
}

namespace
{

struct query_latency_result
{
    std::string variant_name;
    std::size_t num_queries {};
    double total_ms {};
    double p50_us {};
    double p99_us {};
    double min_us {};
    double max_us {};
    std::size_t total_rows_returned {};
};

auto measure_query_latencies(motif::db::database_manager& mgr,
                             std::vector<motif::db::zobrist_hash> const& hashes,
                             std::string_view variant_name) -> query_latency_result
{
    auto& positions = mgr.positions();
    std::vector<double> latencies_us;
    latencies_us.reserve(hashes.size());
    std::size_t total_rows = 0;

    for (auto const hash : hashes) {
        auto const start = std::chrono::steady_clock::now();
        auto res = positions.query_by_zobrist(hash);
        auto const end = std::chrono::steady_clock::now();

        auto const elapsed_us = std::chrono::duration<double, std::micro>(end - start).count();
        latencies_us.push_back(elapsed_us);

        if (res) {
            total_rows += res->size();
        }
    }

    std::ranges::sort(latencies_us);

    auto const count = latencies_us.size();
    auto total_ms = 0.0;
    for (auto const latency : latencies_us) {
        total_ms += latency;
    }
    total_ms /= us_per_ms;

    auto const p50_idx = std::min(count - 1, static_cast<std::size_t>(static_cast<double>(count) * 0.50));
    auto const p99_idx = std::min(count - 1, static_cast<std::size_t>(static_cast<double>(count) * 0.99));

    return query_latency_result {
        .variant_name = std::string {variant_name},
        .num_queries = count,
        .total_ms = total_ms,
        .p50_us = count > 0 ? latencies_us[p50_idx] : 0.0,
        .p99_us = count > 0 ? latencies_us[p99_idx] : 0.0,
        .min_us = count > 0 ? latencies_us.front() : 0.0,
        .max_us = count > 0 ? latencies_us.back() : 0.0,
        .total_rows_returned = total_rows,
    };
}

}  // namespace

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("query_latency: unsorted vs sorted by zobrist", "[performance][query-latency]")
{
    skip_perf_unless_release_build();

    auto const pgn_file = perf_pgn_path();
    if (!std::filesystem::exists(pgn_file)) {
        SKIP("PGN corpus not available");
    }

    auto const tmp = std::filesystem::temp_directory_path() / "query_latency_bench";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto mgr = motif::db::database_manager::create(tmp / "db", "qlatency");
    REQUIRE(mgr.has_value());

    std::filesystem::remove_all(perf_log_dir());
    auto init_log = motif::import::initialize_logging({.log_dir = perf_log_dir()});
    REQUIRE(init_log.has_value());

    motif::import::import_config const sqlite_only_config {
        .num_workers = 1,
        .num_lines = 1,
        .rebuild_positions_after_import = false,
        .batch_size = perf_batch_size,
    };

    motif::import::import_pipeline pipeline {*mgr};
    auto summary = pipeline.run(pgn_file, sqlite_only_config);
    REQUIRE(summary.has_value());
    REQUIRE(summary->committed > 0);

    auto rebuild_res = mgr->rebuild_position_store(
        /*sort_by_zobrist=*/false);
    REQUIRE(rebuild_res.has_value());

    auto& positions = mgr->positions();
    auto const row_count_res = positions.row_count();
    REQUIRE(row_count_res.has_value());
    fmt::print("position row count: {}\n", *row_count_res);

    constexpr std::size_t num_warmup = 5;
    for (std::size_t i = 0; i < num_warmup; ++i) {
        auto dummy = positions.query_by_zobrist(motif::db::zobrist_hash {0});
        (void)dummy;
    }

    constexpr std::size_t num_samples = 200;
    auto hashes_res = positions.sample_zobrist_hashes(num_samples, query_sample_seed);
    REQUIRE(hashes_res.has_value());
    auto sample_hashes = std::move(*hashes_res);

    REQUIRE_FALSE(sample_hashes.empty());
    fmt::print("sample hashes collected: {}\n", sample_hashes.size());

    auto print_result = [](query_latency_result const& result) -> void
    {
        fmt::print("\n=== {} ===\n"
                   "  queries:      {}\n"
                   "  total:        {} ms\n"
                   "  p50:          {} us\n"
                   "  p99:          {} us\n"
                   "  min:          {} us\n"
                   "  max:          {} us\n"
                   "  total rows:   {}\n",
                   result.variant_name, result.num_queries, result.total_ms,
                   result.p50_us, result.p99_us, result.min_us, result.max_us, result.total_rows_returned);
    };

    auto r_unsorted = measure_query_latencies(*mgr, sample_hashes, "unsorted");
    print_result(r_unsorted);

    auto sort_res = positions.sort_by_zobrist();
    REQUIRE(sort_res.has_value());

    for (std::size_t i = 0; i < num_warmup; ++i) {
        auto dummy = positions.query_by_zobrist(sample_hashes.front());
        (void)dummy;
    }

    auto r_sorted = measure_query_latencies(*mgr, sample_hashes, "sorted by zobrist");
    print_result(r_sorted);

    auto const shutdown_result = motif::import::shutdown_logging();
    (void)shutdown_result;
    mgr->close();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: ingest-only peak RSS on 1M (isolates pgn_reader buffer)", "[performance][motif-import]")
{
    skip_perf_unless_release_build();

    auto const pgn_file = perf_pgn_path();
    if (!std::filesystem::exists(pgn_file)) {
        SKIP("1M-game PGN not available");
    }

    auto const tmp = std::filesystem::temp_directory_path() / "ipl_rss_ingest_only";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto mgr = motif::db::database_manager::create(tmp / "db", "perf");
    REQUIRE(mgr.has_value());

    motif::import::import_pipeline pipeline {*mgr};

    // No position rebuild, no sort, no rollup -- isolates PGN read (pgn_reader's
    // whole-file buffer) + SQLite game write from all DuckDB memory use, so this
    // number is directly attributable to the import_stream migration's buffer.
    auto const sqlite_only = motif::import::import_config {
        .rebuild_positions_after_import = false,
    };

    static constexpr std::size_t bytes_per_mb = 1024UZ * 1024UZ;
    auto const rss_baseline = test_helpers::read_rss_bytes();
    test_helpers::peak_rss_sampler const sampler;
    auto summary = pipeline.run(pgn_file, sqlite_only);
    auto const peak_rss = sampler.peak();

    REQUIRE(summary.has_value());
    auto const delta_mb = peak_rss > rss_baseline ? (peak_rss - rss_baseline) / bytes_per_mb : 0;
    fmt::print("\n=== import_pipeline: ingest-only peak RSS on 1M ===\n"
               "  elapsed:      {} ms\n"
               "  committed:    {}\n"
               "  baseline RSS: {} MB\n"
               "  peak RSS:     {} MB\n"
               "  delta:        {} MB\n",
               summary->elapsed.count(), summary->committed,
               rss_baseline / bytes_per_mb, peak_rss / bytes_per_mb, delta_mb);

    mgr->close();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: inline path peak RSS on 1M", "[performance][motif-import]")
{
    skip_perf_unless_release_build();

    auto const pgn_file = perf_pgn_path();
    if (!std::filesystem::exists(pgn_file)) {
        SKIP("1M-game PGN not available");
    }

    auto const tmp = std::filesystem::temp_directory_path() / "ipl_rss_inline";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto mgr = motif::db::database_manager::create(tmp / "db", "perf");
    REQUIRE(mgr.has_value());

    motif::import::import_pipeline pipeline {*mgr};

    static constexpr std::size_t bytes_per_mb = 1024UZ * 1024UZ;
    auto const rss_baseline = test_helpers::read_rss_bytes();
    test_helpers::peak_rss_sampler const sampler;
    auto summary = pipeline.run(pgn_file, motif::import::import_config {});
    auto const peak_rss = sampler.peak();

    REQUIRE(summary.has_value());
    auto const delta_mb = peak_rss > rss_baseline ? (peak_rss - rss_baseline) / bytes_per_mb : 0;
    fmt::print("\n=== import_pipeline: inline path peak RSS on 1M ===\n"
               "  elapsed:      {} ms\n"
               "  committed:    {}\n"
               "  baseline RSS: {} MB\n"
               "  peak RSS:     {} MB\n"
               "  delta:        {} MB\n",
               summary->elapsed.count(), summary->committed,
               rss_baseline / bytes_per_mb, peak_rss / bytes_per_mb, delta_mb);

    mgr->close();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: rebuild path peak RSS on 1M", "[performance][motif-import]")
{
    skip_perf_unless_release_build();

    auto const pgn_file = perf_pgn_path();
    if (!std::filesystem::exists(pgn_file)) {
        SKIP("1M-game PGN not available");
    }

    auto const tmp = std::filesystem::temp_directory_path() / "ipl_rss_rebuild";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto mgr = motif::db::database_manager::create(tmp / "db", "perf");
    REQUIRE(mgr.has_value());

    motif::import::import_pipeline pipeline {*mgr};

    auto const sqlite_only = motif::import::import_config {
        .rebuild_positions_after_import = false,
    };

    static constexpr std::size_t bytes_per_mb = 1024UZ * 1024UZ;
    auto const rss_baseline = test_helpers::read_rss_bytes();
    test_helpers::peak_rss_sampler const sampler;
    auto summary = pipeline.run(pgn_file, sqlite_only);
    REQUIRE(summary.has_value());
    auto rebuild_res = mgr->rebuild_position_store();
    REQUIRE(rebuild_res.has_value());
    auto const peak_rss = sampler.peak();

    auto const delta_mb = peak_rss > rss_baseline ? (peak_rss - rss_baseline) / bytes_per_mb : 0;
    fmt::print("\n=== import_pipeline: rebuild path peak RSS on 1M ===\n"
               "  elapsed:      {} ms\n"
               "  committed:    {}\n"
               "  baseline RSS: {} MB\n"
               "  peak RSS:     {} MB\n"
               "  delta:        {} MB\n",
               summary->elapsed.count(), summary->committed,
               rss_baseline / bytes_per_mb, peak_rss / bytes_per_mb, delta_mb);

    mgr->close();
    std::filesystem::remove_all(tmp);
}
