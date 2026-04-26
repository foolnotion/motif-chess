#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <optional>
#include <ratio>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <chesslib/board/board.hpp>
#include <chesslib/board/move_codec.hpp>
#include <chesslib/util/san.hpp>
#include <fmt/base.h>
#include <fmt/format.h>
#include <glaze/json/read.hpp>
#include <httplib.h>
// NOLINTNEXTLINE(hicpp-deprecated-headers,modernize-deprecated-headers) -- POSIX setenv/unsetenv are declared here.
#include <stdlib.h>

#include "motif/db/database_manager.hpp"
#include "motif/db/types.hpp"
#include "motif/http/server.hpp"
#include "motif/import/checkpoint.hpp"
#include "motif/import/import_pipeline.hpp"
#include "motif/import/logger.hpp"
#include "test_helpers.hpp"

// Placed in a named namespace so glaze aggregate reflection can find it
// (glaze cannot reflect types in anonymous namespaces).
namespace motif_http_test
{

// Union of all fields that appear in SSE data lines (progress, complete, error).
// Unknown keys are silently ignored by glaze.
struct sse_event_payload
{
    std::optional<std::size_t> games_processed;
    std::optional<std::size_t> games_committed;
    std::optional<std::size_t> games_skipped;
    std::optional<double> elapsed_seconds;
    std::optional<std::size_t> total_attempted;
    std::optional<std::size_t> committed;
    std::optional<std::size_t> skipped;
    std::optional<std::size_t> errors;
    std::optional<std::size_t> elapsed_ms;
    std::optional<std::string> error;
};

}  // namespace motif_http_test

namespace
{

using test_helpers::is_sanitized_build;

constexpr int wait_poll_count {40};
constexpr std::chrono::milliseconds wait_poll_interval {5};
constexpr std::chrono::milliseconds import_short_settle_ms {100};
constexpr int sse_read_timeout_s {10};
constexpr std::size_t long_import_game_count {2000};
constexpr auto us_per_ms = 1000.0;
constexpr auto perf_sample_hashes = std::size_t {200};
constexpr auto perf_sample_seed = std::uint64_t {42};
constexpr auto perf_p99_limit_us = 100000.0;
constexpr auto opening_stats_perf_p99_limit_us = 500000.0;
constexpr std::chrono::milliseconds opening_stats_endpoint_limit {500};
constexpr std::string_view fail_next_import_worker_start_env {"MOTIF_HTTP_TEST_FAIL_NEXT_IMPORT_WORKER_START"};
constexpr std::string_view fail_next_import_worker_run_env {"MOTIF_HTTP_TEST_FAIL_NEXT_IMPORT_WORKER_RUN"};

struct tmp_dir
{
    std::filesystem::path path;

    explicit tmp_dir(std::string const& suffix)
    {
        auto const tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / fmt::format("motif_http_test_{}_{}", suffix, tick);
    }

    ~tmp_dir() { std::filesystem::remove_all(path); }

    tmp_dir(tmp_dir const&) = delete;
    auto operator=(tmp_dir const&) -> tmp_dir& = delete;
    tmp_dir(tmp_dir&&) = delete;
    auto operator=(tmp_dir&&) -> tmp_dir& = delete;
};

class import_logging_scope
{
  public:
    explicit import_logging_scope(std::filesystem::path const& log_dir)
    {
        auto init_log = motif::import::initialize_logging({.log_dir = log_dir});
        REQUIRE(init_log.has_value());
        active_ = true;
    }

    ~import_logging_scope() noexcept
    {
        if (active_) {
            // Result cannot be propagated from a destructor; use shutdown() to detect errors.
            static_cast<void>(motif::import::shutdown_logging());
        }
    }

    import_logging_scope(import_logging_scope const&) = delete;
    auto operator=(import_logging_scope const&) -> import_logging_scope& = delete;
    import_logging_scope(import_logging_scope&&) = delete;
    auto operator=(import_logging_scope&&) -> import_logging_scope& = delete;

    void shutdown()
    {
        if (!active_) {
            return;
        }
        auto const shutdown_result = motif::import::shutdown_logging();
        active_ = false;
        REQUIRE(shutdown_result.has_value());
    }

  private:
    bool active_ {false};
};

class env_flag_scope
{
  public:
    explicit env_flag_scope(std::string_view name)
        : name_ {name}
    {
        // NOLINTNEXTLINE(concurrency-mt-unsafe) -- set before server threads start.
        auto const set_result = ::setenv(name_.c_str(), "1", 1);
        REQUIRE(set_result == 0);
    }

    ~env_flag_scope() noexcept
    {
        if (active_) {
            // NOLINTNEXTLINE(concurrency-mt-unsafe) -- test cleanup after construction-time read.
            static_cast<void>(::unsetenv(name_.c_str()));
        }
    }

    env_flag_scope(env_flag_scope const&) = delete;
    auto operator=(env_flag_scope const&) -> env_flag_scope& = delete;
    env_flag_scope(env_flag_scope&&) = delete;
    auto operator=(env_flag_scope&&) -> env_flag_scope& = delete;

    void clear()
    {
        if (!active_) {
            return;
        }
        // NOLINTNEXTLINE(concurrency-mt-unsafe) -- clear immediately after server construction.
        auto const unset_result = ::unsetenv(name_.c_str());
        active_ = false;
        REQUIRE(unset_result == 0);
    }

