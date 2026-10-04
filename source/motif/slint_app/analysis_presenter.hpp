#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "motif/engine/engine_manager.hpp"
#include "motif/slint_app/analysis_service.hpp"
#include "motif/slint_app/config.hpp"

namespace motif::slint_app
{

enum class analysis_status : std::uint8_t
{
    // Nothing has run since the engine or its options last changed.
    idle,
    // A session was asked for and has not finished.
    analysing,
    // The engine reached its depth or time limit. Analysis is still on and
    // follows the board.
    finished,
    // The user stopped it. The last lines stay visible.
    stopped,
    // The engine could not be launched.
    failed,
    // The engine died mid-analysis. The last lines stay visible.
    crashed,
};

struct engine_choice
{
    // The file name of the path, with a numeric suffix when two paths share it.
    std::string name;
    std::string path;
};

// One row of the panel: a MultiPV rank with its latest depth, White-relative
// score and numbered SAN line.
struct analysis_line
{
    int rank {1};
    int depth {0};
    std::string score;
    std::string moves;
    std::string accessible_label;
};

// Everything the panel shows. Slint draws this and decides nothing.
struct analysis_state
{
    std::vector<engine_choice> engines;
    // Index into `engines`, or -1 when there is none.
    int selected_engine {-1};
    int multipv {default_engine_multipv};
    analysis_limit_kind limit {analysis_limit_kind::depth};
    int depth {default_engine_depth};
    int movetime_ms {default_engine_movetime_ms};
    analysis_status status {analysis_status::idle};
    // Analysis is switched on: starting, running, or finished and following the
    // board. Controls that change the session are locked while it is.
    bool active {false};
    // Also the accessible announcement of the state.
    std::string status_text;
    // Feedback for the last engine-list action (duplicate path, save failure).
    std::string notice;
    std::vector<analysis_line> lines;
    // The lines no longer belong to a live session.
    bool lines_stale {false};
    bool can_start {false};
};

// The file names of `paths`, in order. A name already taken gets " (2)", " (3)"
// and so on; a path with no file name is shown whole.
[[nodiscard]] auto engine_display_names(std::vector<std::string> const& paths) -> std::vector<std::string>;

// UCI scores are relative to the side to move; the panel shows them from
// White's side. Centipawns as "+0.35", "-1.20" and "0.00"; mate as "M3" and
// "-M2". `fen` supplies the side to move.
[[nodiscard]] auto format_white_score(motif::engine::score_value const& score, std::string const& fen) -> std::string;

// Toolkit-neutral state machine of the engine panel. It performs no I/O and
// never blocks, and is used from the event-loop thread only. Every request it
// hands out carries a fresh generation; events of any other generation are
// dropped, which is how a replaced, stopped or closed session stays silent.
class analysis_presenter
{
  public:
    // Adopts the persisted engine list and options. A running analysis keeps its
    // own request; the list under it changes only through select/add/remove
    // while analysis is off.
    void configure(engine_settings const& settings);

    // Which options the panel changes. Ignored while analysis is on. Values are
    // clamped to their ranges.
    void set_multipv(int value);
    void set_limit(analysis_limit_kind kind);
    void set_limit_value(int value);
    // Index into the engine list; an out-of-range index changes nothing.
    // Returns the chosen engine's path.
    auto select_engine(int index) -> std::optional<std::string>;
    [[nodiscard]] auto locked() const noexcept -> bool { return state_.active; }

    void set_notice(std::string notice);

    // Turns analysis on for `fen`. Nothing happens, and nullopt comes back,
    // when analysis is already on, no engine is selected or `fen` is empty.
    [[nodiscard]] auto begin(std::string const& fen) -> std::optional<analysis_request>;

    // The board moved while analysis is on: the old lines go, the old session's
    // events are dropped and the state reads "Analysing" until restart() issues
    // the request for the new position. False when analysis is off.
    auto position_changed() -> bool;

    // The request for `fen` after position_changed(). Nullopt unless a restart
    // is pending, so a timer that fires after Stop does nothing.
    [[nodiscard]] auto restart(std::string const& fen) -> std::optional<analysis_request>;

    // Turns analysis off. The last lines stay, marked stale.
    void stop();

    // The workspace under the analysis is gone or replaced: same as stop(), but
    // lines of a position that no longer exists are dropped.
    void invalidate();

    // Each returns whether the panel changed. An event whose generation is not
    // the current one, or that arrives when no session is expected, is dropped.
    auto apply_info(std::uint64_t generation, motif::engine::info_event const& event) -> bool;
    auto apply_complete(std::uint64_t generation) -> bool;
    auto apply_error(std::uint64_t generation, std::string const& message) -> bool;
    auto apply_start_failure(std::uint64_t generation) -> bool;

    [[nodiscard]] auto state() const noexcept -> analysis_state const& { return state_; }

    // The position of the session asked for last.
    [[nodiscard]] auto analysed_fen() const noexcept -> std::string const& { return fen_; }

    [[nodiscard]] auto restart_pending() const noexcept -> bool { return restart_pending_; }

    [[nodiscard]] auto generation() const noexcept -> std::uint64_t { return generation_; }

  private:
    analysis_state state_;
    std::string fen_;
    std::uint64_t generation_ {0};
    bool restart_pending_ {false};

    auto make_request(std::string const& fen) -> analysis_request;
    void refresh_idle_text();
    void clear_session_display();
    // Engine, options or list changed: any earlier session display is obsolete.
    void reset_display();
    void refresh_derived();
    [[nodiscard]] auto engine_name() const -> std::string;
};

}  // namespace motif::slint_app
