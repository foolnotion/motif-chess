#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "motif/engine/engine_manager.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
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

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
namespace
{

struct replay_counts
{
    std::atomic<int> infos {0};
    std::atomic<int> completes {0};
    std::atomic<int> errors {0};
};

// Runs `body` on its own thread, with the manager it uses kept alive by that thread, and
// reports whether it finished in time. A deadlocked body is abandoned, so a regression
// fails this check instead of hanging the whole suite.
auto finishes_within(std::shared_ptr<motif::engine::engine_manager> const& manager,
                     std::function<void(motif::engine::engine_manager&)> body,
                     std::chrono::milliseconds const timeout) -> bool
{
    auto done = std::make_shared<std::promise<void>>();
    auto finished = done->get_future();
    std::thread {[manager, done, body = std::move(body)]() -> void
                 {
                     body(*manager);
                     done->set_value();
                 }}
        .detach();
    return finished.wait_for(timeout) == std::future_status::ready;
}

// Calls back into the manager the way a subscriber well may: list, stop, release.
void reenter(motif::engine::engine_manager& manager, std::string const& analysis_id, bool const release)
{
    static_cast<void>(manager.list_engines());
    static_cast<void>(manager.stop_analysis(analysis_id));
    if (release) {
        static_cast<void>(manager.release_analysis(analysis_id));
    }
}

constexpr auto reentry_timeout = std::chrono::seconds {10};

}  // namespace

TEST_CASE("engine_manager: replay callbacks may call back into the manager without deadlock", "[motif-engine]")
{
    // A finished session replays its lines, then its completion. The line callback lists and
    // stops; the completion callback also releases the session it is running for.
    auto const fake_engine = fake_uci::fake_engine {fake_uci::fast_complete_body()};
    auto const manager = std::make_shared<motif::engine::engine_manager>();
    REQUIRE(manager->configure_engine(motif::engine::engine_config {.name = "fake", .path = fake_engine.path()}).has_value());
    auto const analysis_id = manager->start_analysis(motif::engine::analysis_params {
        .fen = startpos_fen, .engine = "fake", .multipv = 1, .depth = default_depth, .movetime_ms = std::nullopt});
    REQUIRE(analysis_id.has_value());
    std::atomic<bool> complete_seen {false};
    REQUIRE(manager
                ->subscribe(
                    *analysis_id,
                    [](motif::engine::info_event const&) -> void {},
                    [&complete_seen](motif::engine::complete_event const&) -> void { complete_seen.store(true); },
                    [](motif::engine::error_event const&) -> void {})
                .has_value());
    REQUIRE(wait_until(complete_seen));
    auto const counts = std::make_shared<replay_counts>();
    auto const& session_id = *analysis_id;

    auto const finished = finishes_within(
        manager,
        [counts, session_id](motif::engine::engine_manager& target) -> void
        {
            static_cast<void>(target.subscribe(
                session_id,
                [&target, counts, session_id](motif::engine::info_event const&) -> void
                {
                    ++counts->infos;
                    reenter(target, session_id, /*release=*/false);
                },
                [&target, counts, session_id](motif::engine::complete_event const&) -> void
                {
                    ++counts->completes;
                    reenter(target, session_id, /*release=*/true);
                },
                [](motif::engine::error_event const&) -> void {}));
        },
        reentry_timeout);

    REQUIRE(finished);
    CHECK(counts->infos.load() == 1);
    CHECK(counts->completes.load() == 1);
    // The completion callback released the session.
    CHECK(manager->release_analysis(session_id).error() == motif::engine::error_code::analysis_not_found);
}