  private:
    std::string name_;
    bool active_ {true};
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
auto seed_positions(motif::db::database_manager& dbmgr, std::uint64_t hash, std::size_t count) -> void
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

auto encode_moves_for_game(std::initializer_list<char const*> sans) -> std::vector<std::uint16_t>
{
    auto board = chesslib::board {};
    auto moves = std::vector<std::uint16_t> {};
    for (char const* const san : sans) {
        auto move = chesslib::san::from_string(board, san);
        REQUIRE(move.has_value());
        moves.push_back(chesslib::codec::encode(*move));
        chesslib::move_maker {board, *move}.make();
    }
    return moves;
}

auto count_game_ids(std::string const& body) -> std::size_t
{
    std::size_t count = 0;
    for (auto pos = body.find("\"game_id\""); pos != std::string::npos; pos = body.find("\"game_id\"", pos + 1)) {
        ++count;
    }
    return count;
}

auto count_game_list_ids(std::string const& body) -> std::size_t
{
    std::size_t count = 0;
    for (auto pos = body.find("\"id\""); pos != std::string::npos; pos = body.find("\"id\"", pos + 1)) {
        ++count;
    }
    return count;
}

struct http_game_seed
{
    std::string white;
    std::string black;
    std::string result;
    std::string event;
    std::string date;
    std::string eco;
    std::uint16_t move_seed {};
};

auto make_http_player(std::string name) -> motif::db::player
{
    return motif::db::player {
        .name = std::move(name),
        .elo = std::nullopt,
        .title = std::nullopt,
        .country = std::nullopt,
    };
}

auto insert_http_game(motif::db::database_manager& dbmgr, http_game_seed seed) -> std::uint32_t
{
    auto game = motif::db::game {
        .white = make_http_player(std::move(seed.white)),
        .black = make_http_player(std::move(seed.black)),
        .event_details =
            motif::db::event {
                .name = std::move(seed.event),
                .site = std::nullopt,
                .date = std::nullopt,
            },
        .date = std::move(seed.date),
        .result = std::move(seed.result),
        .eco = std::move(seed.eco),
        .moves = {seed.move_seed},
        .extra_tags = {},
    };
    auto inserted = dbmgr.store().insert(game);
    REQUIRE(inserted.has_value());
    return *inserted;
}

auto insert_detailed_http_game(motif::db::database_manager& dbmgr) -> std::pair<std::uint32_t, std::vector<std::uint16_t>>
{
    constexpr std::int32_t white_elo {2865};
    constexpr std::int32_t black_elo {2792};
    auto moves = encode_moves_for_game({"e4", "e5"});
    auto game = motif::db::game {
        .white =
            motif::db::player {
                .name = "Magnus Carlsen",
                .elo = white_elo,
                .title = "GM",
                .country = "NO",
            },
        .black =
            motif::db::player {
                .name = "Ian Nepomniachtchi",
                .elo = black_elo,
                .title = "GM",
                .country = "RU",
            },
        .event_details =
            motif::db::event {
                .name = "WCC 2021",
                .site = "Dubai",
                .date = "2021",
            },
        .date = "2021.12.03",
        .result = "1-0",
        .eco = "C88",
        .moves = moves,
        .extra_tags = {{"Opening", "Ruy Lopez"}, {"Round", "6"}},
    };
    auto inserted = dbmgr.store().insert(game);
    REQUIRE(inserted.has_value());
    return {*inserted, std::move(moves)};
}

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

constexpr auto three_game_pgn_content = R"pgn(
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

auto write_pgn_fixture(std::filesystem::path const& dir, std::string_view content) -> std::filesystem::path
{
    auto path = dir / "fixture.pgn";
    std::ofstream out {path};
    out << content;
    return path;
}

auto make_repeated_pgn(std::size_t const game_count) -> std::string
{
    auto pgn = std::string {};
    for (std::size_t game_index = 0; game_index < game_count; ++game_index) {
        pgn += fmt::format(
            "[Event \"Long Import {}\"]\n[Site \"?\"]\n[Date \"2024.01.01\"]\n[Round \"{}\"]\n"
            "[White \"A{}\"]\n[Black \"B{}\"]\n[Result \"1-0\"]\n\n1. e4 e5 2. Nf3 Nc6 1-0\n\n",
            game_index,
            game_index + 1,
            game_index,
            game_index);
    }
    return pgn;
}

auto extract_import_id(std::string const& body) -> std::string
{
    auto const key = std::string {R"("import_id":")"};
    auto const id_start = body.find(key);
    REQUIRE(id_start != std::string::npos);
    auto const value_start = id_start + key.size();
    auto const id_end = body.find('"', value_start);
    REQUIRE(id_end != std::string::npos);
    return body.substr(value_start, id_end - value_start);
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

    auto manager = motif::db::database_manager::create(tdir.path / "db", "http-perf");
    REQUIRE(manager.has_value());

    auto logging = import_logging_scope {tdir.path / "logs"};

    motif::import::import_pipeline pipeline {*manager};
    auto summary = pipeline.run(pgn_file, motif::import::import_config {});
    REQUIRE(summary.has_value());
    REQUIRE(summary->committed > 0);

    auto sample_hashes = manager->positions().sample_zobrist_hashes(perf_sample_hashes, perf_sample_seed);
    REQUIRE(sample_hashes.has_value());
    REQUIRE_FALSE(sample_hashes->empty());

    constexpr std::uint16_t test_port {18094};
    motif::http::server srv {*manager};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    std::vector<double> latencies_us;
    latencies_us.reserve(sample_hashes->size());

    for (auto const hash : *sample_hashes) {
        auto const path = fmt::format("/api/positions/{}", hash);
        auto const start = std::chrono::steady_clock::now();
        auto const res = cli.Get(path);
        auto const stop = std::chrono::steady_clock::now();

        REQUIRE(res != nullptr);
        REQUIRE(res->status == 200);

        latencies_us.push_back(std::chrono::duration<double, std::micro>(stop - start).count());
    }

    srv.stop();
    server_thread.join();

    logging.shutdown();

    std::ranges::sort(latencies_us);
    REQUIRE_FALSE(latencies_us.empty());

    auto const count = latencies_us.size();
    auto total_ms = 0.0;
    for (auto const latency : latencies_us) {
        total_ms += latency;
    }
    total_ms /= us_per_ms;

    auto const p50_idx = std::min(count - 1, static_cast<std::size_t>(static_cast<double>(count) * 0.50));
    auto const p99_idx = std::min(count - 1, static_cast<std::size_t>(static_cast<double>(count) * 0.99));

    auto const result = query_latency_result {
        .num_queries = count,
        .total_ms = total_ms,
        .p50_us = latencies_us[p50_idx],
        .p99_us = latencies_us[p99_idx],
        .min_us = latencies_us.front(),
        .max_us = latencies_us.back(),
    };

    fmt::print(stdout,
               "\n=== position search HTTP endpoint ===\n"
               "  queries:      {}\n"
               "  total:        {} ms\n"
               "  p50:          {} us\n"
               "  p99:          {} us\n"
               "  min:          {} us\n"
               "  max:          {} us\n",
               result.num_queries,
               result.total_ms,
               result.p50_us,
               result.p99_us,
               result.min_us,
               result.max_us);

    CHECK(result.p99_us < perf_p99_limit_us);
}

// NOLINTEND(readability-function-cognitive-complexity)

// NOLINTBEGIN(readability-function-cognitive-complexity)
void run_http_opening_stats_perf_test()
{
    skip_perf_unless_release_build();

    auto const pgn_file = perf_pgn_path();
    if (!std::filesystem::exists(pgn_file)) {
        SKIP("PGN corpus not available");
    }

    tmp_dir const tdir {"http_ostats_perf"};

    auto manager = motif::db::database_manager::create(tdir.path / "db", "ostats-perf");
    REQUIRE(manager.has_value());

    auto logging = import_logging_scope {tdir.path / "logs"};

    motif::import::import_pipeline pipeline {*manager};
    auto summary = pipeline.run(pgn_file, motif::import::import_config {});
    REQUIRE(summary.has_value());
    REQUIRE(summary->committed > 0);

    auto sample_hashes = manager->positions().sample_zobrist_hashes(perf_sample_hashes, perf_sample_seed);
    REQUIRE(sample_hashes.has_value());
    REQUIRE_FALSE(sample_hashes->empty());

    constexpr std::uint16_t test_port {18100};
    motif::http::server srv {*manager};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    std::vector<double> latencies_us;
    latencies_us.reserve(sample_hashes->size());

    for (auto const hash : *sample_hashes) {
        auto const path = fmt::format("/api/openings/{}/stats", hash);
        auto const start = std::chrono::steady_clock::now();
        auto const res = cli.Get(path);
        auto const stop = std::chrono::steady_clock::now();

        REQUIRE(res != nullptr);
        REQUIRE(res->status == 200);

        latencies_us.push_back(std::chrono::duration<double, std::micro>(stop - start).count());
    }

    srv.stop();
    server_thread.join();

    logging.shutdown();

    std::ranges::sort(latencies_us);
    REQUIRE_FALSE(latencies_us.empty());

    auto const count = latencies_us.size();
    auto total_ms = 0.0;
    for (auto const latency : latencies_us) {
        total_ms += latency;
    }
    total_ms /= us_per_ms;

    auto const p50_idx = std::min(count - 1, static_cast<std::size_t>(static_cast<double>(count) * 0.50));
    auto const p99_idx = std::min(count - 1, static_cast<std::size_t>(static_cast<double>(count) * 0.99));

    auto const result = query_latency_result {
        .num_queries = count,
        .total_ms = total_ms,
        .p50_us = latencies_us[p50_idx],
        .p99_us = latencies_us[p99_idx],
        .min_us = latencies_us.front(),
        .max_us = latencies_us.back(),
    };

    fmt::print(stdout,
               "\n=== opening stats HTTP endpoint ===\n"
               "  queries:      {}\n"
               "  total:        {} ms\n"
               "  p50:          {} us\n"
               "  p99:          {} us\n"
               "  min:          {} us\n"
               "  max:          {} us\n",
               result.num_queries,
               result.total_ms,
               result.p50_us,
               result.p99_us,
               result.min_us,
               result.max_us);

    CHECK(result.p99_us < opening_stats_perf_p99_limit_us);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void run_import_sse_test()
{
    auto const tdir = tmp_dir {"import_sse"};
    auto db_res = motif::db::database_manager::create(tdir.path / "db", "import-sse");
    REQUIRE(db_res.has_value());

    auto const pgn_path = write_pgn_fixture(tdir.path, three_game_pgn_content);
    auto logging = import_logging_scope {tdir.path / "logs"};

    constexpr std::uint16_t test_port {18103};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    cli.set_read_timeout(sse_read_timeout_s);

    auto const post_res = cli.Post("/api/imports", R"({"path":")" + pgn_path.string() + R"("})", "application/json");
    REQUIRE(post_res != nullptr);
    REQUIRE(post_res->status == 202);

    auto const import_id = extract_import_id(post_res->body);
    REQUIRE(!import_id.empty());

    std::string collected_events;
    cli.Get("/api/imports/" + import_id + "/progress",
            httplib::Headers {},
            [&collected_events](const char* data, size_t size) -> bool
            {
                collected_events.append(data, size);
                return true;
            });

    srv.stop();
    server_thread.join();
    logging.shutdown();

    CHECK(collected_events.contains("games_processed"));
    CHECK(collected_events.contains("games_committed"));
    CHECK(collected_events.contains("games_skipped"));
    CHECK(collected_events.contains("elapsed_seconds"));
    CHECK(collected_events.contains("event: complete"));
    CHECK(collected_events.contains("total_attempted"));
    CHECK(collected_events.contains("committed"));
    CHECK(collected_events.contains("skipped"));
    CHECK(collected_events.contains("errors"));
    CHECK(collected_events.contains("elapsed_ms"));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void run_import_delete_test()
{
    auto const tdir = tmp_dir {"import_delete"};
    auto db_res = motif::db::database_manager::create(tdir.path / "db", "import-del");
    REQUIRE(db_res.has_value());

    auto const pgn_path = write_pgn_fixture(tdir.path, make_repeated_pgn(long_import_game_count));
    auto logging = import_logging_scope {tdir.path / "logs"};

    constexpr std::uint16_t test_port {18104};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    cli.set_read_timeout(sse_read_timeout_s);

    auto const post_res = cli.Post("/api/imports", R"({"path":")" + pgn_path.string() + R"("})", "application/json");
    REQUIRE(post_res != nullptr);
    REQUIRE(post_res->status == 202);

    auto const import_id = extract_import_id(post_res->body);
    REQUIRE(!import_id.empty());

    auto const del_res = cli.Delete("/api/imports/" + import_id);
    REQUIRE(del_res != nullptr);
    CHECK(del_res->status == 200);
    CHECK(del_res->body.contains(R"("cancellation_requested")"));

    std::string collected_events;
    cli.Get("/api/imports/" + import_id + "/progress",
            httplib::Headers {},
            [&collected_events](const char* data, size_t size) -> bool
            {
                collected_events.append(data, size);
                return true;
            });

    CHECK(collected_events.contains("event: complete"));
    CHECK(std::filesystem::exists(motif::import::checkpoint_path(db_res->dir())));

    srv.stop();
    server_thread.join();
    logging.shutdown();
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

    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};

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

    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};

    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get("/health");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->get_header_value("Access-Control-Allow-Origin") == "*");
    CHECK(res->get_header_value("Access-Control-Allow-Methods") == "GET, POST, DELETE, OPTIONS");
    CHECK(res->get_header_value("Access-Control-Allow-Headers") == "Content-Type");
}

TEST_CASE("server: OPTIONS preflight returns 200 with CORS headers", "[motif-http]")
{
    auto const tdir = tmp_dir {"preflight"};
    auto db_res = motif::db::database_manager::create(tdir.path, "preflight-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18082};
    motif::http::server srv {*db_res};

    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};

    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Options("/health");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 200);
    CHECK(res->get_header_value("Access-Control-Allow-Origin") == "*");
}

