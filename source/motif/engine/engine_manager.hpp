#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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
    auto configure_engine(engine_config cfg) -> tl::expected<void, error_code>;

    // Return all registered engine configurations.
    auto list_engines() -> std::vector<engine_config>;

    // Start an analysis session. Returns an opaque analysis_id on success.
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

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

}  // namespace motif::engine
