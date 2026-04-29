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
#include <unordered_map>
#include <utility>
#include <vector>

#include "motif/http/server.hpp"

#include <chesslib/board/board.hpp>
#include <chesslib/board/move_generator.hpp>
#include <chesslib/core/types.hpp>
#include <chesslib/util/fen.hpp>
#include <chesslib/util/san.hpp>
#include <chesslib/util/uci.hpp>
#include <fmt/format.h>
#include <glaze/json/read.hpp>
#include <glaze/json/write.hpp>
#include <httplib.h>
#include <pgnlib/pgnlib.hpp>
#include <tl/expected.hpp>

#include "motif/db/database_manager.hpp"
#include "motif/db/error.hpp"
#include "motif/db/move_codec.hpp"
#include "motif/db/types.hpp"
#include "motif/http/error.hpp"
#include "motif/import/error.hpp"
#include "motif/import/import_pipeline.hpp"
#include "motif/import/import_worker.hpp"
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

struct game_provenance_response
{
    std::string source_type;
    std::optional<std::string> source_label;
    std::string review_status;
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
    game_provenance_response provenance;
};

struct game_list_entry_response
{
    std::uint32_t id {};
    std::string white;
    std::string black;
    std::string result;
    std::string event;
    std::string date;
    std::string eco;
    std::string source_type;
    std::optional<std::string> source_label;
    std::string review_status;
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

struct patch_game_request
{
    std::optional<std::string> white_name;
    std::optional<std::int32_t> white_elo;
    std::optional<std::string> black_name;
    std::optional<std::int32_t> black_elo;
    std::optional<std::string> event;
    std::optional<std::string> site;
    std::optional<std::string> date;
    std::optional<std::string> result;
    std::optional<std::string> eco;
    std::optional<std::string> source_label;
    std::optional<std::string> review_status;
    std::optional<std::string> notes;
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
constexpr int http_not_implemented {501};
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
        .provenance =
            detail::game_provenance_response {
                .source_type = source.provenance.source_type,
                .source_label = source.provenance.source_label,
                .review_status = source.provenance.review_status,
            },
    };
}

