#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "motif/engine/engine_manager.hpp"

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "motif/engine/error.hpp"

namespace
{

constexpr auto startpos_fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
constexpr auto default_depth = 10;
constexpr auto startup_depth = 20;
constexpr auto validation_movetime_ms = 1000;
constexpr auto wait_poll_count = 100;
constexpr auto wait_poll_ms = 10;

auto make_fake_engine(std::string_view go_body) -> std::filesystem::path
{
    auto const stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto const path = std::filesystem::temp_directory_path() / fmt::format("motif_fake_uci_engine_{}.sh", stamp);
    auto script = std::ofstream {path};
    script << "#!/usr/bin/env sh\n"
              "while IFS= read -r line; do\n"
              "  case \"$line\" in\n"
              "    uci)\n"
              "      printf '%s\\n' 'id name MotifFake'\n"
              "      printf '%s\\n' 'uciok'\n"
              "      ;;\n"
              "    isready)\n"
              "      printf '%s\\n' 'readyok'\n"
              "      ;;\n"
              "    setoption*)\n"
              "      ;;\n"
              "    position*)\n"
              "      ;;\n"
              "    go*)\n"
           << go_body
           << "      ;;\n"
              "    stop)\n"
              "      printf '%s\\n' 'bestmove e2e4'\n"
              "      ;;\n"
              "    quit)\n"
              "      exit 0\n"
              "      ;;\n"
              "  esac\n"
              "done\n";
    script.close();
    std::filesystem::permissions(
        path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec);
    return path;
}

auto make_fast_complete_engine() -> std::filesystem::path
{
    return make_fake_engine(
        "      printf '%s\\n' 'info depth 1 seldepth 1 multipv 1 score cp 13 nodes 42 nps 1000 time 1 pv e2e4 e7e5'\n"
        "      printf '%s\\n' 'bestmove e2e4 ponder e7e5'\n");
}

auto make_wait_for_stop_engine() -> std::filesystem::path
{
    return make_fake_engine("      printf '%s\\n' 'info depth 1 seldepth 1 multipv 1 score cp 13 nodes 42 nps 1000 time 1 pv e2e4 e7e5'\n");
}

auto wait_until(std::atomic<bool> const& flag) -> bool
{
    for (auto tries = 0; tries < wait_poll_count; ++tries) {
        if (flag.load()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {wait_poll_ms});
    }
    return flag.load();
}

}  // namespace

// ---------------------------------------------------------------------------
// Preserved stub behavior: no engine registered
// ---------------------------------------------------------------------------

TEST_CASE("engine_manager: start_analysis reports engine not configured when no engine registered", "[motif-engine]")
{
    motif::engine::engine_manager manager;

    auto const result = manager.start_analysis(motif::engine::analysis_params {
        .fen = startpos_fen,
        .engine = "",
        .multipv = 1,
        .depth = startup_depth,
        .movetime_ms = std::nullopt,
    });

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == motif::engine::error_code::engine_not_configured);
}

TEST_CASE("engine_manager: stop_analysis reports analysis not found for unknown id", "[motif-engine]")
{
    motif::engine::engine_manager manager;

    auto const result = manager.stop_analysis("unknown-analysis");

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == motif::engine::error_code::analysis_not_found);
}

TEST_CASE("engine_manager: subscribe reports analysis not found for unknown id", "[motif-engine]")
{
    motif::engine::engine_manager manager;

    auto const result = manager.subscribe(
        "unknown-analysis",
        [](motif::engine::info_event const&) -> void {},
        [](motif::engine::complete_event const&) -> void {},
        [](motif::engine::error_event const&) -> void {});

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == motif::engine::error_code::analysis_not_found);
}

// ---------------------------------------------------------------------------
// configure_engine / list_engines
// ---------------------------------------------------------------------------

TEST_CASE("engine_manager: configure_engine stores engine and list_engines returns it", "[motif-engine]")
{
    motif::engine::engine_manager manager;

    auto const res = manager.configure_engine(motif::engine::engine_config {.name = "stockfish", .path = "/usr/bin/stockfish"});
    REQUIRE(res.has_value());

    auto const engines = manager.list_engines();
    REQUIRE(engines.size() == 1);
    CHECK(engines.front().name == "stockfish");
    CHECK(engines.front().path == "/usr/bin/stockfish");
}

TEST_CASE("engine_manager: configure_engine overwrites existing entry with same name", "[motif-engine]")
{
    motif::engine::engine_manager manager;

    REQUIRE(manager.configure_engine(motif::engine::engine_config {.name = "sf", .path = "/old/stockfish"}).has_value());
    REQUIRE(manager.configure_engine(motif::engine::engine_config {.name = "sf", .path = "/new/stockfish"}).has_value());

    auto const engines = manager.list_engines();
    REQUIRE(engines.size() == 1);
    CHECK(engines.front().path == "/new/stockfish");
}

