#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "motif/slint_app/analysis_presenter.hpp"

#include <catch2/catch_test_macros.hpp>

#include "motif/engine/engine_manager.hpp"
#include "motif/slint_app/analysis_service.hpp"
#include "motif/slint_app/config.hpp"

namespace
{

using motif::engine::info_event;
using motif::slint_app::analysis_limit_kind;
using motif::slint_app::analysis_presenter;
using motif::slint_app::analysis_status;
using motif::slint_app::engine_settings;

constexpr auto start_fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
constexpr auto black_fen = "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1";

auto make_settings(std::vector<std::string> paths, std::string selected = {}) -> engine_settings
{
    auto settings = engine_settings {};
    settings.paths = std::move(paths);
    settings.selected = std::move(selected);
    return settings;
}

auto make_info(int const rank, int const depth, int const centipawns, std::vector<std::string> san = {}) -> info_event
{
    auto event = info_event {};
    event.multipv = rank;
    event.depth = depth;
    event.score = {.type = "cp", .value = centipawns};
    event.pv_uci = {"e2e4"};
    if (!san.empty()) {
        event.pv_san = std::move(san);
    }
    return event;
}

// A presenter with one engine, analysing the starting position.
struct running
{
    analysis_presenter presenter;
    motif::slint_app::analysis_request request;

    explicit running(std::string const& fen = start_fen)
    {
        presenter.configure(make_settings({"/opt/engines/stockfish"}));
        auto started = presenter.begin(fen);
        REQUIRE(started);
        request = *started;
    }
};

}  // namespace

TEST_CASE("analysis presenter: engine names are file names, numbered when two paths share one", "[motif-slint-app][analysis]")
{
    CHECK(motif::slint_app::engine_display_names({"/opt/a/stockfish", "/usr/bin/lc0", "/opt/b/stockfish", "/opt/c/stockfish"})
          == std::vector<std::string> {"stockfish", "lc0", "stockfish (2)", "stockfish (3)"});
    // A path that already looks like a numbered name does not collide with it.
    CHECK(motif::slint_app::engine_display_names({"/a/engine", "/b/engine", "/c/engine (2)"})
          == std::vector<std::string> {"engine", "engine (2)", "engine (2) (2)"});
    CHECK(motif::slint_app::engine_display_names({"/opt/engines/"}) == std::vector<std::string> {"/opt/engines/"});
    CHECK(motif::slint_app::engine_display_names({}).empty());
}

TEST_CASE("analysis presenter: scores are shown from White's side whoever is to move", "[motif-slint-app][analysis]")
{
    using motif::engine::score_value;
    using motif::slint_app::format_white_score;

    CHECK(format_white_score(score_value {.type = "cp", .value = 35}, start_fen) == "+0.35");
    CHECK(format_white_score(score_value {.type = "cp", .value = -120}, start_fen) == "-1.20");
    CHECK(format_white_score(score_value {.type = "cp", .value = 0}, start_fen) == "0.00");
    CHECK(format_white_score(score_value {.type = "cp", .value = 1234}, start_fen) == "+12.34");

    // Black to move: a UCI score is from Black's side, so it flips.
    CHECK(format_white_score(score_value {.type = "cp", .value = 40}, black_fen) == "-0.40");
    CHECK(format_white_score(score_value {.type = "cp", .value = -75}, black_fen) == "+0.75");
    CHECK(format_white_score(score_value {.type = "cp", .value = 0}, black_fen) == "0.00");

    CHECK(format_white_score(score_value {.type = "mate", .value = 3}, start_fen) == "M3");
    CHECK(format_white_score(score_value {.type = "mate", .value = -2}, start_fen) == "-M2");
    CHECK(format_white_score(score_value {.type = "mate", .value = 3}, black_fen) == "-M3");
    CHECK(format_white_score(score_value {.type = "mate", .value = -2}, black_fen) == "M2");
    // The side to move is already mated.
    CHECK(format_white_score(score_value {.type = "mate", .value = 0}, start_fen) == "-M0");
    CHECK(format_white_score(score_value {.type = "mate", .value = 0}, black_fen) == "M0");
}

TEST_CASE("analysis presenter: with no engine the panel says so and cannot start", "[motif-slint-app][analysis]")
{
    auto presenter = analysis_presenter {};
    presenter.configure(make_settings({}));

    auto const& state = presenter.state();
    CHECK(state.status_text == "No engine configured - add an engine path");
    CHECK(state.selected_engine == -1);
    CHECK_FALSE(state.can_start);
    CHECK_FALSE(presenter.begin(start_fen).has_value());
    CHECK_FALSE(presenter.state().active);
}

