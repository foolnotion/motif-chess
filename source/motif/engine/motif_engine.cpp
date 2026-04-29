#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <chesslib/board/board.hpp>
#include <chesslib/util/fen.hpp>
#include <chesslib/util/san.hpp>
#include <chesslib/util/uci.hpp>
#include <fmt/base.h>
#include <fmt/format.h>
#include <tl/expected.hpp>
#include <ucilib/engine.hpp>
#include <ucilib/types.hpp>

#include "motif/engine/engine_manager.hpp"
#include "motif/engine/error.hpp"

namespace motif::engine
{

namespace
{

constexpr auto startpos_fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

auto generate_analysis_id() -> std::string
{
    static thread_local std::mt19937_64 rng {std::random_device {}()};
    std::uniform_int_distribution<std::uint64_t> dist;
    return fmt::format("{:016x}{:016x}", dist(rng), dist(rng));
}

auto map_info(uci::info const& info, std::string const& start_fen) -> info_event
{
    info_event event;
    event.depth = info.depth.value_or(0);
    event.seldepth = info.seldepth;
    event.multipv = info.multipv.value_or(1);

    if (info.score.has_value()) {
        event.score.type = info.score->type == uci::score_type::cp ? "cp" : "mate";
        event.score.value = info.score->value;
    }

    event.pv_uci = info.pv;

    if (!info.pv.empty()) {
        auto san = pv_to_san(start_fen, info.pv);
        if (!san.empty()) {
            event.pv_san = std::move(san);
        }
    }

    event.nodes = info.nodes;
    event.nps = info.nps;
    event.time_ms = info.time_ms;

    return event;
}

}  // namespace

// ---------------------------------------------------------------------------
// pv_to_san — public free function (declared in engine_manager.hpp)
// ---------------------------------------------------------------------------

auto pv_to_san(std::string_view start_fen, std::vector<std::string> const& pv_uci) -> std::vector<std::string>
{
    auto board_result = chesslib::fen::read(start_fen);
    if (!board_result) {
        return {};
    }
    auto board = *board_result;
    std::vector<std::string> san_list;
    for (auto const& uci_str : pv_uci) {
        auto move_result = chesslib::uci::from_string(board, uci_str);
        if (!move_result) {
            break;
        }
        san_list.push_back(chesslib::san::to_string(board, *move_result));
        chesslib::move_maker {board, *move_result}.make();
    }
    return san_list;
}

// ---------------------------------------------------------------------------
// session — private to this translation unit
// ---------------------------------------------------------------------------

struct session
{
    std::string analysis_id;
    std::string start_fen;
    analysis_params params;
    std::atomic<bool> terminal {false};
    std::mutex callback_mutex;
    info_callback on_info;
    complete_callback on_complete;
    error_callback on_error;
    std::optional<complete_event> last_complete;
    // Buffered info events so subscribe() can replay them to late subscribers.
    std::vector<info_event> info_history;
    // Declared last so its destructor (which joins the ucilib reader thread)
    // runs before the callback members above are destroyed.
    uci::engine engine;
};

// ---------------------------------------------------------------------------
// engine_manager::impl
// ---------------------------------------------------------------------------

struct engine_manager::impl
{
    std::mutex mutex;
    std::unordered_map<std::string, engine_config> engines;
    std::vector<std::string> engine_order;
    std::unordered_map<std::string, std::unique_ptr<session>> sessions;
};

// ---------------------------------------------------------------------------
// Internal helpers (free functions; avoid referencing engine_manager::impl
// by name to stay within language access rules)
// ---------------------------------------------------------------------------

namespace
{

auto validate_params(analysis_params const& params) -> tl::expected<void, error_code>
{
    if (params.multipv < 1) {
        return tl::unexpected {error_code::invalid_analysis_params};
    }
    if (params.depth.has_value() == params.movetime_ms.has_value()) {
        return tl::unexpected {error_code::invalid_analysis_params};
    }
    if (params.depth.has_value() && *params.depth < 1) {
        return tl::unexpected {error_code::invalid_analysis_params};
    }
    if (params.movetime_ms.has_value() && *params.movetime_ms < 1) {
        return tl::unexpected {error_code::invalid_analysis_params};
    }
    return {};
}

auto resolve_engine_config(std::unordered_map<std::string, engine_config> const& engines,
                           std::vector<std::string> const& engine_order,
                           std::string const& engine_name) -> tl::expected<engine_config, error_code>
{
    if (engines.empty()) {
        return tl::unexpected {error_code::engine_not_configured};
    }
    if (engine_name.empty()) {
        return engines.at(engine_order.front());
    }
    auto iter = engines.find(engine_name);
    if (iter == engines.end()) {
        return tl::unexpected {error_code::engine_not_configured};
    }
    return iter->second;
}

auto launch_engine(session& sess, engine_config const& cfg) -> tl::expected<void, error_code>
{
    if (!sess.engine.start(cfg.path)) {
        return tl::unexpected {error_code::engine_start_failed};
    }
    if (!sess.engine.is_ready()) {
        return tl::unexpected {error_code::engine_start_failed};
    }
    return {};
}

auto configure_position(session& sess, analysis_params const& params) -> tl::expected<void, error_code>
{
    if (params.multipv > 1) {
        if (!sess.engine.set_option("MultiPV", fmt::format("{}", params.multipv))) {
            return tl::unexpected {error_code::engine_start_failed};
        }
    }
    if (sess.start_fen == startpos_fen) {
        if (!sess.engine.set_position_startpos()) {
            return tl::unexpected {error_code::engine_start_failed};
        }
    } else {
        if (!sess.engine.set_position(sess.start_fen)) {
            return tl::unexpected {error_code::engine_start_failed};
        }
    }
    return {};
}

auto make_go_params(analysis_params const& params) -> uci::go_params
{
    uci::go_params go_cmd;
    if (params.depth.has_value()) {
        go_cmd.depth = params.depth;
    } else if (params.movetime_ms.has_value()) {
        go_cmd.movetime = uci::milliseconds {*params.movetime_ms};
    }
    return go_cmd;
}

auto reserve_analysis_id(std::unordered_map<std::string, std::unique_ptr<session>> const& sessions) -> std::string
{
    auto analysis_id = generate_analysis_id();
    while (sessions.contains(analysis_id)) {
        analysis_id = generate_analysis_id();
    }
    return analysis_id;
}

void register_callbacks(session* sess_ptr)
{
    sess_ptr->engine.on_info(
        [sess_ptr](uci::info const& info) -> void
        {
            auto event = map_info(info, sess_ptr->start_fen);
            info_callback callback;
            {
                const std::scoped_lock cb_lock {sess_ptr->callback_mutex};
                sess_ptr->info_history.push_back(event);
                callback = sess_ptr->on_info;
            }
            if (callback) {
                try {
                    callback(event);
                } catch (...) {
                    fmt::print(stderr, "engine_manager: on_info callback threw\n");
                }
            }
        });

    sess_ptr->engine.on_bestmove(
        [sess_ptr](uci::best_move const& best_mv) -> void
        {
            const complete_event event {
                .best_move_uci = best_mv.move,
                .ponder_uci = best_mv.ponder,
            };
            sess_ptr->terminal.store(true, std::memory_order_relaxed);
            complete_callback callback;
            {
                const std::scoped_lock cb_lock {sess_ptr->callback_mutex};
                sess_ptr->last_complete = event;
                callback = sess_ptr->on_complete;
            }
            if (callback) {
                try {
                    callback(event);
                } catch (...) {
                    fmt::print(stderr, "engine_manager: on_complete callback threw\n");
                }
            }
        });

    sess_ptr->engine.on_error(
        [sess_ptr](std::error_code err) -> void
        {
            error_callback callback;
            {
                const std::scoped_lock cb_lock {sess_ptr->callback_mutex};
                if (!sess_ptr->terminal.exchange(true, std::memory_order_relaxed)) {
                    callback = sess_ptr->on_error;
                }
            }
            if (callback) {
                try {
                    callback(error_event {.message = fmt::format("engine crashed: {}", err.message())});
                } catch (...) {
                    fmt::print(stderr, "engine_manager: on_error callback threw\n");
                }
            }
        });
}

}  // namespace

// ---------------------------------------------------------------------------
// engine_manager
// ---------------------------------------------------------------------------

engine_manager::engine_manager()
    : impl_ {std::make_unique<impl>()}
{
}

engine_manager::~engine_manager()
{
    if (!impl_) {
        return;
    }
    // Explicitly quit all sessions before they are destroyed.
    // ucilib's impl::~impl() explicitly calls std::terminate() if the reader
    // thread is still joinable when the impl destructs. In the normal flow,
    // engine::quit() joins the reader thread. For crashed engines (where the
    // process has already died), quit() still needs to be called so that the
    // reader thread is joined — even though writing "quit\n" to the dead process
    // will fail, reproc::process::stop() handles the cleanup and the thread join
    // proceeds normally.
    //
    // We move sessions out of the map and quit them outside the lock to avoid
    // any lock-order issues with callbacks that might try to acquire impl_->mutex.
    std::unordered_map<std::string, std::unique_ptr<session>> sessions_to_quit;
    {
        const std::scoped_lock lock {impl_->mutex};
        sessions_to_quit = std::move(impl_->sessions);
    }
    for (auto& [id, sess] : sessions_to_quit) {
        static_cast<void>(id);
        static_cast<void>(sess->engine.quit());
    }
    // sessions_to_quit destructs here — engines have been quit, so ~engine() is safe.
}

auto engine_manager::configure_engine(engine_config cfg) -> tl::expected<void, error_code>
{
    if (cfg.path.empty()) {
        return tl::unexpected {error_code::engine_not_configured};
    }
    const std::scoped_lock lock {impl_->mutex};
    if (!impl_->engines.contains(cfg.name)) {
        impl_->engine_order.push_back(cfg.name);
    }
    impl_->engines.insert_or_assign(cfg.name, std::move(cfg));
    return {};
}

auto engine_manager::list_engines() -> std::vector<engine_config>
{
    const std::scoped_lock lock {impl_->mutex};
    std::vector<engine_config> result;
    result.reserve(impl_->engines.size());
    for (auto const& name : impl_->engine_order) {
        result.push_back(impl_->engines.at(name));
    }
    return result;
}

auto engine_manager::start_analysis(analysis_params const& params) -> tl::expected<std::string, error_code>
{
    if (auto params_result = validate_params(params); !params_result) {
        return tl::unexpected {params_result.error()};
    }

    engine_config cfg;
    std::string new_id;
    {
        const std::scoped_lock lock {impl_->mutex};
        auto cfg_result = resolve_engine_config(impl_->engines, impl_->engine_order, params.engine);
        if (!cfg_result) {
            return tl::unexpected {cfg_result.error()};
        }
        cfg = *cfg_result;
        new_id = reserve_analysis_id(impl_->sessions);
    }

    auto sess = std::make_unique<session>();
    sess->analysis_id = new_id;
    sess->start_fen = params.fen.empty() ? std::string {startpos_fen} : params.fen;
    sess->params = params;

    if (auto launch_result = launch_engine(*sess, cfg); !launch_result) {
        return tl::unexpected {launch_result.error()};
    }
    if (auto pos_result = configure_position(*sess, params); !pos_result) {
        return tl::unexpected {pos_result.error()};
    }

    register_callbacks(sess.get());
    auto* sess_ptr = sess.get();

    {
        const std::scoped_lock lock {impl_->mutex};
        if (!impl_->sessions.emplace(new_id, std::move(sess)).second) {
            return tl::unexpected {error_code::engine_start_failed};
        }
    }

    if (!sess_ptr->engine.go(make_go_params(params))) {
        const std::scoped_lock lock {impl_->mutex};
        impl_->sessions.erase(new_id);
        return tl::unexpected {error_code::engine_start_failed};
    }

    return new_id;
}

auto engine_manager::stop_analysis(std::string const& analysis_id) -> tl::expected<void, error_code>
{
    const std::scoped_lock lock {impl_->mutex};
    auto iter = impl_->sessions.find(analysis_id);
    if (iter == impl_->sessions.end()) {
        return tl::unexpected {error_code::analysis_not_found};
    }
    auto& sess = *iter->second;
    if (sess.terminal.load(std::memory_order_relaxed)) {
        return tl::unexpected {error_code::analysis_already_terminal};
    }
    if (!sess.engine.stop()) {
        fmt::print(stderr, "engine_manager: engine.stop() failed for {}\n", analysis_id);
        return tl::unexpected {error_code::engine_stop_failed};
    }
    sess.terminal.store(true, std::memory_order_relaxed);
    return {};
}

auto engine_manager::subscribe(std::string const& analysis_id,
                               info_callback const& on_info,
                               complete_callback const& on_complete,
                               error_callback const& on_error) -> tl::expected<subscription, error_code>
{
    std::optional<complete_event> complete_to_replay;
    std::vector<info_event> info_to_replay;
    {
        const std::scoped_lock lock {impl_->mutex};
        auto iter = impl_->sessions.find(analysis_id);
        if (iter == impl_->sessions.end()) {
            return tl::unexpected {error_code::analysis_not_found};
        }
        auto& sess = *iter->second;
        {
            const std::scoped_lock cb_lock {sess.callback_mutex};
            sess.on_info = on_info;
            sess.on_complete = on_complete;
            sess.on_error = on_error;
            info_to_replay = sess.info_history;
            complete_to_replay = sess.last_complete;
        }
    }
    // Replay buffered info events first, then the terminal complete event.
    // These events were emitted before subscribe() was called (e.g. a fast engine
    // that finished before the SSE subscriber connected).
    if (on_info) {
        for (auto const& evt : info_to_replay) {
            try {
                on_info(evt);
            } catch (...) {
                fmt::print(stderr, "engine_manager: on_info callback threw\n");
            }
        }
    }
    if (complete_to_replay.has_value() && on_complete) {
        try {
            on_complete(*complete_to_replay);
        } catch (...) {
            fmt::print(stderr, "engine_manager: on_complete callback threw\n");
        }
    }
    return subscription {};
}

}  // namespace motif::engine