TEST_CASE("engine_manager: list_engines preserves registration order", "[motif-engine]")
{
    motif::engine::engine_manager manager;

    REQUIRE(manager.configure_engine(motif::engine::engine_config {.name = "first", .path = "/first"}).has_value());
    REQUIRE(manager.configure_engine(motif::engine::engine_config {.name = "second", .path = "/second"}).has_value());
    REQUIRE(manager.configure_engine(motif::engine::engine_config {.name = "first", .path = "/first-updated"}).has_value());

    auto const engines = manager.list_engines();
    REQUIRE(engines.size() == 2);
    CHECK(engines[0].name == "first");
    CHECK(engines[0].path == "/first-updated");
    CHECK(engines[1].name == "second");
}

TEST_CASE("engine_manager: configure_engine with empty path returns engine_not_configured", "[motif-engine]")
{
    motif::engine::engine_manager manager;

    auto const res = manager.configure_engine(motif::engine::engine_config {.name = "stockfish", .path = ""});

    REQUIRE_FALSE(res.has_value());
    CHECK(res.error() == motif::engine::error_code::engine_not_configured);
}

TEST_CASE("engine_manager: configure_engine with empty path does not add to list", "[motif-engine]")
{
    motif::engine::engine_manager manager;

    static_cast<void>(manager.configure_engine(motif::engine::engine_config {.name = "sf", .path = ""}));

    CHECK(manager.list_engines().empty());
}

TEST_CASE("engine_manager: list_engines returns empty when no engines configured", "[motif-engine]")
{
    motif::engine::engine_manager manager;
    CHECK(manager.list_engines().empty());
}

// ---------------------------------------------------------------------------
// start_analysis error paths (no subprocess needed)
// ---------------------------------------------------------------------------

TEST_CASE("engine_manager: start_analysis with named engine not found returns engine_not_configured", "[motif-engine]")
{
    motif::engine::engine_manager manager;
    REQUIRE(manager.configure_engine(motif::engine::engine_config {.name = "stockfish", .path = "/usr/bin/stockfish"}).has_value());

    auto const result = manager.start_analysis(motif::engine::analysis_params {
        .fen = startpos_fen,
        .engine = "lc0",
        .multipv = 1,
        .depth = default_depth,
        .movetime_ms = std::nullopt,
    });

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == motif::engine::error_code::engine_not_configured);
}

TEST_CASE("engine_manager: start_analysis with empty engine field and no engines returns engine_not_configured", "[motif-engine]")
{
    motif::engine::engine_manager manager;

    auto const result = manager.start_analysis(motif::engine::analysis_params {
        .fen = startpos_fen,
        .engine = "",
        .multipv = 1,
        .depth = default_depth,
        .movetime_ms = std::nullopt,
    });

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == motif::engine::error_code::engine_not_configured);
}

TEST_CASE("engine_manager: start_analysis rejects invalid analysis params before launching", "[motif-engine]")
{
    motif::engine::engine_manager manager;
    REQUIRE(manager.configure_engine(motif::engine::engine_config {.name = "stockfish", .path = "/usr/bin/stockfish"}).has_value());

    auto both_limits = manager.start_analysis(motif::engine::analysis_params {
        .fen = startpos_fen,
        .engine = "stockfish",
        .multipv = 1,
        .depth = default_depth,
        .movetime_ms = validation_movetime_ms,
    });
    REQUIRE_FALSE(both_limits.has_value());
    CHECK(both_limits.error() == motif::engine::error_code::invalid_analysis_params);

    auto no_limits = manager.start_analysis(motif::engine::analysis_params {
        .fen = startpos_fen,
        .engine = "stockfish",
        .multipv = 1,
        .depth = std::nullopt,
        .movetime_ms = std::nullopt,
    });
    REQUIRE_FALSE(no_limits.has_value());
    CHECK(no_limits.error() == motif::engine::error_code::invalid_analysis_params);

    auto invalid_values = manager.start_analysis(motif::engine::analysis_params {
        .fen = startpos_fen,
        .engine = "stockfish",
        .multipv = 0,
        .depth = 1,
        .movetime_ms = std::nullopt,
    });
    REQUIRE_FALSE(invalid_values.has_value());
    CHECK(invalid_values.error() == motif::engine::error_code::invalid_analysis_params);
}

TEST_CASE("engine_manager: start_analysis uses first registered engine when engine field is empty", "[motif-engine]")
{
    auto const fake_engine = make_fast_complete_engine();
    motif::engine::engine_manager manager;
    REQUIRE(manager.configure_engine(motif::engine::engine_config {.name = "first", .path = fake_engine.string()}).has_value());
    REQUIRE(manager.configure_engine(motif::engine::engine_config {.name = "second", .path = "/not/a/real/engine"}).has_value());

    auto const result = manager.start_analysis(motif::engine::analysis_params {
        .fen = startpos_fen,
        .engine = "",
        .multipv = 1,
        .depth = 1,
        .movetime_ms = std::nullopt,
    });

    REQUIRE(result.has_value());
    static_cast<void>(std::filesystem::remove(fake_engine));
}