TEST_CASE("analysis presenter: begin asks for the selected engine, MultiPV and exactly one limit", "[motif-slint-app][analysis]")
{
    auto presenter = analysis_presenter {};
    auto settings = make_settings({"/opt/a/stockfish", "/opt/b/lc0"}, "/opt/b/lc0");
    settings.multipv = 4;
    settings.depth = 30;
    settings.movetime_ms = 2500;
    presenter.configure(settings);

    auto const by_depth = presenter.begin(start_fen);
    REQUIRE(by_depth);
    CHECK(by_depth->engine_name == "lc0");
    CHECK(by_depth->engine_path == "/opt/b/lc0");
    CHECK(by_depth->fen == start_fen);
    CHECK(by_depth->multipv == 4);
    CHECK(by_depth->depth == 30);
    CHECK_FALSE(by_depth->movetime_ms.has_value());
    CHECK(by_depth->generation != 0);
    CHECK(presenter.state().status == analysis_status::analysing);
    CHECK(presenter.state().status_text == "Analysing");

    presenter.stop();
    presenter.set_limit(analysis_limit_kind::movetime);
    auto const by_time = presenter.begin(start_fen);
    REQUIRE(by_time);
    CHECK(by_time->movetime_ms == 2500);
    CHECK_FALSE(by_time->depth.has_value());
    CHECK(by_time->generation > by_depth->generation);
}

TEST_CASE("analysis presenter: option values are clamped to their ranges", "[motif-slint-app][analysis]")
{
    auto presenter = analysis_presenter {};
    presenter.configure(make_settings({"/e"}));

    presenter.set_multipv(0);
    CHECK(presenter.state().multipv == 1);
    presenter.set_multipv(9);
    CHECK(presenter.state().multipv == 5);

    presenter.set_limit(analysis_limit_kind::depth);
    presenter.set_limit_value(0);
    CHECK(presenter.state().depth == 1);
    presenter.set_limit_value(500);
    CHECK(presenter.state().depth == 99);

    presenter.set_limit(analysis_limit_kind::movetime);
    presenter.set_limit_value(5);
    CHECK(presenter.state().movetime_ms == 100);
    presenter.set_limit_value(10'000'000);
    CHECK(presenter.state().movetime_ms == 600'000);
    // The depth the user chose earlier is untouched by editing the time.
    CHECK(presenter.state().depth == 99);
}

TEST_CASE("analysis presenter: each MultiPV line shows rank, depth, White-relative score and numbered SAN", "[motif-slint-app][analysis]")
{
    auto test = running {};
    auto settings = make_settings({"/opt/engines/stockfish"});
    CHECK(test.presenter.apply_info(test.request.generation, make_info(2, 18, -30, {"d4", "d5"})));
    CHECK(test.presenter.apply_info(test.request.generation, make_info(1, 20, 35, {"e4", "e5", "Nf3"})));

    auto const& lines = test.presenter.state().lines;
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].rank == 1);
    CHECK(lines[0].depth == 20);
    CHECK(lines[0].score == "+0.35");
    CHECK(lines[0].moves == "1.e4 e5 2.Nf3");
    CHECK(lines[1].rank == 2);
    CHECK(lines[1].score == "-0.30");
    CHECK(lines[1].moves == "1.d4 d5");
    CHECK(lines[0].accessible_label.find("+0.35") != std::string::npos);

    // A deeper line for the same rank replaces it.
    CHECK(test.presenter.apply_info(test.request.generation, make_info(1, 22, 41, {"e4", "c5"})));
    REQUIRE(test.presenter.state().lines.size() == 2);
    CHECK(test.presenter.state().lines[0].depth == 22);
    CHECK(test.presenter.state().lines[0].moves == "1.e4 c5");
}

TEST_CASE("analysis presenter: a line for a black-to-move position is numbered with an ellipsis and scored for White", "[motif-slint-app][analysis]")
{
    auto test = running {black_fen};

    CHECK(test.presenter.apply_info(test.request.generation, make_info(1, 12, 40, {"e5", "Nf3", "Nc6"})));

    REQUIRE(test.presenter.state().lines.size() == 1);
    CHECK(test.presenter.state().lines[0].score == "-0.40");
    CHECK(test.presenter.state().lines[0].moves == "1...e5 2.Nf3 Nc6");
}

TEST_CASE("analysis presenter: a line without SAN is shown as unavailable, never made up from UCI", "[motif-slint-app][analysis]")
{
    auto test = running {};

    CHECK(test.presenter.apply_info(test.request.generation, make_info(1, 5, 10)));

    REQUIRE(test.presenter.state().lines.size() == 1);
    CHECK(test.presenter.state().lines[0].moves.empty());
    CHECK(test.presenter.state().lines[0].accessible_label.find("no line available") != std::string::npos);
    CHECK(test.presenter.state().lines[0].accessible_label.find("e2e4") == std::string::npos);
}

