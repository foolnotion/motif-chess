#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <tl/expected.hpp>

#include "motif/engine/error.hpp"

// Phase 2 stub — engine subprocess management and ucilib wiring are deferred.
// This header defines the contract that motif_http depends on for type-checking
// and route registration. Implementations are Phase 2 work.

namespace motif::engine
{

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
// Destroying the handle unsubscribes the callbacks. Phase 2 will implement
// the actual lifetime management.
struct subscription
{
    // Phase 2 stub: no state needed for the contract.
};

using info_callback = std::function<void(info_event const&)>;
using complete_callback = std::function<void(complete_event const&)>;
using error_callback = std::function<void(error_event const&)>;

class engine_manager
{
  public:
    engine_manager() = default;
    ~engine_manager() = default;
    engine_manager(engine_manager const&) = delete;
    auto operator=(engine_manager const&) -> engine_manager& = delete;
    engine_manager(engine_manager&&) = delete;
    auto operator=(engine_manager&&) -> engine_manager& = delete;

    // Start an analysis session. Returns an opaque analysis_id on success.
    // Phase 2 will call ucilib to start the engine process and invoke go().
    auto start_analysis(analysis_params const& params) -> tl::expected<std::string, error_code>;

    // Stop an active analysis session.
    // Returns analysis_already_terminal if the session is already done.
    auto stop_analysis(std::string const& analysis_id) -> tl::expected<void, error_code>;

    // Register callbacks for SSE event forwarding.
    // Returns analysis_not_found if the analysis_id is unknown.
    auto subscribe(std::string const& analysis_id,
                   info_callback const& on_info,
                   complete_callback const& on_complete,
                   error_callback const& on_error) -> tl::expected<subscription, error_code>;
};

}  // namespace motif::engine