TEST_CASE("engine_manager: subscribe replays completion when engine finishes before subscription", "[motif-engine]")
{
    auto const fake_engine = make_fast_complete_engine();
    motif::engine::engine_manager manager;
    REQUIRE(manager.configure_engine(motif::engine::engine_config {.name = "fake", .path = fake_engine.string()}).has_value());

    auto const analysis_id = manager.start_analysis(motif::engine::analysis_params {
        .fen = startpos_fen,
        .engine = "fake",
        .multipv = 1,
        .depth = 1,
        .movetime_ms = std::nullopt,
    });
    REQUIRE(analysis_id.has_value());

    std::atomic<bool> complete_seen {false};
    auto const subscription = manager.subscribe(
        *analysis_id,
        [](motif::engine::info_event const&) -> void {},
        [&complete_seen](motif::engine::complete_event const& event) -> void
        {
            if (event.best_move_uci == "e2e4") {
                complete_seen.store(true);
            }
        },
        [](motif::engine::error_event const&) -> void {});

    REQUIRE(subscription.has_value());
    CHECK(wait_until(complete_seen));
    static_cast<void>(std::filesystem::remove(fake_engine));
}

TEST_CASE("engine_manager: stop_analysis transitions active session to terminal", "[motif-engine]")
{
    auto const fake_engine = make_wait_for_stop_engine();
    motif::engine::engine_manager manager;
    REQUIRE(manager.configure_engine(motif::engine::engine_config {.name = "fake", .path = fake_engine.string()}).has_value());

    auto const analysis_id = manager.start_analysis(motif::engine::analysis_params {
        .fen = startpos_fen,
        .engine = "fake",
        .multipv = 1,
        .depth = 1,
        .movetime_ms = std::nullopt,
    });
    REQUIRE(analysis_id.has_value());

    auto const stop_result = manager.stop_analysis(*analysis_id);
    REQUIRE(stop_result.has_value());

    auto const second_stop = manager.stop_analysis(*analysis_id);
    REQUIRE_FALSE(second_stop.has_value());
    CHECK(second_stop.error() == motif::engine::error_code::analysis_already_terminal);
    static_cast<void>(std::filesystem::remove(fake_engine));
}

TEST_CASE("engine_manager: on_error fires when engine crashes during analysis", "[motif-engine]")
{
    auto const fake_engine = make_fake_engine("      exit 1\n");
    motif::engine::engine_manager manager;
    REQUIRE(manager.configure_engine(motif::engine::engine_config {.name = "crash", .path = fake_engine.string()}).has_value());

    auto const analysis_id = manager.start_analysis(motif::engine::analysis_params {
        .fen = startpos_fen,
        .engine = "crash",
        .multipv = 1,
        .depth = default_depth,
        .movetime_ms = std::nullopt,
    });
    REQUIRE(analysis_id.has_value());

    std::atomic<bool> error_seen {false};
    auto const subscription = manager.subscribe(
        *analysis_id,
        [](motif::engine::info_event const&) -> void {},
        [](motif::engine::complete_event const&) -> void {},
        [&error_seen](motif::engine::error_event const& evt) -> void
        {
            if (!evt.message.empty()) {
                error_seen.store(true);
            }
        });

    REQUIRE(subscription.has_value());
    CHECK(wait_until(error_seen));
    static_cast<void>(std::filesystem::remove(fake_engine));
}

// ---------------------------------------------------------------------------
// pv_to_san
// ---------------------------------------------------------------------------

TEST_CASE("pv_to_san: converts standard opening moves from starting position", "[motif-engine]")
{
    constexpr auto fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

    auto const san = motif::engine::pv_to_san(fen, {"e2e4", "e7e5", "g1f3", "b8c6"});

    REQUIRE(san.size() == 4);
    CHECK(san[0] == "e4");
    CHECK(san[1] == "e5");
    CHECK(san[2] == "Nf3");
    CHECK(san[3] == "Nc6");
}

TEST_CASE("pv_to_san: single move conversion", "[motif-engine]")
{
    constexpr auto fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

    auto const san = motif::engine::pv_to_san(fen, {"d2d4"});

    REQUIRE(san.size() == 1);
    CHECK(san[0] == "d4");
}

TEST_CASE("pv_to_san: stops at first invalid UCI move and returns partial output", "[motif-engine]")
{
    constexpr auto fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

    auto const san = motif::engine::pv_to_san(fen, {"e2e4", "INVALID", "g1f3"});

    REQUIRE(san.size() == 1);
    CHECK(san[0] == "e4");
}

TEST_CASE("pv_to_san: returns empty vector for invalid FEN", "[motif-engine]")
{
    auto const san = motif::engine::pv_to_san("not-a-fen", {"e2e4"});

    CHECK(san.empty());
}

TEST_CASE("pv_to_san: returns empty vector for empty PV", "[motif-engine]")
{
    constexpr auto fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

    auto const san = motif::engine::pv_to_san(fen, {});

    CHECK(san.empty());
}
