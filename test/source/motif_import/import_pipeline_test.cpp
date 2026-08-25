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
#include "motif/search/opening_stats.hpp"
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

// Exact matches for the starting position served through the durable postings
// index.
auto start_position_matches(motif::db::database_manager& mgr) -> std::size_t
{
    auto const start_hash = motif::db::zobrist_hash {motif::chess::board {}.hash()};
    auto const matches = mgr.query_position_matches(start_hash);
    REQUIRE(matches.has_value());
    return matches->size();
}

// Canonical game built directly from SANs; used to grow the store without a
// postings rebuild so postings-stale behavior can be observed.
auto game_from_sans(char const* white, char const* black, std::string_view result, std::initializer_list<char const*> sans)
    -> motif::db::game
{
    auto board = motif::chess::board {};
    auto moves = std::vector<std::uint16_t> {};
    for (char const* const san : sans) {
        auto const encoded = motif::chess::apply_san(board, san);
        REQUIRE(encoded.has_value());
        moves.push_back(*encoded);
    }
    return motif::db::game {
        .white = {.name = white, .elo = std::nullopt, .title = std::nullopt, .country = std::nullopt},
        .black = {.name = black, .elo = std::nullopt, .title = std::nullopt, .country = std::nullopt},
        .event_details = std::nullopt,
        .date = std::nullopt,
        .result = std::string {result},
        .eco = std::nullopt,
        .moves = std::move(moves),
        .extra_tags = {},
        .provenance = {},
    };
}

constexpr auto import_perf_limit_ms = std::int64_t {120'000};
constexpr std::size_t perf_batch_size {10'000};

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
               "  dedup:        {} ms\n"
               "  elapsed:      {} ms\n",
               label, summary.total_attempted, summary.committed, summary.skipped,
               summary.errors, summary.ingest_elapsed.count(), summary.dedup_elapsed.count(), summary.elapsed.count());
}

void print_duration_result(std::string_view const label, std::chrono::milliseconds const elapsed)
{
    fmt::print("\n=== {} ===\n" "  elapsed:      {} ms\n", label, elapsed.count());
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
        .build_position_postings_after_import = true,
        .batch_size = motif::import::import_config::default_batch_size,
    };
}

constexpr std::size_t wide_parallel_lines_per_worker = 16;

auto wide_parallel_config(std::size_t const workers) -> motif::import::import_config
{
    return motif::import::import_config {
        .num_workers = workers,
        .num_lines = workers * wide_parallel_lines_per_worker,
        .build_position_postings_after_import = true,
        .batch_size = motif::import::import_config::default_batch_size,
    };
}

auto ingest_only_serial_perf_config() -> motif::import::import_config
{
    return motif::import::import_config {
        .num_workers = 1,
        .num_lines = 1,
        .build_position_postings_after_import = false,
        .batch_size = perf_batch_size,
    };
}

auto postings_serial_perf_config() -> motif::import::import_config
{
    return motif::import::import_config {
        .num_workers = 1,
        .num_lines = 1,
        .build_position_postings_after_import = true,
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
    auto import_summary = pipeline.run(pgn_file, ingest_only_serial_perf_config());
    if (!import_summary.has_value()) {
        auto const shutdown_result = motif::import::shutdown_logging();
        (void)shutdown_result;
        mgr->close();
        std::filesystem::remove_all(tmp);
        return tl::unexpected {import_summary.error()};
    }

    auto const rebuild_start = std::chrono::steady_clock::now();
    auto rebuild_metrics = motif::db::position_postings_build_metrics {};
    auto rebuild_res = mgr->rebuild_position_postings(&rebuild_metrics);
    auto const rebuild_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - rebuild_start);
    if (!rebuild_res.has_value()) {
        auto const shutdown_result = motif::import::shutdown_logging();
        (void)shutdown_result;
        mgr->close();
        std::filesystem::remove_all(tmp);
        return tl::unexpected {motif::import::error_code::io_failure};
    }
    fmt::print("  postings occurrences: {}\n", rebuild_metrics.occurrence_count);

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
    auto import_summary = pipeline.run(pgn_file, ingest_only_serial_perf_config());
    if (!import_summary.has_value()) {
        auto const shutdown_result = motif::import::shutdown_logging();
        (void)shutdown_result;
        mgr->close();
        std::filesystem::remove_all(tmp);
        return tl::unexpected {import_summary.error()};
    }

    auto const rebuild_start = std::chrono::steady_clock::now();
    auto rebuild_metrics = motif::db::position_postings_build_metrics {};
    auto rebuild_res = mgr->rebuild_position_postings(&rebuild_metrics);
    auto const rebuild_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - rebuild_start);
    if (!rebuild_res.has_value()) {
        auto const shutdown_result = motif::import::shutdown_logging();
        (void)shutdown_result;
        mgr->close();
        std::filesystem::remove_all(tmp);
        return tl::unexpected {motif::import::error_code::io_failure};
    }
    fmt::print("  postings occurrences: {}\n", rebuild_metrics.occurrence_count);

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
    CHECK(summary->position_postings_metrics.has_value());
    CHECK_FALSE(std::filesystem::exists(motif::import::checkpoint_path(mgr->dir())));
    REQUIRE(mgr->manifest().position_postings.has_value());
    CHECK(start_position_matches(*mgr) == 3U);

    mgr->close();
    std::filesystem::remove_all(tmp);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros dominate this parity test's score.