TEST_CASE("server: position search empty DB returns 200 with empty array", "[motif-http]")
{
    auto const tdir = tmp_dir {"pos_empty"};
    auto db_res = motif::db::database_manager::create(tdir.path, "pos-empty-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18083};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
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
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
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
    auto db_res = motif::db::database_manager::create(tdir.path, "pos-emptyhash-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18088};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
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
TEST_CASE("server: position search populated DB returns 200 with correct fields", "[motif-http]")
{
    constexpr std::uint64_t pop_hash {999};

    auto const tdir = tmp_dir {"pos_populated"};
    auto db_res = motif::db::database_manager::create(tdir.path, "pos-pop-db");
    REQUIRE(db_res.has_value());
    seed_positions(*db_res, pop_hash, /*count=*/2);

    constexpr std::uint16_t test_port {18085};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get("/api/positions/999");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);
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
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
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
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
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
    auto db_res = motif::db::database_manager::create(tdir.path, "default-limit-db");
    REQUIRE(db_res.has_value());
    seed_positions(*db_res, page_hash, page_total);

    constexpr std::uint16_t test_port {18089};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
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
    auto db_res = motif::db::database_manager::create(tdir.path, "limit-clamp-db");
    REQUIRE(db_res.has_value());
    seed_positions(*db_res, page_hash, page_total);

    constexpr std::uint16_t test_port {18090};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get("/api/positions/44?limit=999");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 200);
    CHECK(count_game_ids(res->body) == 500);
}

TEST_CASE("server: position search limit zero returns empty array", "[motif-http]")
{
    constexpr std::uint64_t page_hash {45};
    constexpr std::size_t page_total {10};

    auto const tdir = tmp_dir {"pos_limit_zero"};
    auto db_res = motif::db::database_manager::create(tdir.path, "limit-zero-db");
    REQUIRE(db_res.has_value());
    seed_positions(*db_res, page_hash, page_total);

    constexpr std::uint16_t test_port {18091};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
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
    auto db_res = motif::db::database_manager::create(tdir.path, "badoffset-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18092};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get("/api/positions/1?offset=-1");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 400);
    CHECK(res->body == R"({"error":"invalid pagination parameters"})");
}

TEST_CASE("server: position search offset beyond end returns empty array", "[motif-http]")
{
    constexpr std::uint64_t page_hash {46};
    constexpr std::size_t page_total {5};

    auto const tdir = tmp_dir {"pos_offset_end"};
    auto db_res = motif::db::database_manager::create(tdir.path, "offset-end-db");
    REQUIRE(db_res.has_value());
    seed_positions(*db_res, page_hash, page_total);

    constexpr std::uint16_t test_port {18093};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get("/api/positions/46?limit=3&offset=99");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 200);
    CHECK(res->body == "[]");
}

TEST_CASE("server: position search endpoint meets latency target", "[performance][motif-http]")
{
    run_http_position_search_perf_test();
}

TEST_CASE("server: opening stats rejects invalid hash", "[motif-http]")
{
    auto const tdir = tmp_dir {"ostats_badhash"};
    auto db_res = motif::db::database_manager::create(tdir.path, "ostats-bad-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18095};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get("/api/openings/not-a-hash/stats");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 400);
    CHECK(res->body == R"({"error":"invalid zobrist hash"})");
}

TEST_CASE("server: opening stats rejects negative hash", "[motif-http]")
{
    auto const tdir = tmp_dir {"ostats_neghash"};
    auto db_res = motif::db::database_manager::create(tdir.path, "ostats-neg-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18098};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get("/api/openings/-1/stats");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 400);
    CHECK(res->body == R"({"error":"invalid zobrist hash"})");
}

TEST_CASE("server: opening stats rejects overflow hash", "[motif-http]")
{
    auto const tdir = tmp_dir {"ostats_overhash"};
    auto db_res = motif::db::database_manager::create(tdir.path, "ostats-over-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18099};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get("/api/openings/18446744073709551616/stats");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 400);
    CHECK(res->body == R"({"error":"invalid zobrist hash"})");
}

TEST_CASE("server: opening stats returns empty for unknown position", "[motif-http]")
{
    auto const tdir = tmp_dir {"ostats_empty"};
    auto db_res = motif::db::database_manager::create(tdir.path, "ostats-empty-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18096};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get("/api/openings/99999999999/stats");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 200);
    CHECK(res->body == R"({"continuations":[]})");
}

// NOLINTBEGIN(readability-function-cognitive-complexity)
TEST_CASE("server: opening stats returns continuations for populated DB", "[motif-http]")
{
    constexpr std::int32_t white_elo {1600};
    constexpr std::int32_t black_elo {1500};

    auto const tdir = tmp_dir {"ostats_populated"};
    auto db_res = motif::db::database_manager::create(tdir.path, "ostats-pop-db");
    REQUIRE(db_res.has_value());

    motif::db::game test_game {};
    test_game.white = {.name = "Alice", .elo = white_elo, .title = std::nullopt, .country = std::nullopt};
    test_game.black = {.name = "Bob", .elo = black_elo, .title = std::nullopt, .country = std::nullopt};
    test_game.result = "1-0";
    test_game.moves = encode_moves_for_game({"e4", "e5", "Nf3"});

    auto inserted = db_res->store().insert(test_game);
    REQUIRE(inserted.has_value());
    auto rebuilt = db_res->rebuild_position_store();
    REQUIRE(rebuilt.has_value());

    // The position store records positions AFTER each move (ply = move_index + 1),
    // so query from the position after e4, which IS in the DB.
    auto board = chesslib::board {};
    auto e4_move = chesslib::san::from_string(board, "e4");
    REQUIRE(e4_move.has_value());
    chesslib::move_maker {board, *e4_move}.make();
    auto const after_e4_hash = board.hash();
    auto e5_move = chesslib::san::from_string(board, "e5");
    REQUIRE(e5_move.has_value());
    chesslib::move_maker {board, *e5_move}.make();
    auto const expected_result_hash = board.hash();

    constexpr std::uint16_t test_port {18097};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const path = fmt::format("/api/openings/{}/stats", after_e4_hash);
    auto const start = std::chrono::steady_clock::now();
    auto const res = cli.Get(path);
    auto const stop = std::chrono::steady_clock::now();

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);
    CHECK(stop - start < opening_stats_endpoint_limit);
    auto const& body = res->body;
    CHECK(body.contains(R"("continuations")"));
    CHECK(body.contains(R"("san")"));
    CHECK(body.contains(R"("result_hash")"));
    CHECK(body.contains(R"("frequency")"));
    CHECK(body.contains(R"("white_wins")"));
    CHECK(body.contains(R"("draws")"));
    CHECK(body.contains(R"("black_wins")"));
    CHECK(body.contains(R"("average_white_elo")"));
    CHECK(body.contains(R"("average_black_elo")"));
    CHECK(body.contains(fmt::format(R"("result_hash":"{}")", expected_result_hash)));
    CHECK_FALSE(body.contains(fmt::format(R"("result_hash":{})", expected_result_hash)));
}

TEST_CASE("server: opening stats includes eco and opening_name when game has them", "[motif-http]")
{
    auto const tdir = tmp_dir {"ostats_eco"};
    auto db_res = motif::db::database_manager::create(tdir.path, "ostats-eco-db");
    REQUIRE(db_res.has_value());

    motif::db::game test_game {};
    test_game.white = {.name = "Alice", .elo = std::nullopt, .title = std::nullopt, .country = std::nullopt};
    test_game.black = {.name = "Bob", .elo = std::nullopt, .title = std::nullopt, .country = std::nullopt};
    test_game.result = "0-1";
    test_game.eco = "C00";
    test_game.extra_tags = {{"Opening", "French Defense"}};
    test_game.moves = encode_moves_for_game({"e4", "e6", "d4"});

    auto inserted = db_res->store().insert(test_game);
    REQUIRE(inserted.has_value());
    auto rebuilt = db_res->rebuild_position_store();
    REQUIRE(rebuilt.has_value());

    auto board = chesslib::board {};
    auto e4_move = chesslib::san::from_string(board, "e4");
    REQUIRE(e4_move.has_value());
    chesslib::move_maker {board, *e4_move}.make();
    auto const after_e4_hash = board.hash();

    constexpr std::uint16_t test_port {18098};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get(fmt::format("/api/openings/{}/stats", after_e4_hash));

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);
    auto const& body = res->body;
    CHECK(body.contains(R"("eco":"C00")"));
    CHECK(body.contains(R"("opening_name":"French Defense")"));
}

// NOLINTEND(readability-function-cognitive-complexity)

TEST_CASE("server: opening stats endpoint meets latency target", "[performance][motif-http]")
{
    run_http_opening_stats_perf_test();
}

TEST_CASE("server: game list empty DB returns 200 with empty array", "[motif-http]")
{
    auto const tdir = tmp_dir {"games_empty"};
    auto db_res = motif::db::database_manager::create(tdir.path, "games-empty-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18107};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get("/api/games");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 200);
    CHECK(res->body == "[]");
}

// NOLINTBEGIN(readability-function-cognitive-complexity)
TEST_CASE("server: game list populated DB returns required fields", "[motif-http]")
{
    auto const tdir = tmp_dir {"games_populated"};
    auto db_res = motif::db::database_manager::create(tdir.path, "games-populated-db");
    REQUIRE(db_res.has_value());
    auto const game_id = insert_http_game(*db_res,
                                          http_game_seed {
                                              .white = "Magnus Carlsen",
                                              .black = "Ian Nepomniachtchi",
                                              .result = "1-0",
                                              .event = "WCC 2021",
                                              .date = "2021.12.03",
                                              .eco = "C88",
                                              .move_seed = 1,
                                          });

    constexpr std::uint16_t test_port {18108};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get("/api/games");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 200);
    auto const& body = res->body;
    CHECK(body.front() == '[');
    CHECK(body.contains(fmt::format(R"("id":{})", game_id)));
    CHECK(body.contains(R"("white":"Magnus Carlsen")"));
    CHECK(body.contains(R"("black":"Ian Nepomniachtchi")"));
    CHECK(body.contains(R"("result":"1-0")"));
    CHECK(body.contains(R"("event":"WCC 2021")"));
    CHECK(body.contains(R"("date":"2021.12.03")"));
    CHECK(body.contains(R"("eco":"C88")"));
}

TEST_CASE("server: game list filters by player and result", "[motif-http]")
{
    auto const tdir = tmp_dir {"games_filters"};
    auto db_res = motif::db::database_manager::create(tdir.path, "games-filters-db");
    REQUIRE(db_res.has_value());
    insert_http_game(*db_res,
                     http_game_seed {
                         .white = "Magnus Carlsen",
                         .black = "Ian Nepomniachtchi",
                         .result = "1-0",
                         .event = "WCC 2021",
                         .date = "2021.12.03",
                         .eco = "C88",
                         .move_seed = 1,
                     });
    auto const expected_id = insert_http_game(*db_res,
                                              http_game_seed {
                                                  .white = "Levon Aronian",
                                                  .black = "Magnus Carlsen",
                                                  .result = "0-1",
                                                  .event = "Candidates",
                                                  .date = "2024.04.05",
                                                  .eco = "A04",
                                                  .move_seed = 2,
                                              });
    insert_http_game(*db_res,
                     http_game_seed {
                         .white = "Hikaru Nakamura",
                         .black = "Fabiano Caruana",
                         .result = "1-0",
                         .event = "Candidates",
                         .date = "2024.04.06",
                         .eco = "B12",
                         .move_seed = 3,
                     });

    constexpr std::uint16_t test_port {18109};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get("/api/games?player=Carlsen&result=0-1");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 200);
    CHECK(count_game_list_ids(res->body) == 1);
    CHECK(res->body.contains(fmt::format(R"("id":{})", expected_id)));
    CHECK(res->body.contains(R"("white":"Levon Aronian")"));
    CHECK(res->body.contains(R"("black":"Magnus Carlsen")"));
}

TEST_CASE("server: game list applies limit and offset", "[motif-http]")
{
    auto const tdir = tmp_dir {"games_page"};
    auto db_res = motif::db::database_manager::create(tdir.path, "games-page-db");
    REQUIRE(db_res.has_value());
    insert_http_game(*db_res,
                     {.white = "A", .black = "B", .result = "1-0", .event = "E1", .date = "2024.01.01", .eco = "A00", .move_seed = 1});
    auto const expected_id = insert_http_game(
        *db_res, {.white = "C", .black = "D", .result = "0-1", .event = "E2", .date = "2024.01.02", .eco = "B00", .move_seed = 2});
    insert_http_game(*db_res,
                     {.white = "E", .black = "F", .result = "1/2-1/2", .event = "E3", .date = "2024.01.03", .eco = "C00", .move_seed = 3});

    constexpr std::uint16_t test_port {18110};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get("/api/games?limit=1&offset=1");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 200);
    CHECK(count_game_list_ids(res->body) == 1);
    CHECK(res->body.contains(fmt::format(R"("id":{})", expected_id)));
}

TEST_CASE("server: game list clamps oversized limit", "[motif-http]")
{
    constexpr std::size_t inserted_games {201};

    auto const tdir = tmp_dir {"games_limit_clamp"};
    auto db_res = motif::db::database_manager::create(tdir.path, "games-limit-clamp-db");
    REQUIRE(db_res.has_value());
    for (std::size_t game_index = 0; game_index < inserted_games; ++game_index) {
        auto const suffix = fmt::format("{}", game_index);
        insert_http_game(*db_res,
                         http_game_seed {
                             .white = "White " + suffix,
                             .black = "Black " + suffix,
                             .result = "1-0",
                             .event = "Event " + suffix,
                             .date = "2024.01.01",
                             .eco = "A00",
                             .move_seed = static_cast<std::uint16_t>(game_index + 1U),
                         });
    }

    constexpr std::uint16_t test_port {18111};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get("/api/games?limit=999");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 200);
    CHECK(count_game_list_ids(res->body) == 200);
}

