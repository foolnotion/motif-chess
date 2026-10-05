#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <slint-platform.h>
#include <slint.h>

// The element-query API is gated behind two feature macros in its header, but the
// Slint library this project builds against is compiled with them: the dev
// shell's Slint carries the TESTING feature. The queries read the item tree, so
// they work on the headless snapshot platform. Only this file uses them.
#ifndef SLINT_FEATURE_TESTING
#    define SLINT_FEATURE_TESTING
#endif
#ifndef SLINT_FEATURE_EXPERIMENTAL
#    define SLINT_FEATURE_EXPERIMENTAL
#endif
#include <slint-testing.h>

#include "fake_uci_engine.hpp"
#include "motif/slint_app/event_loop.hpp"
#include "motif/slint_app/file_picker.hpp"
#include "motif/slint_app/picker_host.hpp"
#include "snapshot_platform.hpp"
#include "workspace.h"

// Test values are plain numbers; element geometry is in logical pixels.
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
namespace
{

using snapshot_test::snapshot_state;
using snapshot_test::use_snapshot_platform;

using element = slint::testing::ElementHandle;
using window_handle = slint::ComponentHandle<WorkspaceWindow>;

struct window_size
{
    std::uint32_t width;
    std::uint32_t height;
};

constexpr auto narrow = window_size {.width = 960, .height = 560};
constexpr auto wide = window_size {.width = 1280, .height = 800};

constexpr auto open_label = std::string_view {"Browse for the database folder to open"};
constexpr auto create_label = std::string_view {"Browse for the folder to create the database in"};
constexpr auto import_label = std::string_view {"Browse for a PGN file to import"};
constexpr auto engine_label = std::string_view {"Browse for an engine executable"};
constexpr auto hint_text = std::string_view {"File dialog open. It may be behind this window."};

constexpr auto margin = 8.F;
constexpr auto minimum_height = 20.F;
constexpr auto row_alignment_tolerance = 4.F;
// "Browse…" needs room for its word; the compact "…" only for the glyph.
constexpr auto full_width = 60.F;
constexpr auto compact_width = 24.F;
constexpr auto minimum_field_width = 80.F;

struct rect
{
    float left;
    float top;
    float right;
    float bottom;

    [[nodiscard]] auto width() const -> float { return right - left; }

    [[nodiscard]] auto height() const -> float { return bottom - top; }

