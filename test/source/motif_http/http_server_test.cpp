#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <ratio>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <httplib.h>

#include "motif/db/database_manager.hpp"
#include "motif/db/types.hpp"
#include "motif/http/server.hpp"
#include "motif/import/import_pipeline.hpp"
#include "motif/import/logger.hpp"
#include "test_helpers.hpp"

namespace
{

using test_helpers::is_sanitized_build;

constexpr int wait_poll_count {40};
constexpr std::chrono::milliseconds wait_poll_interval {5};
constexpr auto us_per_ms = 1000.0;
constexpr auto perf_sample_hashes = std::size_t {200};
constexpr auto perf_sample_seed = std::uint64_t {42};
constexpr auto perf_p99_limit_us = 100000.0;

struct tmp_dir
{
    std::filesystem::path path;

    explicit tmp_dir(std::string const& suffix)
    {
        auto const tick =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path()
            / ("motif_http_test_" + suffix + "_" + std::to_string(tick));
    }

    ~tmp_dir() { std::filesystem::remove_all(path); }

    tmp_dir(tmp_dir const&) = delete;
    auto operator=(tmp_dir const&) -> tmp_dir& = delete;
    tmp_dir(tmp_dir&&) = delete;
    auto operator=(tmp_dir&&) -> tmp_dir& = delete;
};

// Wait up to ~200 ms for the server to become ready.
auto wait_for_ready(motif::http::server const& srv) -> bool
{
    for (int i = 0; i < wait_poll_count && !srv.is_running(); ++i) {
        std::this_thread::sleep_for(wait_poll_interval);
    }
    return srv.is_running();
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
auto seed_positions(motif::db::database_manager& dbmgr,
                    std::uint64_t hash,
                    std::size_t count) -> void
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    constexpr std::uint16_t seed_ply {10};
    constexpr std::int16_t seed_white_elo {1600};
    constexpr std::int16_t seed_black_elo {1500};

    std::vector<motif::db::position_row> rows;
    rows.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        rows.push_back(motif::db::position_row {
            .zobrist_hash = hash,
            .game_id = static_cast<std::uint32_t>(i + 1),
            .ply = seed_ply,
            .result = std::int8_t {1},
            .white_elo = seed_white_elo,
            .black_elo = seed_black_elo,
        });
    }
    [[maybe_unused]] auto const res = dbmgr.positions().insert_batch(rows);
}

auto count_game_ids(std::string const& body) -> std::size_t
{
    std::size_t count = 0;
    for (auto pos = body.find("\"game_id\""); pos != std::string::npos;
         pos = body.find("\"game_id\"", pos + 1))
    {
        ++count;
    }
    return count;
}

