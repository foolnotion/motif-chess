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
#include <glaze/json/write.hpp>
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

struct start_analysis_response
{
    std::string analysis_id;
};

struct provenance_response
{
    std::string source_type;
    std::optional<std::string> source_label;
    std::string review_status;
};

struct create_game_response
{
    std::uint32_t id {};
    std::string source_type;
    std::optional<std::string> source_label;
    std::string review_status;
};

struct game_response
{
    std::uint32_t id {};
    std::string result;
    provenance_response provenance;
};

struct game_list_entry_response
{
    std::uint32_t id {};
    std::string source_type;
    std::string review_status;
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
        .provenance = {},
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
        .provenance = {},
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
    CHECK(res->get_header_value("Access-Control-Allow-Methods") == "GET, POST, PATCH, DELETE, OPTIONS");
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

// Helpers for legal-moves and apply-move response structs used only in test scope.
namespace motif_http_test
{

struct legal_move_entry
{
    std::string uci;
    std::string san;
    std::string from;
    std::string to;
    std::optional<std::string> promotion;
};

struct legal_moves_response
{
    std::string fen;
    std::vector<legal_move_entry> legal_moves;
};

struct apply_move_response
{
    std::string uci;
    std::string san;
    std::string fen;
};

}  // namespace motif_http_test

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

// ─── Legal moves endpoint tests ──────────────────────────────────────────────

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("server: legal-moves initial position returns 200 with 20 moves", "[motif-http]")
{
    auto const tdir = tmp_dir {"legal_moves_initial"};
    auto db_res = motif::db::database_manager::create(tdir.path, "legal-moves-initial");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18120};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    constexpr std::string_view initial_fen {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"};
    auto const encoded_fen = httplib::encode_query_component(std::string {initial_fen});
    auto const res = cli.Get("/api/positions/legal-moves?fen=" + encoded_fen);

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);
    auto const& body = res->body;
    CHECK(body.contains(R"("fen")"));
    CHECK(body.contains(R"("legal_moves")"));
    CHECK(body.contains(R"("uci":"e2e4")"));
    CHECK(body.contains(R"("san":"e4")"));
    CHECK(body.contains(R"("from":"e2")"));
    CHECK(body.contains(R"("to":"e4")"));