TEST_CASE("engine_manager: a replayed crash may call back into the manager without deadlock", "[motif-engine]")
{
    auto const fake_engine = fake_uci::fake_engine {fake_uci::crash_body()};
    auto const manager = std::make_shared<motif::engine::engine_manager>();
    REQUIRE(manager->configure_engine(motif::engine::engine_config {.name = "crash", .path = fake_engine.path()}).has_value());
    auto const analysis_id = manager->start_analysis(motif::engine::analysis_params {
        .fen = startpos_fen, .engine = "crash", .multipv = 1, .depth = default_depth, .movetime_ms = std::nullopt});
    REQUIRE(analysis_id.has_value());
    std::atomic<bool> error_seen {false};
    REQUIRE(manager
                ->subscribe(
                    *analysis_id,
                    [](motif::engine::info_event const&) -> void {},
                    [](motif::engine::complete_event const&) -> void {},
                    [&error_seen](motif::engine::error_event const&) -> void { error_seen.store(true); })
                .has_value());
    REQUIRE(wait_until(error_seen));
    auto const counts = std::make_shared<replay_counts>();
    auto const& session_id = *analysis_id;

    auto const finished = finishes_within(
        manager,
        [counts, session_id](motif::engine::engine_manager& target) -> void
        {
            static_cast<void>(target.subscribe(
                session_id,
                [](motif::engine::info_event const&) -> void {},
                [](motif::engine::complete_event const&) -> void {},
                [&target, counts, session_id](motif::engine::error_event const&) -> void
                {
                    ++counts->errors;
                    reenter(target, session_id, /*release=*/true);
                }));
        },
        reentry_timeout);

    REQUIRE(finished);
    CHECK(counts->errors.load() == 1);
    CHECK(manager->release_analysis(session_id).error() == motif::engine::error_code::analysis_not_found);
}

