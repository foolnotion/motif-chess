#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "motif/engine/engine_manager.hpp"

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "fake_uci_engine.hpp"
#include "motif/engine/error.hpp"

namespace
{

constexpr auto startpos_fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
constexpr auto default_depth = 10;
constexpr auto startup_depth = 20;
constexpr auto validation_movetime_ms = 1000;
constexpr auto wait_poll_count = 100;
constexpr auto wait_poll_ms = 10;

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
    auto const fake_engine = fake_uci::fake_engine {fake_uci::fast_complete_body()};
    motif::engine::engine_manager manager;
    REQUIRE(manager.configure_engine(motif::engine::engine_config {.name = "first", .path = fake_engine.path()}).has_value());
    REQUIRE(manager.configure_engine(motif::engine::engine_config {.name = "second", .path = "/not/a/real/engine"}).has_value());

    auto const result = manager.start_analysis(motif::engine::analysis_params {
        .fen = startpos_fen,
        .engine = "",
        .multipv = 1,
        .depth = 1,
        .movetime_ms = std::nullopt,
    });

    REQUIRE(result.has_value());
}

TEST_CASE("engine_manager: subscribe replays completion when engine finishes before subscription", "[motif-engine]")
{
    auto const fake_engine = fake_uci::fake_engine {fake_uci::fast_complete_body()};
    motif::engine::engine_manager manager;
    REQUIRE(manager.configure_engine(motif::engine::engine_config {.name = "fake", .path = fake_engine.path()}).has_value());

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
}

TEST_CASE("engine_manager: stop_analysis transitions active session to terminal", "[motif-engine]")
{
    auto const fake_engine = fake_uci::fake_engine {fake_uci::wait_for_stop_body()};
    motif::engine::engine_manager manager;
    REQUIRE(manager.configure_engine(motif::engine::engine_config {.name = "fake", .path = fake_engine.path()}).has_value());

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
}

TEST_CASE("engine_manager: on_error fires when engine crashes during analysis", "[motif-engine]")
{
    auto const fake_engine = fake_uci::fake_engine {fake_uci::crash_body()};
    motif::engine::engine_manager manager;
    REQUIRE(manager.configure_engine(motif::engine::engine_config {.name = "crash", .path = fake_engine.path()}).has_value());

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
}