TEST_CASE("server: game list invalid pagination returns 400", "[motif-http]")
{
    auto const tdir = tmp_dir {"games_bad_page"};
    auto db_res = motif::db::database_manager::create(tdir.path, "games-bad-page-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18112};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const bad_limit = cli.Get("/api/games?limit=abc");
    auto const bad_offset = cli.Get("/api/games?offset=-1");
    auto const empty_limit = cli.Get("/api/games?limit=");

    srv.stop();
    server_thread.join();

    REQUIRE(bad_limit != nullptr);
    REQUIRE(bad_offset != nullptr);
    REQUIRE(empty_limit != nullptr);
    CHECK(bad_limit->status == 400);
    CHECK(bad_offset->status == 400);
    CHECK(empty_limit->status == 400);
    CHECK(bad_limit->body == R"({"error":"invalid pagination parameters"})");
    CHECK(bad_offset->body == R"({"error":"invalid pagination parameters"})");
    CHECK(empty_limit->body == R"({"error":"invalid pagination parameters"})");
}

TEST_CASE("server: game list limit=0 returns empty array", "[motif-http]")
{
    auto const tdir = tmp_dir {"games_limit_zero"};
    auto db_res = motif::db::database_manager::create(tdir.path, "games-limit-zero-db");
    REQUIRE(db_res.has_value());
    insert_http_game(*db_res,
                     {.white = "A", .black = "B", .result = "1-0", .event = "E", .date = "2024.01.01", .eco = "A00", .move_seed = 1});

    constexpr std::uint16_t test_port {18113};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get("/api/games?limit=0");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 200);
    CHECK(res->body == "[]");
}