    motif_http_test::legal_moves_response parsed;
    auto const parse_err = glz::read_json(parsed, body);
    REQUIRE(!parse_err);
    CHECK(parsed.legal_moves.size() == 20);
    auto const has_e2e4 = std::ranges::any_of(
        parsed.legal_moves, [](auto const& move_entry) -> bool { return move_entry.uci == "e2e4" && move_entry.san == "e4"; });
    CHECK(has_e2e4);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("server: legal-moves castling position includes castling moves", "[motif-http]")
{
    auto const tdir = tmp_dir {"legal_moves_castle"};
    auto db_res = motif::db::database_manager::create(tdir.path, "legal-moves-castle");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18121};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    constexpr std::string_view castle_fen {"r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1"};
    auto const encoded_fen = httplib::encode_query_component(std::string {castle_fen});
    auto const res = cli.Get("/api/positions/legal-moves?fen=" + encoded_fen);

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);

    motif_http_test::legal_moves_response parsed;
    auto const parse_err = glz::read_json(parsed, res->body);
    REQUIRE(!parse_err);

    auto const has_kingside_castle = std::ranges::any_of(
        parsed.legal_moves, [](auto const& move_entry) -> bool { return move_entry.uci == "e1g1" && move_entry.san == "O-O"; });
    auto const has_queenside_castle = std::ranges::any_of(
        parsed.legal_moves, [](auto const& move_entry) -> bool { return move_entry.uci == "e1c1" && move_entry.san == "O-O-O"; });
    CHECK(has_kingside_castle);
    CHECK(has_queenside_castle);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("server: legal-moves promotion position includes all promotion suffixes", "[motif-http]")
{
    auto const tdir = tmp_dir {"legal_moves_promo"};
    auto db_res = motif::db::database_manager::create(tdir.path, "legal-moves-promo");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18122};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    constexpr std::string_view promo_fen {"8/P7/8/8/8/8/8/4k2K w - - 0 1"};
    auto const encoded_fen = httplib::encode_query_component(std::string {promo_fen});
    auto const res = cli.Get("/api/positions/legal-moves?fen=" + encoded_fen);

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);

    motif_http_test::legal_moves_response parsed;
    auto const parse_err = glz::read_json(parsed, res->body);
    REQUIRE(!parse_err);

    auto has_suffix = [&](std::string const& suffix) -> bool
    {
        return std::ranges::any_of(parsed.legal_moves,
                                   [&](auto const& move_entry) -> bool
                                   { return move_entry.promotion.has_value() && *move_entry.promotion == suffix; });
    };
    CHECK(has_suffix("q"));
    CHECK(has_suffix("r"));
    CHECK(has_suffix("b"));
    CHECK(has_suffix("n"));
    auto const non_promo_has_field = std::ranges::any_of(
        parsed.legal_moves, [](auto const& move_entry) -> bool { return !move_entry.promotion.has_value() && move_entry.uci.size() > 4; });
    CHECK_FALSE(non_promo_has_field);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("server: legal-moves check-constrained position excludes non-evasion moves", "[motif-http]")
{
    auto const tdir = tmp_dir {"legal_moves_check"};
    auto db_res = motif::db::database_manager::create(tdir.path, "legal-moves-check");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18123};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    // White king on e1 in check from black queen on e8 — white pawn on a2 cannot move
    constexpr std::string_view check_fen {"4q3/8/8/8/8/8/P7/4K2k w - - 0 1"};
    auto const encoded_fen = httplib::encode_query_component(std::string {check_fen});
    auto const res = cli.Get("/api/positions/legal-moves?fen=" + encoded_fen);

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);

    motif_http_test::legal_moves_response parsed;
    auto const parse_err = glz::read_json(parsed, res->body);
    REQUIRE(!parse_err);

    // Pawn on a2 cannot move (it doesn't block check)
    auto const a2_pawn_moves =
        std::ranges::any_of(parsed.legal_moves, [](auto const& move_entry) -> bool { return move_entry.from == "a2"; });
    CHECK_FALSE(a2_pawn_moves);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("server: legal-moves missing fen returns 400", "[motif-http]")
{
    auto const tdir = tmp_dir {"legal_moves_no_fen"};
    auto db_res = motif::db::database_manager::create(tdir.path, "legal-moves-no-fen");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18124};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const no_param = cli.Get("/api/positions/legal-moves");
    auto const empty_param = cli.Get("/api/positions/legal-moves?fen=");
    auto const malformed = cli.Get("/api/positions/legal-moves?fen=not_a_fen");
    auto const few_fields = cli.Get("/api/positions/legal-moves?fen=rnbqkbnr%2F8");

    srv.stop();
    server_thread.join();

    REQUIRE(no_param != nullptr);
    REQUIRE(empty_param != nullptr);
    REQUIRE(malformed != nullptr);
    REQUIRE(few_fields != nullptr);
    CHECK(no_param->status == 400);
    CHECK(empty_param->status == 400);
    CHECK(malformed->status == 400);
    CHECK(few_fields->status == 400);
    CHECK(no_param->body.contains(R"("error")"));
    CHECK(empty_param->body.contains(R"("error")"));
    CHECK(malformed->body.contains(R"("error")"));
    CHECK(few_fields->body.contains(R"("error")"));
}