TEST_CASE("engine_manager: releasing the session from a replayed line stops the replay", "[motif-engine]")
{
    // The burst session holds 400 lines; the callback releases on the first one it gets.
    auto const fake_engine = fake_uci::fake_engine {fake_uci::burst_complete_body()};
    auto const manager = std::make_shared<motif::engine::engine_manager>();
    REQUIRE(manager->configure_engine(motif::engine::engine_config {.name = "burst", .path = fake_engine.path()}).has_value());
    auto const analysis_id = manager->start_analysis(motif::engine::analysis_params {
        .fen = startpos_fen, .engine = "burst", .multipv = 1, .depth = default_depth, .movetime_ms = std::nullopt});
    REQUIRE(analysis_id.has_value());
    REQUIRE(fake_uci::wait_for([&]() -> bool { return !fake_engine.commands_starting_with("go").empty(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds {300});
    auto const counts = std::make_shared<replay_counts>();
    auto const& session_id = *analysis_id;

    auto const finished = finishes_within(
        manager,
        [counts, session_id](motif::engine::engine_manager& target) -> void
        {
            static_cast<void>(target.subscribe(
                session_id,
                [&target, counts, session_id](motif::engine::info_event const&) -> void
                {
                    ++counts->infos;
                    reenter(target, session_id, /*release=*/true);
                },
                [counts](motif::engine::complete_event const&) -> void { ++counts->completes; },
                [](motif::engine::error_event const&) -> void {}));
        },
        reentry_timeout);

    REQUIRE(finished);
    CHECK(counts->infos.load() == 1);
    CHECK(counts->completes.load() == 0);
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

auto start_fake_session(motif::engine::engine_manager& manager, fake_uci::fake_engine const& engine, std::string const& name) -> std::string
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

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
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
// start_analysis cancellation
// ---------------------------------------------------------------------------

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("engine_manager: a stop token cancels a blocked uci or isready handshake and reports it as cancelled", "[motif-engine]")
{
    auto const phase = GENERATE(fake_uci::blocked_phase::uci, fake_uci::blocked_phase::isready);
    auto const fake_engine = fake_uci::fake_engine {fake_uci::wait_for_stop_body(), phase};
    motif::engine::engine_manager manager;
    REQUIRE(manager.configure_engine(motif::engine::engine_config {.name = "fake", .path = fake_engine.path()}).has_value());
    auto const params = motif::engine::analysis_params {
        .fen = startpos_fen,
        .engine = "fake",
        .multipv = 1,
        .depth = default_depth,
        .movetime_ms = std::nullopt,
    };
    std::stop_source source;
    auto launch = std::async(std::launch::async, [&]() -> auto { return manager.start_analysis(params, source.get_token()); });
    REQUIRE(fake_engine.wait_blocked());
    auto const stalled = fake_engine.process_ids();
    REQUIRE(stalled.size() == 1);
    REQUIRE(fake_uci::process_alive(stalled.front()));

    source.request_stop();

    REQUIRE(launch.wait_for(std::chrono::seconds {1}) == std::future_status::ready);
    auto const result = launch.get();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == motif::engine::error_code::engine_start_cancelled);
    // The child is reaped by the time the call returns, and the search never began.
    CHECK_FALSE(fake_uci::process_alive(stalled.front()));
    CHECK(fake_engine.commands_starting_with("go").empty());
    // Cancelling left no session behind and the manager still launches engines.
    auto const analysis_id = start_fake_session(manager, fake_engine, "fake");
    CHECK(fake_uci::process_alive(engine_process(fake_engine, 1)));
    CHECK(manager.release_analysis(analysis_id).has_value());
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("engine_manager: a stop token that is already requested cancels the launch before any engine runs", "[motif-engine]")
{
    auto const fake_engine = fake_uci::fake_engine {fake_uci::wait_for_stop_body()};
    motif::engine::engine_manager manager;
    REQUIRE(manager.configure_engine(motif::engine::engine_config {.name = "fake", .path = fake_engine.path()}).has_value());
    auto const params = motif::engine::analysis_params {
        .fen = startpos_fen,
        .engine = "fake",
        .multipv = 1,
        .depth = default_depth,
        .movetime_ms = std::nullopt,
    };
    // libc++ 21's request_stop is non-const on Darwin.
    // NOLINTNEXTLINE(misc-const-correctness)
    std::stop_source source;
    source.request_stop();

    auto const result = manager.start_analysis(params, source.get_token());

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == motif::engine::error_code::engine_start_cancelled);
    // The script never ran: it logs a pid on `uci`, and every command it reads.
    CHECK(fake_engine.commands().empty());
    CHECK(fake_engine.process_ids().empty());
    // No session was left behind, and the same manager still launches engines.
    auto const analysis_id = start_fake_session(manager, fake_engine, "fake");
    CHECK(fake_uci::process_alive(engine_process(fake_engine)));
    CHECK(manager.release_analysis(analysis_id).has_value());
}

TEST_CASE("engine_manager: a genuine launch failure is not reported as cancelled while the stop token is live", "[motif-engine]")
{
    motif::engine::engine_manager manager;
    REQUIRE(manager.configure_engine(motif::engine::engine_config {.name = "missing", .path = "/nonexistent/engine"}).has_value());
    std::stop_source const source;

    auto const result = manager.start_analysis(
        motif::engine::analysis_params {
            .fen = startpos_fen,
            .engine = "missing",
            .multipv = 1,
            .depth = default_depth,
            .movetime_ms = std::nullopt,
        },
        source.get_token());

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == motif::engine::error_code::engine_start_failed);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("engine_manager: a subscriber attached to a fast session sees each line once, in order, then the completion last",
          "[motif-engine]")
{
    // The fake engine prints 400 numbered lines at once, so the subscription lands
    // in the middle of them in some iterations: replayed lines must come first and
    // the live ones must continue without a gap, an overlap or a reversal.
    auto const fake_engine = fake_uci::fake_engine {fake_uci::burst_complete_body()};
    motif::engine::engine_manager manager;
    REQUIRE(manager.configure_engine(motif::engine::engine_config {.name = "burst", .path = fake_engine.path()}).has_value());

    for (auto round = 0; round < 25; ++round) {
        auto const analysis_id = manager.start_analysis(motif::engine::analysis_params {
            .fen = startpos_fen, .engine = "burst", .multipv = 1, .depth = default_depth, .movetime_ms = std::nullopt});
        REQUIRE(analysis_id.has_value());

        std::mutex mutex;
        std::vector<int> scores;
        auto complete_after = std::optional<std::size_t> {};
        REQUIRE(manager
                    .subscribe(
                        *analysis_id,
                        [&](motif::engine::info_event const& event) -> void
                        {
                            std::scoped_lock const lock {mutex};
                            scores.push_back(event.score.value);
                        },
                        [&](motif::engine::complete_event const&) -> void
                        {
                            std::scoped_lock const lock {mutex};
                            complete_after = scores.size();
                        },
                        [](motif::engine::error_event const&) -> void {})
                    .has_value());

        REQUIRE(fake_uci::wait_for(
            [&]() -> bool
            {
                std::scoped_lock const lock {mutex};
                return complete_after.has_value();
            }));
        std::scoped_lock const lock {mutex};
        REQUIRE(scores.size() == 400);
        CHECK(complete_after.value_or(0) == 400);
        for (auto index = std::size_t {0}; index < scores.size(); ++index) {
            REQUIRE(scores[index] == static_cast<int>(index) + 1);
        }
    }
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

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
