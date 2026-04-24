#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <ratio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "motif/import/import_pipeline.hpp"

#include <catch2/catch_test_macros.hpp>
#include <pgnlib/types.hpp>
#include <tl/expected.hpp>

#include "motif/db/database_manager.hpp"
#include "motif/import/checkpoint.hpp"
#include "motif/import/error.hpp"
#include "motif/import/logger.hpp"
#include "motif/import/pgn_helpers.hpp"
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
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
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

    auto const ignored_shutdown_result = motif::import::shutdown_logging();
    (void)ignored_shutdown_result;
    mgr->close();
    if (!keep_perf_bundle()) {
        std::filesystem::remove_all(tmp);
    }
    return summary;
}

void print_import_perf_summary(std::string_view const label, motif::import::import_summary const& summary)
{
    std::cout << "\n=== " << label << " ===\n"
              << "  attempted:    " << summary.total_attempted << "\n"
              << "  committed:    " << summary.committed << "\n"
              << "  skipped:      " << summary.skipped << "\n"
              << "  errors:       " << summary.errors << "\n"
              << "  elapsed:      " << summary.elapsed.count() << " ms\n";
}

void print_duration_result(std::string_view const label, std::chrono::milliseconds const elapsed)
{
    std::cout << "\n=== " << label << " ===\n"
              << "  elapsed:      " << elapsed.count() << " ms\n";
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

constexpr auto perf_batch_size = std::size_t {10'000};

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
        auto const ignored_shutdown_result = motif::import::shutdown_logging();
        (void)ignored_shutdown_result;
        mgr->close();
        std::filesystem::remove_all(tmp);
        return tl::unexpected {import_summary.error()};
    }

    auto const rebuild_start = std::chrono::steady_clock::now();
    auto rebuild_res = mgr->rebuild_position_store();
    auto const rebuild_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - rebuild_start);
    if (!rebuild_res.has_value()) {
        auto const ignored_shutdown_result = motif::import::shutdown_logging();
        (void)ignored_shutdown_result;
        mgr->close();
        std::filesystem::remove_all(tmp);
        return tl::unexpected {motif::import::error_code::io_failure};
    }

    auto const ignored_shutdown_result = motif::import::shutdown_logging();
    (void)ignored_shutdown_result;
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
        auto const ignored_shutdown_result = motif::import::shutdown_logging();
        (void)ignored_shutdown_result;
        mgr->close();
        std::filesystem::remove_all(tmp);
        return tl::unexpected {import_summary.error()};
    }

    auto const rebuild_start = std::chrono::steady_clock::now();
    auto rebuild_res = mgr->rebuild_position_store();
    auto const rebuild_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - rebuild_start);
    if (!rebuild_res.has_value()) {
        auto const ignored_shutdown_result = motif::import::shutdown_logging();
        (void)ignored_shutdown_result;
        mgr->close();
        std::filesystem::remove_all(tmp);
        return tl::unexpected {motif::import::error_code::io_failure};
    }

    auto const ignored_shutdown_result = motif::import::shutdown_logging();
    (void)ignored_shutdown_result;
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
    CHECK(*row_count == 13);

    mgr->close();
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
    motif::import::import_checkpoint const fake_chk {
        .source_path = pgn_file.string(),
        .byte_offset = 0,
        .games_committed = 0,
        .last_game_id = 0,
    };
    REQUIRE(motif::import::write_checkpoint(mgr->dir(), fake_chk).has_value());

    // Resume: all 3 games are duplicates → none newly committed
    auto second_run = pipeline.resume(pgn_file, big_batch);
    REQUIRE(second_run.has_value());
    CHECK(second_run->committed == 0);
    CHECK(second_run->skipped == 3);

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

TEST_CASE("import_pipeline: rebuild_positions_after_import=false leaves position store empty", "[motif-import]")
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

    CHECK(config.num_workers == 4);
    CHECK(config.num_lines == 64);
    CHECK(config.rebuild_positions_after_import);
    CHECK(config.sort_positions_by_zobrist_after_rebuild);
    CHECK(config.batch_size == motif::import::import_config::default_batch_size);
    CHECK(config.num_workers == motif::import::import_config::default_num_workers);
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

    std::cout << "attempted=" << summary->total_attempted << " committed=" << summary->committed << " skipped=" << summary->skipped
              << " errors=" << summary->errors << '\n';

    CHECK(summary->total_attempted == 10'000);
    CHECK(summary->committed + summary->skipped == summary->total_attempted);
    CHECK(summary->errors <= summary->skipped);

    mgr->close();
    std::filesystem::remove_all(tmp);
}

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

constexpr auto query_latency_sample_seed = std::uint64_t {42};
constexpr auto us_per_ms = 1000.0;

// NOLINTNEXTLINE(misc-use-anonymous-namespace)
static auto measure_query_latencies(motif::db::database_manager& mgr,
                                    std::vector<std::uint64_t> const& hashes,
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

    auto const sample_count = latencies_us.size();
    auto total_ms = 0.0;
    for (auto const latency_us : latencies_us) {
        total_ms += latency_us;
    }
    total_ms /= us_per_ms;

    auto const p50_idx = std::min(sample_count - 1, static_cast<std::size_t>(static_cast<double>(sample_count) * 0.50));
    auto const p99_idx = std::min(sample_count - 1, static_cast<std::size_t>(static_cast<double>(sample_count) * 0.99));

    return query_latency_result {
        .variant_name = std::string {variant_name},
        .num_queries = sample_count,
        .total_ms = total_ms,
        .p50_us = sample_count > 0 ? latencies_us[p50_idx] : 0.0,
        .p99_us = sample_count > 0 ? latencies_us[p99_idx] : 0.0,
        .min_us = sample_count > 0 ? latencies_us.front() : 0.0,
        .max_us = sample_count > 0 ? latencies_us.back() : 0.0,
        .total_rows_returned = total_rows,
    };
}

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
        .batch_size = 10'000,
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
    std::cout << "position row count: " << *row_count_res << "\n";

    constexpr std::size_t num_warmup = 5;
    for (std::size_t i = 0; i < num_warmup; ++i) {
        auto dummy = positions.query_by_zobrist(0);
        (void)dummy;
    }

    constexpr std::size_t num_samples = 200;
    auto hashes_res = positions.sample_zobrist_hashes(num_samples, query_latency_sample_seed);
    REQUIRE(hashes_res.has_value());
    auto sample_hashes = std::move(*hashes_res);

    REQUIRE_FALSE(sample_hashes.empty());
    std::cout << "sample hashes collected: " << sample_hashes.size() << "\n";

    auto print_result = [](query_latency_result const& result) -> void
    {
        std::cout << "\n=== " << result.variant_name << " ===\n"
                  << "  queries:      " << result.num_queries << "\n"
                  << "  total:        " << result.total_ms << " ms\n"
                  << "  p50:          " << result.p50_us << " us\n"
                  << "  p99:          " << result.p99_us << " us\n"
                  << "  min:          " << result.min_us << " us\n"
                  << "  max:          " << result.max_us << " us\n"
                  << "  total rows:   " << result.total_rows_returned << "\n";
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

    auto const ignored_shutdown_result = motif::import::shutdown_logging();
    (void)ignored_shutdown_result;
    mgr->close();
    std::filesystem::remove_all(tmp);
}
