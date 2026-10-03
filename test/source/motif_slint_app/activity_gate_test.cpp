#include <array>
#include <string>

#include "motif/slint_app/activity_gate.hpp"

#include <catch2/catch_test_macros.hpp>

#include "motif/slint_app/board_presenter.hpp"

namespace
{

using motif::slint_app::workspace_activity;

// One activity per flag, each with only that flag set.
constexpr auto busy_cases = std::array {
    workspace_activity {.import_active = true},
    workspace_activity {.workspace_busy = true},
    workspace_activity {.browser_busy = true},
    workspace_activity {.board_busy = true},
    workspace_activity {.search_busy = true},
};

}  // namespace

TEST_CASE("activity gate: SAN entry acts when the workspace is idle", "[motif-slint-app]")
{
    auto presenter = motif::slint_app::board_presenter {};

    auto const ran = motif::slint_app::run_when_idle(workspace_activity {}, [&presenter]() -> void { (void)presenter.play_san("e4"); });

    CHECK(ran);
    CHECK(presenter.state().total_plies == 1);
    CHECK(presenter.state().unsaved);
}

TEST_CASE("activity gate: SAN entry is rejected under every busy condition and changes nothing", "[motif-slint-app]")
{
    for (auto const& activity : busy_cases) {
        auto presenter = motif::slint_app::board_presenter {};
        auto const fen = presenter.state().fen;

        auto const ran = motif::slint_app::run_when_idle(activity, [&presenter]() -> void { (void)presenter.play_san("e4"); });

        CHECK_FALSE(ran);
        CHECK(presenter.state().fen == fen);
        CHECK(presenter.state().total_plies == 0);
        CHECK_FALSE(presenter.state().unsaved);
    }
}

TEST_CASE("activity gate: only an all-clear activity is idle", "[motif-slint-app]")
{
    CHECK(workspace_activity {}.idle());
    for (auto const& activity : busy_cases) {
        CHECK_FALSE(activity.idle());
    }
    CHECK_FALSE((workspace_activity {.import_active = true, .search_busy = true}).idle());
}