TEST_CASE("analysis presenter: info without a score or for a rank beyond MultiPV is ignored", "[motif-slint-app][analysis]")
{
    auto test = running {};
    test.presenter.stop();
    test.presenter.set_multipv(2);
    test.request = *test.presenter.begin(start_fen);

    auto no_score = make_info(1, 3, 0);
    no_score.score = {};
    CHECK_FALSE(test.presenter.apply_info(test.request.generation, no_score));
    CHECK_FALSE(test.presenter.apply_info(test.request.generation, make_info(3, 3, 10)));
    CHECK_FALSE(test.presenter.apply_info(test.request.generation, make_info(0, 3, 10)));
    CHECK(test.presenter.state().lines.empty());
}

TEST_CASE("analysis presenter: finishing keeps the lines and analysis stays on", "[motif-slint-app][analysis]")
{
    auto test = running {};
    REQUIRE(test.presenter.apply_info(test.request.generation, make_info(1, 20, 35, {"e4"})));

    CHECK(test.presenter.apply_complete(test.request.generation));

    auto const& state = test.presenter.state();
    CHECK(state.status == analysis_status::finished);
    CHECK(state.status_text == "Finished");
    CHECK(state.active);
    CHECK_FALSE(state.can_start);
    CHECK(state.lines.size() == 1);
    CHECK_FALSE(state.lines_stale);
    // Nothing more is expected from a finished session.
    CHECK_FALSE(test.presenter.apply_info(test.request.generation, make_info(1, 21, 35, {"e4"})));
    CHECK_FALSE(test.presenter.apply_complete(test.request.generation));
}

TEST_CASE("analysis presenter: Stop keeps the last lines, marked stale, and drops late events", "[motif-slint-app][analysis]")
{
    auto test = running {};
    REQUIRE(test.presenter.apply_info(test.request.generation, make_info(1, 20, 35, {"e4"})));

    test.presenter.stop();

    auto const& state = test.presenter.state();
    CHECK(state.status == analysis_status::stopped);
    CHECK(state.status_text == "Stopped");
    CHECK_FALSE(state.active);
    CHECK(state.can_start);
    CHECK(state.lines_stale);
    CHECK(state.lines.size() == 1);
    // The bestmove an engine prints in answer to stop must not turn this into "Finished".
    CHECK_FALSE(test.presenter.apply_complete(test.request.generation));
    CHECK_FALSE(test.presenter.apply_info(test.request.generation, make_info(1, 30, 99, {"d4"})));
    CHECK(test.presenter.state().status == analysis_status::stopped);
    CHECK(test.presenter.state().lines[0].depth == 20);
    CHECK_FALSE(test.presenter.position_changed());
}

TEST_CASE("analysis presenter: an engine crash is visible with its message and Start becomes available again", "[motif-slint-app][analysis]")
{
    auto test = running {};
    REQUIRE(test.presenter.apply_info(test.request.generation, make_info(1, 8, 12, {"e4"})));

    CHECK(test.presenter.apply_error(test.request.generation, "engine crashed: engine process crashed"));

    auto const& state = test.presenter.state();
    CHECK(state.status == analysis_status::crashed);
    CHECK(state.status_text == "Engine crashed: engine process crashed");
    CHECK_FALSE(state.active);
    CHECK(state.can_start);
    CHECK(state.lines_stale);
    CHECK(state.lines.size() == 1);

    // Restart is the ordinary Start, with a new session.
    auto const again = test.presenter.begin(start_fen);
    REQUIRE(again);
    CHECK(again->generation > test.request.generation);
    CHECK(test.presenter.state().lines.empty());
}

TEST_CASE("analysis presenter: a start failure says only to check the path, naming the engine", "[motif-slint-app][analysis]")
{
    auto presenter = analysis_presenter {};
    presenter.configure(make_settings({"/nowhere/missing-engine"}));
    auto const request = presenter.begin(start_fen);
    REQUIRE(request);

    CHECK(presenter.apply_start_failure(request->generation));

    auto const& state = presenter.state();
    CHECK(state.status == analysis_status::failed);
    CHECK(state.status_text == "Engine could not start - check the path (missing-engine)");
    CHECK_FALSE(state.active);
    CHECK(state.can_start);
}