// ─── Apply-move endpoint tests ────────────────────────────────────────────────

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("server: apply-move legal move returns 200 with san and resulting fen", "[motif-http]")
{
    auto const tdir = tmp_dir {"apply_move_legal"};
    auto db_res = motif::db::database_manager::create(tdir.path, "apply-move-legal");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18125};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    constexpr std::string_view body_json {R"({"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1","uci":"e2e4"})"};
    auto const res = cli.Post("/api/positions/apply-move", std::string {body_json}, "application/json");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);

    motif_http_test::apply_move_response parsed;
    auto const parse_err = glz::read_json(parsed, res->body);
    REQUIRE(!parse_err);
    CHECK(parsed.uci == "e2e4");
    CHECK(parsed.san == "e4");
    CHECK(parsed.fen == "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1");
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("server: apply-move illegal move returns 400", "[motif-http]")
{
    auto const tdir = tmp_dir {"apply_move_illegal"};
    auto db_res = motif::db::database_manager::create(tdir.path, "apply-move-illegal");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18126};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const illegal_move = cli.Post("/api/positions/apply-move",
                                       R"({"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1","uci":"e2e5"})",
                                       "application/json");
    auto const bad_uci = cli.Post("/api/positions/apply-move",
                                  R"({"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1","uci":"notauci"})",
                                  "application/json");
    auto const bad_promotion_suffix = cli.Post("/api/positions/apply-move",
                                               R"({"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1","uci":"e2e4x"})",
                                               "application/json");

    srv.stop();
    server_thread.join();

    REQUIRE(illegal_move != nullptr);
    REQUIRE(bad_uci != nullptr);
    REQUIRE(bad_promotion_suffix != nullptr);
    CHECK(illegal_move->status == 400);
    CHECK(bad_uci->status == 400);
    CHECK(bad_promotion_suffix->status == 400);
    CHECK(illegal_move->body.contains(R"("error")"));
    CHECK(bad_uci->body.contains(R"("error")"));
    CHECK(bad_promotion_suffix->body.contains(R"("error")"));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("server: apply-move promotion returns 200 with promotion san and correct fen", "[motif-http]")
{
    auto const tdir = tmp_dir {"apply_move_promo"};
    auto db_res = motif::db::database_manager::create(tdir.path, "apply-move-promo");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18127};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Post("/api/positions/apply-move", R"({"fen":"8/P7/8/8/8/8/8/4k2K w - - 0 1","uci":"a7a8q"})", "application/json");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);

    motif_http_test::apply_move_response parsed;
    auto const parse_err = glz::read_json(parsed, res->body);
    REQUIRE(!parse_err);
    CHECK(parsed.uci == "a7a8q");
    CHECK(parsed.san.contains("=Q"));
    CHECK(parsed.fen == "Q7/8/8/8/8/8/8/4k2K b - - 0 1");
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("server: apply-move bad request body returns 400", "[motif-http]")
{
    auto const tdir = tmp_dir {"apply_move_bad_body"};
    auto db_res = motif::db::database_manager::create(tdir.path, "apply-move-bad-body");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18128};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const missing_uci =
        cli.Post("/api/positions/apply-move", R"({"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"})", "application/json");
    auto const bad_fen = cli.Post("/api/positions/apply-move", R"({"fen":"notafen","uci":"e2e4"})", "application/json");
    auto const not_json = cli.Post("/api/positions/apply-move", "not json at all", "application/json");

    srv.stop();
    server_thread.join();

    REQUIRE(missing_uci != nullptr);
    REQUIRE(bad_fen != nullptr);
    REQUIRE(not_json != nullptr);
    CHECK(missing_uci->status == 400);
    CHECK(bad_fen->status == 400);
    CHECK(not_json->status == 400);
}

// ---------------------------------------------------------------------------
// Engine analysis route tests (AC 1, 3, 7 — Story 4d.2)
// ---------------------------------------------------------------------------

TEST_CASE("server: POST /api/engine/analyses valid body returns 202 with analysis_id", "[motif-http]")
{
    auto const tdir = tmp_dir {"engine_analyses_valid"};
    auto db_res = motif::db::database_manager::create(tdir.path, "engine-analyses-valid");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18130};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Post(
        "/api/engine/analyses", R"({"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1","depth":20})", "application/json");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    REQUIRE(res->status == 202);

    motif_http_test::start_analysis_response parsed;
    auto const parse_err = glz::read_json(parsed, res->body);
    REQUIRE(!parse_err);
    CHECK(!parsed.analysis_id.empty());
}

TEST_CASE("server: POST /api/engine/analyses missing fen returns 400", "[motif-http]")
{
    auto const tdir = tmp_dir {"engine_analyses_no_fen"};
    auto db_res = motif::db::database_manager::create(tdir.path, "engine-analyses-no-fen");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18131};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Post("/api/engine/analyses", R"({"depth":20})", "application/json");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 400);
}

TEST_CASE("server: POST /api/engine/analyses invalid fen returns 400", "[motif-http]")
{
    auto const tdir = tmp_dir {"engine_analyses_invalid_fen"};
    auto db_res = motif::db::database_manager::create(tdir.path, "engine-analyses-invalid-fen");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18138};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Post("/api/engine/analyses", R"({"fen":"not_a_fen","depth":20})", "application/json");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 400);
}

TEST_CASE("server: POST /api/engine/analyses with both depth and movetime_ms returns 400", "[motif-http]")
{
    auto const tdir = tmp_dir {"engine_analyses_both_limits"};
    auto db_res = motif::db::database_manager::create(tdir.path, "engine-analyses-both-limits");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18132};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Post("/api/engine/analyses",
                              R"({"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1","depth":20,"movetime_ms":5000})",
                              "application/json");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 400);
}

TEST_CASE("server: POST /api/engine/analyses with neither depth nor movetime_ms returns 400", "[motif-http]")
{
    auto const tdir = tmp_dir {"engine_analyses_no_limit"};
    auto db_res = motif::db::database_manager::create(tdir.path, "engine-analyses-no-limit");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18133};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res =
        cli.Post("/api/engine/analyses", R"({"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"})", "application/json");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 400);
}

