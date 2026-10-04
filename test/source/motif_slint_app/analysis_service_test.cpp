#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "motif/slint_app/analysis_service.hpp"

#include <catch2/catch_test_macros.hpp>

#include "fake_uci_engine.hpp"
#include "motif/engine/engine_manager.hpp"

// The test engines and timings are plain numbers.
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
namespace
{

using motif::slint_app::analysis_event;
using motif::slint_app::analysis_request;
using motif::slint_app::analysis_service;
using motif::slint_app::analysis_start_failure;

constexpr auto start_fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
constexpr auto after_e4_fen = "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1";

// Thread-safe record of what reached the sink.
class recorder
{
  public:
    void add(analysis_event event)
    {
        std::scoped_lock const lock {mutex_};
        events_.push_back(std::move(event));
    }

    [[nodiscard]] auto events() const -> std::vector<analysis_event>
    {
        std::scoped_lock const lock {mutex_};
        return events_;
    }

    template<typename Payload>
    [[nodiscard]] auto count(std::uint64_t const generation) const -> std::size_t
    {
        auto total = std::size_t {0};
        for (auto const& event : events()) {
            if (event.generation == generation && std::holds_alternative<Payload>(event.payload)) {
                ++total;
            }
        }
        return total;
    }

    [[nodiscard]] auto count_other_than(std::uint64_t const generation) const -> std::size_t
    {
        return static_cast<std::size_t>(std::ranges::count_if(
            events(), [generation](analysis_event const& event) -> bool { return event.generation != generation; }));
    }

  private:
    mutable std::mutex mutex_;
    std::vector<analysis_event> events_;
};

auto make_request(fake_uci::fake_engine const& engine, std::uint64_t const generation, std::string const& fen = start_fen) -> analysis_request
{
    return analysis_request {
        .epoch = 7,
        .generation = generation,
        .engine_name = "fake",
        .engine_path = engine.path(),
        .fen = fen,
        .multipv = 1,
        .depth = 12,
    };
}

auto process_gone(pid_t const process) -> bool
{
    return fake_uci::wait_for([process]() -> bool { return !fake_uci::process_alive(process); });
}

}  // namespace

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("analysis service: a session reports its lines and completion with the epoch and generation it was asked for", "[motif-slint-app][analysis]")
{
    auto const engine = fake_uci::fake_engine {fake_uci::fast_complete_body()};
    auto events = recorder {};
    auto service = analysis_service {[&events](analysis_event event) -> void { events.add(std::move(event)); }};

    service.start(make_request(engine, 5));

    REQUIRE(fake_uci::wait_for([&]() -> bool { return events.count<motif::engine::complete_event>(5) == 1; }));
    CHECK(events.count<motif::engine::info_event>(5) == 1);
    for (auto const& event : events.events()) {
        CHECK(event.epoch == 7);
        CHECK(event.generation == 5);
    }
    auto const commands = engine.commands_starting_with("position");
    REQUIRE(commands.size() == 1);
    CHECK(commands.front() == "position startpos");
}

TEST_CASE("analysis service: the engine analyses the position it was given", "[motif-slint-app][analysis]")
{
    auto const engine = fake_uci::fake_engine {fake_uci::wait_for_stop_body()};
    auto events = recorder {};
    auto service = analysis_service {[&events](analysis_event event) -> void { events.add(std::move(event)); }};

    service.start(make_request(engine, 1, after_e4_fen));
    service.wait_until_idle();

    REQUIRE(fake_uci::wait_for([&]() -> bool { return engine.commands_starting_with("go depth 12").size() == 1; }));
    auto const commands = engine.commands_starting_with("position");
    REQUIRE(commands.size() == 1);
    CHECK(commands.front() == std::string {"position fen "} + after_e4_fen);
}

TEST_CASE("analysis service: an engine that cannot start is reported as a start failure", "[motif-slint-app][analysis]")
{
    auto events = recorder {};
    auto service = analysis_service {[&events](analysis_event event) -> void { events.add(std::move(event)); }};
    auto request = analysis_request {
        .epoch = 3, .generation = 9, .engine_name = "missing", .engine_path = "/nonexistent/engine", .fen = start_fen, .multipv = 1, .depth = 5};

    service.start(std::move(request));

    REQUIRE(fake_uci::wait_for([&]() -> bool { return events.count<analysis_start_failure>(9) == 1; }));
    CHECK(events.events().size() == 1);
    CHECK(events.events().front().epoch == 3);
}