TEST_CASE("server: single game returns complete payload", "[motif-http]")
{
    auto const tdir = tmp_dir {"game_get"};
    auto db_res = motif::db::database_manager::create(tdir.path, "game-get-db");
    REQUIRE(db_res.has_value());
    auto const [game_id, moves] = insert_detailed_http_game(*db_res);

    constexpr std::uint16_t test_port {18114};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get(fmt::format("/api/games/{}", game_id));

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);
    auto const& body = res->body;
    CHECK(body.contains(fmt::format(R"("id":{})", game_id)));
    CHECK(body.contains(R"("white")"));
    CHECK(body.contains(R"("black")"));
    CHECK(body.contains(R"("event")"));
    CHECK(body.contains(R"("tags")"));
    CHECK(body.contains(R"("moves")"));
    CHECK(body.contains(R"("result")"));
    CHECK(body.contains(R"("eco")"));
    CHECK(body.contains(R"("date")"));
    CHECK(body.contains("Magnus Carlsen"));
    CHECK(body.contains("Ian Nepomniachtchi"));
    CHECK(body.contains("2865"));
    CHECK(body.contains("2792"));
    CHECK(body.contains("GM"));
    CHECK(body.contains("NO"));
    CHECK(body.contains("RU"));
    CHECK(body.contains("WCC 2021"));
    CHECK(body.contains("Dubai"));
    CHECK(body.contains("2021.12.03"));
    CHECK(body.contains("1-0"));
    CHECK(body.contains("C88"));
    CHECK(body.contains("Opening"));
    CHECK(body.contains("Ruy Lopez"));
    CHECK(body.contains("Round"));
    CHECK(body.contains(fmt::format("{}", moves.at(0))));
    CHECK(body.contains(fmt::format("{}", moves.at(1))));
}