TEST_CASE("server: POST /api/engine/analyses with multipv=0 returns 400", "[motif-http]")
{
    auto const tdir = tmp_dir {"engine_analyses_multipv_low"};
    auto db_res = motif::db::database_manager::create(tdir.path, "engine-analyses-multipv-low");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18134};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Post("/api/engine/analyses",
                              R"({"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1","depth":10,"multipv":0})",
                              "application/json");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 400);
}

TEST_CASE("server: POST /api/engine/analyses with multipv=6 returns 400", "[motif-http]")
{
    auto const tdir = tmp_dir {"engine_analyses_multipv_high"};
    auto db_res = motif::db::database_manager::create(tdir.path, "engine-analyses-multipv-high");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18135};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Post("/api/engine/analyses",
                              R"({"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1","depth":10,"multipv":6})",
                              "application/json");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 400);
}

TEST_CASE("server: GET /api/engine/analyses/unknown-id/stream returns 404 or 501", "[motif-http]")
{
    auto const tdir = tmp_dir {"engine_analyses_stream_unknown"};
    auto db_res = motif::db::database_manager::create(tdir.path, "engine-analyses-stream-unknown");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18136};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get("/api/engine/analyses/unknown-id/stream");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK((res->status == 404 || res->status == 501));
}

TEST_CASE("server: DELETE /api/engine/analyses/unknown-id returns 404 or 501", "[motif-http]")
{
    auto const tdir = tmp_dir {"engine_analyses_delete_unknown"};
    auto db_res = motif::db::database_manager::create(tdir.path, "engine-analyses-delete-unknown");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18137};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Delete("/api/engine/analyses/unknown-id");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK((res->status == 404 || res->status == 501));
}

// ── CRUD game tests (Story 4d.3) ─────────────────────────────────────────────
// NOLINTBEGIN(readability-function-cognitive-complexity) -- Catch2 macros inflate complexity

namespace
{

constexpr std::string_view single_game_pgn = R"pgn(
[Event "Test Event"]
[Site "Test Site"]
[Date "2024.06.01"]
[Round "1"]
[White "Alice"]
[Black "Bob"]
[Result "1-0"]
[WhiteElo "1800"]
[BlackElo "1750"]

1. e4 e5 2. Nf3 Nc6 3. Bb5 a6 4. Ba4 Nf6 1-0
)pgn";

constexpr std::string_view another_game_pgn = R"pgn(
[Event "Another Event"]
[Site "?"]
[Date "2024.07.01"]
[Round "1"]
[White "Charlie"]
[Black "Diana"]
[Result "0-1"]

1. d4 d5 2. c4 c6 3. Nc3 Nf6 0-1
)pgn";

constexpr std::string_view malformed_pgn = R"pgn(
[Event "Bad Game"]
this is not valid PGN text at all
)pgn";

constexpr std::string_view invalid_san_pgn = R"pgn(
[Event "Bad Moves"]
[White "X"]
[Black "Y"]
[Result "1-0"]

1. e4 e5 2. ZZ99 1-0
)pgn";

}  // namespace

TEST_CASE("server: POST /api/games creates manual game returns 201", "[motif-http]")
{
    auto const tdir = tmp_dir {"crud_create"};
    auto db_res = motif::db::database_manager::create(tdir.path, "crud-create-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18140};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const body = fmt::format(R"({{"pgn":{},"source_label":"test-client","review_status":"new"}})",
                                  glz::write_json(std::string {single_game_pgn}).value_or("\"\""));
    auto const res = cli.Post("/api/games", body, "application/json");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 201);
    CHECK(res->body.contains(R"("source_type":"manual")"));
    CHECK(res->body.contains(R"("source_label":"test-client")"));
    CHECK(res->body.contains(R"("review_status":"new")"));

    motif_http_test::create_game_response parsed;
    auto const err = glz::read_json(parsed, res->body);
    CHECK(!err);
    CHECK(parsed.source_type == "manual");
    CHECK(parsed.id > 0);
}