TEST_CASE("import_pipeline: default postings path matches an explicit postings rebuild", "[motif-import]")
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
    REQUIRE(mgr_a->manifest().position_postings.has_value());
    auto const start_hash = motif::db::zobrist_hash {motif::chess::board {}.hash()};
    auto const matches_a = mgr_a->query_position_matches(start_hash);
    REQUIRE(matches_a.has_value());
    mgr_a->close();

    // Ingest-only import followed by an explicit postings rebuild.
    constexpr motif::import::import_config rebuild_config {
        .num_workers = 1,
        .num_lines = 4,
        .build_position_postings_after_import = false,
        .batch_size = 2,
    };
    auto mgr_b = motif::db::database_manager::create(tmp / "db_b", "test");
    REQUIRE(mgr_b.has_value());
    motif::import::import_pipeline pipeline_b {*mgr_b};
    auto summary_b = pipeline_b.run(pgn_file, rebuild_config);
    REQUIRE(summary_b.has_value());
    auto rebuild_metrics = motif::db::position_postings_build_metrics {};
    REQUIRE(mgr_b->rebuild_position_postings(&rebuild_metrics).has_value());
    CHECK(rebuild_metrics.occurrence_count == 16);
    auto const matches_b = mgr_b->query_position_matches(start_hash);
    REQUIRE(matches_b.has_value());
    mgr_b->close();

    CHECK(summary_a->committed == summary_b->committed);
    REQUIRE(matches_a->size() == matches_b->size());
    CHECK(matches_a->size() == 3U);
    for (std::size_t index = 0; index < matches_a->size(); ++index) {
        CHECK((*matches_a)[index].game_id == (*matches_b)[index].game_id);
        CHECK((*matches_a)[index].ply == (*matches_b)[index].ply);
        CHECK((*matches_a)[index].result == (*matches_b)[index].result);
        CHECK((*matches_a)[index].white_elo == (*matches_b)[index].white_elo);
        CHECK((*matches_a)[index].black_elo == (*matches_b)[index].black_elo);
    }

    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: postings import builds opening continuations", "[motif-import]")
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
        .build_position_postings_after_import = true,
        .batch_size = 2,
    };
    motif::import::import_pipeline pipeline {*mgr};
    auto const summary = pipeline.run(pgn_file, config);
    REQUIRE(summary.has_value());
    REQUIRE(summary->committed == 3);

    auto const start_hash = motif::db::zobrist_hash {motif::chess::board {}.hash()};
    auto const continuations = mgr->query_unfiltered_opening_stats(start_hash);
    REQUIRE(continuations.has_value());
    CHECK(continuations->size() == 3);

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

    // The resume's own postings rebuild republished the exact index.
    REQUIRE(mgr->manifest().position_postings.has_value());
    CHECK(start_position_matches(*mgr) == 3U);

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
    REQUIRE(motif::import::write_checkpoint(mgr->dir(),
                                            {.source_path = pgn_file.string(), .byte_offset = 0, .games_committed = 0, .last_game_id = 0})
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

    motif::import::import_checkpoint const checkpoint {
        .source_path = pgn_file.string(),
        .byte_offset = 0,
        .games_committed = 1,
        .last_game_id = static_cast<std::int64_t>(interrupted_id->value),
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

    // The moveless interrupted game still records its ply-0 occurrence in
    // the postings index (every replayed game contributes a starting row),
    // so four games means four starting-position matches.
    CHECK(start_position_matches(*mgr) == 4U);

    mgr->close();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: cancellation leaves the position table dirty, and reopen repairs it", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "ipl_cancel_repair";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto const pgn_file = tmp / "games.pgn";
    {
        std::ofstream out {pgn_file};
        out << k_three_game_pgn;
    }

    auto mgr = motif::db::database_manager::create(tmp / "db", "test");
    REQUIRE(mgr.has_value());

    {
        motif::import::import_pipeline pipeline {*mgr};
        auto const summary = pipeline.run(pgn_file, k_single_worker);
        REQUIRE(summary.has_value());
        REQUIRE(summary->committed == 3);
        // Default config publishes postings that serve exact matches.
        CHECK(start_position_matches(*mgr) == 3U);
    }

    // A second run against the same source is canceled before the worker
    // starts (request_stop() called ahead of run(), mirroring a UI cancel
    // that lands before any line is read). commit_sqlite_batch() still opens
    // and commits the (empty) SQLite transaction and, per its real code
    // path, calls mark_position_table_dirty() unconditionally -- the run
    // returns early without a rebuild, so that dirty state must survive into
    // the on-disk manifest.
    {
        motif::import::import_pipeline pipeline {*mgr};
        pipeline.request_stop();
        auto const summary = pipeline.run(pgn_file, k_single_worker);
        REQUIRE(summary.has_value());
    }

    mgr->close();

    auto const manifest_after_cancel = motif::db::read_manifest(mgr->dir() / "manifest.json");
    REQUIRE(manifest_after_cancel.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(manifest_after_cancel->position_index_dirty);

    // The canceled run invalidated the postings generation. Reopen repairs
    // directly from canonical SQLite.

    auto reopened = motif::db::database_manager::open(tmp / "db");
    REQUIRE(reopened.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(start_position_matches(*reopened) == 3U);

    reopened->close();
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

    // The duplicate pair collapses to one surviving game; exact occurrences
    // must all reference it through the default postings index, not the
    // raw-inserted-then-deleted duplicate.
    auto const games = mgr->find_games(motif::db::search_filter {});
    REQUIRE(games.has_value());
    REQUIRE(games->games.size() == 1);
    auto const survivor = games->games.front().id;

    REQUIRE(mgr->manifest().position_postings.has_value());
    auto const start_hash = motif::db::zobrist_hash {motif::chess::board {}.hash()};
    auto const matches = mgr->query_position_matches(start_hash);
    REQUIRE(matches.has_value());
    REQUIRE(matches->size() == 1U);
    CHECK(matches->front().game_id == survivor);

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
    auto summary = pipeline.run(pgn_file, ingest_only_serial_perf_config());
    REQUIRE(summary.has_value());
    CHECK(summary->committed == 3);
    CHECK_FALSE(summary->position_postings_metrics.has_value());
    CHECK_FALSE(mgr->manifest().position_postings.has_value());

    auto const start_hash = motif::db::zobrist_hash {motif::chess::board {}.hash()};
    auto const matches = mgr->query_position_matches(start_hash);
    REQUIRE_FALSE(matches.has_value());
    CHECK(matches.error() == motif::db::error_code::io_failure);

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

    motif::import::import_checkpoint const checkpoint {
        .source_path = pgn_file.string(),
        .byte_offset = std::filesystem::file_size(pgn_file),
        .games_committed = 3,
        .last_game_id = 3,
    };
    REQUIRE(motif::import::write_checkpoint(mgr->dir(), checkpoint).has_value());

    auto checkpointed_result = pipeline.run(pgn_file, zero_workers);
    REQUIRE_FALSE(checkpointed_result.has_value());
    CHECK(checkpointed_result.error() == motif::import::error_code::invalid_state);
    auto const preserved = motif::import::read_checkpoint(mgr->dir());
    REQUIRE(preserved.has_value());
    CHECK(preserved->byte_offset == checkpoint.byte_offset);

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

TEST_CASE("import_pipeline: disabled postings build leaves exact queries unavailable", "[motif-import]")
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
        .build_position_postings_after_import = false,
        .batch_size = 2,
    };

    motif::import::import_pipeline pipeline {*mgr};
    auto summary = pipeline.run(pgn_file, no_pos_config);
    REQUIRE(summary.has_value());
    CHECK(summary->committed == 3);

    CHECK_FALSE(mgr->manifest().position_postings.has_value());
    auto const matches = mgr->query_position_matches(motif::db::zobrist_hash {motif::chess::board {}.hash()});
    REQUIRE_FALSE(matches.has_value());
    CHECK(matches.error() == motif::db::error_code::io_failure);

    mgr->close();
    std::filesystem::remove_all(tmp);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one integration case intentionally inventories every postings query path.
TEST_CASE("import_pipeline: literal default serves every position path from postings", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "ipl_default_postings";
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
    auto const summary = pipeline.run(pgn_file, motif::import::import_config {});
    REQUIRE(summary.has_value());
    CHECK(summary->committed == 3);
    REQUIRE(summary->position_postings_metrics.has_value());
    CHECK(summary->position_postings_metrics.value_or(motif::db::position_postings_build_metrics {}).game_count == 3U);

    // The exact postings index is the published derived artifact.
    REQUIRE(mgr->manifest().position_postings.has_value());
    CHECK(std::filesystem::exists(tmp / "db"
                                  / mgr->manifest().position_postings.value_or(motif::db::derived_index_manifest_entry {}).filename));

    auto const start_hash = motif::db::zobrist_hash {motif::chess::board {}.hash()};
    auto const after_e4e5 = [&]() -> motif::db::zobrist_hash
    {
        auto board = motif::chess::board {};
        // apply_san applies the move to the board and returns its encoding.
        REQUIRE(motif::chess::apply_san(board, "e4").has_value());
        REQUIRE(motif::chess::apply_san(board, "e5").has_value());
        return motif::db::zobrist_hash {board.hash()};
    }();

    // Exact position search, pagination, game ids, and summary counts.
    auto const matches = mgr->query_position_matches(start_hash);
    REQUIRE(matches.has_value());
    REQUIRE(matches->size() == 3U);
    CHECK((*matches)[0].game_id == motif::db::game_id {1U});
    CHECK((*matches)[1].game_id == motif::db::game_id {2U});
    CHECK((*matches)[2].game_id == motif::db::game_id {3U});
    auto const paged = mgr->query_position_matches(start_hash, 1U, 1U);
    REQUIRE(paged.has_value());
    REQUIRE(paged->size() == 1U);
    CHECK(paged->front().game_id == motif::db::game_id {2U});
    auto const game_ids = mgr->position_game_ids(start_hash);
    REQUIRE(game_ids.has_value());
    CHECK(game_ids->size() == 3U);
    auto const summary_one = mgr->position_summary(start_hash);
    REQUIRE(summary_one.has_value());
    REQUIRE(summary_one->has_value());
    CHECK(summary_one->value_or(motif::db::position_postings_summary {}).distinct_game_count == 3U);
    auto const after_e4e5_matches = mgr->query_position_matches(after_e4e5);
    REQUIRE(after_e4e5_matches.has_value());
    CHECK(after_e4e5_matches->size() == 1U);

    // Unfiltered opening statistics.
    auto const unfiltered = mgr->query_unfiltered_opening_stats(start_hash);
    REQUIRE(unfiltered.has_value());
    REQUIRE(unfiltered->size() == 3U);
    for (auto const& row : *unfiltered) {
        CHECK(row.frequency == 1U);
        CHECK(row.transposition_frequency == 1U);
    }

    // Filtered opening statistics through the public search API.
    auto filter = motif::db::search_filter {};
    filter.player_name = std::string {"C"};
    auto const filtered = motif::search::opening_stats::query(*mgr, start_hash, filter);
    REQUIRE(filtered.has_value());
    CHECK(filtered->total_games == 1U);
    REQUIRE(filtered->continuations.size() == 1U);
    CHECK(filtered->continuations.front().san == "d4");

    // Elo distributions: unfiltered and metadata-filtered.
    auto const distribution = mgr->query_elo_distribution(start_hash, {}, 200);
    REQUIRE(distribution.has_value());
    REQUIRE(distribution->size() == 2U);  // game 3 carries no Elo and is excluded
    auto const elo_filter = []() -> motif::db::search_filter
    {
        constexpr auto minimum_elo = std::int32_t {2050};
        auto filtered_filter = motif::db::search_filter {};
        filtered_filter.min_elo = minimum_elo;
        return filtered_filter;
    }();
    auto const filtered_distribution = mgr->query_elo_distribution(start_hash, elo_filter, 200);
    REQUIRE(filtered_distribution.has_value());
    REQUIRE(filtered_distribution->size() == 1U);
    CHECK(filtered_distribution->front().game_count == 1U);

    // Deep traversal outside any shallow tree: bounded slice from a root the
    // (absent) opening-tree index never covered, unfiltered and game-filtered.
    auto const deep = mgr->query_tree_slice(after_e4e5, 10U);
    REQUIRE(deep.has_value());
    REQUIRE(deep->size() == 3U);  // Nf3, Nc6, Bb5 remain in game 1
    CHECK(deep->front().depth == 1U);
    CHECK(deep->back().depth == 3U);
    auto const deep_filtered = mgr->query_tree_slice(after_e4e5, 10U, {motif::db::game_id {2U}});
    REQUIRE(deep_filtered.has_value());
    CHECK(deep_filtered->empty());

    mgr->close();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: literal-default bundle reopens from postings", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "ipl_default_reopen";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto const pgn_file = tmp / "games.pgn";
    {
        std::ofstream out {pgn_file};
        out << k_three_game_pgn;
    }

    {
        auto mgr = motif::db::database_manager::create(tmp / "db", "test");
        REQUIRE(mgr.has_value());
        motif::import::import_pipeline pipeline {*mgr};
        REQUIRE(pipeline.run(pgn_file, motif::import::import_config {}).has_value());
        mgr->close();
    }

    // Checksum-verified postings that cover canonical SQLite are a clean
    // position-index state.
    {
        auto const manifest_after_close = motif::db::read_manifest(tmp / "db" / "manifest.json");
        REQUIRE(manifest_after_close.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        CHECK_FALSE(manifest_after_close->position_index_dirty);
    }

    {
        auto reopened = motif::db::database_manager::open(tmp / "db");
        REQUIRE(reopened.has_value());
        // Reopen recognizes the covering postings generation.
        REQUIRE(reopened->manifest().position_postings.has_value());
        CHECK(start_position_matches(*reopened) == 3U);

        // Canonical growth without a postings rebuild makes exact queries
        // fail closed rather than silently serving stale data.
        REQUIRE(reopened->insert_game(game_from_sans("White Four", "Black Four", "1-0", {"e4", "c5"})).has_value());
        auto const start_hash = motif::db::zobrist_hash {motif::chess::board {}.hash()};
        auto const stale = reopened->query_position_matches(start_hash);
        REQUIRE_FALSE(stale.has_value());
        CHECK(stale.error() == motif::db::error_code::io_failure);

        // Rebuilding the exact index restores serving.
        REQUIRE(reopened->rebuild_position_postings().has_value());
        CHECK(start_position_matches(*reopened) == 4U);

        reopened->close();
    }

    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: mutation on a postings-only bundle fails closed and reopens repaired", "[motif-import]")
{
    auto const tmp = std::filesystem::temp_directory_path() / "ipl_default_mutation";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto const pgn_file = tmp / "games.pgn";
    {
        std::ofstream out {pgn_file};
        out << k_three_game_pgn;
    }

    {
        auto mgr = motif::db::database_manager::create(tmp / "db", "test");
        REQUIRE(mgr.has_value());
        motif::import::import_pipeline pipeline {*mgr};
        REQUIRE(pipeline.run(pgn_file, motif::import::import_config {}).has_value());

        REQUIRE(mgr->set_manual_game_provenance(motif::db::game_id {1U}, std::nullopt, "new").has_value());
        auto patch = motif::db::game_patch {};
        patch.result = "0-1";
        REQUIRE(mgr->patch_game_metadata(motif::db::game_id {1U}, patch).has_value());
        CHECK_FALSE(mgr->manifest().position_postings.has_value());

        // The mutation invalidated postings, so exact queries fail closed
        // instead of serving stale data.
        auto const start_hash = motif::db::zobrist_hash {motif::chess::board {}.hash()};
        auto const refused = mgr->query_position_matches(start_hash);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error() == motif::db::error_code::io_failure);

        mgr->close();
    }

    {
        // Reopen repairs postings directly from canonical SQLite, then serves
        // the patched result.
        auto reopened = motif::db::database_manager::open(tmp / "db");
        REQUIRE(reopened.has_value());
        REQUIRE(reopened->manifest().position_postings.has_value());
        auto const start_hash = motif::db::zobrist_hash {motif::chess::board {}.hash()};
        auto const matches = reopened->query_position_matches(start_hash);
        REQUIRE(matches.has_value());
        REQUIRE(matches->size() == 3U);
        CHECK((*matches)[0].game_id == motif::db::game_id {1U});
        CHECK((*matches)[0].result == -1);
        reopened->close();
    }

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
    // Exact postings are the default durable position index.
    CHECK(config.build_position_postings_after_import);
    CHECK_FALSE(config.build_opening_tree_index_after_import);
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

TEST_CASE("import_pipeline: ingest-only serial perf", "[performance][motif-import]")
{
    skip_perf_unless_release_build();

    auto const pgn_file = perf_pgn_path();
    if (!std::filesystem::exists(pgn_file)) {
        SKIP("1M-game PGN not available");
    }

    auto summary = run_perf_import(ingest_only_serial_perf_config());
    REQUIRE(summary.has_value());
    print_import_perf_summary("import_pipeline: ingest-only serial perf", *summary);
    check_release_calibrated_perf(summary->elapsed.count());
}

TEST_CASE("import_pipeline: ingest plus postings rebuild perf", "[performance][motif-import]")
{
    skip_perf_unless_release_build();

    auto const pgn_file = perf_pgn_path();
    if (!std::filesystem::exists(pgn_file)) {
        SKIP("1M-game PGN not available");
    }

    auto total_elapsed = run_sqlite_then_rebuild_perf();
    REQUIRE(total_elapsed.has_value());
    print_duration_result("import_pipeline: ingest plus postings rebuild perf", *total_elapsed);
    check_release_calibrated_perf(total_elapsed->count());
}

TEST_CASE("import_pipeline: postings rebuild-only perf", "[performance][motif-import]")
{
    skip_perf_unless_release_build();

    auto const pgn_file = perf_pgn_path();
    if (!std::filesystem::exists(pgn_file)) {
        SKIP("1M-game PGN not available");
    }

    auto rebuild_elapsed = run_rebuild_only_perf();
    REQUIRE(rebuild_elapsed.has_value());
    print_duration_result("import_pipeline: postings rebuild-only perf", *rebuild_elapsed);
    check_release_calibrated_perf(rebuild_elapsed->count());
}

TEST_CASE("import_pipeline: serial postings build perf", "[performance][motif-import]")
{
    skip_perf_unless_release_build();

    auto const pgn_file = perf_pgn_path();
    if (!std::filesystem::exists(pgn_file)) {
        SKIP("1M-game PGN not available");
    }

    auto summary = run_perf_import(postings_serial_perf_config());
    REQUIRE(summary.has_value());
    print_import_perf_summary("import_pipeline: serial postings build perf", *summary);
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

    // No derived-index build: isolates PGN read and canonical game writes.
    auto const ingest_only = motif::import::import_config {
        .build_position_postings_after_import = false,
    };

    static constexpr std::size_t bytes_per_mb = 1024UZ * 1024UZ;
    auto const rss_baseline = test_helpers::read_rss_bytes();
    test_helpers::peak_rss_sampler const sampler;
    auto summary = pipeline.run(pgn_file, ingest_only);
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

    auto const ingest_only = motif::import::import_config {
        .build_position_postings_after_import = false,
    };

    static constexpr std::size_t bytes_per_mb = 1024UZ * 1024UZ;
    auto const rss_baseline = test_helpers::read_rss_bytes();
    test_helpers::peak_rss_sampler const sampler;
    auto summary = pipeline.run(pgn_file, ingest_only);
    REQUIRE(summary.has_value());
    auto rebuild_res = mgr->rebuild_position_postings();
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