auto to_game_list_entry_response(motif::db::game_list_entry const& src) -> detail::game_list_entry_response
{
    return detail::game_list_entry_response {
        .id = src.id,
        .white = src.white,
        .black = src.black,
        .result = src.result,
        .event = src.event,
        .date = src.date,
        .eco = src.eco,
        .source_type = src.source_type,
        .source_label = src.source_label,
        .review_status = src.review_status,
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

auto generate_analysis_id() -> std::string
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

// Extract "from" and "to" square names from a UCI string (e.g. "e2e4" → "e2", "e4").
// Returns nullopt if the UCI string is too short to be valid.
auto uci_squares(std::string const& uci_str) -> std::optional<std::pair<std::string, std::string>>
{
    if (uci_str.size() < 4) {
        return std::nullopt;
    }
    return std::pair<std::string, std::string> {uci_str.substr(0, 2), uci_str.substr(2, 2)};
}

// Extract promotion character from UCI string if present (5th char: q/r/b/n).
auto uci_promotion(std::string const& uci_str) -> std::optional<std::string>
{
    constexpr std::size_t uci_promotion_length {5};
    constexpr std::size_t uci_promotion_index {4};
    if (uci_str.size() >= uci_promotion_length) {
        return std::string {uci_str[uci_promotion_index]};
    }
    return std::nullopt;
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
auto game_to_pgn(std::uint32_t const game_id, motif::db::game const& game) -> std::string
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
    append_tag("MotifGameId", fmt::format("{}", game_id));
    for (auto const& [key, value] : game.extra_tags) {
        append_tag(key, value);
    }

    out += '\n';

    auto board = chesslib::board {};
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
        auto move_result = motif::db::decode_move(encoded);
        if (white_to_move) {
            append_token(fmt::format("{}.", move_number));
        }
        if (move_result) {
            append_token(chesslib::san::to_string(board, *move_result));
            chesslib::move_maker {board, *move_result}.make();
        } else {
            append_token("?");
        }
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

    auto board = chesslib::board {};
    auto const starting_hash = fmt::format("{}", board.hash());

    for (auto const encoded : game.moves) {
        auto move_result = motif::db::decode_move(encoded);
        if (move_result) {
            sans.push_back(chesslib::san::to_string(board, *move_result));
            chesslib::move_maker {board, *move_result}.make();
        } else {
            sans.push_back("?");
        }
        fens.push_back(chesslib::fen::write(board));
        hashes.push_back(fmt::format("{}", board.hash()));
    }

    return detail::game_positions_response {
        .starting_hash = starting_hash,
        .fens = std::move(fens),
        .sans = std::move(sans),
        .hashes = std::move(hashes),
    };
}

auto to_legal_move_response(chesslib::board const& board, chesslib::move const mov) -> detail::legal_move_response
{
    auto const uci_str = chesslib::uci::to_string(mov);
    auto const san_str = chesslib::san::to_string(board, mov);
    auto const squares = uci_squares(uci_str);
    auto const from_sq = squares ? squares->first : std::string {};
    auto const to_sq = squares ? squares->second : std::string {};
    return detail::legal_move_response {
        .uci = uci_str,
        .san = san_str,
        .from = from_sq,
        .to = to_sq,
        .promotion = uci_promotion(uci_str),
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

                auto board_result = chesslib::fen::read(fen_param);
                if (!board_result) {
                    set_json_error(res, http_bad_request, "invalid fen");
                    return;
                }

                auto const& board = *board_result;
                auto const moves = chesslib::legal_moves(board);
                auto move_responses = std::vector<detail::legal_move_response> {};
                move_responses.reserve(moves.size());
                for (auto const& mov : moves) {
                    move_responses.push_back(to_legal_move_response(board, mov));
                }

                std::string body {};
                [[maybe_unused]] auto const err = glz::write_json(
                    detail::legal_moves_response {
                        .fen = chesslib::fen::write(board),
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

                 auto board_result = chesslib::fen::read(req_body.fen);
                 if (!board_result) {
                     set_json_error(res, http_bad_request, "invalid fen");
                     return;
                 }

                 auto& board = *board_result;
                 auto move_result = chesslib::uci::from_string(board, req_body.uci);
                 if (!move_result) {
                     set_json_error(res, http_bad_request, "illegal move");
                     return;
                 }

                 auto const accepted_uci = chesslib::uci::to_string(*move_result);
                 auto const accepted_san = chesslib::san::to_string(board, *move_result);
                 chesslib::move_maker {board, *move_result}.make();
                 auto const result_fen = chesslib::fen::write(board);

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
                auto board_result = chesslib::fen::read(fen_param);
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

                auto responses = std::vector<detail::game_list_entry_response> {};
                responses.reserve(games->size());
                for (auto const& entry : *games) {
                    responses.push_back(to_game_list_entry_response(entry));
                }

                std::string body {};
                [[maybe_unused]] auto const err = glz::write_json(responses, body);
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

                 std::uint32_t game_id {};
                 {
                     std::scoped_lock const lock {database_mutex};
                     motif::import::import_worker worker {database.store(), database.positions()};
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
                         set_json_error(res, http_internal_error, "game creation failed");
                         return;
                     }

                     game_id = worker_result->game_id;

                     // import_worker inserts with source_type='imported' (schema default);
                     // flip to 'manual' provenance now.
                     auto prov_res = database.store().set_manual_provenance(game_id, req_body.source_label, review_status);
                     if (!prov_res) {
                         static_cast<void>(database.store().remove(game_id));
                         set_json_error(res, http_internal_error, "game creation failed");
                         return;
                     }
                 }

                 std::string body {};
                 [[maybe_unused]] auto const err = glz::write_json(
                     detail::create_game_response {
                         .id = game_id,
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

    svr.Patch("/api/games/:id",
              [this](httplib::Request const& req, httplib::Response& res) -> void
              {
                  auto const& id_str = req.path_params.at("id");
                  auto const game_id = parse_game_id(id_str);
                  if (!game_id) {
                      set_json_error(res, http_bad_request, "invalid game id");
                      return;
                  }

                  detail::patch_game_request req_body;
                  if (auto parse_err = glz::read_json(req_body, req.body); parse_err) {
                      set_json_error(res, http_bad_request, "invalid request body");
                      return;
                  }

                  if (req_body.result.has_value() && !is_valid_result(*req_body.result)) {
                      set_json_error(res, http_bad_request, "invalid result; allowed: 1-0, 0-1, 1/2-1/2, *");
                      return;
                  }

                  if (req_body.review_status.has_value() && !is_valid_review_status(*req_body.review_status)) {
                      set_json_error(res, http_bad_request, "invalid review_status; allowed: new, needs_review, studied, archived");
                      return;
                  }

                  auto db_patch = motif::db::game_patch {
                      .white_name = req_body.white_name,
                      .white_elo = req_body.white_elo,
                      .black_name = req_body.black_name,
                      .black_elo = req_body.black_elo,
                      .event = req_body.event,
                      .site = req_body.site,
                      .date = req_body.date,
                      .result = req_body.result,
                      .eco = req_body.eco,
                      .source_label = req_body.source_label,
                      .review_status = req_body.review_status,
                      .notes = req_body.notes,
                  };

                  auto patch_result = [this, &game_id, &db_patch]() -> motif::db::result<void>
                  {
                      std::scoped_lock const lock {database_mutex};
                      return database.store().patch_metadata(*game_id, db_patch);
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

                       // Rebuild DuckDB position store to remove deleted game's position rows.
                       // position_store has no delete-by-game-id API; rebuild is the safe path (NFR09).
                       static_cast<void>(database.rebuild_position_store(/*sort_by_zobrist=*/false));
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

    // Engine analysis routes — exact path before parameterized paths.
    svr.Post("/api/engine/analyses",
             [](httplib::Request const& req, httplib::Response& res) -> void
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

                 if (!chesslib::fen::read(req_body.fen)) {
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

                 auto const analysis_id = generate_analysis_id();
                 std::string body {};
                 [[maybe_unused]] auto const err = glz::write_json(detail::start_analysis_response {analysis_id}, body);
                 res.set_content(body, "application/json");
                 res.status = http_accepted;
             });

    // GET /api/engine/analyses/:analysis_id/stream — SSE body is Phase 2 work.
    svr.Get("/api/engine/analyses/:analysis_id/stream",
            [](httplib::Request const& /*req*/, httplib::Response& res) -> void
            { set_json_error(res, http_not_implemented, "engine analysis not yet implemented"); });

    // DELETE /api/engine/analyses/:analysis_id — stop logic is Phase 2 work.
    svr.Delete("/api/engine/analyses/:analysis_id",
               [](httplib::Request const& /*req*/, httplib::Response& res) -> void
               { set_json_error(res, http_not_implemented, "engine analysis not yet implemented"); });

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
                        auto const phase_str = [](motif::import::import_phase p) -> std::string_view
                        {
                            switch (p) {
                                case motif::import::import_phase::ingesting:
                                    return "ingesting";
                                case motif::import::import_phase::rebuilding:
                                    return "rebuilding";
                                default:
                                    return "idle";
                            }
                        }(prog.phase);
                        auto event = fmt::format(
                            "data: {{\"games_processed\":{},\"games_committed\":{},\"games_skipped\":{},\"elapsed_seconds\":{:.3f},\"phase\":\"{}\"}}\n\n",
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