TEST_CASE("server: POST /api/games created game readable via GET", "[motif-http]")
{
    auto const tdir = tmp_dir {"crud_create_get"};
    auto db_res = motif::db::database_manager::create(tdir.path, "crud-create-get-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18141};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const post_body = fmt::format(R"({{"pgn":{}}})", glz::write_json(std::string {single_game_pgn}).value_or("\"\""));
    auto const post_res = cli.Post("/api/games", post_body, "application/json");
    REQUIRE(post_res != nullptr);
    REQUIRE(post_res->status == 201);

    motif_http_test::create_game_response created;
    REQUIRE(!glz::read_json(created, post_res->body));
    REQUIRE(created.id > 0);

    auto const get_res = cli.Get(fmt::format("/api/games/{}", created.id));

    srv.stop();
    server_thread.join();

    REQUIRE(get_res != nullptr);
    CHECK(get_res->status == 200);
    CHECK(get_res->body.contains(R"("source_type":"manual")"));
    CHECK(get_res->body.contains(R"("review_status":"new")"));
    CHECK(get_res->body.contains(R"("provenance")"));
}

TEST_CASE("server: POST /api/games created game visible in game list", "[motif-http]")
{
    auto const tdir = tmp_dir {"crud_create_list"};
    auto db_res = motif::db::database_manager::create(tdir.path, "crud-create-list-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18142};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const post_body = fmt::format(R"({{"pgn":{}}})", glz::write_json(std::string {single_game_pgn}).value_or("\"\""));
    auto const post_res = cli.Post("/api/games", post_body, "application/json");
    REQUIRE(post_res != nullptr);
    REQUIRE(post_res->status == 201);

    auto const list_res = cli.Get("/api/games");

    srv.stop();
    server_thread.join();

    REQUIRE(list_res != nullptr);
    CHECK(list_res->status == 200);
    CHECK(list_res->body.contains(R"("source_type":"manual")"));
    CHECK(count_game_list_ids(list_res->body) == 1);
}

TEST_CASE("server: PATCH /api/games/{id} updates metadata returns 200", "[motif-http]")
{
    auto const tdir = tmp_dir {"crud_patch"};
    auto db_res = motif::db::database_manager::create(tdir.path, "crud-patch-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18143};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const post_body = fmt::format(R"({{"pgn":{}}})", glz::write_json(std::string {single_game_pgn}).value_or("\"\""));
    auto const post_res = cli.Post("/api/games", post_body, "application/json");
    REQUIRE(post_res != nullptr);
    REQUIRE(post_res->status == 201);

    motif_http_test::create_game_response created;
    REQUIRE(!glz::read_json(created, post_res->body));

    auto const* const patch_body = R"({"review_status":"studied","source_label":"my-label"})";
    auto const patch_res = cli.Patch(fmt::format("/api/games/{}", created.id), patch_body, "application/json");

    srv.stop();
    server_thread.join();

    REQUIRE(patch_res != nullptr);
    CHECK(patch_res->status == 200);
    CHECK(patch_res->body.contains(R"("review_status":"studied")"));
    CHECK(patch_res->body.contains(R"("source_label":"my-label")"));
}

TEST_CASE("server: DELETE /api/games/{id} removes manual game returns 204", "[motif-http]")
{
    auto const tdir = tmp_dir {"crud_delete"};
    auto db_res = motif::db::database_manager::create(tdir.path, "crud-delete-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18144};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const post_body = fmt::format(R"({{"pgn":{}}})", glz::write_json(std::string {single_game_pgn}).value_or("\"\""));
    auto const post_res = cli.Post("/api/games", post_body, "application/json");
    REQUIRE(post_res != nullptr);
    REQUIRE(post_res->status == 201);

    motif_http_test::create_game_response created;
    REQUIRE(!glz::read_json(created, post_res->body));

    auto const del_res = cli.Delete(fmt::format("/api/games/{}", created.id));
    auto const get_res = cli.Get(fmt::format("/api/games/{}", created.id));

    srv.stop();
    server_thread.join();

    REQUIRE(del_res != nullptr);
    CHECK(del_res->status == 204);
    REQUIRE(get_res != nullptr);
    CHECK(get_res->status == 404);
}

TEST_CASE("server: POST /api/games malformed PGN returns 400", "[motif-http]")
{
    auto const tdir = tmp_dir {"crud_bad_pgn"};
    auto db_res = motif::db::database_manager::create(tdir.path, "crud-bad-pgn-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18145};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const body = fmt::format(R"({{"pgn":{}}})", glz::write_json(std::string {malformed_pgn}).value_or("\"\""));
    auto const res = cli.Post("/api/games", body, "application/json");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 400);
    CHECK(res->body.contains("error"));
}

TEST_CASE("server: POST /api/games invalid SAN returns 400", "[motif-http]")
{
    auto const tdir = tmp_dir {"crud_bad_san"};
    auto db_res = motif::db::database_manager::create(tdir.path, "crud-bad-san-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18146};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const body = fmt::format(R"({{"pgn":{}}})", glz::write_json(std::string {invalid_san_pgn}).value_or("\"\""));
    auto const res = cli.Post("/api/games", body, "application/json");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 400);
}