auto perf_pgn_path() -> std::filesystem::path
{
    // NOLINTNEXTLINE(concurrency-mt-unsafe) -- test-only env override read once
    auto const* const perf_pgn = std::getenv("MOTIF_IMPORT_PERF_PGN");
    if (perf_pgn != nullptr) {
        return std::filesystem::path {perf_pgn};
    }

    auto repo_local = std::filesystem::path {MOTIF_PROJECT_SOURCE_DIR} / "bench"
        / "data" / "twic-bench.pgn";
    if (std::filesystem::exists(repo_local)) {
        return repo_local;
    }

    repo_local = std::filesystem::path {MOTIF_PROJECT_SOURCE_DIR} / "bench"
        / "data" / "twic-1m.pgn";
    if (std::filesystem::exists(repo_local)) {
        return repo_local;
    }

    return "/data/chess/1m_games.pgn";
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

struct query_latency_result
{
    std::size_t num_queries {};
    double total_ms {};
    double p50_us {};
    double p99_us {};
    double min_us {};
    double max_us {};
};

// NOLINTBEGIN(readability-function-cognitive-complexity) -- timing and HTTP
// assertions make this branchy.
void run_http_position_search_perf_test()
{
    skip_perf_unless_release_build();

    auto const pgn_file = perf_pgn_path();
    if (!std::filesystem::exists(pgn_file)) {
        SKIP("PGN corpus not available");
    }

    tmp_dir const tdir {"http_perf"};

    auto manager =
        motif::db::database_manager::create(tdir.path / "db", "http-perf");
    REQUIRE(manager.has_value());

    auto init_log =
        motif::import::initialize_logging({.log_dir = tdir.path / "logs"});
    REQUIRE(init_log.has_value());

    motif::import::import_pipeline pipeline {*manager};
    auto summary = pipeline.run(pgn_file, motif::import::import_config {});
    REQUIRE(summary.has_value());
    REQUIRE(summary->committed > 0);

    auto sample_hashes = manager->positions().sample_zobrist_hashes(
        perf_sample_hashes, perf_sample_seed);
    REQUIRE(sample_hashes.has_value());
    REQUIRE_FALSE(sample_hashes->empty());

    constexpr std::uint16_t test_port {18094};
    motif::http::server srv {*manager};
    std::thread server_thread {
        [&]() -> void
        { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    std::vector<double> latencies_us;
    latencies_us.reserve(sample_hashes->size());

    for (auto const hash : *sample_hashes) {
        auto const path =
            std::string {"/api/positions/"} + std::to_string(hash);
        auto const start = std::chrono::steady_clock::now();
        auto const res = cli.Get(path);
        auto const stop = std::chrono::steady_clock::now();

        REQUIRE(res != nullptr);
        REQUIRE(res->status == 200);

        latencies_us.push_back(
            std::chrono::duration<double, std::micro>(stop - start).count());
    }

    srv.stop();
    server_thread.join();

    auto const shutdown_result = motif::import::shutdown_logging();
    REQUIRE(shutdown_result.has_value());

    std::ranges::sort(latencies_us);
    REQUIRE_FALSE(latencies_us.empty());

    auto const count = latencies_us.size();
    auto total_ms = 0.0;
    for (auto const latency : latencies_us) {
        total_ms += latency;
    }
    total_ms /= us_per_ms;

    auto const p50_idx = std::min(
        count - 1, static_cast<std::size_t>(static_cast<double>(count) * 0.50));
    auto const p99_idx = std::min(
        count - 1, static_cast<std::size_t>(static_cast<double>(count) * 0.99));

    auto const result = query_latency_result {
        .num_queries = count,
        .total_ms = total_ms,
        .p50_us = latencies_us[p50_idx],
        .p99_us = latencies_us[p99_idx],
        .min_us = latencies_us.front(),
        .max_us = latencies_us.back(),
    };

    std::cout << "\n=== position search HTTP endpoint ===\n"
              << "  queries:      " << result.num_queries << "\n"
              << "  total:        " << result.total_ms << " ms\n"
              << "  p50:          " << result.p50_us << " us\n"
              << "  p99:          " << result.p99_us << " us\n"
              << "  min:          " << result.min_us << " us\n"
              << "  max:          " << result.max_us << " us\n";

    CHECK(result.p99_us < perf_p99_limit_us);
}

// NOLINTEND(readability-function-cognitive-complexity)

}  // namespace

TEST_CASE("server: health endpoint returns 200 with ok body", "[motif-http]")
{
    auto const tdir = tmp_dir {"health"};
    auto db_res = motif::db::database_manager::create(tdir.path, "test-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18080};
    motif::http::server srv {*db_res};

    std::thread server_thread {
        [&]() -> void
        { [[maybe_unused]] auto start_res = srv.start(test_port); }};

    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get("/health");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 200);
    CHECK(res->body == R"({"status":"ok"})");
}

TEST_CASE("server: CORS headers present on health response", "[motif-http]")
{
    auto const tdir = tmp_dir {"cors"};
    auto db_res = motif::db::database_manager::create(tdir.path, "cors-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18081};
    motif::http::server srv {*db_res};

    std::thread server_thread {
        [&]() -> void
        { [[maybe_unused]] auto start_res = srv.start(test_port); }};

    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get("/health");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->get_header_value("Access-Control-Allow-Origin") == "*");
    CHECK(res->get_header_value("Access-Control-Allow-Methods")
          == "GET, POST, DELETE, OPTIONS");
    CHECK(res->get_header_value("Access-Control-Allow-Headers")
          == "Content-Type");
}

TEST_CASE("server: OPTIONS preflight returns 200 with CORS headers",
          "[motif-http]")
{
    auto const tdir = tmp_dir {"preflight"};
    auto db_res =
        motif::db::database_manager::create(tdir.path, "preflight-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18082};
    motif::http::server srv {*db_res};

    std::thread server_thread {
        [&]() -> void
        { [[maybe_unused]] auto start_res = srv.start(test_port); }};

    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Options("/health");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 200);
    CHECK(res->get_header_value("Access-Control-Allow-Origin") == "*");
}

TEST_CASE("server: position search empty DB returns 200 with empty array",
          "[motif-http]")
{
    auto const tdir = tmp_dir {"pos_empty"};
    auto db_res =
        motif::db::database_manager::create(tdir.path, "pos-empty-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18083};
    motif::http::server srv {*db_res};
    std::thread server_thread {
        [&]() -> void
        { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get("/api/positions/12345");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 200);
    CHECK(res->body == "[]");
}

TEST_CASE("server: position search invalid hash returns 400", "[motif-http]")
{
    auto const tdir = tmp_dir {"pos_badhash"};
    auto db_res = motif::db::database_manager::create(tdir.path, "pos-bad-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18084};
    motif::http::server srv {*db_res};
    std::thread server_thread {
        [&]() -> void
        { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get("/api/positions/not_a_number");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 400);
    CHECK(res->body == R"({"error":"invalid zobrist hash"})");
}

TEST_CASE("server: position search empty hash returns 400", "[motif-http]")
{
    auto const tdir = tmp_dir {"pos_emptyhash"};
    auto db_res =
        motif::db::database_manager::create(tdir.path, "pos-emptyhash-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18088};
    motif::http::server srv {*db_res};
    std::thread server_thread {
        [&]() -> void
        { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get("/api/positions/");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 400);
    CHECK(res->body == R"({"error":"invalid zobrist hash"})");
}

// NOLINTBEGIN(readability-function-cognitive-complexity) -- Catch2 CHECK
// macros inflate complexity in this assertion-heavy test.
TEST_CASE(
    "server: position search populated DB returns 200 with correct fields",
    "[motif-http]")
{
    constexpr std::uint64_t pop_hash {999};

    auto const tdir = tmp_dir {"pos_populated"};
    auto db_res = motif::db::database_manager::create(tdir.path, "pos-pop-db");
    REQUIRE(db_res.has_value());
    seed_positions(*db_res, pop_hash, /*count=*/2);

    constexpr std::uint16_t test_port {18085};
    motif::http::server srv {*db_res};
    std::thread server_thread {
        [&]() -> void
        { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get("/api/positions/999");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 200);
    auto const& body = res->body;
    CHECK(body.front() == '[');
    CHECK(body.contains("\"game_id\""));
    CHECK(body.contains("\"ply\""));
    CHECK(body.contains("\"result\""));
    CHECK(body.contains("\"white_elo\""));
    CHECK(body.contains("\"black_elo\""));
}

// NOLINTEND(readability-function-cognitive-complexity)

TEST_CASE("server: position search honors limit parameter", "[motif-http]")
{
    constexpr std::uint64_t page_hash {42};
    constexpr std::size_t page_total {10};

    auto const tdir = tmp_dir {"pos_page"};
    auto db_res = motif::db::database_manager::create(tdir.path, "page-db");
    REQUIRE(db_res.has_value());
    seed_positions(*db_res, page_hash, page_total);

    constexpr std::uint16_t test_port {18086};
    motif::http::server srv {*db_res};
    std::thread server_thread {
        [&]() -> void
        { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get("/api/positions/42?limit=3&offset=0");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 200);
    auto const& body = res->body;
    CHECK(body.front() == '[');
    CHECK(count_game_ids(body) == 3);
}

TEST_CASE("server: position search invalid limit returns 400", "[motif-http]")
{
    auto const tdir = tmp_dir {"pos_badlimit"};
    auto db_res = motif::db::database_manager::create(tdir.path, "badlimit-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18087};
    motif::http::server srv {*db_res};
    std::thread server_thread {
        [&]() -> void
        { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get("/api/positions/1?limit=abc");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 400);
    CHECK(res->body == R"({"error":"invalid pagination parameters"})");
}

TEST_CASE("server: position search defaults limit to 100", "[motif-http]")
{
    constexpr std::uint64_t page_hash {43};
    constexpr std::size_t page_total {150};

    auto const tdir = tmp_dir {"pos_default_limit"};
    auto db_res =
        motif::db::database_manager::create(tdir.path, "default-limit-db");
    REQUIRE(db_res.has_value());
    seed_positions(*db_res, page_hash, page_total);

    constexpr std::uint16_t test_port {18089};
    motif::http::server srv {*db_res};
    std::thread server_thread {
        [&]() -> void
        { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get("/api/positions/43");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 200);
    CHECK(count_game_ids(res->body) == 100);
}

TEST_CASE("server: position search clamps limit to 500", "[motif-http]")
{
    constexpr std::uint64_t page_hash {44};
    constexpr std::size_t page_total {600};

    auto const tdir = tmp_dir {"pos_limit_clamp"};
    auto db_res =
        motif::db::database_manager::create(tdir.path, "limit-clamp-db");
    REQUIRE(db_res.has_value());
    seed_positions(*db_res, page_hash, page_total);

    constexpr std::uint16_t test_port {18090};
    motif::http::server srv {*db_res};
    std::thread server_thread {
        [&]() -> void
        { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get("/api/positions/44?limit=999");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 200);
    CHECK(count_game_ids(res->body) == 500);
}

TEST_CASE("server: position search limit zero returns empty array",
          "[motif-http]")
{
    constexpr std::uint64_t page_hash {45};
    constexpr std::size_t page_total {10};

    auto const tdir = tmp_dir {"pos_limit_zero"};
    auto db_res =
        motif::db::database_manager::create(tdir.path, "limit-zero-db");
    REQUIRE(db_res.has_value());
    seed_positions(*db_res, page_hash, page_total);

    constexpr std::uint16_t test_port {18091};
    motif::http::server srv {*db_res};
    std::thread server_thread {
        [&]() -> void
        { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get("/api/positions/45?limit=0&offset=4");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 200);
    CHECK(res->body == "[]");
}

TEST_CASE("server: position search invalid offset returns 400", "[motif-http]")
{
    auto const tdir = tmp_dir {"pos_badoffset"};
    auto db_res =
        motif::db::database_manager::create(tdir.path, "badoffset-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18092};
    motif::http::server srv {*db_res};
    std::thread server_thread {
        [&]() -> void
        { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get("/api/positions/1?offset=-1");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 400);
    CHECK(res->body == R"({"error":"invalid pagination parameters"})");
}

TEST_CASE("server: position search offset beyond end returns empty array",
          "[motif-http]")
{
    constexpr std::uint64_t page_hash {46};
    constexpr std::size_t page_total {5};

    auto const tdir = tmp_dir {"pos_offset_end"};
    auto db_res =
        motif::db::database_manager::create(tdir.path, "offset-end-db");
    REQUIRE(db_res.has_value());
    seed_positions(*db_res, page_hash, page_total);

    constexpr std::uint16_t test_port {18093};
    motif::http::server srv {*db_res};
    std::thread server_thread {
        [&]() -> void
        { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get("/api/positions/46?limit=3&offset=99");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 200);
    CHECK(res->body == "[]");
}

TEST_CASE("server: position search endpoint meets latency target",
          "[performance][motif-http]")
{
    run_http_position_search_perf_test();
}