TEST_CASE("server: single game maps missing and invalid IDs", "[motif-http]")
{
    auto const tdir = tmp_dir {"game_get_errors"};
    auto db_res = motif::db::database_manager::create(tdir.path, "game-get-errors-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18115};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const missing = cli.Get("/api/games/99999");
    auto const alpha = cli.Get("/api/games/abc");
    auto const negative = cli.Get("/api/games/-1");
    auto const zero = cli.Get("/api/games/0");
    auto const overflow = cli.Get("/api/games/4294967296");
    auto const bare = cli.Get("/api/games/");

    srv.stop();
    server_thread.join();

    REQUIRE(missing != nullptr);
    REQUIRE(alpha != nullptr);
    REQUIRE(negative != nullptr);
    REQUIRE(zero != nullptr);
    REQUIRE(overflow != nullptr);
    REQUIRE(bare != nullptr);
    CHECK(missing->status == 404);
    CHECK(missing->body == R"({"error":"not_found"})");
    CHECK(alpha->status == 400);
    CHECK(negative->status == 400);
    CHECK(zero->status == 400);
    CHECK(overflow->status == 400);
    CHECK(bare->status == 400);
    CHECK(alpha->body == R"({"error":"invalid game id"})");
    CHECK(negative->body == R"({"error":"invalid game id"})");
    CHECK(zero->body == R"({"error":"invalid game id"})");
    CHECK(overflow->body == R"({"error":"invalid game id"})");
    CHECK(bare->body == R"({"error":"invalid game id"})");
}

// NOLINTEND(readability-function-cognitive-complexity)

TEST_CASE("server: import rejects nonexistent file", "[motif-http]")
{
    auto const tdir = tmp_dir {"import_badfile"};
    auto db_res = motif::db::database_manager::create(tdir.path / "db", "import-bad");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18101};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Post("/api/imports", R"({"path":"/nonexistent/path/file.pgn"})", "application/json");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 400);
}

