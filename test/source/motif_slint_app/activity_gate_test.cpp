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

        CHECK(!ran);
        CHECK(presenter.state().fen == fen);
        CHECK(presenter.state().total_plies == 0);
        CHECK(!presenter.state().unsaved);
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

namespace
{

// Stand-ins for the runners main() composes; each reports one flag.
struct fake_importer
{
    bool running {false};

    [[nodiscard]] auto active() const noexcept -> bool { return running; }
};

struct fake_workspace
{
    bool busy {false};

    [[nodiscard]] auto is_busy() const noexcept -> bool { return busy; }
};

struct fake_browser
{
    bool busy {false};

    [[nodiscard]] auto is_busy() const noexcept -> bool { return busy; }
};

struct fake_search
{
    bool busy {false};

    [[nodiscard]] auto is_busy() const noexcept -> bool { return busy; }
};

}  // namespace

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("activity gate: the production composition maps every source to its own flag", "[motif-slint-app]")
{
    using motif::slint_app::current_activity;

    auto const idle = current_activity(fake_importer {}, fake_workspace {}, fake_browser {}, fake_search {});
    CHECK(idle.idle());

    // Each source on its own must be reported, and under its own name: dropping
    // a source from the composition leaves the workspace looking idle.
    auto const import = current_activity(fake_importer {.running = true}, fake_workspace {}, fake_browser {}, fake_search {});
    CHECK_FALSE(import.idle());
    CHECK(import.import_active);
    CHECK_FALSE(import.workspace_busy);
    CHECK_FALSE(import.browser_busy);
    CHECK_FALSE(import.search_busy);

    auto const workspace = current_activity(fake_importer {}, fake_workspace {.busy = true}, fake_browser {}, fake_search {});
    CHECK_FALSE(workspace.idle());
    CHECK(workspace.workspace_busy);
    CHECK_FALSE(workspace.import_active);
    CHECK_FALSE(workspace.browser_busy);
    CHECK_FALSE(workspace.search_busy);

    auto const browser = current_activity(fake_importer {}, fake_workspace {}, fake_browser {.busy = true}, fake_search {});
    CHECK_FALSE(browser.idle());
    CHECK(browser.browser_busy);
    CHECK_FALSE(browser.import_active);
    CHECK_FALSE(browser.workspace_busy);
    CHECK_FALSE(browser.search_busy);

    auto const search = current_activity(fake_importer {}, fake_workspace {}, fake_browser {}, fake_search {.busy = true});
    CHECK_FALSE(search.idle());
    CHECK(search.search_busy);
    CHECK_FALSE(search.import_active);
    CHECK_FALSE(search.workspace_busy);
    CHECK_FALSE(search.browser_busy);
}