TEST_CASE("server: POST /api/games duplicate returns 409", "[motif-http]")
{
    auto const tdir = tmp_dir {"crud_dup"};
    auto db_res = motif::db::database_manager::create(tdir.path, "crud-dup-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18147};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const post_body = fmt::format(R"({{"pgn":{}}})", glz::write_json(std::string {single_game_pgn}).value_or("\"\""));
    auto const first_res = cli.Post("/api/games", post_body, "application/json");
    REQUIRE(first_res != nullptr);
    REQUIRE(first_res->status == 201);
    auto const dup_res = cli.Post("/api/games", post_body, "application/json");

    srv.stop();
    server_thread.join();

    REQUIRE(dup_res != nullptr);
    CHECK(dup_res->status == 409);
}

TEST_CASE("server: PATCH /api/games/{id} invalid fields returns 400", "[motif-http]")
{
    auto const tdir = tmp_dir {"crud_patch_bad"};
    auto db_res = motif::db::database_manager::create(tdir.path, "crud-patch-bad-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18148};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const post_body = fmt::format(R"({{"pgn":{}}})", glz::write_json(std::string {single_game_pgn}).value_or("\"\""));
    auto const post_res = cli.Post("/api/games", post_body, "application/json");
    REQUIRE(post_res != nullptr);
    REQUIRE(post_res->status == 201);

    motif_http_test::create_game_response created;
    REQUIRE(!glz::read_json(created, post_res->body));

    auto const bad_status = cli.Patch(fmt::format("/api/games/{}", created.id), R"({"review_status":"invalid_value"})", "application/json");
    auto const bad_result = cli.Patch(fmt::format("/api/games/{}", created.id), R"({"result":"bad-result"})", "application/json");

    srv.stop();
    server_thread.join();

    REQUIRE(bad_status != nullptr);
    CHECK(bad_status->status == 400);
    REQUIRE(bad_result != nullptr);
    CHECK(bad_result->status == 400);
}

TEST_CASE("server: PATCH /api/games/{id} missing game returns 404", "[motif-http]")
{
    auto const tdir = tmp_dir {"crud_patch_404"};
    auto db_res = motif::db::database_manager::create(tdir.path, "crud-patch-404-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18149};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Patch("/api/games/99999", R"({"review_status":"new"})", "application/json");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 404);
}

TEST_CASE("server: PATCH/DELETE imported game returns 409", "[motif-http]")
{
    auto const tdir = tmp_dir {"crud_imported_409"};
    auto db_res = motif::db::database_manager::create(tdir.path, "crud-imported-409-db");
    REQUIRE(db_res.has_value());

    // Seed an imported (non-manual) game directly via game_store.
    constexpr std::uint16_t imported_seed_move {42};
    auto imported_id = insert_http_game(*db_res,
                                        {.white = "Imported",
                                         .black = "Game",
                                         .result = "1-0",
                                         .event = "Import",
                                         .date = "2024.01.01",
                                         .eco = "A00",
                                         .move_seed = imported_seed_move});

    constexpr std::uint16_t test_port {18150};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const patch_res = cli.Patch(fmt::format("/api/games/{}", imported_id), R"({"review_status":"new"})", "application/json");
    auto const del_res = cli.Delete(fmt::format("/api/games/{}", imported_id));

    srv.stop();
    server_thread.join();

    REQUIRE(patch_res != nullptr);
    CHECK(patch_res->status == 409);
    REQUIRE(del_res != nullptr);
    CHECK(del_res->status == 409);
}

TEST_CASE("server: DELETE /api/games/{id} removes game from position search", "[motif-http]")
{
    auto const tdir = tmp_dir {"crud_delete_positions"};
    auto db_res = motif::db::database_manager::create(tdir.path, "crud-delete-pos-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18151};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const post_body = fmt::format(R"({{"pgn":{}}})", glz::write_json(std::string {single_game_pgn}).value_or("\"\""));
    auto const post_res = cli.Post("/api/games", post_body, "application/json");
    REQUIRE(post_res != nullptr);
    REQUIRE(post_res->status == 201);

    motif_http_test::create_game_response created;
    REQUIRE(!glz::read_json(created, post_res->body));

    // Verify the game shows up.
    auto const get_before = cli.Get(fmt::format("/api/games/{}", created.id));
    REQUIRE(get_before != nullptr);
    REQUIRE(get_before->status == 200);

    auto const del_res = cli.Delete(fmt::format("/api/games/{}", created.id));
    REQUIRE(del_res != nullptr);
    REQUIRE(del_res->status == 204);

    auto const get_after = cli.Get(fmt::format("/api/games/{}", created.id));

    srv.stop();
    server_thread.join();

    REQUIRE(get_after != nullptr);
    CHECK(get_after->status == 404);
}