TEST_CASE("engine_manager: subscribe delivers a crash exactly once, whether it happened before or after subscribing", "[motif-engine]")
{
    auto const fake_engine = fake_uci::fake_engine {fake_uci::crash_body()};
    motif::engine::engine_manager manager;
    REQUIRE(manager.configure_engine(motif::engine::engine_config {.name = "crash", .path = fake_engine.path()}).has_value());

    auto const analysis_id = manager.start_analysis(motif::engine::analysis_params {
        .fen = startpos_fen,
        .engine = "crash",
        .multipv = 1,
        .depth = default_depth,
        .movetime_ms = std::nullopt,
    });
    REQUIRE(analysis_id.has_value());
    // The engine has seen `go` and exits on it. Waiting a little longer makes the
    // crash precede the subscription in nearly every run; either order must report it once.
    REQUIRE(fake_uci::wait_for([&]() -> bool { return !fake_engine.commands_starting_with("go").empty(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds {200});

    std::atomic<int> errors {0};
    auto const subscription = manager.subscribe(
        *analysis_id,
        [](motif::engine::info_event const&) -> void {},
        [](motif::engine::complete_event const&) -> void {},
        [&errors](motif::engine::error_event const&) -> void { ++errors; });
    REQUIRE(subscription.has_value());

    CHECK(fake_uci::wait_for([&]() -> bool { return errors.load() > 0; }));
    std::this_thread::sleep_for(std::chrono::milliseconds {100});
    CHECK(errors.load() == 1);
}

// ---------------------------------------------------------------------------
// release_analysis
// ---------------------------------------------------------------------------

namespace
{

auto start_fake_session(motif::engine::engine_manager& manager, fake_uci::fake_engine const& engine, std::string const& name)
    -> std::string
{
    REQUIRE(manager.configure_engine(motif::engine::engine_config {.name = name, .path = engine.path()}).has_value());
    auto const analysis_id = manager.start_analysis(motif::engine::analysis_params {
        .fen = startpos_fen,
        .engine = name,
        .multipv = 1,
        .depth = default_depth,
        .movetime_ms = std::nullopt,
    });
    REQUIRE(analysis_id.has_value());
    return *analysis_id;
}

auto engine_process(fake_uci::fake_engine const& engine, std::size_t const index = 0) -> pid_t
{
    REQUIRE(fake_uci::wait_for([&]() -> bool { return engine.process_ids().size() > index; }));
    return engine.process_ids().at(index);
}

auto process_gone(pid_t const process) -> bool
{
    return fake_uci::wait_for([process]() -> bool { return !fake_uci::process_alive(process); });
}

}  // namespace

TEST_CASE("engine_manager: release_analysis ends a running session and its engine process", "[motif-engine]")
{
    auto const fake_engine = fake_uci::fake_engine {fake_uci::wait_for_stop_body()};
    motif::engine::engine_manager manager;
    auto const analysis_id = start_fake_session(manager, fake_engine, "fake");
    auto const process = engine_process(fake_engine);
    REQUIRE(fake_uci::process_alive(process));

    REQUIRE(manager.release_analysis(analysis_id).has_value());

    CHECK(process_gone(process));
    auto const stopped = manager.stop_analysis(analysis_id);
    REQUIRE_FALSE(stopped.has_value());
    CHECK(stopped.error() == motif::engine::error_code::analysis_not_found);
    auto const subscribed = manager.subscribe(
        analysis_id,
        [](motif::engine::info_event const&) -> void {},
        [](motif::engine::complete_event const&) -> void {},
        [](motif::engine::error_event const&) -> void {});
    REQUIRE_FALSE(subscribed.has_value());
    CHECK(subscribed.error() == motif::engine::error_code::analysis_not_found);
}

TEST_CASE("engine_manager: release_analysis reports an already released session as not found", "[motif-engine]")
{
    auto const fake_engine = fake_uci::fake_engine {fake_uci::wait_for_stop_body()};
    motif::engine::engine_manager manager;
    auto const analysis_id = start_fake_session(manager, fake_engine, "fake");

    REQUIRE(manager.release_analysis(analysis_id).has_value());
    auto const again = manager.release_analysis(analysis_id);

    REQUIRE_FALSE(again.has_value());
    CHECK(again.error() == motif::engine::error_code::analysis_not_found);
}

TEST_CASE("engine_manager: release_analysis ends a finished session and its engine process", "[motif-engine]")
{
    auto const fake_engine = fake_uci::fake_engine {fake_uci::fast_complete_body()};
    motif::engine::engine_manager manager;
    auto const analysis_id = start_fake_session(manager, fake_engine, "fake");
    auto const process = engine_process(fake_engine);
    std::atomic<bool> complete_seen {false};
    REQUIRE(manager
                .subscribe(
                    analysis_id,
                    [](motif::engine::info_event const&) -> void {},
                    [&complete_seen](motif::engine::complete_event const&) -> void { complete_seen.store(true); },
                    [](motif::engine::error_event const&) -> void {})
                .has_value());
    REQUIRE(wait_until(complete_seen));

    REQUIRE(manager.release_analysis(analysis_id).has_value());

    CHECK(process_gone(process));
}

TEST_CASE("engine_manager: release_analysis ends a crashed session and joins its reader", "[motif-engine]")
{
    auto const fake_engine = fake_uci::fake_engine {fake_uci::crash_body()};
    motif::engine::engine_manager manager;
    auto const analysis_id = start_fake_session(manager, fake_engine, "crash");
    auto const process = engine_process(fake_engine);
    std::atomic<bool> error_seen {false};
    REQUIRE(manager
                .subscribe(
                    analysis_id,
                    [](motif::engine::info_event const&) -> void {},
                    [](motif::engine::complete_event const&) -> void {},
                    [&error_seen](motif::engine::error_event const&) -> void { error_seen.store(true); })
                .has_value());
    REQUIRE(wait_until(error_seen));

    REQUIRE(manager.release_analysis(analysis_id).has_value());

    // A crashed engine is a zombie until its process is waited for; release does so.
    CHECK(process_gone(process));
}

TEST_CASE("engine_manager: release_analysis reports an unknown session as not found", "[motif-engine]")
{
    motif::engine::engine_manager manager;

    auto const released = manager.release_analysis("unknown-analysis");

    REQUIRE_FALSE(released.has_value());
    CHECK(released.error() == motif::engine::error_code::analysis_not_found);
}

TEST_CASE("engine_manager: release_analysis leaves other sessions running", "[motif-engine]")
{
    auto const first_engine = fake_uci::fake_engine {fake_uci::wait_for_stop_body()};
    auto const second_engine = fake_uci::fake_engine {fake_uci::wait_for_stop_body()};
    motif::engine::engine_manager manager;
    auto const first = start_fake_session(manager, first_engine, "first");
    auto const second = start_fake_session(manager, second_engine, "second");
    auto const first_process = engine_process(first_engine);
    auto const second_process = engine_process(second_engine);

    REQUIRE(manager.release_analysis(first).has_value());

    CHECK(process_gone(first_process));
    CHECK(fake_uci::process_alive(second_process));
    CHECK(manager.stop_analysis(second).has_value());
}

TEST_CASE("engine_manager: destroying the manager ends every live engine process", "[motif-engine]")
{
    auto const first_engine = fake_uci::fake_engine {fake_uci::wait_for_stop_body()};
    auto const second_engine = fake_uci::fake_engine {fake_uci::fast_complete_body()};
    auto first_process = pid_t {};
    auto second_process = pid_t {};
    {
        motif::engine::engine_manager manager;
        static_cast<void>(start_fake_session(manager, first_engine, "running"));
        static_cast<void>(start_fake_session(manager, second_engine, "finished"));
        first_process = engine_process(first_engine);
        second_process = engine_process(second_engine);
        REQUIRE(fake_uci::process_alive(first_process));
    }

    CHECK(process_gone(first_process));
    CHECK(process_gone(second_process));
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
