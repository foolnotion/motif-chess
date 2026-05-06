#include <algorithm>
#include <array>
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
#include <utility>
#include <vector>

#include "motif/http/server.hpp"

#include <fmt/format.h>
#include <glaze/json/read.hpp>
#include <glaze/json/write.hpp>
#include <gtl/phmap.hpp>
#include <httplib.h>
#include <pgnlib/pgnlib.hpp>
#include <tl/expected.hpp>

#include "motif/chess/chess.hpp"
#include "motif/db/database_manager.hpp"
#include "motif/db/error.hpp"
#include "motif/db/types.hpp"
#include "motif/engine/engine_manager.hpp"
#include "motif/engine/error.hpp"
#include "motif/http/error.hpp"
#include "motif/import/error.hpp"
#include "motif/import/import_pipeline.hpp"
#include "motif/import/import_worker.hpp"
#include "motif/search/opening_stats.hpp"
#include "motif/search/position_search.hpp"

template<>
struct glz::meta<motif::db::game_id>
{
    using T = motif::db::game_id;  // NOLINT(readability-identifier-naming) — glaze convention
    static constexpr auto value = &T::value;
};

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

struct game_tag_response
{
    std::string key;
    std::string value;
};

struct game_response
{
    std::uint32_t id {};
    motif::db::player white;
    motif::db::player black;
    std::optional<motif::db::event> event;
    std::optional<std::string> date;
    std::string result;
    std::optional<std::string> eco;
    std::vector<game_tag_response> tags;
    std::vector<std::uint16_t> moves;
    motif::db::game_provenance provenance;
};

struct create_game_request
{
    std::string pgn;
    std::optional<std::string> source_label;
    std::optional<std::string> review_status;
};

struct create_game_response
{
    std::uint32_t id {};
    std::string source_type;
    std::optional<std::string> source_label;
    std::string review_status;
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
    std::uint32_t total_games {};
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

struct game_count_response
{
    std::int64_t count {};
};

struct position_hash_response
{
    std::string hash;
};

struct game_positions_response
{
    std::string starting_hash;
    std::vector<std::string> fens;
    std::vector<std::string> sans;
    std::vector<std::string> hashes;
};

struct legal_move_response
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
    std::vector<legal_move_response> legal_moves;
};

struct apply_move_request
{
    std::string fen;
    std::string uci;
};

struct apply_move_response
{
    std::string uci;
    std::string san;
    std::string fen;
};

struct start_analysis_request
{
    std::string fen;
    std::string engine;
    std::optional<int> multipv;
    std::optional<int> depth;
    std::optional<int> movetime_ms;
};

struct start_analysis_response
{
    std::string analysis_id;
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

struct configure_engine_request
{
    std::string name;
    std::string path;
};

struct engine_list_entry
{
    std::string name;
    std::string path;
};

struct engine_list_response
{
    std::vector<engine_list_entry> engines;
};

struct sse_score_value
{
    std::string type;
    int value {};
};

struct sse_analysis_info_event
{
    int depth {};
    std::optional<int> seldepth;
    int multipv {1};
    sse_score_value score;
    std::vector<std::string> pv_uci;
    std::optional<std::vector<std::string>> pv_san;
    std::optional<std::int64_t> nodes;
    std::optional<int> nps;
    std::optional<int> time_ms;
};

struct sse_analysis_complete_event
{
    std::string best_move_uci;
    std::optional<std::string> ponder_uci;
};

struct sse_analysis_error_event
{
    std::string message;
};

}  // namespace motif::http::detail

