#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "motif/http/server.hpp"

#include <fmt/format.h>
#include <glaze/json/read.hpp>
#include <glaze/json/write.hpp>
#include <httplib.h>
#include <tl/expected.hpp>

#include "motif/db/database_manager.hpp"
#include "motif/db/error.hpp"
#include "motif/db/types.hpp"
#include "motif/http/error.hpp"
#include "motif/import/error.hpp"
#include "motif/import/import_pipeline.hpp"
#include "motif/search/opening_stats.hpp"
#include "motif/search/position_search.hpp"

// These structs must live in a named namespace — glaze reflection cannot
// resolve types in anonymous namespaces (no external linkage).
namespace motif::http::detail
{

struct health_response
{
    std::string status {"ok"};
};

struct error_response
{
    std::string error;
};

struct game_player_response
{
    std::string name;
    std::optional<std::int32_t> elo;
    std::optional<std::string> title;
    std::optional<std::string> country;
};

struct game_event_response
{
    std::string name;
    std::optional<std::string> site;
    std::optional<std::string> date;
};

struct game_tag_response
{
    std::string key;
    std::string value;
};

struct game_response
{
    std::uint32_t id {};
    game_player_response white;
    game_player_response black;
    std::optional<game_event_response> event;
    std::optional<std::string> date;
    std::string result;
    std::optional<std::string> eco;
    std::vector<game_tag_response> tags;
    std::vector<std::uint16_t> moves;
};

struct opening_continuation_response
{
    std::string san;
    std::string result_hash;
    std::uint32_t frequency {};
    std::uint32_t white_wins {};
    std::uint32_t draws {};
    std::uint32_t black_wins {};
    std::optional<double> average_white_elo;
    std::optional<double> average_black_elo;
    std::optional<std::string> eco;
    std::optional<std::string> opening_name;
};

struct opening_stats_response
{
    std::vector<opening_continuation_response> continuations;
};

struct import_response
{
    std::string import_id;
};

struct import_request_body
{
    std::string path;
};

struct import_session
{
    std::unique_ptr<motif::import::import_pipeline> pipeline;
    std::atomic<bool> done {false};
    // Store with release so acquire on `done` forms a happens-before fence
    // covering `error_message`, `summary`, and `failed`.
    std::atomic<bool> failed {false};
    motif::import::import_summary summary {};
    std::string error_message;
    std::mutex cv_mutex;
    std::condition_variable cv;
};

}  // namespace motif::http::detail

