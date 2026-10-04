#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "motif/slint_app/analysis_presenter.hpp"

#include <fmt/format.h>

#include "motif/engine/engine_manager.hpp"
#include "motif/slint_app/analysis_service.hpp"
#include "motif/slint_app/config.hpp"

namespace motif::slint_app
{

namespace
{

constexpr auto no_engine_text = "No engine configured - add an engine path";
constexpr auto crash_prefix = std::string_view {"engine crashed: "};
constexpr auto centipawns_per_pawn = 100.0;

// The fields of a FEN the panel reads: whose turn it is and the move number.
struct position_clock
{
    bool white_to_move {true};
    int move_number {1};
};

auto read_position_clock(std::string const& fen) -> position_clock
{
    auto clock = position_clock {};
    auto fields = std::vector<std::string_view> {};
    for (auto const field : std::views::split(std::string_view {fen}, ' ')) {
        fields.emplace_back(field.begin(), field.end());
    }
    constexpr auto side_field = std::size_t {1};
    constexpr auto move_number_field = std::size_t {5};
    if (fields.size() > side_field) {
        clock.white_to_move = fields[side_field] != "b";
    }
    if (fields.size() > move_number_field) {
        auto number = 0;
        auto const& text = fields[move_number_field];
        auto const parsed = std::from_chars(std::to_address(text.begin()), std::to_address(text.end()), number);
        if (parsed.ec == std::errc {} && number >= 1) {
            clock.move_number = number;
        }
    }
    return clock;
}

// "12.Nf3 Nc6 13.Bb5", or "12...Nc6 13.Bb5" when Black moves first. Only the
// SAN supplied by motif_engine is used; nothing is replayed or invented here.
auto numbered_moves(std::vector<std::string> const& san, position_clock const clock) -> std::string
{
    auto text = std::string {};
    auto number = clock.move_number;
    auto white = clock.white_to_move;
    for (auto const& move : san) {
        if (!text.empty()) {
            text += ' ';
        }
        if (white) {
            text += fmt::format("{}.{}", number, move);
        } else {
            text += text.empty() ? fmt::format("{}...{}", number, move) : move;
            ++number;
        }
        white = !white;
    }
    return text;
}

auto make_line(motif::engine::info_event const& event, std::string const& fen) -> analysis_line
{
    auto line = analysis_line {.rank = event.multipv, .depth = event.depth, .score = format_white_score(event.score, fen)};
    if (event.pv_san && !event.pv_san->empty()) {
        line.moves = numbered_moves(*event.pv_san, read_position_clock(fen));
    }
    // An engine line whose first move is not legal in the position has no SAN;
    // the row says so instead of showing raw engine output.
    auto const shown = line.moves.empty() ? std::string {"no line available"} : line.moves;
    line.accessible_label = fmt::format("Line {}, score {}, depth {}, {}", line.rank, line.score, line.depth, shown);
    return line;
}

}  // namespace

auto engine_display_names(std::vector<std::string> const& paths) -> std::vector<std::string>
{
    auto names = std::vector<std::string> {};
    auto taken = std::set<std::string> {};
    names.reserve(paths.size());
    for (auto const& path : paths) {
        auto base = std::filesystem::path {path}.filename().string();
        if (base.empty()) {
            base = path;
        }
        auto name = base;
        for (auto suffix = 2; taken.contains(name); ++suffix) {
            name = fmt::format("{} ({})", base, suffix);
        }
        taken.insert(name);
        names.push_back(std::move(name));
    }
    return names;
}

auto format_white_score(motif::engine::score_value const& score, std::string const& fen) -> std::string
{
    auto const white_to_move = read_position_clock(fen).white_to_move;
    if (score.type == "cp") {
        auto const white_centipawns = white_to_move ? score.value : -score.value;
        if (white_centipawns == 0) {
            return "0.00";
        }
        return fmt::format("{:+.2f}", static_cast<double>(white_centipawns) / centipawns_per_pawn);
    }
    if (score.type == "mate") {
        // Positive: the side to move mates. Zero: the side to move is mated.
        auto const side_to_move_wins = score.value > 0;
        auto const white_wins = score.value == 0 ? !white_to_move : (side_to_move_wins == white_to_move);
        return fmt::format("{}M{}", white_wins ? "" : "-", std::abs(score.value));
    }
    return "-";
}

void analysis_presenter::configure(engine_settings const& settings)
{
    auto const names = engine_display_names(settings.paths);
    state_.engines.clear();
    state_.selected_engine = -1;
    for (auto index = std::size_t {0}; index < settings.paths.size(); ++index) {
        state_.engines.push_back(engine_choice {.name = names[index], .path = settings.paths[index]});
        if (settings.paths[index] == settings.selected) {
            state_.selected_engine = static_cast<int>(index);
        }
    }
    if (state_.selected_engine < 0 && !state_.engines.empty()) {
        state_.selected_engine = 0;
    }
    if (!state_.active) {
        state_.multipv = clamp_engine_multipv(settings.multipv);
        state_.limit = settings.limit;
        state_.depth = clamp_engine_depth(settings.depth);
        state_.movetime_ms = clamp_engine_movetime_ms(settings.movetime_ms);
        reset_display();
    }
}

void analysis_presenter::set_multipv(int const value)
{
    if (!state_.active) {
        state_.multipv = clamp_engine_multipv(value);
    }
}

void analysis_presenter::set_limit(analysis_limit_kind const kind)
{
    if (!state_.active) {
        state_.limit = kind;
    }
}

void analysis_presenter::set_limit_value(int const value)
{
    if (state_.active) {
        return;
    }
    if (state_.limit == analysis_limit_kind::depth) {
        state_.depth = clamp_engine_depth(value);
    } else {
        state_.movetime_ms = clamp_engine_movetime_ms(value);
    }
}

auto analysis_presenter::select_engine(int const index) -> std::optional<std::string>
{
    if (state_.active || index < 0 || std::cmp_greater_equal(index, state_.engines.size())) {
        return std::nullopt;
    }
    state_.selected_engine = index;
    reset_display();
    return state_.engines[static_cast<std::size_t>(index)].path;
}

void analysis_presenter::set_notice(std::string notice)
{
    state_.notice = std::move(notice);
}

auto analysis_presenter::begin(std::string const& fen) -> std::optional<analysis_request>
{
    if (state_.active || state_.selected_engine < 0 || fen.empty()) {
        return std::nullopt;
    }
    state_.active = true;
    return make_request(fen);
}

auto analysis_presenter::position_changed() -> bool
{
    if (!state_.active) {
        return false;
    }
    ++generation_;
    restart_pending_ = true;
    clear_session_display();
    state_.status = analysis_status::analysing;
    state_.status_text = "Analysing";
    return true;
}

auto analysis_presenter::restart(std::string const& fen) -> std::optional<analysis_request>
{
    if (!state_.active || !restart_pending_ || fen.empty()) {
        return std::nullopt;
    }
    return make_request(fen);
}

void analysis_presenter::stop()
{
    if (!state_.active) {
        return;
    }
    ++generation_;
    restart_pending_ = false;
    state_.active = false;
    state_.status = analysis_status::stopped;
    state_.status_text = "Stopped";
    state_.lines_stale = !state_.lines.empty();
    refresh_derived();
}

void analysis_presenter::invalidate()
{
    ++generation_;
    state_.active = false;
    reset_display();
}

auto analysis_presenter::apply_info(std::uint64_t const generation, motif::engine::info_event const& event) -> bool
{
    auto const expected = state_.active && !restart_pending_ && generation == generation_ && state_.status == analysis_status::analysing;
    if (!expected || event.score.type.empty() || event.multipv < 1 || event.multipv > state_.multipv) {
        return false;
    }
    auto line = make_line(event, fen_);
    auto const slot = std::ranges::lower_bound(state_.lines, line.rank, {}, &analysis_line::rank);
    if (slot != state_.lines.end() && slot->rank == line.rank) {
        *slot = std::move(line);
    } else {
        state_.lines.insert(slot, std::move(line));
    }
    return true;
}

auto analysis_presenter::apply_complete(std::uint64_t const generation) -> bool
{
    if (!state_.active || restart_pending_ || generation != generation_ || state_.status != analysis_status::analysing) {
        return false;
    }
    state_.status = analysis_status::finished;
    state_.status_text = "Finished";
    return true;
}

auto analysis_presenter::apply_error(std::uint64_t const generation, std::string const& message) -> bool
{
    if (!state_.active || restart_pending_ || generation != generation_) {
        return false;
    }
    auto detail = std::string_view {message};
    if (detail.starts_with(crash_prefix)) {
        detail.remove_prefix(crash_prefix.size());
    }
    state_.active = false;
    state_.status = analysis_status::crashed;
    state_.status_text = detail.empty() ? std::string {"Engine crashed"} : fmt::format("Engine crashed: {}", detail);
    state_.lines_stale = !state_.lines.empty();
    refresh_derived();
    return true;
}

auto analysis_presenter::apply_start_failure(std::uint64_t const generation) -> bool
{
    if (!state_.active || restart_pending_ || generation != generation_ || state_.status != analysis_status::analysing) {
        return false;
    }
    state_.active = false;
    state_.status = analysis_status::failed;
    state_.status_text = fmt::format("Engine could not start - check the path ({})", engine_name());
    clear_session_display();
    refresh_derived();
    return true;
}

auto analysis_presenter::make_request(std::string const& fen) -> analysis_request
{
    auto const& engine = state_.engines[static_cast<std::size_t>(state_.selected_engine)];
    ++generation_;
    fen_ = fen;
    restart_pending_ = false;
    clear_session_display();
    state_.notice.clear();
    state_.status = analysis_status::analysing;
    state_.status_text = "Analysing";
    refresh_derived();
    auto request = analysis_request {
        .generation = generation_,
        .engine_name = engine.name,
        .engine_path = engine.path,
        .fen = fen,
        .multipv = state_.multipv,
    };
    if (state_.limit == analysis_limit_kind::depth) {
        request.depth = state_.depth;
    } else {
        request.movetime_ms = state_.movetime_ms;
    }
    return request;
}

void analysis_presenter::clear_session_display()
{
    state_.lines.clear();
    state_.lines_stale = false;
}

void analysis_presenter::reset_display()
{
    restart_pending_ = false;
    clear_session_display();
    state_.notice.clear();
    state_.status = analysis_status::idle;
    state_.status_text = state_.engines.empty() ? no_engine_text : "Ready";
    refresh_derived();
}

void analysis_presenter::refresh_derived()
{
    state_.can_start = !state_.active && state_.selected_engine >= 0;
}

auto analysis_presenter::engine_name() const -> std::string
{
    if (state_.selected_engine < 0) {
        return {};
    }
    return state_.engines[static_cast<std::size_t>(state_.selected_engine)].name;
}

}  // namespace motif::slint_app