TEST_CASE("analysis service: an engine crash is reported once and its process is reaped on release", "[motif-slint-app][analysis]")
{
    auto const engine = fake_uci::fake_engine {fake_uci::crash_body()};
    auto events = recorder {};
    auto service = analysis_service {[&events](analysis_event event) -> void { events.add(std::move(event)); }};

    service.start(make_request(engine, 4));

    REQUIRE(fake_uci::wait_for([&]() -> bool { return events.count<motif::engine::error_event>(4) >= 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds {100});
    CHECK(events.count<motif::engine::error_event>(4) == 1);
    auto const processes = engine.process_ids();
    REQUIRE(processes.size() == 1);

    service.stop();
    service.wait_until_idle();

    CHECK(process_gone(processes.front()));
}

TEST_CASE("analysis service: stop releases the engine and silences its late events", "[motif-slint-app][analysis]")
{
    auto const engine = fake_uci::fake_engine {fake_uci::wait_for_stop_body()};
    auto events = recorder {};
    auto service = analysis_service {[&events](analysis_event event) -> void { events.add(std::move(event)); }};
    service.start(make_request(engine, 2));
    REQUIRE(fake_uci::wait_for([&]() -> bool { return events.count<motif::engine::info_event>(2) == 1; }));
    auto const processes = engine.process_ids();
    REQUIRE(processes.size() == 1);

    service.stop();
    auto const before = events.events().size();
    service.wait_until_idle();

    CHECK(process_gone(processes.front()));
    // Stopping makes an engine print its bestmove; that must not reach the sink.
    CHECK(events.events().size() == before);
    CHECK(events.count<motif::engine::complete_event>(2) == 0);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("analysis service: a new session replaces the old one, which is released before the next engine runs", "[motif-slint-app][analysis]")
{
    auto const engine = fake_uci::fake_engine {fake_uci::wait_for_stop_body()};
    auto events = recorder {};
    auto service = analysis_service {[&events](analysis_event event) -> void { events.add(std::move(event)); }};
    service.start(make_request(engine, 1));
    REQUIRE(fake_uci::wait_for([&]() -> bool { return events.count<motif::engine::info_event>(1) == 1; }));

    service.start(make_request(engine, 2, after_e4_fen));
    auto const old_count = events.count<motif::engine::info_event>(1);
    service.wait_until_idle();

    REQUIRE(fake_uci::wait_for([&]() -> bool { return events.count<motif::engine::info_event>(2) == 1; }));
    // Nothing of generation 1 arrived after start() returned.
    CHECK(events.count<motif::engine::info_event>(1) == old_count);
    CHECK(events.count<motif::engine::complete_event>(1) == 0);
    auto const processes = engine.process_ids();
    REQUIRE(processes.size() == 2);
    CHECK(process_gone(processes[0]));
    CHECK(fake_uci::process_alive(processes[1]));
    service.shutdown();
    CHECK(process_gone(processes[1]));
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("analysis service: a burst of starts leaves one live engine and the last position", "[motif-slint-app][analysis]")
{
    auto const engine = fake_uci::fake_engine {fake_uci::wait_for_stop_body()};
    auto events = recorder {};
    auto service = analysis_service {[&events](analysis_event event) -> void { events.add(std::move(event)); }};

    for (auto generation = std::uint64_t {1}; generation <= 6; ++generation) {
        service.start(make_request(engine, generation, generation == 6 ? after_e4_fen : start_fen));
    }
    service.wait_until_idle();

    REQUIRE(fake_uci::wait_for([&]() -> bool { return events.count<motif::engine::info_event>(6) == 1; }));
    auto const processes = engine.process_ids();
    REQUIRE_FALSE(processes.empty());
    // Earlier asks that never reached the worker never launched an engine at all.
    CHECK(processes.size() <= 6);
    auto alive = std::ranges::count_if(processes, [](pid_t const process) -> bool { return fake_uci::process_alive(process); });
    CHECK(alive == 1);
    auto const positions = engine.commands_starting_with("position");
    REQUIRE_FALSE(positions.empty());
    CHECK(positions.back() == std::string {"position fen "} + after_e4_fen);
    service.stop();
    service.wait_until_idle();
}

TEST_CASE("analysis service: shutdown ends a running engine and later calls do nothing", "[motif-slint-app][analysis]")
{
    auto const engine = fake_uci::fake_engine {fake_uci::wait_for_stop_body()};
    auto events = recorder {};
    auto service = analysis_service {[&events](analysis_event event) -> void { events.add(std::move(event)); }};
    service.start(make_request(engine, 1));
    REQUIRE(fake_uci::wait_for([&]() -> bool { return events.count<motif::engine::info_event>(1) == 1; }));
    auto const processes = engine.process_ids();
    REQUIRE(processes.size() == 1);

    service.shutdown();

    CHECK(process_gone(processes.front()));
    service.start(make_request(engine, 2));
    service.stop();
    service.wait_until_idle();
    service.shutdown();
    CHECK(engine.process_ids().size() == 1);
    CHECK(events.count<motif::engine::info_event>(2) == 0);
}

TEST_CASE("analysis service: destroying the service while analysing ends the engine", "[motif-slint-app][analysis]")
{
    auto const engine = fake_uci::fake_engine {fake_uci::wait_for_stop_body()};
    auto events = recorder {};
    auto process = pid_t {};
    {
        auto service = analysis_service {[&events](analysis_event event) -> void { events.add(std::move(event)); }};
        service.start(make_request(engine, 1));
        REQUIRE(fake_uci::wait_for([&]() -> bool { return events.count<motif::engine::info_event>(1) == 1; }));
        process = engine.process_ids().front();
    }

    CHECK(process_gone(process));
}

TEST_CASE("analysis service: a burst of engine output is delivered in full and in order for the live session", "[motif-slint-app][analysis]")
{
    auto const engine = fake_uci::fake_engine {fake_uci::burst_body()};
    auto events = recorder {};
    auto service = analysis_service {[&events](analysis_event event) -> void { events.add(std::move(event)); }};

    service.start(make_request(engine, 1));

    REQUIRE(fake_uci::wait_for([&]() -> bool { return events.count<motif::engine::info_event>(1) == 400; }));
    CHECK(events.count_other_than(1) == 0);
    auto last = 0;
    for (auto const& event : events.events()) {
        if (auto const* const info = std::get_if<motif::engine::info_event>(&event.payload)) {
            CHECK(info->score.value > last);
            last = info->score.value;
        }
    }
}

namespace
{

// An executable that never answers `uci`: it ignores its input and sleeps.
class silent_engine
{
  public:
    silent_engine()
        : path_ {std::filesystem::temp_directory_path() / ("motif_silent_engine_" + fake_uci::unique_stamp() + ".sh")}
    {
        {
            auto script = std::ofstream {path_};
            script << "#!/usr/bin/env sh\nexec sleep 60\n";
        }
        std::filesystem::permissions(
            path_, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec);
    }

    ~silent_engine()
    {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    silent_engine(silent_engine const&) = delete;
    auto operator=(silent_engine const&) -> silent_engine& = delete;
    silent_engine(silent_engine&&) = delete;
    auto operator=(silent_engine&&) -> silent_engine& = delete;

    [[nodiscard]] auto request(std::uint64_t const generation) const -> analysis_request
    {
        return analysis_request {
            .epoch = 1, .generation = generation, .engine_name = "silent", .engine_path = path_.string(), .fen = start_fen, .multipv = 1, .depth = 5};
    }

  private:
    std::filesystem::path path_;
};

}  // namespace

// The UCI handshake gives up after ten seconds, so these two cases take that long.
TEST_CASE("analysis service: an executable that never answers uci ends in a start failure after the handshake timeout",
          "[motif-slint-app][analysis][slow]")
{
    auto const engine = silent_engine {};
    auto events = recorder {};
    auto service = analysis_service {[&events](analysis_event event) -> void { events.add(std::move(event)); }};
    auto const begin = std::chrono::steady_clock::now();

    service.start(engine.request(3));

    REQUIRE(fake_uci::wait_for([&]() -> bool { return events.count<analysis_start_failure>(3) == 1; }, std::chrono::seconds {25}));
    CHECK(std::chrono::steady_clock::now() - begin < std::chrono::seconds {20});
    service.wait_until_idle();
    CHECK(events.events().size() == 1);
    service.shutdown();
}

TEST_CASE("analysis service: shutdown during a hung handshake returns within the handshake timeout", "[motif-slint-app][analysis][slow]")
{
    auto const engine = silent_engine {};
    auto events = recorder {};
    auto service = analysis_service {[&events](analysis_event event) -> void { events.add(std::move(event)); }};
    service.start(engine.request(1));
    std::this_thread::sleep_for(std::chrono::milliseconds {300});
    auto const begin = std::chrono::steady_clock::now();

    service.shutdown();

    CHECK(std::chrono::steady_clock::now() - begin < std::chrono::seconds {20});
    // The superseded start never reports: shutdown silenced its generation.
    CHECK(events.events().empty());
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