TEST_CASE("analysis presenter: a position change clears the lines and drops the old session's events", "[motif-slint-app][analysis]")
{
    auto test = running {};
    REQUIRE(test.presenter.apply_info(test.request.generation, make_info(1, 20, 35, {"e4"})));
    REQUIRE(test.presenter.apply_complete(test.request.generation));

    REQUIRE(test.presenter.position_changed());

    auto const& state = test.presenter.state();
    CHECK(state.status == analysis_status::analysing);
    CHECK(state.status_text == "Analysing");
    CHECK(state.active);
    CHECK(state.lines.empty());
    CHECK(test.presenter.restart_pending());
    // Late events of the old session never reach the panel, whatever their kind.
    CHECK_FALSE(test.presenter.apply_info(test.request.generation, make_info(1, 21, 35, {"e4"})));
    CHECK_FALSE(test.presenter.apply_complete(test.request.generation));
    CHECK_FALSE(test.presenter.apply_error(test.request.generation, "engine crashed: late"));
    CHECK(test.presenter.state().lines.empty());
    CHECK(test.presenter.state().status == analysis_status::analysing);

    auto const next = test.presenter.restart(black_fen);
    REQUIRE(next);
    CHECK(next->fen == black_fen);
    CHECK(next->generation > test.request.generation);
    CHECK_FALSE(test.presenter.restart_pending());
    CHECK(test.presenter.analysed_fen() == black_fen);
    CHECK(test.presenter.apply_info(next->generation, make_info(1, 10, 20, {"e5"})));
    CHECK(test.presenter.state().lines[0].score == "-0.20");
}

TEST_CASE("analysis presenter: a restart after Stop or without a pending position change does nothing", "[motif-slint-app][analysis]")
{
    auto test = running {};
    CHECK_FALSE(test.presenter.restart(black_fen).has_value());

    REQUIRE(test.presenter.position_changed());
    test.presenter.stop();

    CHECK_FALSE(test.presenter.restart(black_fen).has_value());
    CHECK(test.presenter.state().status == analysis_status::stopped);
}

TEST_CASE("analysis presenter: Start while analysis is on does not start a second session", "[motif-slint-app][analysis]")
{
    auto test = running {};

    CHECK_FALSE(test.presenter.begin(start_fen).has_value());

    CHECK(test.presenter.generation() == test.request.generation);
}

TEST_CASE("analysis presenter: engines, MultiPV and limit cannot change while analysis is on", "[motif-slint-app][analysis]")
{
    auto presenter = analysis_presenter {};
    presenter.configure(make_settings({"/a/one", "/b/two"}));
    REQUIRE(presenter.begin(start_fen));

    CHECK_FALSE(presenter.select_engine(1).has_value());
    presenter.set_multipv(1);
    presenter.set_limit(analysis_limit_kind::movetime);
    presenter.set_limit_value(100);

    auto const& state = presenter.state();
    CHECK(state.selected_engine == 0);
    CHECK(state.multipv == 3);
    CHECK(state.limit == analysis_limit_kind::depth);
    CHECK(state.depth == 20);
}

TEST_CASE("analysis presenter: choosing another engine resets the previous result", "[motif-slint-app][analysis]")
{
    auto test = running {};
    REQUIRE(test.presenter.apply_info(test.request.generation, make_info(1, 20, 35, {"e4"})));
    test.presenter.stop();
    test.presenter.configure(make_settings({"/opt/engines/stockfish", "/opt/engines/lc0"}));

    CHECK(test.presenter.select_engine(1) == "/opt/engines/lc0");

    auto const& state = test.presenter.state();
    CHECK(state.status == analysis_status::idle);
    CHECK(state.lines.empty());
    CHECK(state.selected_engine == 1);
    CHECK_FALSE(test.presenter.select_engine(5).has_value());
    CHECK_FALSE(test.presenter.select_engine(-1).has_value());
}

TEST_CASE("analysis presenter: invalidating for a workspace change drops the lines and every late event", "[motif-slint-app][analysis]")
{
    auto test = running {};
    REQUIRE(test.presenter.apply_info(test.request.generation, make_info(1, 20, 35, {"e4"})));

    test.presenter.invalidate();

    auto const& state = test.presenter.state();
    CHECK(state.status == analysis_status::idle);
    CHECK_FALSE(state.active);
    CHECK(state.lines.empty());
    CHECK(state.can_start);
    CHECK_FALSE(test.presenter.apply_info(test.request.generation, make_info(1, 21, 35, {"e4"})));
    CHECK_FALSE(test.presenter.apply_complete(test.request.generation));
}

TEST_CASE("analysis presenter: a removed selected engine falls back to the first one", "[motif-slint-app][analysis]")
{
    auto presenter = analysis_presenter {};
    presenter.configure(make_settings({"/a/one", "/b/two"}, "/b/two"));
    REQUIRE(presenter.state().selected_engine == 1);

    presenter.configure(make_settings({"/a/one"}, "/b/two"));

    CHECK(presenter.state().selected_engine == 0);
    presenter.configure(make_settings({}));
    CHECK(presenter.state().selected_engine == -1);
    CHECK(presenter.state().status_text == "No engine configured - add an engine path");
}