TEST_CASE("server: import rejects unreadable file", "[motif-http]")
{
    auto const tdir = tmp_dir {"import_unreadable"};
    auto db_res = motif::db::database_manager::create(tdir.path / "db", "import-unreadable");
    REQUIRE(db_res.has_value());

    auto const pgn_path = write_pgn_fixture(tdir.path, three_game_pgn_content);
    std::filesystem::permissions(pgn_path, std::filesystem::perms::none);
    if (std::ifstream {pgn_path, std::ios::binary}.is_open()) {
        std::filesystem::permissions(pgn_path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
        SKIP("filesystem permissions do not make fixture unreadable for this user");
    }

    constexpr std::uint16_t test_port {18105};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Post("/api/imports", R"({"path":")" + pgn_path.string() + R"("})", "application/json");

    srv.stop();
    server_thread.join();
    std::filesystem::permissions(pgn_path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);

    REQUIRE(res != nullptr);
    CHECK(res->status == 400);
}

TEST_CASE("server: import accepts valid file and returns 202 with import_id", "[motif-http]")
{
    auto const tdir = tmp_dir {"import_valid"};
    auto db_res = motif::db::database_manager::create(tdir.path / "db", "import-valid");
    REQUIRE(db_res.has_value());

    auto const pgn_path = write_pgn_fixture(tdir.path, three_game_pgn_content);
    auto logging = import_logging_scope {tdir.path / "logs"};

    constexpr std::uint16_t test_port {18102};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Post("/api/imports", R"({"path":")" + pgn_path.string() + R"("})", "application/json");

    std::this_thread::sleep_for(import_short_settle_ms);
    srv.stop();
    server_thread.join();
    logging.shutdown();

    REQUIRE(res != nullptr);
    CHECK(res->status == 202);
    CHECK(res->body.contains(R"("import_id")"));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("server: import rejects a second active import", "[motif-http]")
{
    auto const tdir = tmp_dir {"import_single_active"};
    auto db_res = motif::db::database_manager::create(tdir.path / "db", "import-single");
    REQUIRE(db_res.has_value());

    auto const pgn_path = write_pgn_fixture(tdir.path, make_repeated_pgn(long_import_game_count));
    auto logging = import_logging_scope {tdir.path / "logs"};

    constexpr std::uint16_t test_port {18106};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    cli.set_read_timeout(sse_read_timeout_s);

    auto const first_res = cli.Post("/api/imports", R"({"path":")" + pgn_path.string() + R"("})", "application/json");
    REQUIRE(first_res != nullptr);
    REQUIRE(first_res->status == 202);
    auto const import_id = extract_import_id(first_res->body);

    auto const second_res = cli.Post("/api/imports", R"({"path":")" + pgn_path.string() + R"("})", "application/json");
    REQUIRE(second_res != nullptr);
    CHECK(second_res->status == 409);
    CHECK(second_res->body.contains("import already running"));

    auto const del_res = cli.Delete("/api/imports/" + import_id);
    REQUIRE(del_res != nullptr);
    CHECK(del_res->status == 200);

    std::string collected_events;
    cli.Get("/api/imports/" + import_id + "/progress",
            httplib::Headers {},
            [&collected_events](const char* data, size_t size) -> bool
            {
                collected_events.append(data, size);
                return true;
            });

    srv.stop();
    server_thread.join();
    logging.shutdown();
}

TEST_CASE("server: import SSE streams progress and completion events", "[motif-http]")
{
    run_import_sse_test();
}

TEST_CASE("server: import DELETE requests cancellation", "[motif-http]")
{
    run_import_delete_test();
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("server: second POST while import active returns 409 immediately", "[motif-http]")
{
    auto const tdir = tmp_dir {"import_conflict2"};
    auto db_res = motif::db::database_manager::create(tdir.path / "db", "import-conflict2");
    REQUIRE(db_res.has_value());

    auto const pgn_path = write_pgn_fixture(tdir.path, make_repeated_pgn(long_import_game_count));
    auto logging = import_logging_scope {tdir.path / "logs"};

    constexpr std::uint16_t test_port {18116};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    cli.set_read_timeout(sse_read_timeout_s);

    auto const first_res = cli.Post("/api/imports", R"({"path":")" + pgn_path.string() + R"("})", "application/json");
    REQUIRE(first_res != nullptr);
    REQUIRE(first_res->status == 202);

    // Second POST before first completes — conflict check must fire before any
    // heap allocation for session or pipeline (AC 6).
    auto const second_res = cli.Post("/api/imports", R"({"path":")" + pgn_path.string() + R"("})", "application/json");
    REQUIRE(second_res != nullptr);
    CHECK(second_res->status == 409);
    CHECK(second_res->body.contains("import already running"));

    auto const import_id = extract_import_id(first_res->body);
    auto const del_res = cli.Delete("/api/imports/" + import_id);
    REQUIRE(del_res != nullptr);
    CHECK(del_res->status == 200);

    std::string collected_events;
    cli.Get("/api/imports/" + import_id + "/progress",
            httplib::Headers {},
            [&collected_events](const char* data, size_t size) -> bool
            {
                collected_events.append(data, size);
                return true;
            });

    srv.stop();
    server_thread.join();
    logging.shutdown();
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("server: import worker start failure returns 500 and does not block later imports", "[motif-http]")
{
    auto const tdir = tmp_dir {"import_worker_start_failure"};
    auto db_res = motif::db::database_manager::create(tdir.path / "db", "import-worker-start-failure");
    REQUIRE(db_res.has_value());

    auto const pgn_path = write_pgn_fixture(tdir.path, three_game_pgn_content);
    auto logging = import_logging_scope {tdir.path / "logs"};

    constexpr std::uint16_t test_port {18119};
    auto fail_start = env_flag_scope {fail_next_import_worker_start_env};
    motif::http::server srv {*db_res};
    fail_start.clear();
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    cli.set_read_timeout(sse_read_timeout_s);

    auto const failed_res = cli.Post("/api/imports", R"({"path":")" + pgn_path.string() + R"("})", "application/json");
    REQUIRE(failed_res != nullptr);
    CHECK(failed_res->status == 500);
    CHECK(failed_res->body.contains("failed to start import worker"));

    auto const retry_res = cli.Post("/api/imports", R"({"path":")" + pgn_path.string() + R"("})", "application/json");
    REQUIRE(retry_res != nullptr);
    REQUIRE(retry_res->status == 202);

    auto const import_id = extract_import_id(retry_res->body);
    cli.Get(
        "/api/imports/" + import_id + "/progress", httplib::Headers {}, [](const char* /*data*/, size_t /*size*/) -> bool { return true; });

    srv.stop();
    server_thread.join();
    logging.shutdown();
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("server: SSE error event data field is valid JSON even with special characters", "[motif-http]")
{
    // The error message format embeds the error code inside literal quotes
    // (e.g. import failed: "io_failure"), which would break the old raw
    // fmt::format approach.  After the glaze fix the data field must round-trip
    // through JSON.parse without throwing.
    auto const tdir = tmp_dir {"import_sse_error_escape"};
    auto db_res = motif::db::database_manager::create(tdir.path / "db", "import-sse-escape");
    REQUIRE(db_res.has_value());

    auto const pgn_path = write_pgn_fixture(tdir.path, three_game_pgn_content);
    auto logging = import_logging_scope {tdir.path / "logs"};

    constexpr std::uint16_t test_port {18117};
    auto fail_run = env_flag_scope {fail_next_import_worker_run_env};
    motif::http::server srv {*db_res};
    fail_run.clear();
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    cli.set_read_timeout(sse_read_timeout_s);

    auto const post_res = cli.Post("/api/imports", R"({"path":")" + pgn_path.string() + R"("})", "application/json");
    REQUIRE(post_res != nullptr);
    REQUIRE(post_res->status == 202);
    auto const import_id = extract_import_id(post_res->body);

    std::string collected_events;
    cli.Get("/api/imports/" + import_id + "/progress",
            httplib::Headers {},
            [&collected_events](const char* data, size_t size) -> bool
            {
                collected_events.append(data, size);
                return true;
            });

    srv.stop();
    server_thread.join();
    logging.shutdown();

    CHECK(collected_events.contains("event: error"));
    CHECK(collected_events.contains(R"("error":"import failed: \"invalid_state\"")"));

    // Every "data:" line in the SSE stream must carry parseable JSON.
    auto pos = std::size_t {0};
    auto data_line_count = std::size_t {0};
    while (pos < collected_events.size()) {
        auto const newline_pos = collected_events.find('\n', pos);
        auto const line = collected_events.substr(pos, newline_pos == std::string::npos ? std::string::npos : newline_pos - pos);
        pos = (newline_pos == std::string::npos) ? collected_events.size() : newline_pos + 1;

        if (!line.starts_with("data: ")) {
            continue;
        }
        auto const json_str = line.substr(6);  // strip "data: "
        ++data_line_count;

        motif_http_test::sse_event_payload payload;
        auto const parse_result = glz::read_json(payload, json_str);
        CHECK(!parse_result);  // must parse without error
    }
    CHECK(data_line_count > 0);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("server: completed sessions remain queryable up to bounded count", "[motif-http]")
{
    auto const tdir = tmp_dir {"import_session_pruning"};
    auto db_res = motif::db::database_manager::create(tdir.path / "db", "import-pruning");
    REQUIRE(db_res.has_value());

    auto const pgn_path = write_pgn_fixture(tdir.path, three_game_pgn_content);
    auto logging = import_logging_scope {tdir.path / "logs"};

    constexpr std::uint16_t test_port {18118};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    cli.set_read_timeout(sse_read_timeout_s);

    auto const run_one_import = [&]() -> std::string
    {
        auto const post_res = cli.Post("/api/imports", R"({"path":")" + pgn_path.string() + R"("})", "application/json");
        REQUIRE(post_res != nullptr);
        REQUIRE(post_res->status == 202);
        auto const iid = extract_import_id(post_res->body);
        // Drain SSE to completion so the worker finishes and the session is done.
        cli.Get(
            "/api/imports/" + iid + "/progress", httplib::Headers {}, [](const char* /*data*/, size_t /*size*/) -> bool { return true; });
        return iid;
    };

    auto import_ids = std::vector<std::string> {};
    constexpr std::size_t imports_to_trigger_eviction {66};
    import_ids.reserve(imports_to_trigger_eviction);
    for (std::size_t index = 0; index < imports_to_trigger_eviction; ++index) {
        import_ids.push_back(run_one_import());
    }

    auto const oldest_res = cli.Get("/api/imports/" + import_ids.front() + "/progress");
    REQUIRE(oldest_res != nullptr);
    CHECK(oldest_res->status == 404);

    auto recent_events = std::string {};
    auto const recent_ok = cli.Get("/api/imports/" + import_ids.back() + "/progress",
                                   httplib::Headers {},
                                   [&recent_events](const char* data, size_t size) -> bool
                                   {
                                       recent_events.append(data, size);
                                       return true;
                                   });
    CHECK(recent_ok);
    CHECK(recent_events.contains("event: complete"));

    srv.stop();
    server_thread.join();
    logging.shutdown();
}
