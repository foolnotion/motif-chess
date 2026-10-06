#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <tl/expected.hpp>

#include "motif/engine/error.hpp"

namespace motif::engine
{

struct engine_config
{
    std::string name;
    std::string path;
};

struct analysis_params
{
    std::string fen;
    std::string engine;
    int multipv {1};
    // Exactly one of depth or movetime_ms must be set.
    std::optional<int> depth;
    std::optional<int> movetime_ms;
};

struct score_value
{
    std::string type;  // "cp" or "mate"
    int value {};
};

struct info_event
{
    int depth {};
    std::optional<int> seldepth;
    int multipv {1};
    score_value score;
    std::vector<std::string> pv_uci;
    std::optional<std::vector<std::string>> pv_san;
    std::optional<std::int64_t> nodes;
    std::optional<int> nps;
    std::optional<int> time_ms;
};

struct complete_event
{
    std::string best_move_uci;
    std::optional<std::string> ponder_uci;
};

struct error_event
{
    std::string message;
};

// Opaque subscription handle returned by engine_manager::subscribe.
struct subscription
{
};

using info_callback = std::function<void(info_event const&)>;
using complete_callback = std::function<void(complete_event const&)>;
using error_callback = std::function<void(error_event const&)>;

// Convert a sequence of UCI moves to SAN starting from start_fen.
// Stops at the first illegal or unparsable move; partial output is valid.
auto pv_to_san(std::string_view start_fen, std::vector<std::string> const& pv_uci) -> std::vector<std::string>;

class engine_manager
{
  public:
    engine_manager();
    ~engine_manager();
    engine_manager(engine_manager const&) = delete;
    auto operator=(engine_manager const&) -> engine_manager& = delete;
    engine_manager(engine_manager&&) = delete;
    auto operator=(engine_manager&&) -> engine_manager& = delete;

    // Register or overwrite an engine configuration.
    // Returns engine_not_configured if path is empty.
    auto configure_engine(engine_config cfg) -> result<void>;

    // Return all registered engine configurations.
    auto list_engines() -> std::vector<engine_config>;

    // Start an analysis session. Returns an opaque analysis_id on success.
    // `stop_token` cancels the blocking engine launch (process start, `uci` and
    // `isready` handshakes): the engine process is terminated and its reader joined
    // before the call returns engine_start_cancelled, which is distinct from a
    // genuine engine_start_failed. The token is not consulted once the launch has
    // succeeded.
    auto start_analysis(analysis_params const& params, std::stop_token const& stop_token = {}) -> result<std::string>;

    // Stop an active analysis session.
    // Returns analysis_already_terminal if the session is already done.
    auto stop_analysis(std::string const& analysis_id) -> result<void>;

    // Forget a session and shut its engine process down, whether the session is
    // running, finished or crashed. Blocks until the engine has quit and its
    // reader thread has joined; once it returns no callback of the session runs.
    // Never call it from inside one of the session's own callbacks (that is the
    // reader thread) and keep it off any thread that must stay responsive.
    // Returns analysis_not_found if the analysis_id is unknown, including when
    // it was already released. A later subscribe() or stop_analysis() on a
    // released id reports analysis_not_found.
    auto release_analysis(std::string const& analysis_id) -> result<void>;

    // Attach the callbacks of one subscriber to a session (a second call replaces
    // them). Events the session produced before the call (its lines, then its
    // completion or its crash) are replayed on the calling thread; subscribe()
    // returns when the replay is done. No engine_manager lock is held while a
    // callback runs, so a callback may call list_engines, stop_analysis or
    // release_analysis (but not subscribe on the same session). Live events that
    // arrive during the replay are queued and delivered after it, in order, by the
    // same call; later ones are delivered on the engine's reader thread. If the
    // session is released meanwhile the replay stops. Callbacks must not block
    // for long: they delay the replay, or stall the reader thread.
    // Register callbacks for SSE event forwarding.
    // Returns analysis_not_found if the analysis_id is unknown.
    auto subscribe(std::string const& analysis_id,
                   info_callback const& on_info,
                   complete_callback const& on_complete,
                   error_callback const& on_error) -> result<subscription>;

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

}  // namespace motif::engine
