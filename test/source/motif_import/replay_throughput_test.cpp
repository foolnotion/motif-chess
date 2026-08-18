// Spike: throughput of the replay primitive a DuckDB-free redesign depends
// on -- decode + apply_encoded_move + read the incremental Zobrist hash,
// not SAN parsing. Release-only, like the other [performance] tests.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/core.h>

#include "motif/chess/chess.hpp"
#include "motif/import/pgn_reader.hpp"
#include "test_helpers.hpp"

using test_helpers::is_sanitized_build;

namespace
{

auto replay_perf_pgn_path() -> std::filesystem::path
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

// Pre-decoded corpus: one encoded_move stream per game. Built once via
// motif::chess::apply_san, the exact conversion import already performs
// (see import_worker.cpp), so the timed loop below never touches SAN text.
auto load_encoded_corpus(std::filesystem::path const& pgn_path) -> std::vector<std::vector<std::uint16_t>>
{
    auto corpus = std::vector<std::vector<std::uint16_t>> {};
    motif::import::pgn_reader reader {pgn_path};

    while (true) {
        auto game_res = reader.next();
        if (!game_res) {
            if (game_res.error() == motif::import::error_code::parse_error) {
                continue;
            }
            break;
        }

        auto board = motif::chess::board {};
        auto encoded = std::vector<std::uint16_t> {};
        encoded.reserve(game_res->moves.size());

        auto parsed_cleanly = true;
        for (auto const& node : game_res->moves) {
            auto move_res = motif::chess::apply_san(board, node.san);
            if (!move_res) {
                parsed_cleanly = false;
                break;
            }
            encoded.push_back(*move_res);
        }

        if (parsed_cleanly && !encoded.empty()) {
            corpus.push_back(std::move(encoded));
        }
    }

    return corpus;
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- perf setup/reporting is branchy; Catch2 macros inflate this further.
TEST_CASE("replay_throughput: apply_encoded_move + hash on a real corpus", "[performance][replay-spike]")
{
    if (is_sanitized_build) {
        SKIP("performance checks are skipped in sanitize builds");
    }
#ifndef NDEBUG
    SKIP("performance checks run only in release builds");
#endif

    auto const pgn_path = replay_perf_pgn_path();
    if (!std::filesystem::exists(pgn_path)) {
        SKIP("no perf corpus available");
    }

    auto const corpus = load_encoded_corpus(pgn_path);
    REQUIRE(!corpus.empty());

    std::size_t total_moves = 0;
    for (auto const& game : corpus) {
        total_moves += game.size();
    }

    constexpr auto rounds = std::size_t {3};
    constexpr auto nanoseconds_per_second = 1'000'000'000.0;
    constexpr auto sanity_floor_moves_per_second = 1'000'000.0;
    std::uint64_t checksum = 0;
    std::size_t timed_moves = 0;

    auto const started = std::chrono::steady_clock::now();
    for (std::size_t round = 0; round < rounds; ++round) {
        for (auto const& game : corpus) {
            auto board = motif::chess::board {};
            for (auto const encoded_move : game) {
                motif::chess::apply_encoded_move(board, encoded_move);
                checksum ^= board.hash();
                ++timed_moves;
            }
        }
    }
    auto const elapsed = std::chrono::steady_clock::now() - started;

    auto const elapsed_ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    auto const ns_per_move = elapsed_ns / static_cast<double>(timed_moves);
    auto const moves_per_second = static_cast<double>(timed_moves) * nanoseconds_per_second / elapsed_ns;

    fmt::print("\n=== replay_throughput: apply_encoded_move + hash ===\n"
               "  games:          {}\n"
               "  total moves:    {}\n"
               "  rounds:         {}\n"
               "  timed moves:    {}\n"
               "  elapsed:        {} ms\n"
               "  ns/move:        {:.2f}\n"
               "  moves/sec:      {:.0f}\n"
               "  checksum:       {}\n",
               corpus.size(), total_moves, rounds, timed_moves,
               std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), ns_per_move, moves_per_second, checksum);

    // Sanity floor, not a hard perf gate: catches a build that accidentally
    // runs unoptimized (e.g. -O0 slipping into a "release" preset) rather
    // than asserting a specific throughput target the redesign estimate
    // depends on.
    CHECK(moves_per_second > sanity_floor_moves_per_second);
}
