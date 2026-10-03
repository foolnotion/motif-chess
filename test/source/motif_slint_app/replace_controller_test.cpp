#include <string>
#include <vector>

#include "motif/slint_app/replace_controller.hpp"

#include <catch2/catch_test_macros.hpp>

#include "motif/slint_app/board_presenter.hpp"

namespace
{

// Records what the toolkit adapter would be asked to do.
struct adapter_log
{
    std::vector<std::string> events;
    bool edited {false};

    auto controller() -> motif::slint_app::replace_controller
    {
        return motif::slint_app::replace_controller {motif::slint_app::replace_controller::hooks {
            .edited = [this]() -> bool { return edited; },
            .show = [this](std::string_view const label) -> void { events.push_back("show:" + std::string {label}); },
            .hide = [this]() -> void { events.emplace_back("hide"); },
            .restore_focus = [this]() -> void { events.emplace_back("focus"); },
        }};
    }
};

}  // namespace

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("replace_controller: an untouched board runs any replacing action at once", "[motif-slint-app]")
{
    auto log = adapter_log {};
    auto controller = log.controller();
    auto runs = 0;

    for (auto const* label : {"Load game", "Start new game", "Quit", "Close database", "Open database"}) {
        controller.request(label, [&runs]() -> void { ++runs; });
    }

    CHECK(runs == 5);
    CHECK(log.events.empty());
    CHECK_FALSE(controller.pending());
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("replace_controller: quit, close and database switches wait for confirmation while the game is edited", "[motif-slint-app]")
{
    for (auto const* label :
         {"Quit", "Close database", "Open database", "Open scratch database", "Create database", "Load game", "Start new game"})
    {
        auto log = adapter_log {.edited = true};
        auto controller = log.controller();
        auto runs = 0;

        controller.request(label, [&runs]() -> void { ++runs; });
        CHECK(runs == 0);
        CHECK(controller.pending());
        REQUIRE(log.events.size() == 1);
        CHECK(log.events.front() == std::string {"show:"} + label);

        controller.accept();
        CHECK(runs == 1);
        CHECK_FALSE(controller.pending());
        CHECK(log.events.back() == "hide");
    }
}

TEST_CASE("replace_controller: cancel keeps everything, hides the overlay and returns focus", "[motif-slint-app]")
{
    auto log = adapter_log {.edited = true};
    auto controller = log.controller();
    auto runs = 0;

    controller.request("Quit", [&runs]() -> void { ++runs; });
    controller.cancel();
    controller.accept();

    CHECK(runs == 0);
    CHECK_FALSE(controller.pending());
    CHECK(log.events == std::vector<std::string> {"show:Quit", "hide", "focus", "hide"});
}

TEST_CASE("replace_controller: follows the presenter's edited flag from an unsaved game to New Game", "[motif-slint-app]")
{
    auto presenter = motif::slint_app::board_presenter {};
    auto log = adapter_log {};
    auto controller = motif::slint_app::replace_controller {motif::slint_app::replace_controller::hooks {
        .edited = [&presenter]() -> bool { return presenter.state().edited; },
        .show = [&log](std::string_view const label) -> void { log.events.push_back("show:" + std::string {label}); },
        .hide = []() -> void {},
        .restore_focus = []() -> void {},
    }};
    auto closed = 0;

    controller.request("Quit", [&closed]() -> void { ++closed; });
    CHECK(closed == 1);

    REQUIRE(presenter.play_san("d4").has_value());
    controller.request("Quit", [&closed]() -> void { ++closed; });
    CHECK(closed == 1);
    CHECK(controller.pending());

    controller.cancel();
    presenter.start_new_game();
    controller.request("Quit", [&closed]() -> void { ++closed; });
    CHECK(closed == 2);
}