namespace motif::http
{

namespace
{

constexpr int http_ok {200};
constexpr int http_created {201};
constexpr int http_no_content {204};
constexpr int http_accepted {202};
constexpr int http_bad_request {400};
constexpr int http_not_found {404};
constexpr int http_conflict {409};
constexpr int http_service_unavailable {503};
[[maybe_unused]] constexpr int http_not_implemented {501};
constexpr int http_internal_error {500};

constexpr std::array valid_results {"1-0", "0-1", "1/2-1/2", "*"};
constexpr std::array valid_review_statuses {"new", "needs_review", "studied", "archived"};

auto is_valid_result(std::string_view const val) -> bool
{
    return std::ranges::any_of(valid_results, [&](std::string_view entry) -> bool { return entry == val; });
}

auto is_valid_review_status(std::string_view const val) -> bool
{
    return std::ranges::any_of(valid_review_statuses, [&](std::string_view entry) -> bool { return entry == val; });
}

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

auto parse_int32(std::string_view str) -> std::optional<std::int32_t>
{
    if (str.empty()) {
        return std::nullopt;
    }
    std::int32_t val {};
    auto const [ptr, ec] = std::from_chars(str.data(),
                                           str.data() + str.size(),  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                                           val);
    if (ec != std::errc {} || ptr != str.data() + str.size()) {  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        return std::nullopt;
    }
    return val;
}

auto parse_game_id(std::string_view id_str) -> std::optional<motif::db::game_id>
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
    return motif::db::game_id {static_cast<std::uint32_t>(value)};
}

auto to_game_response(motif::db::game_id const game_id, motif::db::game const& source) -> detail::game_response
{
    auto tags = std::vector<detail::game_tag_response> {};
    tags.reserve(source.extra_tags.size());
    for (auto const& [key, value] : source.extra_tags) {
        tags.push_back({.key = key, .value = value});
    }
    return detail::game_response {
        .id = static_cast<std::uint32_t>(game_id),
        .white = source.white,
        .black = source.black,
        .event = source.event_details,
        .date = source.date,
        .result = source.result,
        .eco = source.eco,
        .tags = std::move(tags),
        .moves = source.moves,
        .provenance = source.provenance,
    };
}

[[nodiscard]] auto to_opening_continuation_response(motif::search::opening_stats::continuation const& source)
    -> detail::opening_continuation_response
{
    return detail::opening_continuation_response {
        .san = source.san,
        .result_hash = fmt::format("{}", static_cast<std::uint64_t>(source.result_hash)),
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
    return detail::opening_stats_response {.total_games = source.total_games, .continuations = std::move(continuations)};
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

auto is_valid_uci_syntax(std::string const& uci_str) -> bool
{
    constexpr std::size_t uci_move_length {4};
    constexpr std::size_t uci_promotion_length {5};
    auto const is_file = [](char const value) -> bool { return value >= 'a' && value <= 'h'; };
    auto const is_rank = [](char const value) -> bool { return value >= '1' && value <= '8'; };
    auto const is_promotion = [](char const value) -> bool { return value == 'q' || value == 'r' || value == 'b' || value == 'n'; };

    if (uci_str.size() != uci_move_length && uci_str.size() != uci_promotion_length) {
        return false;
    }
    if (!is_file(uci_str[0]) || !is_rank(uci_str[1]) || !is_file(uci_str[2]) || !is_rank(uci_str[3])) {
        return false;
    }
    return uci_str.size() == uci_move_length || is_promotion(uci_str[uci_move_length]);
}

// Reconstruct PGN text from a stored game.
// Moves are decoded from their 16-bit encoding and replayed from the starting
// position to produce SAN. Any move that fails to decode produces "?" for that
// half-move; the loop continues so the caller always gets a complete string.
auto game_to_pgn(motif::db::game_id const game_id, motif::db::game const& game) -> std::string
{
    auto out = std::string {};

    auto append_tag = [&out](std::string_view key, std::string_view value) -> void { out += fmt::format("[{} \"{}\"]\n", key, value); };

    append_tag("Event", game.event_details ? game.event_details->name : "?");
    append_tag("Site", (game.event_details && game.event_details->site) ? *game.event_details->site : "?");
    append_tag("Date", game.date.value_or("????.??.??"));
    append_tag("Round", "?");
    append_tag("White", game.white.name);
    append_tag("Black", game.black.name);
    append_tag("Result", game.result);
    if (game.white.elo) {
        append_tag("WhiteElo", fmt::format("{}", *game.white.elo));
    }
    if (game.black.elo) {
        append_tag("BlackElo", fmt::format("{}", *game.black.elo));
    }
    if (game.white.title) {
        append_tag("WhiteTitle", *game.white.title);
    }
    if (game.black.title) {
        append_tag("BlackTitle", *game.black.title);
    }
    if (game.eco) {
        append_tag("ECO", *game.eco);
    }
    append_tag("MotifGameId", fmt::format("{}", game_id.value));
    for (auto const& [key, value] : game.extra_tags) {
        append_tag(key, value);
    }

    out += '\n';

    auto board = motif::chess::board {};
    auto move_number = std::uint32_t {1};
    bool white_to_move {true};
    bool first_token {true};

    auto append_token = [&out, &first_token](std::string_view token) -> void
    {
        if (!first_token) {
            out += ' ';
        }
        out += token;
        first_token = false;
    };

    for (auto const encoded : game.moves) {
        if (white_to_move) {
            append_token(fmt::format("{}.", move_number));
        }
        append_token(motif::chess::san(board, encoded));
        motif::chess::apply_encoded_move(board, encoded);
        if (white_to_move) {
            white_to_move = false;
        } else {
            white_to_move = true;
            ++move_number;
        }
    }

    if (!game.moves.empty()) {
        out += ' ';
    }
    out += game.result;
    out += '\n';

    return out;
}

// Replay all moves from the starting position and return one FEN, one SAN,
// and one Zobrist hash per ply. fens[i]/sans[i]/hashes[i] describe the
// position *after* ply i+1. starting_hash is the hash of the initial position
// before any moves are played. The initial FEN is omitted — the client already
// knows it. Moves that fail to decode produce "?" in sans; the board stays in
// its last valid state so subsequent entries remain consistent.
auto game_to_positions(motif::db::game const& game) -> detail::game_positions_response
{
    auto fens = std::vector<std::string> {};
    auto sans = std::vector<std::string> {};
    auto hashes = std::vector<std::string> {};
    fens.reserve(game.moves.size());
    sans.reserve(game.moves.size());
    hashes.reserve(game.moves.size());

    auto board = motif::chess::board {};
    auto const starting_hash = fmt::format("{}", board.hash());

    for (auto const encoded : game.moves) {
        sans.push_back(motif::chess::san(board, encoded));
        motif::chess::apply_encoded_move(board, encoded);
        fens.push_back(motif::chess::write_fen(board));
        hashes.push_back(fmt::format("{}", board.hash()));
    }

    return detail::game_positions_response {
        .starting_hash = starting_hash,
        .fens = std::move(fens),
        .sans = std::move(sans),
        .hashes = std::move(hashes),
    };
}

auto to_legal_move_response(motif::chess::move_info const& move) -> detail::legal_move_response
{
    return detail::legal_move_response {
        .uci = move.uci,
        .san = move.san,
        .from = move.from,
        .to = move.to,
        .promotion = move.promotion,
    };
}

void register_cors(httplib::Server& svr, std::vector<std::string> const& allowed_origins)
{
    svr.set_post_routing_handler(
        [allowed_origins](httplib::Request const& req, httplib::Response& res) -> void
        {
            if (allowed_origins.empty()) {
                res.set_header("Access-Control-Allow-Origin", "*");
            } else {
                auto const origin_header = req.get_header_value("Origin");
                if (!origin_header.empty()) {
                    auto const found = std::ranges::find(allowed_origins, origin_header);
                    if (found != allowed_origins.end()) {
                        res.set_header("Access-Control-Allow-Origin", origin_header);
                        res.set_header("Vary", "Origin");
                    }
                }
            }
            res.set_header("Access-Control-Allow-Methods", "GET, POST, PATCH, DELETE, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type");
        });

    svr.Options(".*", [](httplib::Request const& /*req*/, httplib::Response& res) -> void { res.status = http_ok; });
}

// Thread-safe event queue connecting ucilib callbacks (engine reader thread)
// to the httplib content provider (httplib thread pool thread).
// No glaze serialization needed — internal state only.
struct analysis_sse_session
{
    std::mutex queue_mutex;
    std::deque<std::string> event_queue;
    std::condition_variable cv;
    std::atomic<bool> terminal {false};
};

auto make_info_sse_event(motif::engine::info_event const& evt) -> std::string
{
    detail::sse_analysis_info_event payload {
        .depth = evt.depth,
        .seldepth = evt.seldepth,
        .multipv = evt.multipv,
        .score = {.type = evt.score.type, .value = evt.score.value},
        .pv_uci = evt.pv_uci,
        .pv_san = evt.pv_san,
        .nodes = evt.nodes,
        .nps = evt.nps,
        .time_ms = evt.time_ms,
    };
    std::string json;
    [[maybe_unused]] auto const err = glz::write_json(payload, json);
    return fmt::format("event: info\ndata: {}\n\n", json);
}

auto make_complete_sse_event(motif::engine::complete_event const& evt) -> std::string
{
    detail::sse_analysis_complete_event payload {
        .best_move_uci = evt.best_move_uci,
        .ponder_uci = evt.ponder_uci,
    };
    std::string json;
    [[maybe_unused]] auto const err = glz::write_json(payload, json);
    return fmt::format("event: complete\ndata: {}\n\n", json);
}

auto make_error_sse_event(motif::engine::error_event const& evt) -> std::string
{
    detail::sse_analysis_error_event payload {.message = evt.message};
    std::string json;
    [[maybe_unused]] auto const err = glz::write_json(payload, json);
    return fmt::format("event: error\ndata: {}\n\n", json);
}

}  // namespace

// server::impl is a private nested type — all route registration is done via
// the setup_routes() member function so no external code needs to name impl.
struct server::impl
{
    httplib::Server svr;
    motif::engine::engine_manager engine_mgr;
    std::mutex database_mutex;
    std::mutex sessions_mutex;
    gtl::flat_hash_map<std::string, std::shared_ptr<detail::import_session>> sessions;
    // Each entry pairs an import_id with its worker thread for lifecycle tracking.
    std::deque<std::pair<std::string, std::jthread>> import_workers;
    // Insertion-ordered list of completed import IDs, capped at max_completed_sessions.
    std::deque<std::string> completed_session_ids;
    std::mutex analyses_mutex;
    gtl::flat_hash_map<std::string, std::shared_ptr<analysis_sse_session>> analyses;
    std::atomic<bool> fail_next_import_worker_start_for_test {false};
    std::atomic<bool> fail_next_import_worker_run_for_test {false};
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    motif::db::database_manager& database;
    std::vector<std::string> allowed_origins;

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
    register_cors(svr, {});
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

    svr.Get("/api/positions/legal-moves",
            [](httplib::Request const& req, httplib::Response& res) -> void
            {
                auto const fen_param = req.get_param_value("fen");
                if (fen_param.empty()) {
                    set_json_error(res, http_bad_request, "invalid fen");
                    return;
                }

                auto board_result = motif::chess::parse_fen(fen_param);
                if (!board_result) {
                    set_json_error(res, http_bad_request, "invalid fen");
                    return;
                }

                auto const& board = *board_result;
                auto const moves = motif::chess::legal_moves(board);
                auto move_responses = std::vector<detail::legal_move_response> {};
                move_responses.reserve(moves.size());
                for (auto const& mov : moves) {
                    move_responses.push_back(to_legal_move_response(mov));
                }

                std::string body {};
                [[maybe_unused]] auto const err = glz::write_json(
                    detail::legal_moves_response {
                        .fen = motif::chess::write_fen(board),
                        .legal_moves = std::move(move_responses),
                    },
                    body);
                res.set_content(body, "application/json");
                res.status = http_ok;
            });

    svr.Post("/api/positions/apply-move",
             [](httplib::Request const& req, httplib::Response& res) -> void
             {
                 detail::apply_move_request req_body;
                 if (auto parse_err = glz::read_json(req_body, req.body); parse_err) {
                     set_json_error(res, http_bad_request, "invalid request body");
                     return;
                 }

                 if (req_body.fen.empty()) {
                     set_json_error(res, http_bad_request, "invalid fen");
                     return;
                 }
                 if (req_body.uci.empty()) {
                     set_json_error(res, http_bad_request, "invalid uci");
                     return;
                 }
                 if (!is_valid_uci_syntax(req_body.uci)) {
                     set_json_error(res, http_bad_request, "invalid uci");
                     return;
                 }

                 auto board_result = motif::chess::parse_fen(req_body.fen);
                 if (!board_result) {
                     set_json_error(res, http_bad_request, "invalid fen");
                     return;
                 }

                 auto board = std::move(*board_result);
                 auto move_result = motif::chess::apply_uci(board, req_body.uci);
                 if (!move_result) {
                     set_json_error(res, http_bad_request, "illegal move");
                     return;
                 }

                 auto const accepted_uci = move_result->uci;
                 auto const accepted_san = move_result->san;
                 auto const result_fen = motif::chess::write_fen(board);

                 std::string body {};
                 [[maybe_unused]] auto const err = glz::write_json(
                     detail::apply_move_response {
                         .uci = accepted_uci,
                         .san = accepted_san,
                         .fen = result_fen,
                     },
                     body);
                 res.set_content(body, "application/json");
                 res.status = http_ok;
             });

    svr.Get("/api/positions/hash",
            [](httplib::Request const& req, httplib::Response& res) -> void
            {
                auto const fen_param = req.get_param_value("fen");
                if (fen_param.empty()) {
                    set_json_error(res, http_bad_request, "invalid fen");
                    return;
                }
                auto board_result = motif::chess::parse_fen(fen_param);
                if (!board_result) {
                    set_json_error(res, http_bad_request, "invalid fen");
                    return;
                }
                std::string body {};
                [[maybe_unused]] auto const err =
                    glz::write_json(detail::position_hash_response {fmt::format("{}", board_result->hash())}, body);
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

                auto matches = [this, hash_val, limit, offset]() -> decltype(motif::search::position_search::find(
                                                                     database, motif::db::zobrist_hash {hash_val}, limit, offset))
                {
                    std::scoped_lock const lock {database_mutex};
                    return motif::search::position_search::find(database, motif::db::zobrist_hash {hash_val}, limit, offset);
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

                auto query_result =
                    [this, hash_val]() -> decltype(motif::search::opening_stats::query(database, motif::db::zobrist_hash {hash_val}))
                {
                    std::scoped_lock const lock {database_mutex};
                    return motif::search::opening_stats::query(database, motif::db::zobrist_hash {hash_val});
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

                std::optional<std::int32_t> min_elo;
                std::optional<std::int32_t> max_elo;
                if (req.has_param("min_elo")) {
                    min_elo = parse_int32(req.get_param_value("min_elo"));
                    if (!min_elo) {
                        set_json_error(res, http_bad_request, "invalid elo filter");
                        return;
                    }
                }
                if (req.has_param("max_elo")) {
                    max_elo = parse_int32(req.get_param_value("max_elo"));
                    if (!max_elo) {
                        set_json_error(res, http_bad_request, "invalid elo filter");
                        return;
                    }
                }

                auto to_filter = [](std::string value) -> std::optional<std::string>
                { return value.empty() ? std::nullopt : std::optional<std::string> {std::move(value)}; };
                auto query = motif::db::game_list_query {
                    .player = req.has_param("player") ? to_filter(req.get_param_value("player")) : std::nullopt,
                    .result = req.has_param("result") ? to_filter(req.get_param_value("result")) : std::nullopt,
                    .eco_prefix = req.has_param("eco") ? to_filter(req.get_param_value("eco")) : std::nullopt,
                    .date_from = req.has_param("date_from") ? to_filter(req.get_param_value("date_from")) : std::nullopt,
                    .date_to = req.has_param("date_to") ? to_filter(req.get_param_value("date_to")) : std::nullopt,
                    .min_elo = min_elo,
                    .max_elo = max_elo,
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

    svr.Get("/api/games/:id/pgn",
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

                res.set_content(game_to_pgn(*game_id, *game_result), "text/plain; charset=utf-8");
                res.status = http_ok;
            });

    svr.Get("/api/games/:id/positions",
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
                if (auto const err = glz::write_json(game_to_positions(*game_result), body); err) {
                    set_json_error(res, http_internal_error, "game retrieval failed");
                    return;
                }
                res.set_content(body, "application/json");
                res.status = http_ok;
            });

    svr.Post("/api/games",
             [this](httplib::Request const& req, httplib::Response& res) -> void
             {
                 detail::create_game_request req_body;
                 if (auto parse_err = glz::read_json(req_body, req.body); parse_err) {
                     set_json_error(res, http_bad_request, "invalid request body");
                     return;
                 }
                 if (req_body.pgn.empty()) {
                     set_json_error(res, http_bad_request, "pgn is required");
                     return;
                 }

                 auto const review_status = req_body.review_status.value_or(std::string {"new"});
                 if (!is_valid_review_status(review_status)) {
                     set_json_error(res, http_bad_request, "invalid review_status; allowed: new, needs_review, studied, archived");
                     return;
                 }

                 auto parse_result = pgn::parse_string(req_body.pgn);
                 if (!parse_result) {
                     set_json_error(res, http_bad_request, "pgn parse error: invalid PGN syntax");
                     return;
                 }
                 auto const& games = *parse_result;
                 if (games.empty()) {
                     set_json_error(res, http_bad_request, "no game found in PGN");
                     return;
                 }
                 if (games.size() > 1) {
                     set_json_error(res, http_bad_request, "only one game per request is supported");
                     return;
                 }

                 auto const& pgn_game = games.front();

                 auto game_id = motif::db::game_id {};
                 {
                     std::scoped_lock const lock {database_mutex};
                     motif::import::import_worker worker {database};
                     auto worker_result = worker.process(pgn_game);

                     if (!worker_result) {
                         if (worker_result.error() == motif::import::error_code::duplicate) {
                             set_json_error(res, http_conflict, "duplicate game");
                             return;
                         }
                         if (worker_result.error() == motif::import::error_code::parse_error) {
                             set_json_error(res, http_bad_request, "pgn parse error: invalid SAN move");
                             return;
                         }
                         if (worker_result.error() == motif::import::error_code::empty_game) {
                             set_json_error(res, http_bad_request, "game has no moves");
                             return;
                         }
                         set_json_error(res, http_internal_error, "game creation failed");
                         return;
                     }

                     game_id = worker_result->game_id;

                     // import_worker inserts with source_type='imported' (schema default);
                     // flip to 'manual' provenance now.
                     auto prov_res = database.store().set_manual_provenance(game_id, req_body.source_label, review_status);
                     if (!prov_res) {
                         static_cast<void>(database.positions().delete_by_game_id(game_id));
                         static_cast<void>(database.store().remove(game_id));
                         set_json_error(res, http_internal_error, "game creation failed");
                         return;
                     }
                 }

                 std::string body {};
                 [[maybe_unused]] auto const err = glz::write_json(
                     detail::create_game_response {
                         .id = game_id.value,
                         .source_type = "manual",
                         .source_label = req_body.source_label,
                         .review_status = review_status,
                     },
                     body);
                 res.set_content(body, "application/json");
                 res.status = http_created;
             });

    svr.Get("/api/games/count",
            [this](httplib::Request const& /*req*/, httplib::Response& res) -> void
            {
                auto count = [this]() -> decltype(database.store().count_games())
                {
                    std::scoped_lock const lock {database_mutex};
                    return database.store().count_games();
                }();
                if (!count) {
                    set_json_error(res, http_internal_error, "game count query failed");
                    return;
                }
                std::string body {};
                [[maybe_unused]] auto const err = glz::write_json(detail::game_count_response {*count}, body);
                res.set_content(body, "application/json");
                res.status = http_ok;
            });

    svr.Post("/api/positions/rebuild",
             [this](httplib::Request const& /*req*/, httplib::Response& res) -> void
             {
                 std::scoped_lock const lock {database_mutex};
                 auto rebuild_res = database.rebuild_position_store();
                 if (!rebuild_res) {
                     set_json_error(res, http_internal_error, "position store rebuild failed");
                     return;
                 }
                 res.status = http_no_content;
             });

    svr.Patch("/api/games/:id",
              [this](httplib::Request const& req, httplib::Response& res) -> void
              {
                  auto const& id_str = req.path_params.at("id");
                  auto const game_id = parse_game_id(id_str);
                  if (!game_id) {
                      set_json_error(res, http_bad_request, "invalid game id");
                      return;
                  }

                  motif::db::game_patch patch;
                  if (auto parse_err = glz::read_json(patch, req.body); parse_err) {
                      set_json_error(res, http_bad_request, "invalid request body");
                      return;
                  }

                  if (patch.result.has_value() && !is_valid_result(*patch.result)) {
                      set_json_error(res, http_bad_request, "invalid result; allowed: 1-0, 0-1, 1/2-1/2, *");
                      return;
                  }

                  if (patch.review_status.has_value() && !is_valid_review_status(*patch.review_status)) {
                      set_json_error(res, http_bad_request, "invalid review_status; allowed: new, needs_review, studied, archived");
                      return;
                  }

                  auto patch_result = [this, &game_id, &patch]() -> motif::db::result<void>
                  {
                      std::scoped_lock const lock {database_mutex};
                      return database.patch_game_metadata(*game_id, patch);
                  }();

                  if (!patch_result) {
                      if (patch_result.error() == motif::db::error_code::not_found) {
                          set_json_error(res, http_not_found, "not_found");
                          return;
                      }
                      if (patch_result.error() == motif::db::error_code::not_editable) {
                          set_json_error(res, http_conflict, "game is not user-added and cannot be modified");
                          return;
                      }
                      if (patch_result.error() == motif::db::error_code::duplicate) {
                          set_json_error(res, http_conflict, "patch would create a duplicate game");
                          return;
                      }
                      set_json_error(res, http_internal_error, "patch failed");
                      return;
                  }

                  auto game_result = [this, game_id]() -> decltype(database.store().get(*game_id))
                  {
                      std::scoped_lock const lock {database_mutex};
                      return database.store().get(*game_id);
                  }();
                  if (!game_result) {
                      set_json_error(res, http_internal_error, "game retrieval failed after patch");
                      return;
                  }

                  std::string body {};
                  if (auto const err = glz::write_json(to_game_response(*game_id, *game_result), body); err) {
                      set_json_error(res, http_internal_error, "game retrieval failed after patch");
                      return;
                  }
                  res.set_content(body, "application/json");
                  res.status = http_ok;
              });

    svr.Delete("/api/games/:id",
               [this](httplib::Request const& req, httplib::Response& res) -> void
               {
                   auto const& id_str = req.path_params.at("id");
                   auto const game_id = parse_game_id(id_str);
                   if (!game_id) {
                       set_json_error(res, http_bad_request, "invalid game id");
                       return;
                   }

                   {
                       std::scoped_lock const lock {database_mutex};
                       auto delete_result = database.store().remove_user_game(*game_id);

                       if (!delete_result) {
                           if (delete_result.error() == motif::db::error_code::not_found) {
                               set_json_error(res, http_not_found, "not_found");
                               return;
                           }
                           if (delete_result.error() == motif::db::error_code::not_editable) {
                               set_json_error(res, http_conflict, "game is not user-added and cannot be deleted");
                               return;
                           }
                           set_json_error(res, http_internal_error, "delete failed");
                           return;
                       }

                       // Remove deleted game's position rows from DuckDB.
                       // position_store has no bulk delete API; delete_by_game_id
                       // is cheaper than a full rebuild for a single game.
                       static_cast<void>(database.positions().delete_by_game_id(*game_id));
                   }

                   res.status = http_no_content;
               });

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

    // POST /api/engine/engines — register or overwrite an engine configuration.
    svr.Post("/api/engine/engines",
             [this](httplib::Request const& req, httplib::Response& res) -> void
             {
                 detail::configure_engine_request req_body;
                 if (auto parse_err = glz::read_json(req_body, req.body); parse_err) {
                     set_json_error(res, http_bad_request, "invalid request body");
                     return;
                 }
                 if (req_body.name.empty() || req_body.path.empty()) {
                     set_json_error(res, http_bad_request, "name and path are required");
                     return;
                 }
                 auto result = engine_mgr.configure_engine({.name = req_body.name, .path = req_body.path});
                 if (!result) {
                     set_json_error(res, http_bad_request, "engine configuration failed");
                     return;
                 }
                 res.set_content(R"({"status":"ok"})", "application/json");
                 res.status = http_ok;
             });

    // GET /api/engine/engines — list all registered engines.
    svr.Get("/api/engine/engines",
            [this](httplib::Request const& /*req*/, httplib::Response& res) -> void
            {
                auto const configs = engine_mgr.list_engines();
                auto entries = std::vector<detail::engine_list_entry> {};
                entries.reserve(configs.size());
                for (auto const& cfg : configs) {
                    entries.push_back({.name = cfg.name, .path = cfg.path});
                }
                std::string body;
                [[maybe_unused]] auto const err = glz::write_json(detail::engine_list_response {std::move(entries)}, body);
                res.set_content(body, "application/json");
                res.status = http_ok;
            });

    // Engine analysis routes — exact path before parameterized paths.
    svr.Post("/api/engine/analyses",
             [this](httplib::Request const& req, httplib::Response& res) -> void
             {
                 detail::start_analysis_request req_body;
                 if (auto parse_err = glz::read_json(req_body, req.body); parse_err) {
                     set_json_error(res, http_bad_request, "invalid request body");
                     return;
                 }

                 if (req_body.fen.empty()) {
                     set_json_error(res, http_bad_request, "missing fen");
                     return;
                 }

                 if (!motif::chess::parse_fen(req_body.fen)) {
                     set_json_error(res, http_bad_request, "invalid fen");
                     return;
                 }

                 auto const has_depth = req_body.depth.has_value();
                 auto const has_movetime = req_body.movetime_ms.has_value();
                 if (has_depth == has_movetime) {
                     set_json_error(res, http_bad_request, "provide exactly one of depth or movetime_ms");
                     return;
                 }

                 constexpr int multipv_min {1};
                 constexpr int multipv_max {5};
                 auto const multipv = req_body.multipv.value_or(1);
                 if (multipv < multipv_min || multipv > multipv_max) {
                     set_json_error(res, http_bad_request, "multipv must be between 1 and 5");
                     return;
                 }

                 constexpr int depth_min {1};
                 constexpr int depth_max {100};
                 if (has_depth && (*req_body.depth < depth_min || *req_body.depth > depth_max)) {
                     set_json_error(res, http_bad_request, "depth must be between 1 and 100");
                     return;
                 }

                 constexpr int movetime_min {1};
                 constexpr int movetime_max {300000};
                 if (has_movetime && (*req_body.movetime_ms < movetime_min || *req_body.movetime_ms > movetime_max)) {
                     set_json_error(res, http_bad_request, "movetime_ms must be between 1 and 300000");
                     return;
                 }

                 const motif::engine::analysis_params params {
                     .fen = req_body.fen,
                     .engine = req_body.engine,
                     .multipv = multipv,
                     .depth = req_body.depth,
                     .movetime_ms = req_body.movetime_ms,
                 };

                 auto analysis_result = engine_mgr.start_analysis(params);
                 if (!analysis_result) {
                     switch (analysis_result.error().code) {
                         case motif::engine::error_code::engine_not_configured:
                             set_json_error(res, http_service_unavailable, "no engine configured");
                             return;
                         case motif::engine::error_code::invalid_analysis_params:
                             set_json_error(res, http_bad_request, "invalid analysis params");
                             return;
                         default:
                             set_json_error(res, http_service_unavailable, "engine failed to start");
                             return;
                     }
                 }

                 auto const& analysis_id = *analysis_result;
                 auto sse_sess = std::make_shared<analysis_sse_session>();

                 auto push_event = [sse_sess](std::string msg) -> void
                 {
                     {
                         std::scoped_lock const q_lock {sse_sess->queue_mutex};
                         sse_sess->event_queue.push_back(std::move(msg));
                     }
                     sse_sess->cv.notify_one();
                 };

                 // Subscribe immediately so no events are lost between start and stream.
                 static_cast<void>(engine_mgr.subscribe(
                     analysis_id,
                     [push_event](motif::engine::info_event const& evt) -> void { push_event(make_info_sse_event(evt)); },
                     [sse_sess, push_event](motif::engine::complete_event const& evt) -> void
                     {
                         push_event(make_complete_sse_event(evt));
                         sse_sess->terminal.store(true, std::memory_order_relaxed);
                         sse_sess->cv.notify_all();
                     },
                     [sse_sess, push_event](motif::engine::error_event const& evt) -> void
                     {
                         push_event(make_error_sse_event(evt));
                         sse_sess->terminal.store(true, std::memory_order_relaxed);
                         sse_sess->cv.notify_all();
                     }));

                 {
                     std::scoped_lock const lock {analyses_mutex};
                     analyses.emplace(analysis_id, sse_sess);
                 }

                 std::string body {};
                 [[maybe_unused]] auto const err = glz::write_json(detail::start_analysis_response {analysis_id}, body);
                 res.set_content(body, "application/json");
                 res.status = http_accepted;
             });

    // GET /api/engine/analyses/:analysis_id/stream — SSE stream for analysis events.
    svr.Get("/api/engine/analyses/:analysis_id/stream",
            [this](httplib::Request const& req, httplib::Response& res) -> void
            {
                auto const& analysis_id = req.path_params.at("analysis_id");
                std::shared_ptr<analysis_sse_session> sse_sess;
                {
                    std::scoped_lock const lock {analyses_mutex};
                    auto const analysis_it = analyses.find(analysis_id);
                    if (analysis_it == analyses.end()) {
                        set_json_error(res, http_not_found, "analysis not found");
                        return;
                    }
                    sse_sess = analysis_it->second;
                }

                res.set_chunked_content_provider("text/event-stream",
                                                 [sse_sess](size_t /*offset*/, httplib::DataSink& sink) -> bool
                                                 {
                                                     std::vector<std::string> pending;
                                                     bool is_terminal = false;
                                                     {
                                                         std::unique_lock<std::mutex> lock {sse_sess->queue_mutex};
                                                         constexpr auto sse_poll_interval_ms = std::chrono::milliseconds {250};
                                                         sse_sess->cv.wait_for(
                                                             lock,
                                                             sse_poll_interval_ms,
                                                             [&sse_sess]() -> bool
                                                             { return !sse_sess->event_queue.empty() || sse_sess->terminal.load(); });
                                                         while (!sse_sess->event_queue.empty()) {
                                                             pending.push_back(std::move(sse_sess->event_queue.front()));
                                                             sse_sess->event_queue.pop_front();
                                                         }
                                                         is_terminal = sse_sess->terminal.load();
                                                     }
                                                     for (auto const& evt_str : pending) {
                                                         if (!sink.write(evt_str.data(), evt_str.size())) {
                                                             return false;
                                                         }
                                                     }
                                                     if (is_terminal && pending.empty()) {
                                                         sink.done();
                                                         return false;
                                                     }
                                                     return !is_terminal;
                                                 });
            });

    // DELETE /api/engine/analyses/:analysis_id — stop an active analysis session.
    svr.Delete("/api/engine/analyses/:analysis_id",
               [this](httplib::Request const& req, httplib::Response& res) -> void
               {
                   auto const& analysis_id = req.path_params.at("analysis_id");
                   auto stop_result = engine_mgr.stop_analysis(analysis_id);
                   if (!stop_result) {
                       switch (stop_result.error().code) {
                           case motif::engine::error_code::analysis_not_found:
                               set_json_error(res, http_not_found, "analysis not found");
                               return;
                           case motif::engine::error_code::analysis_already_terminal:
                               set_json_error(res, http_conflict, "analysis already terminal");
                               return;
                           default:
                               set_json_error(res, http_internal_error, "engine stop failed");
                               return;
                       }
                   }
                   res.status = http_no_content;
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

    svr.Post("/api/imports/upload",
             [this](httplib::Request const& req, httplib::Response& res) -> void
             {
                 auto const file_it = req.form.files.find("file");
                 if (file_it == req.form.files.end()) {
                     set_json_error(res, http_bad_request, "missing file field");
                     return;
                 }
                 auto const& file_content = file_it->second.content;

                 std::string import_id;
                 std::shared_ptr<detail::import_session> session;
                 std::filesystem::path temp_path;
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
                 temp_path = std::filesystem::temp_directory_path() / fmt::format("motif_upload_{}.pgn", import_id);
                 {
                     std::ofstream tmp {temp_path, std::ios::binary};
                     if (!tmp.is_open()) {
                         std::scoped_lock const lock {sessions_mutex};
                         sessions.erase(import_id);
                         set_json_error(res, http_internal_error, "failed to create temporary file");
                         return;
                     }
                     tmp.write(file_content.data(), static_cast<std::streamsize>(file_content.size()));
                     if (tmp.fail()) {
                         std::error_code remove_err {};
                         std::filesystem::remove(temp_path, remove_err);
                         std::scoped_lock const lock {sessions_mutex};
                         sessions.erase(import_id);
                         set_json_error(res, http_internal_error, "failed to write temporary file");
                         return;
                     }
                 }

                 try {
                     auto worker = std::jthread {[session, pgn_path = temp_path]() -> void
                                                 {
                                                     try {
                                                         auto result = session->pipeline->run(pgn_path, {});
                                                         if (result) {
                                                             session->summary = *result;
                                                         } else {
                                                             session->error_message = fmt::format(R"(import failed: "{}")",
                                                                                                  motif::import::to_string(result.error()));
                                                             session->failed.store(true, std::memory_order_release);
                                                         }
                                                     } catch (...) {
                                                         std::error_code remove_err {};
                                                         std::filesystem::remove(pgn_path, remove_err);
                                                         session->error_message = "import failed: unexpected error";
                                                         session->failed.store(true, std::memory_order_release);
                                                         session->done.store(true, std::memory_order_release);
                                                         session->cv.notify_all();
                                                         return;
                                                     }
                                                     std::error_code remove_err {};
                                                     std::filesystem::remove(pgn_path, remove_err);
                                                     session->done.store(true, std::memory_order_release);
                                                     session->cv.notify_all();
                                                 }};
                     {
                         std::scoped_lock const lock {sessions_mutex};
                         import_workers.emplace_back(import_id, std::move(worker));
                     }
                 } catch (std::system_error const&) {
                     std::error_code remove_err {};
                     std::filesystem::remove(temp_path, remove_err);
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
                        auto const phase_str = [](motif::import::import_phase phase) -> std::string_view
                        {
                            switch (phase) {
                                case motif::import::import_phase::ingesting:
                                    return "ingesting";
                                case motif::import::import_phase::rebuilding:
                                    return "rebuilding";
                                default:
                                    return "idle";
                            }
                        }(prog.phase);
                        auto event = fmt::format(
                            "data: "
                            "{{\"games_processed\":{},\"games_committed\":{},\"games_skipped\":{},\"elapsed_seconds\":{:.3f},\"phase\":\"{}"
                            "\"}}\n\n",
                            prog.games_processed,
                            prog.games_committed,
                            prog.games_skipped,
                            elapsed,
                            phase_str);
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

auto server::start(std::string const& host, std::uint16_t const port, std::vector<std::string> const& allowed_origins) -> result<void>
{
    impl_->allowed_origins = allowed_origins;
    // Re-register CORS with the configured origins.
    register_cors(impl_->svr, allowed_origins);
    if (!impl_->svr.listen(host, port)) {
        return tl::unexpected {error_code::listen_failed};
    }
    return {};
}

auto server::start(std::uint16_t const port) -> result<void>
{
    return start(std::string {default_host}, port, {});
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