TEST_CASE("server: game list response includes provenance fields", "[motif-http]")
{
    auto const tdir = tmp_dir {"crud_list_provenance"};
    auto db_res = motif::db::database_manager::create(tdir.path, "crud-list-prov-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18152};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};

    // Insert imported game (direct store insert).
    constexpr std::uint16_t imported_move_seed {11};
    insert_http_game(*db_res,
                     {.white = "Imp1",
                      .black = "Imp2",
                      .result = "1-0",
                      .event = "Import",
                      .date = "2024.01.01",
                      .eco = "A00",
                      .move_seed = imported_move_seed});

    // Insert manual game via API.
    auto const post_body = fmt::format(R"({{"pgn":{}}})", glz::write_json(std::string {another_game_pgn}).value_or("\"\""));
    auto const post_res = cli.Post("/api/games", post_body, "application/json");
    REQUIRE(post_res != nullptr);
    REQUIRE(post_res->status == 201);

    auto const list_res = cli.Get("/api/games");

    srv.stop();
    server_thread.join();

    REQUIRE(list_res != nullptr);
    CHECK(list_res->status == 200);
    CHECK(list_res->body.contains(R"("source_type":"imported")"));
    CHECK(list_res->body.contains(R"("source_type":"manual")"));
}

TEST_CASE("server: POST /api/games empty pgn returns 400", "[motif-http]")
{
    auto const tdir = tmp_dir {"crud_empty_pgn"};
    auto db_res = motif::db::database_manager::create(tdir.path, "crud-empty-pgn-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18153};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Post("/api/games", R"({"pgn":""})", "application/json");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 400);
}

TEST_CASE("server: POST /api/games invalid review_status returns 400", "[motif-http]")
{
    auto const tdir = tmp_dir {"crud_bad_review"};
    auto db_res = motif::db::database_manager::create(tdir.path, "crud-bad-review-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18154};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const post_body =
        fmt::format(R"({{"pgn":{},"review_status":"bad_value"}})", glz::write_json(std::string {single_game_pgn}).value_or("\"\""));
    auto const res = cli.Post("/api/games", post_body, "application/json");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 400);
}

TEST_CASE("server: CORS with configured origins echoes matching origin", "[motif-http]")
{
    auto const tdir = tmp_dir {"cors_configured"};
    auto db_res = motif::db::database_manager::create(tdir.path, "cors-configured-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18155};
    motif::http::server srv {*db_res};
    std::thread server_thread {
        [&]() -> void
        { [[maybe_unused]] auto start_res = srv.start("localhost", test_port, {"http://localhost:3000", "http://127.0.0.1:5173"}); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    cli.set_default_headers({{"Origin", "http://localhost:3000"}});
    auto const res = cli.Get("/health");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 200);
    CHECK(res->get_header_value("Access-Control-Allow-Origin") == "http://localhost:3000");
}

TEST_CASE("server: CORS with configured origins rejects non-matching origin", "[motif-http]")
{
    auto const tdir = tmp_dir {"cors_reject"};
    auto db_res = motif::db::database_manager::create(tdir.path, "cors-reject-db");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18156};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void
                               { [[maybe_unused]] auto start_res = srv.start("localhost", test_port, {"http://localhost:3000"}); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    cli.set_default_headers({{"Origin", "http://evil.example.com"}});
    auto const res = cli.Get("/health");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 200);
    CHECK(res->get_header_value("Access-Control-Allow-Origin").empty());
}

TEST_CASE("server: game count returns 0 on empty database", "[motif-http]")
{
    auto const tdir = tmp_dir {"game_count_empty"};
    auto db_res = motif::db::database_manager::create(tdir.path / "db", "game-count-empty");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18157};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Get("/api/games/count");

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 200);
    CHECK(res->body.contains(R"("count":0)"));
}

TEST_CASE("server: game count reflects imported games", "[motif-http]")
{
    auto const tdir = tmp_dir {"game_count_import"};
    auto db_res = motif::db::database_manager::create(tdir.path / "db", "game-count-import");
    REQUIRE(db_res.has_value());

    auto const pgn_path = write_pgn_fixture(tdir.path, three_game_pgn_content);
    auto logging = import_logging_scope {tdir.path / "logs"};

    constexpr std::uint16_t test_port {18158};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    cli.set_read_timeout(sse_read_timeout_s);

    auto const import_res = cli.Post("/api/imports", R"({"path":")" + pgn_path.string() + R"("})", "application/json");
    REQUIRE(import_res != nullptr);
    REQUIRE(import_res->status == 202);
    auto const import_id = extract_import_id(import_res->body);
    REQUIRE(!import_id.empty());

    cli.Get("/api/imports/" + import_id + "/progress",
            httplib::Headers {},
            [](const char* /*data*/, size_t /*size*/) -> bool { return true; });

    auto const count_res = cli.Get("/api/games/count");

    srv.stop();
    server_thread.join();
    logging.shutdown();

    REQUIRE(count_res != nullptr);
    CHECK(count_res->status == 200);
    CHECK(count_res->body.contains(R"("count":3)"));
}

TEST_CASE("server: upload import rejects request with missing file field", "[motif-http]")
{
    auto const tdir = tmp_dir {"upload_missing_field"};
    auto db_res = motif::db::database_manager::create(tdir.path / "db", "upload-missing");
    REQUIRE(db_res.has_value());

    constexpr std::uint16_t test_port {18159};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Post(
        "/api/imports/upload",
        httplib::UploadFormDataItems {
            {.name = "not_file", .content = "dummy", .filename = "dummy.pgn", .content_type = "text/plain"}});

    srv.stop();
    server_thread.join();

    REQUIRE(res != nullptr);
    CHECK(res->status == 400);
    CHECK(res->body.contains("missing file field"));
}

TEST_CASE("server: upload import accepts PGN bytes and returns 202 with import_id", "[motif-http]")
{
    auto const tdir = tmp_dir {"upload_valid"};
    auto db_res = motif::db::database_manager::create(tdir.path / "db", "upload-valid");
    REQUIRE(db_res.has_value());

    auto logging = import_logging_scope {tdir.path / "logs"};

    constexpr std::uint16_t test_port {18160};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    auto const res = cli.Post(
        "/api/imports/upload",
        httplib::UploadFormDataItems {
            {.name = "file", .content = std::string {three_game_pgn_content}, .filename = "games.pgn", .content_type = "text/plain"}});

    std::this_thread::sleep_for(import_short_settle_ms);
    srv.stop();
    server_thread.join();
    logging.shutdown();

    REQUIRE(res != nullptr);
    CHECK(res->status == 202);
    CHECK(res->body.contains(R"("import_id")"));
}

TEST_CASE("server: upload import rejects a second concurrent upload", "[motif-http]")
{
    auto const tdir = tmp_dir {"upload_conflict"};
    auto db_res = motif::db::database_manager::create(tdir.path / "db", "upload-conflict");
    REQUIRE(db_res.has_value());

    auto logging = import_logging_scope {tdir.path / "logs"};

    constexpr std::uint16_t test_port {18161};
    motif::http::server srv {*db_res};
    std::thread server_thread {[&]() -> void { [[maybe_unused]] auto start_res = srv.start(test_port); }};
    REQUIRE(wait_for_ready(srv));

    httplib::Client cli {"localhost", test_port};
    cli.set_read_timeout(sse_read_timeout_s);

    auto const long_pgn = make_repeated_pgn(long_import_game_count);
    auto const first_res = cli.Post(
        "/api/imports/upload",
        httplib::UploadFormDataItems {
            {.name = "file", .content = long_pgn, .filename = "games.pgn", .content_type = "text/plain"}});
    REQUIRE(first_res != nullptr);
    REQUIRE(first_res->status == 202);

    auto const second_res = cli.Post(
        "/api/imports/upload",
        httplib::UploadFormDataItems {
            {.name = "file", .content = std::string {three_game_pgn_content}, .filename = "small.pgn", .content_type = "text/plain"}});
    REQUIRE(second_res != nullptr);
    CHECK(second_res->status == 409);

    auto const import_id = extract_import_id(first_res->body);
    auto const del_res = cli.Delete("/api/imports/" + import_id);
    REQUIRE(del_res != nullptr);
    cli.Get("/api/imports/" + import_id + "/progress",
            httplib::Headers {},
            [](const char* /*data*/, size_t /*size*/) -> bool { return true; });

    srv.stop();
    server_thread.join();
    logging.shutdown();
}

// NOLINTEND(readability-function-cognitive-complexity)