    [[nodiscard]] auto middle() const -> float { return (top + bottom) / 2.F; }
};

auto bounds_of(element const& item) -> rect
{
    auto const position = item.absolute_position();
    auto const size = item.size();
    return {.left = position.x, .top = position.y, .right = position.x + size.width, .bottom = position.y + size.height};
}

auto text_of(slint::SharedString const& value) -> std::string
{
    return std::string {std::string_view {value.data(), value.size()}};
}

auto labelled(window_handle const& window, std::string_view const label) -> std::vector<element>
{
    auto found = std::vector<element> {};
    for (auto const& item : element::find_by_accessible_label(window, label)) {
        found.push_back(item);
    }
    return found;
}

// The one element under this label; the test fails when there is none or several.
auto the_one(window_handle const& window, std::string_view const label) -> element
{
    auto const found = labelled(window, label);
    REQUIRE(found.size() == 1);
    return found.front();
}

// The first text field with this placeholder.
auto the_field(window_handle const& window, std::string_view const placeholder) -> element
{
    auto found = std::vector<element> {};
    element::visit_elements(window,
                            [&found, placeholder](element const& item) -> void
                            {
                                auto const text = item.accessible_placeholder_text().value_or(slint::SharedString {});
                                if (text_of(text) == placeholder) {
                                    found.push_back(item);
                                }
                            });
    REQUIRE(!found.empty());
    return found.front();
}

void render(snapshot_state const& state)
{
    REQUIRE(state.window_adapter != nullptr);
    // The first frame lays out before the window's short-height mode is known.
    state.window_adapter->render();
    state.window_adapter->render();
}

// Conditional elements are rebuilt lazily, so render between key presses.
void press_key(snapshot_state const& state, WorkspaceWindow& window, std::u8string_view const key)
{
    window.window().dispatch_key_press_event(slint::SharedString {key});
    window.window().dispatch_key_release_event(slint::SharedString {key});
    state.window_adapter->render();
}

auto make_setup_window(snapshot_state const& state) -> window_handle
{
    auto window = WorkspaceWindow::create();
    window->set_has_database(false);
    render(state);
    return window;
}

// A window with a database open in the given preset and one engine configured.
auto make_workspace_window(snapshot_state const& state, WorkspacePreset const preset) -> window_handle
{
    auto window = WorkspaceWindow::create();
    window->set_has_database(true);
    window->set_game_column_widths(std::make_shared<slint::VectorModel<std::int32_t>>());
    window->set_board_square_pieces(std::make_shared<slint::VectorModel<slint::SharedString>>());
    window->set_board_square_highlights(std::make_shared<slint::VectorModel<SquareHighlight>>());
    window->set_board_notation(std::make_shared<slint::VectorModel<NotationToken>>());
    window->set_search_matches(std::make_shared<slint::VectorModel<SearchMatchRow>>());
    window->set_search_continuations(std::make_shared<slint::VectorModel<ContinuationRow>>());
    window->set_workspace_preset(preset);
    window->set_analysis_engine_names(
        std::make_shared<slint::VectorModel<slint::SharedString>>(std::vector<slint::SharedString> {slint::SharedString {"stockfish"}}));
    window->set_analysis_selected_engine(0);
    window->set_analysis_can_start(true);
    render(state);
    return window;
}

// One Browse button on one screen: where to find it and what surrounds it.
struct button_case
{
    char const* name;
    std::string_view label;
    // The text field the button belongs to, found by its placeholder.
    std::string_view field_placeholder;
    // What follows the button in its row: a text field (by placeholder) or a button (by label).
    std::string_view next_placeholder;
    std::string_view next_label;
    BrowseTarget target;
    std::function<std::string(WorkspaceWindow&)> read_field;
};

auto open_case() -> button_case
{
    return {.name = "open",
            .label = open_label,
            .field_placeholder = "Bundle directory path",
            .next_placeholder = "",
            .next_label = "Open",
            .target = BrowseTarget::DatabaseOpen,
            .read_field = [](WorkspaceWindow& window) -> std::string { return text_of(window.get_open_path_text()); }};
}

auto create_case() -> button_case
{
    return {.name = "create",
            .label = create_label,
            .field_placeholder = "Directory path",
            .next_placeholder = "Name",
            .next_label = "",
            .target = BrowseTarget::DatabaseCreate,
            .read_field = [](WorkspaceWindow& window) -> std::string { return text_of(window.get_create_path_text()); }};
}

auto import_case() -> button_case
{
    return {.name = "import",
            .label = import_label,
            .field_placeholder = "PGN file path",
            .next_placeholder = "",
            .next_label = "Import PGN",
            .target = BrowseTarget::ImportPgn,
            .read_field = [](WorkspaceWindow& window) -> std::string { return text_of(window.get_import_path_text()); }};
}

auto engine_case() -> button_case
{
    return {.name = "engine",
            .label = engine_label,
            .field_placeholder = "Engine path",
            .next_placeholder = "",
            .next_label = "Add",
            .target = BrowseTarget::Engine,
            .read_field = [](WorkspaceWindow& window) -> std::string { return text_of(window.get_analysis_engine_path_text()); }};
}

// A Browse button must be drawn in full: inside the window, with room to read,
// to the right of its field, level with it, and clear of what follows it.
// Catch2 assertion macros inflate this helper's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void check_visible(window_handle const& window, button_case const& item, window_size const size, float const minimum_width)
{
    INFO(item.name << " at " << size.width << "x" << size.height);
    auto const box = bounds_of(the_one(window, item.label));
    CHECK(box.left >= margin);
    CHECK(box.top >= 0.F);
    CHECK(box.right <= static_cast<float>(size.width) - margin);
    CHECK(box.bottom <= static_cast<float>(size.height) - margin);
    CHECK(box.width() >= minimum_width);
    CHECK(box.height() >= minimum_height);

    auto const field_box = bounds_of(the_field(window, item.field_placeholder));
    CHECK(field_box.right <= box.left + 0.5F);
    CHECK(std::abs(field_box.middle() - box.middle()) <= row_alignment_tolerance);
    CHECK(field_box.width() >= minimum_field_width);

    auto const next_box = [&window, &item]() -> std::optional<rect>
    {
        if (!item.next_placeholder.empty()) {
            return bounds_of(the_field(window, item.next_placeholder));
        }
        if (!item.next_label.empty()) {
            return bounds_of(the_one(window, item.next_label));
        }
        return std::nullopt;
    }();
    if (next_box) {
        CHECK(box.right <= next_box->left + 0.5F);
        CHECK(next_box->right <= static_cast<float>(size.width) - margin);
        if (!item.next_placeholder.empty()) {
            CHECK(next_box->width() >= minimum_field_width);
        }
    }
}

// Tabs until the button's field takes typed text, which only a focused field does.
auto focus_field(snapshot_state const& state, WorkspaceWindow& window, button_case const& item) -> bool
{
    constexpr auto max_tab_stops = 120;
    for (auto stop = 0; stop < max_tab_stops; ++stop) {
        press_key(state, window, slint::platform::key_codes::Tab);
        auto const before = item.read_field(window);
        press_key(state, window, u8"z");
        if (item.read_field(window) != before) {
            return true;
        }
    }
    return false;
}

// Everything the window hears from the controls around a Browse button.
struct heard
{
    std::vector<BrowseTarget> browse;
    std::vector<std::string> actions;
};

void listen(WorkspaceWindow& window, heard& log)
{
    window.on_browse_requested([&log](BrowseTarget const target) -> void { log.browse.push_back(target); });
    window.on_open_requested([&log](slint::SharedString const&) -> void { log.actions.emplace_back("open"); });
    window.on_import_requested([&log](slint::SharedString const&) -> void { log.actions.emplace_back("import"); });
    window.on_analysis_engine_added([&log](slint::SharedString const&) -> void { log.actions.emplace_back("add"); });
}

auto enabled(window_handle const& window, button_case const& item) -> std::optional<bool>
{
    auto const found = labelled(window, item.label);
    if (found.size() != 1) {
        return std::nullopt;
    }
    return found.front().accessible_enabled();
}

}  // namespace

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("browse buttons: the setup screen shows Open and Create in full at narrow and wide sizes", "[motif-slint-app][layout][picker]")
{
    for (auto const size : {narrow, wide}) {
        auto const state = use_snapshot_platform(size.width, size.height);
        auto window = make_setup_window(*state);
        check_visible(window, open_case(), size, full_width);
        check_visible(window, create_case(), size, full_width);
        // The create row keeps its action next to the second field.
        auto const name = bounds_of(the_field(window, "Name"));
        auto const action = bounds_of(the_one(window, "Create"));
        CHECK(name.right <= action.left + 0.5F);
        CHECK(action.right <= static_cast<float>(size.width) - margin);
    }
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("browse buttons: import and engine buttons are drawn in full in every preset at narrow and wide sizes",
          "[motif-slint-app][layout][picker]")
{
    for (auto const size : {narrow, wide}) {
        // The sidebar is about 346 px wide at the narrow size: the label shrinks to "…" below 1100 px.
        auto const minimum = size.width < 1100 ? compact_width : full_width;
        for (auto const preset : {WorkspacePreset::Review, WorkspacePreset::Explorer, WorkspacePreset::Analysis}) {
            INFO("preset " << static_cast<int>(preset));
            auto const state = use_snapshot_platform(size.width, size.height);
            auto window = make_workspace_window(*state, preset);
            check_visible(window, import_case(), size, minimum);
            if (preset == WorkspacePreset::Analysis) {
                check_visible(window, engine_case(), size, minimum);
            } else {
                CHECK(labelled(window, engine_label).empty());
            }
        }
    }
}

TEST_CASE("browse buttons: below 1100 px the sidebar buttons are compact and the label stays complete", "[motif-slint-app][layout][picker]")
{
    auto widths = std::vector<float> {};
    for (auto const size : {narrow, wide}) {
        auto const state = use_snapshot_platform(size.width, size.height);
        auto window = make_workspace_window(*state, WorkspacePreset::Review);
        auto const button = the_one(window, import_label);
        widths.push_back(bounds_of(button).width());
        CHECK(text_of(button.accessible_label().value_or(slint::SharedString {})) == import_label);
    }
    CHECK(widths.front() < widths.back());
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("browse buttons: every button has its own label and a description", "[motif-slint-app][layout][picker]")
{
    auto const state = use_snapshot_platform(wide.width, wide.height);
    auto setup = make_setup_window(*state);
    auto workspace = make_workspace_window(*state, WorkspacePreset::Analysis);
    for (auto const& [window, label] : {std::pair<std::reference_wrapper<window_handle>, std::string_view> {std::ref(setup), open_label},
                                        std::pair {std::ref(setup), create_label},
                                        std::pair {std::ref(workspace), import_label},
                                        std::pair {std::ref(workspace), engine_label}})
    {
        INFO(label);
        auto const button = the_one(window, label);
        auto const description = text_of(button.accessible_description().value_or(slint::SharedString {}));
        CHECK(description.contains("file dialog"));
    }
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("browse buttons: each follows its field in tab order and answers Enter and Space", "[motif-slint-app][layout][picker]")
{
    struct screen
    {
        char const* name;
        std::function<window_handle(snapshot_state const&)> make;
        button_case item;
        // What the control after the button does when it is activated with Enter, if it is a button.
        std::optional<std::string> next_action;
    };

    auto const screens = std::vector<screen> {
        {"setup", [](snapshot_state const& state) -> window_handle { return make_setup_window(state); }, open_case(), "open"},
        {"setup", [](snapshot_state const& state) -> window_handle { return make_setup_window(state); }, create_case(), std::nullopt},
        {"review",
         [](snapshot_state const& state) -> window_handle { return make_workspace_window(state, WorkspacePreset::Review); },
         import_case(),
         "import"},
        {"analysis",
         [](snapshot_state const& state) -> window_handle { return make_workspace_window(state, WorkspacePreset::Analysis); },
         engine_case(),
         "add"},
    };

    for (auto const size : {narrow, wide}) {
        for (auto const& item : screens) {
            INFO(item.name << " " << item.item.name << " at " << size.width << "x" << size.height);
            auto const state = use_snapshot_platform(size.width, size.height);
            auto window = item.make(*state);
            auto log = heard {};
            listen(*window, log);

            // The stop before the button is its field; nothing is requested on the way.
            REQUIRE(focus_field(*state, *window, item.item));
            CHECK(log.browse.empty());

            press_key(*state, *window, slint::platform::key_codes::Tab);
            press_key(*state, *window, slint::platform::key_codes::Return);
            CHECK(log.browse == std::vector<BrowseTarget> {item.item.target});
            press_key(*state, *window, slint::platform::key_codes::Space);
            CHECK(log.browse == std::vector<BrowseTarget> {item.item.target, item.item.target});

            // The action the field feeds is the next stop; it is not run by the Browse button.
            CHECK(log.actions.empty());
            if (item.next_action) {
                press_key(*state, *window, slint::platform::key_codes::Tab);
                press_key(*state, *window, slint::platform::key_codes::Return);
                CHECK(log.actions == std::vector<std::string> {*item.next_action});
            }
        }
    }
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("browse buttons: they are disabled while a dialog is open and never fire from the keyboard", "[motif-slint-app][layout][picker]")
{
    auto const state = use_snapshot_platform(wide.width, wide.height);
    auto setup = make_setup_window(*state);
    auto workspace = make_workspace_window(*state, WorkspacePreset::Analysis);

    for (auto const& [window, item] : {std::pair {std::ref(setup), open_case()},
                                       std::pair {std::ref(setup), create_case()},
                                       std::pair {std::ref(workspace), import_case()},
                                       std::pair {std::ref(workspace), engine_case()}})
    {
        INFO(item.name);
        auto& handle = window.get();
        CHECK(enabled(handle, item) == std::optional<bool> {true});
        handle->set_picker_open(true);
        CHECK(enabled(handle, item) == std::optional<bool> {false});

        auto log = heard {};
        listen(*handle, log);
        for (auto stop = 0; stop < 120; ++stop) {
            press_key(*state, *handle, slint::platform::key_codes::Tab);
            press_key(*state, *handle, slint::platform::key_codes::Return);
            press_key(*state, *handle, slint::platform::key_codes::Escape);
        }
        CHECK(log.browse.empty());

        handle->set_picker_open(false);
        CHECK(enabled(handle, item) == std::optional<bool> {true});
    }
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("browse buttons: each follows the enabled state of its own field", "[motif-slint-app][layout][picker]")
{
    auto const state = use_snapshot_platform(wide.width, wide.height);

    SECTION("setup screen: a busy workspace disables Open and Create")
    {
        auto window = make_setup_window(*state);
        window->set_busy(true);
        render(*state);
        CHECK(enabled(window, open_case()) == std::optional<bool> {false});
        CHECK(enabled(window, create_case()) == std::optional<bool> {false});
    }

    SECTION("import: workspace, browser and search busy each disable it, and a running import hides the row")
    {
        for (auto const &set_busy :
             std::vector<std::function<void(WorkspaceWindow&, bool)>> {
                 [](WorkspaceWindow& window, bool const value) -> void { window.set_busy(value); },
                 [](WorkspaceWindow& window, bool const value) -> void { window.set_browser_busy(value); },
                 [](WorkspaceWindow& window, bool const value) -> void { window.set_search_busy(value); }})
        {
            auto window = make_workspace_window(*state, WorkspacePreset::Review);
            CHECK(enabled(window, import_case()) == std::optional<bool> {true});
            set_busy(*window, true);
            render(*state);
            CHECK(enabled(window, import_case()) == std::optional<bool> {false});
            set_busy(*window, false);
            render(*state);
            CHECK(enabled(window, import_case()) == std::optional<bool> {true});
        }
        auto window = make_workspace_window(*state, WorkspacePreset::Review);
        window->set_import_active(true);
        render(*state);
        CHECK(labelled(window, import_label).empty());
    }

    SECTION("engine: a busy workspace and a running analysis disable it")
    {
        auto window = make_workspace_window(*state, WorkspacePreset::Analysis);
        CHECK(enabled(window, engine_case()) == std::optional<bool> {true});
        window->set_busy(true);
        render(*state);
        CHECK(enabled(window, engine_case()) == std::optional<bool> {false});
        window->set_busy(false);
        window->set_analysis_active(true);
        render(*state);
        CHECK(enabled(window, engine_case()) == std::optional<bool> {false});
    }
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("browse buttons: the status bar says a dialog may be hidden, and an error returns once it closes",
          "[motif-slint-app][layout][picker]")
{
    auto const state = use_snapshot_platform(narrow.width, narrow.height);
    auto window = make_workspace_window(*state, WorkspacePreset::Review);
    CHECK(labelled(window, hint_text).empty());

    window->set_picker_open(true);
    render(*state);
    CHECK(labelled(window, hint_text).size() == 1);

    window->set_error_text("Could not save the preset");
    render(*state);
    CHECK(labelled(window, hint_text).size() == 1);
    CHECK(labelled(window, "Could not save the preset").empty());
    // The hint has nothing to dismiss: the error's button is absent while the dialog is open.
    CHECK(labelled(window, "Dismiss").empty());

    window->set_picker_open(false);
    render(*state);
    CHECK(labelled(window, hint_text).empty());
    CHECK(labelled(window, "Could not save the preset").size() == 1);
    CHECK(labelled(window, "Dismiss").size() == 1);
}

namespace
{

// The event loop of this test: what the runner posts waits until the test runs it.
class posted_tasks
{
  public:
    [[nodiscard]] auto environment() -> motif::slint_app::event_loop_environment
    {
        return motif::slint_app::event_loop_environment {.schedule = [this](std::chrono::milliseconds, std::function<void()> task) -> void
                                                         {
                                                             std::scoped_lock const lock {mutex_};
                                                             tasks_.push_back(std::move(task));
                                                         }};
    }

    // Waits for the dialog's answer to be posted, then runs it.
    void run_when_posted()
    {
        REQUIRE(fake_uci::wait_for(
            [this]() -> bool
            {
                std::scoped_lock const lock {mutex_};
                return !tasks_.empty();
            }));
        auto batch = std::vector<std::function<void()>> {};
        {
            std::scoped_lock const lock {mutex_};
            batch.swap(tasks_);
        }
        for (auto& task : batch) {
            task();
        }
    }

  private:
    std::mutex mutex_;
    std::vector<std::function<void()>> tasks_;
};

}  // namespace

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("browse buttons: cancelling a dialog leaves keyboard focus and the field where they were", "[motif-slint-app][layout][picker]")
{
    using motif::slint_app::scripted_file_picker;
    constexpr auto patience = std::chrono::seconds {10};
    constexpr auto first_stop_after_field = slint::platform::key_codes::Tab;

    for (auto const size : {narrow, wide}) {
        INFO(size.width << "x" << size.height);
        auto const state = use_snapshot_platform(size.width, size.height);
        auto window = make_workspace_window(*state, WorkspacePreset::Review);
        auto picker = std::make_shared<scripted_file_picker>(
            scripted_file_picker::script {.behaviour = scripted_file_picker::mode::hang, .text = {}, .abortable = true});
        auto loop = posted_tasks {};
        auto runner = motif::slint_app::async_picker_runner {window, loop.environment(), picker};
        motif::slint_app::register_picker_callbacks(*window, runner);
        auto actions = std::vector<std::string> {};
        window->on_import_requested([&actions](slint::SharedString const&) -> void { actions.emplace_back("import"); });

        // Focus the import field, type, and move to the Browse button after it.
        REQUIRE(focus_field(*state, *window, import_case()));
        auto const typed = text_of(window->get_import_path_text());
        REQUIRE_FALSE(typed.empty());
        press_key(*state, *window, first_stop_after_field);

        // Enter opens the dialog; the button is disabled while it is open.
        press_key(*state, *window, slint::platform::key_codes::Return);
        REQUIRE(picker->wait_for_calls(1, patience));
        REQUIRE(window->get_picker_open());
        render(*state);
        CHECK(enabled(window, import_case()) == std::optional<bool> {false});

        // The user cancels.
        picker->finish(motif::slint_app::picker_outcome::cancelled());
        loop.run_when_posted();
        render(*state);

        CHECK_FALSE(window->get_picker_open());
        CHECK(enabled(window, import_case()) == std::optional<bool> {true});
        CHECK(text_of(window->get_import_path_text()) == typed);
        CHECK(text_of(window->get_error_text()).empty());
        CHECK(actions.empty());

        // Focus did not move: Space on the same button asks for a second dialog.
        press_key(*state, *window, slint::platform::key_codes::Space);
        CHECK(picker->wait_for_calls(2, patience));

        // Leave nothing blocked behind the test.
        picker->finish(motif::slint_app::picker_outcome::cancelled());
        runner.shutdown();
    }
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
