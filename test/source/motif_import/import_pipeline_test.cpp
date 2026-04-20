#include <chrono>
#include <filesystem>
#include <fstream>

#include "motif/import/import_pipeline.hpp"

#include <catch2/catch_test_macros.hpp>

#include "motif/db/database_manager.hpp"
#include "motif/import/checkpoint.hpp"
#include "motif/import/error.hpp"

namespace
{

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

constexpr motif::import::import_config k_single_worker {
    .num_workers = 1,
    .num_lines = 4,
    .batch_size = 2,
};

}  // namespace

TEST_CASE(
    "import_pipeline: run imports games and deletes checkpoint on success",
    "[motif-import]")
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
    CHECK_FALSE(
        std::filesystem::exists(motif::import::checkpoint_path(mgr->dir())));

    mgr->close();
    std::filesystem::remove_all(tmp);
}

TEST_CASE(
    "import_pipeline: resume skips already-committed games (duplicate policy)",
    "[motif-import]")
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

TEST_CASE(
    "import_pipeline: resume returns io_failure when no checkpoint exists",
    "[motif-import]")
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

TEST_CASE(
    "import_pipeline: resume rejects checkpoints for a different source file",
    "[motif-import]")
{
    auto const tmp =
        std::filesystem::temp_directory_path() / "ipl_wrong_source";
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
    REQUIRE(
        motif::import::write_checkpoint(mgr->dir(), checkpoint).has_value());

    motif::import::import_pipeline pipeline {*mgr};
    auto result = pipeline.resume(second_pgn, k_single_worker);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == motif::import::error_code::invalid_state);

    mgr->close();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: progress reflects committed count after run",
          "[motif-import]")
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

    mgr->close();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: progress is empty before the first run",
          "[motif-import]")
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
    CHECK(prog.elapsed == std::chrono::milliseconds {0});

    mgr->close();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: parse errors count as one attempted game",
          "[motif-import]")
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

    mgr->close();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("import_pipeline: failed runs preserve existing checkpoints",
          "[motif-import]")
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
    REQUIRE(
        motif::import::write_checkpoint(mgr->dir(), checkpoint).has_value());

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

TEST_CASE("import_pipeline: 1M games under 120s", "[performance][motif-import]")
{
    auto const pgn_file = std::filesystem::path {"/data/chess/1m_games.pgn"};
    if (!std::filesystem::exists(pgn_file)) {
        SKIP("1M-game PGN not available");
    }

    auto const tmp = std::filesystem::temp_directory_path() / "ipl_perf";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto mgr = motif::db::database_manager::create(tmp / "db", "perf");
    REQUIRE(mgr.has_value());

    motif::import::import_pipeline pipeline {*mgr};
    auto summary = pipeline.run(pgn_file);
    REQUIRE(summary.has_value());
    CHECK(summary->elapsed.count() < 120'000);

    mgr->close();
    std::filesystem::remove_all(tmp);
}