namespace motif::http
{

namespace
{

constexpr int http_ok {200};
constexpr int http_accepted {202};
constexpr int http_bad_request {400};
constexpr int http_conflict {409};
constexpr int http_not_found {404};
constexpr int http_internal_error {500};

constexpr std::chrono::milliseconds sse_poll_interval {250};
constexpr std::chrono::minutes sse_max_wait {30};

constexpr std::string_view fail_next_import_worker_start_env {"MOTIF_HTTP_TEST_FAIL_NEXT_IMPORT_WORKER_START"};
constexpr std::string_view fail_next_import_worker_run_env {"MOTIF_HTTP_TEST_FAIL_NEXT_IMPORT_WORKER_RUN"};

auto env_flag_enabled(std::string_view const name) -> bool
{
    auto const env_name = std::string {name};
    // NOLINTNEXTLINE(concurrency-mt-unsafe) -- read once during server construction.
    auto const* const value = std::getenv(env_name.c_str());
    return value != nullptr && std::string_view {value} == "1";
}

void set_json_error(httplib::Response& res, int const status, std::string_view const message)
{
    std::string body {};
    [[maybe_unused]] auto const err = glz::write_json(detail::error_response {std::string {message}}, body);
    res.set_content(body, "application/json");
    res.status = status;
}

auto parse_size(std::string_view str) -> std::optional<std::size_t>
{
    if (str.empty()) {
        return std::nullopt;
    }
    std::size_t val {};
    auto const [ptr, ec] = std::from_chars(str.data(),
                                           str.data() + str.size(),  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                                           val);
    if (ec != std::errc {} || ptr != str.data() + str.size()) {  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        return std::nullopt;
    }
    return val;
}

auto parse_game_id(std::string_view id_str) -> std::optional<std::uint32_t>
{
    if (id_str.empty()) {
        return std::nullopt;
    }

    std::uint64_t value {};
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    auto const [ptr, ec] = std::from_chars(id_str.data(), id_str.data() + id_str.size(), value);
    if (ec != std::errc {} || ptr != id_str.data() + id_str.size()) {
        return std::nullopt;
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

    if (value == 0 || value > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(value);
}

auto to_game_player_response(motif::db::player const& source) -> detail::game_player_response
{
    return detail::game_player_response {
        .name = source.name,
        .elo = source.elo,
        .title = source.title,
        .country = source.country,
    };
}

auto to_game_event_response(motif::db::event const& source) -> detail::game_event_response
{
    return detail::game_event_response {
        .name = source.name,
        .site = source.site,
        .date = source.date,
    };
}

auto to_game_response(std::uint32_t const game_id, motif::db::game const& source) -> detail::game_response
{
    auto tags = std::vector<detail::game_tag_response> {};
    tags.reserve(source.extra_tags.size());
    for (auto const& [key, value] : source.extra_tags) {
        tags.push_back(detail::game_tag_response {
            .key = key,
            .value = value,
        });
    }

    return detail::game_response {
        .id = game_id,
        .white = to_game_player_response(source.white),
        .black = to_game_player_response(source.black),
        .event = source.event_details.transform(to_game_event_response),
        .date = source.date,
        .result = source.result,
        .eco = source.eco,
        .tags = std::move(tags),
        .moves = source.moves,
    };
}

[[nodiscard]] auto to_opening_continuation_response(motif::search::opening_stats::continuation const& source)
    -> detail::opening_continuation_response
{
    return detail::opening_continuation_response {
        .san = source.san,
        .result_hash = fmt::format("{}", source.result_hash),
        .frequency = source.frequency,
        .white_wins = source.white_wins,
        .draws = source.draws,
        .black_wins = source.black_wins,
        .average_white_elo = source.average_white_elo,
        .average_black_elo = source.average_black_elo,
        .eco = source.eco,
        .opening_name = source.opening_name,
    };
}

[[nodiscard]] auto to_opening_stats_response(motif::search::opening_stats::stats const& source) -> detail::opening_stats_response
{
    auto continuations = std::vector<detail::opening_continuation_response> {};
    continuations.reserve(source.continuations.size());
    for (auto const& continuation : source.continuations) {
        continuations.push_back(to_opening_continuation_response(continuation));
    }
    return detail::opening_stats_response {.continuations = std::move(continuations)};
}

auto generate_import_id() -> std::string
{
    static std::mutex rng_mutex;
    static std::mt19937_64 rng {std::random_device {}()};
    std::uint64_t high {};
    std::uint64_t low {};
    {
        std::scoped_lock const lock {rng_mutex};
        high = rng();
        low = rng();
    }
    return fmt::format("{:016x}{:016x}", high, low);
}

void register_cors(httplib::Server& svr)
{
    svr.set_post_routing_handler(
        [](httplib::Request const& /*req*/, httplib::Response& res) -> void
        {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type");
        });

    svr.Options(".*", [](httplib::Request const& /*req*/, httplib::Response& res) -> void { res.status = http_ok; });
}

}  // namespace

// server::impl is a private nested type — all route registration is done via
// the setup_routes() member function so no external code needs to name impl.
struct server::impl
{
    httplib::Server svr;
    std::mutex database_mutex;
    std::mutex sessions_mutex;
    std::unordered_map<std::string, std::shared_ptr<detail::import_session>> sessions;
    // Each entry pairs an import_id with its worker thread for lifecycle tracking.
    std::deque<std::pair<std::string, std::jthread>> import_workers;
    // Insertion-ordered list of completed import IDs, capped at max_completed_sessions.
    std::deque<std::string> completed_session_ids;
    std::atomic<bool> fail_next_import_worker_start_for_test {false};
    std::atomic<bool> fail_next_import_worker_run_for_test {false};
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    motif::db::database_manager& database;

    explicit impl(motif::db::database_manager& mgr);
    ~impl();
    impl(impl const&) = delete;
    auto operator=(impl const&) -> impl& = delete;
    impl(impl&&) = delete;
    auto operator=(impl&&) -> impl& = delete;

    // Join and remove workers for completed sessions; prune old completed
    // session map entries beyond max_completed_sessions. Must be called while
    // holding sessions_mutex.
    void prune_completed();

    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    void setup_routes();
};

server::impl::impl(motif::db::database_manager& mgr)
    : fail_next_import_worker_start_for_test {env_flag_enabled(fail_next_import_worker_start_env)}
    , fail_next_import_worker_run_for_test {env_flag_enabled(fail_next_import_worker_run_env)}
    , database {mgr}
{
    register_cors(svr);
    setup_routes();
}

server::impl::~impl()
{
    svr.stop();
    {
        std::scoped_lock const lock {sessions_mutex};
        for (auto const& [import_id, session] : sessions) {
            static_cast<void>(import_id);
            if (session->pipeline) {
                session->pipeline->request_stop();
            }
            // Wake any SSE provider blocked in cv.wait_for so it can observe
            // the server shutting down and exit cleanly.
            session->cv.notify_all();
        }
    }
    // Explicitly join all worker threads before the destructor returns.
    // Workers for sessions that are already done join immediately.
    for (auto& [import_id, worker] : import_workers) {
        static_cast<void>(import_id);
        worker.join();
    }
    import_workers.clear();
}

void server::impl::prune_completed()
{
    // Called while holding sessions_mutex.
    constexpr std::size_t max_completed_sessions {64};

    // Separate active workers from completed ones, joining completed threads.
    std::deque<std::pair<std::string, std::jthread>> active_workers;
    for (auto& [import_id, worker] : import_workers) {
        auto const sess_it = sessions.find(import_id);
        bool const is_done = (sess_it != sessions.end()) && sess_it->second->done.load(std::memory_order_acquire);
        if (is_done) {
            completed_session_ids.push_back(import_id);
            // jthread destructor joins; thread already exited so join is immediate.
        } else {
            active_workers.emplace_back(import_id, std::move(worker));
        }
    }
    import_workers = std::move(active_workers);

    // Evict oldest completed sessions beyond the cap.
    while (completed_session_ids.size() > max_completed_sessions) {
        sessions.erase(completed_session_ids.front());
        completed_session_ids.pop_front();
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void server::impl::setup_routes()
{
    svr.Get("/health",
            [](httplib::Request const& /*req*/, httplib::Response& res) -> void
            {
                std::string body {};
                [[maybe_unused]] auto const err = glz::write_json(detail::health_response {}, body);
                res.set_content(body, "application/json");
                res.status = http_ok;
            });

    auto const invalid_hash_handler = [](httplib::Request const& /*req*/, httplib::Response& res) -> void
    { set_json_error(res, http_bad_request, "invalid zobrist hash"); };

    svr.Get("/api/positions", invalid_hash_handler);
    svr.Get("/api/positions/", invalid_hash_handler);

    svr.Get("/api/positions/:zobrist_hash",
            [this](httplib::Request const& req, httplib::Response& res) -> void
            {
                auto const& hash_str = req.path_params.at("zobrist_hash");
                std::uint64_t hash_val {};
                {
                    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                    auto const [ptr, ec] = std::from_chars(hash_str.data(), hash_str.data() + hash_str.size(), hash_val);
                    if (ec != std::errc {} || ptr != hash_str.data() + hash_str.size()) {
                        set_json_error(res, http_bad_request, "invalid zobrist hash");
                        return;
                    }
                    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                }

                constexpr std::size_t default_limit {100};
                constexpr std::size_t max_limit {500};

                auto const limit_str = req.get_param_value("limit");
                auto const offset_str = req.get_param_value("offset");
                auto const has_limit = req.has_param("limit");

                std::size_t limit = default_limit;
                std::size_t offset = 0;

                if (!limit_str.empty()) {
                    auto parsed = parse_size(limit_str);
                    if (!parsed) {
                        set_json_error(res, http_bad_request, "invalid pagination parameters");
                        return;
                    }
                    limit = std::min(*parsed, max_limit);
                }
                if (!offset_str.empty()) {
                    auto parsed = parse_size(offset_str);
                    if (!parsed) {
                        set_json_error(res, http_bad_request, "invalid pagination parameters");
                        return;
                    }
                    offset = *parsed;
                }

                if (has_limit && limit == 0) {
                    res.set_content("[]", "application/json");
                    res.status = http_ok;
                    return;
                }

                auto matches =
                    [this, hash_val, limit, offset]() -> decltype(motif::search::position_search::find(database, hash_val, limit, offset))
                {
                    std::scoped_lock const lock {database_mutex};
                    return motif::search::position_search::find(database, hash_val, limit, offset);
                }();
                if (!matches) {
                    set_json_error(res, http_internal_error, "search failed");
                    return;
                }

                std::string body {};
                [[maybe_unused]] auto const err = glz::write_json(*matches, body);
                res.set_content(body, "application/json");
                res.status = http_ok;
            });

    svr.Get("/api/openings", invalid_hash_handler);
    svr.Get("/api/openings/", invalid_hash_handler);

    svr.Get("/api/openings/:zobrist_hash/stats",
            [this](httplib::Request const& req, httplib::Response& res) -> void
            {
                auto const& hash_str = req.path_params.at("zobrist_hash");
                std::uint64_t hash_val {};
                {
                    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                    auto const [ptr, ec] = std::from_chars(hash_str.data(), hash_str.data() + hash_str.size(), hash_val);
                    if (ec != std::errc {} || ptr != hash_str.data() + hash_str.size()) {
                        set_json_error(res, http_bad_request, "invalid zobrist hash");
                        return;
                    }
                    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                }

                auto query_result = [this, hash_val]() -> decltype(motif::search::opening_stats::query(database, hash_val))
                {
                    std::scoped_lock const lock {database_mutex};
                    return motif::search::opening_stats::query(database, hash_val);
                }();
                if (!query_result) {
                    set_json_error(res, http_internal_error, "stats query failed");
                    return;
                }

                std::string body {};
                [[maybe_unused]] auto const err = glz::write_json(to_opening_stats_response(*query_result), body);
                res.set_content(body, "application/json");
                res.status = http_ok;
            });

    svr.Get("/api/games",
            [this](httplib::Request const& req, httplib::Response& res) -> void
            {
                constexpr std::size_t default_game_list_limit {50};
                constexpr std::size_t max_game_list_limit {200};

                auto const limit_str = req.get_param_value("limit");
                auto const offset_str = req.get_param_value("offset");

                std::size_t limit = default_game_list_limit;
                std::size_t offset = 0;

                if (req.has_param("limit")) {
                    auto parsed = parse_size(limit_str);
                    if (!parsed) {
                        set_json_error(res, http_bad_request, "invalid pagination parameters");
                        return;
                    }
                    limit = std::min(*parsed, max_game_list_limit);
                }

                if (req.has_param("offset")) {
                    auto parsed = parse_size(offset_str);
                    if (!parsed) {
                        set_json_error(res, http_bad_request, "invalid pagination parameters");
                        return;
                    }
                    offset = *parsed;
                }

                if (limit == 0) {
                    res.set_content("[]", "application/json");
                    res.status = http_ok;
                    return;
                }

                auto to_filter = [](std::string value) -> std::optional<std::string>
                { return value.empty() ? std::nullopt : std::optional<std::string> {std::move(value)}; };
                auto query = motif::db::game_list_query {
                    .player = req.has_param("player") ? to_filter(req.get_param_value("player")) : std::nullopt,
                    .result = req.has_param("result") ? to_filter(req.get_param_value("result")) : std::nullopt,
                    .limit = limit,
                    .offset = offset,
                };

                auto games = [this, &query]() -> decltype(database.store().list_games(query))
                {
                    std::scoped_lock const lock {database_mutex};
                    return database.store().list_games(query);
                }();
                if (!games) {
                    set_json_error(res, http_internal_error, "game list query failed");
                    return;
                }

                std::string body {};
                [[maybe_unused]] auto const err = glz::write_json(*games, body);
                res.set_content(body, "application/json");
                res.status = http_ok;
            });

    svr.Get("/api/games/",
            [](httplib::Request const& /*req*/, httplib::Response& res) -> void
            { set_json_error(res, http_bad_request, "invalid game id"); });

    svr.Get("/api/games/:id",
            [this](httplib::Request const& req, httplib::Response& res) -> void
            {
                auto const& id_str = req.path_params.at("id");
                auto const game_id = parse_game_id(id_str);
                if (!game_id) {
                    set_json_error(res, http_bad_request, "invalid game id");
                    return;
                }

                auto game_result = [this, game_id]() -> decltype(database.store().get(*game_id))
                {
                    std::scoped_lock const lock {database_mutex};
                    return database.store().get(*game_id);
                }();
                if (!game_result) {
                    if (game_result.error() == motif::db::error_code::not_found) {
                        set_json_error(res, http_not_found, "not_found");
                        return;
                    }
                    set_json_error(res, http_internal_error, "game retrieval failed");
                    return;
                }

                std::string body {};
                if (auto const err = glz::write_json(to_game_response(*game_id, *game_result), body); err) {
                    set_json_error(res, http_internal_error, "game retrieval failed");
                    return;
                }
                res.set_content(body, "application/json");
                res.status = http_ok;
            });

    svr.Post("/api/imports",
             [this](httplib::Request const& req, httplib::Response& res) -> void
             {
                 detail::import_request_body req_body;
                 if (auto parse_err = glz::read_json(req_body, req.body); parse_err) {
                     set_json_error(res, http_bad_request, "invalid request body");
                     return;
                 }

                 std::filesystem::path const pgn_path {req_body.path};
                 std::error_code filesystem_error;
                 auto const is_readable_file = std::filesystem::exists(pgn_path, filesystem_error)
                     && std::filesystem::is_regular_file(pgn_path, filesystem_error)
                     && std::ifstream {pgn_path, std::ios::binary}.is_open();
                 if (filesystem_error || !is_readable_file) {
                     set_json_error(res, http_bad_request, "file not found or not readable");
                     return;
                 }

                 // Conflict check runs before any heap allocation for session or pipeline (AC 6).
                 std::string import_id;
                 std::shared_ptr<detail::import_session> session;
                 {
                     std::scoped_lock const lock {sessions_mutex};
                     prune_completed();
                     auto const active_import = std::ranges::any_of(
                         sessions, [](auto const& entry) -> bool { return !entry.second->done.load(std::memory_order_acquire); });
                     if (active_import) {
                         set_json_error(res, http_conflict, "import already running");
                         return;
                     }
                     import_id = generate_import_id();
                     while (sessions.contains(import_id)) {
                         import_id = generate_import_id();
                     }
                     session = std::make_shared<detail::import_session>();
                     session->pipeline = std::make_unique<motif::import::import_pipeline>(database);
                     sessions.emplace(import_id, session);
                 }

                 // Construct jthread outside the lock; catch std::system_error and
                 // clean up the session entry if the OS cannot create the thread (AC 1).
                 auto const simulate_start_failure = fail_next_import_worker_start_for_test.exchange(false);
                 auto const simulate_run_failure = fail_next_import_worker_run_for_test.exchange(false);
                 try {
                     if (simulate_start_failure) {
                         throw std::system_error {std::make_error_code(std::errc::resource_unavailable_try_again),
                                                  "simulated import worker start failure"};
                     }
                     auto worker = std::jthread {
                         [session, pgn_path, simulate_run_failure]() -> void
                         {
                             auto result = simulate_run_failure ? motif::import::result<motif::import::import_summary> {tl::unexpected {
                                                                      motif::import::error_code::invalid_state}}
                                                                : session->pipeline->run(pgn_path, {});
                             if (result) {
                                 session->summary = *result;
                             } else {
                                 // Include error code in message so the SSE JSON
                                 // escaping path is exercised when chars like '"'
                                 // appear after further formatting.
                                 session->error_message = fmt::format(R"(import failed: "{}")", motif::import::to_string(result.error()));
                                 // Release store: pairs with acquire loads on done
                                 // to make error_message visible.
                                 session->failed.store(true, std::memory_order_release);
                             }
                             session->done.store(true, std::memory_order_release);
                             session->cv.notify_all();
                         }};
                     {
                         std::scoped_lock const lock {sessions_mutex};
                         import_workers.emplace_back(import_id, std::move(worker));
                     }
                 } catch (std::system_error const&) {
                     std::scoped_lock const lock {sessions_mutex};
                     sessions.erase(import_id);
                     set_json_error(res, http_internal_error, "failed to start import worker");
                     return;
                 }

                 std::string body {};
                 [[maybe_unused]] auto const err = glz::write_json(detail::import_response {import_id}, body);
                 res.set_content(body, "application/json");
                 res.status = http_accepted;
             });

    svr.Get("/api/imports/:import_id/progress",
            [this](httplib::Request const& req, httplib::Response& res) -> void
            {
                auto const& import_id_str = req.path_params.at("import_id");
                std::shared_ptr<detail::import_session> session;
                {
                    std::scoped_lock const lock {sessions_mutex};
                    auto const session_iter = sessions.find(import_id_str);
                    if (session_iter == sessions.end()) {
                        set_json_error(res, http_not_found, "import not found");
                        return;
                    }
                    session = session_iter->second;
                }

                res.set_header("Cache-Control", "no-cache");
                res.set_header("X-Accel-Buffering", "no");
                res.set_chunked_content_provider(
                    "text/event-stream",
                    [session, start_time = std::chrono::steady_clock::now()](size_t /*offset*/, httplib::DataSink& sink) -> bool
                    {
                        // Null-pointer guard: pipeline should always be set, but
                        // defend against future refactors (AC 7).
                        if (!session->pipeline) {
                            std::string error_json;
                            [[maybe_unused]] auto const write_err =
                                glz::write_json(detail::error_response {"pipeline unavailable"}, error_json);
                            auto const error_event = fmt::format("event: error\ndata: {}\n\n", error_json);
                            sink.write(error_event.data(), error_event.size());
                            sink.done();
                            return false;
                        }

                        auto const prog = session->pipeline->progress();
                        auto const elapsed = static_cast<double>(prog.elapsed.count()) / 1000.0;
                        auto event = fmt::format(
                            "data: {{\"games_processed\":{},\"games_committed\":{},\"games_skipped\":{},\"elapsed_seconds\":{:.3f}}}\n\n",
                            prog.games_processed,
                            prog.games_committed,
                            prog.games_skipped,
                            elapsed);
                        if (!sink.write(event.data(), event.size())) {
                            return false;
                        }

                        // Acquire on `done` synchronizes with the release stores in the worker
                        // (done, failed, error_message, summary) — all prior writes are visible here.
                        if (session->done.load(std::memory_order_acquire)) {
                            if (session->failed.load(std::memory_order_acquire)) {
                                std::string error_json;
                                [[maybe_unused]] auto const write_err = glz::write_json(
                                    detail::error_response {session->error_message.empty() ? "import failed" : session->error_message},
                                    error_json);
                                auto const error_event = fmt::format("event: error\ndata: {}\n\n", error_json);
                                sink.write(error_event.data(), error_event.size());
                                sink.done();
                                return false;
                            }
                            auto const& smry = session->summary;
                            auto final_event = fmt::format(
                                "event: complete\ndata: "
                                "{{\"total_attempted\":{},\"committed\":{},\"skipped\":{},\"errors\":{},\"elapsed_ms\":{}}}\n\n",
                                smry.total_attempted,
                                smry.committed,
                                smry.skipped,
                                smry.errors,
                                smry.elapsed.count());
                            sink.write(final_event.data(), final_event.size());
                            sink.done();
                            return false;
                        }

                        // Enforce a maximum wait to guard against an import thread that
                        // never sets done (AC 10).
                        if (std::chrono::steady_clock::now() - start_time >= sse_max_wait) {
                            std::string error_json;
                            [[maybe_unused]] auto const write_err =
                                glz::write_json(detail::error_response {"timed out waiting for import"}, error_json);
                            auto const error_event = fmt::format("event: error\ndata: {}\n\n", error_json);
                            sink.write(error_event.data(), error_event.size());
                            sink.done();
                            return false;
                        }

                        // Use condition_variable so the final event is sent promptly
                        // when the worker notifies rather than waiting a full poll interval (AC 5).
                        {
                            std::unique_lock<std::mutex> cv_lock {session->cv_mutex};
                            session->cv.wait_for(
                                cv_lock, sse_poll_interval, [&session]() -> bool { return session->done.load(std::memory_order_acquire); });
                        }
                        return true;
                    });
            });

    svr.Delete("/api/imports/:import_id",
               [this](httplib::Request const& req, httplib::Response& res) -> void
               {
                   auto const& import_id_str = req.path_params.at("import_id");
                   std::shared_ptr<detail::import_session> session;
                   {
                       std::scoped_lock const lock {sessions_mutex};
                       auto const session_iter = sessions.find(import_id_str);
                       if (session_iter == sessions.end()) {
                           set_json_error(res, http_not_found, "import not found");
                           return;
                       }
                       session = session_iter->second;
                   }

                   if (session->pipeline) {
                       session->pipeline->request_stop();
                   }

                   res.set_content(R"({"status":"cancellation_requested"})", "application/json");
                   res.status = http_ok;
               });
}

server::server(motif::db::database_manager& database)
    : impl_ {std::make_unique<impl>(database)}
{
}

server::~server() = default;

auto server::start(std::uint16_t port) -> result<void>
{
    if (!impl_->svr.listen("0.0.0.0", port)) {
        return tl::unexpected {error_code::listen_failed};
    }
    return {};
}

auto server::stop() -> void
{
    impl_->svr.stop();
}

auto server::is_running() const -> bool
{
    return impl_->svr.is_running();
}

}  // namespace motif::http
