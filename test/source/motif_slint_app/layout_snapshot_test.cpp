#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <slint-platform.h>
#include <slint.h>

#include "snapshot_platform.hpp"
#include "workspace.h"

namespace
{
using snapshot_test::snapshot_platform;
using snapshot_test::snapshot_state;
using snapshot_test::use_snapshot_platform;

void write_ppm(std::filesystem::path const& directory, std::string const& name, std::span<slint::Rgb8Pixel const> pixels)
{
    std::filesystem::create_directories(directory);
    auto image = std::ofstream {directory / (name + ".ppm"), std::ios::binary | std::ios::trunc};
    image << "P6\n" << snapshot_platform::width << ' ' << snapshot_platform::height << "\n255\n";
    for (auto const pixel : pixels) {
        image.put(static_cast<char>(pixel.r));
        image.put(static_cast<char>(pixel.g));
        image.put(static_cast<char>(pixel.b));
    }
}

constexpr auto near_white_threshold = std::uint8_t {245};

constexpr auto is_visible_non_background_pixel(slint::Rgb8Pixel const pixel) -> bool
{
    return pixel.r < near_white_threshold || pixel.g < near_white_threshold || pixel.b < near_white_threshold;
}

constexpr auto rendered_pixel_threshold = std::uint8_t {250};

auto render_layout(snapshot_state const& state, std::string const& name) -> std::vector<slint::Rgb8Pixel>
{
    REQUIRE(state.window_adapter != nullptr);
    state.window_adapter->render();

    auto const pixels = state.window_adapter->pixels();
    REQUIRE(pixels.size() == static_cast<std::size_t>(snapshot_platform::width) * snapshot_platform::height);
    REQUIRE(std::ranges::any_of(
        pixels,
        [](slint::Rgb8Pixel const pixel) -> bool
        { return pixel.r < rendered_pixel_threshold || pixel.g < rendered_pixel_threshold || pixel.b < rendered_pixel_threshold; }));

    // NOLINTNEXTLINE(concurrency-mt-unsafe): the test binary is serial and only reads a caller-supplied path.
    if (char const* const output = std::getenv("MOTIF_LAYOUT_SNAPSHOT_DIR"); output != nullptr && *output != '\0') {
        write_ppm(output, name, pixels);
    }
    return {pixels.begin(), pixels.end()};
}

}  // namespace

constexpr auto recents_below_center = 120.F;
constexpr auto scroll_to_end_distance = 100000.F;
constexpr auto scroll_animation_settle = std::chrono::milliseconds {500};
constexpr auto clip_probe_columns = 8U;

TEST_CASE("workspace layout: renders right and below notation without a display server", "[motif-slint-app][layout]")
{
    auto const state = use_snapshot_platform();

    auto window = WorkspaceWindow::create();
    window->set_has_database(true);
    window->set_is_scratch(true);
    window->set_display_name("Scratch");
    window->set_game_rows(std::make_shared<slint::VectorModel<GameRow>>());
    window->set_game_column_widths(std::make_shared<slint::VectorModel<std::int32_t>>());
    window->set_board_square_pieces(std::make_shared<slint::VectorModel<slint::SharedString>>());
    window->set_board_square_highlights(std::make_shared<slint::VectorModel<SquareHighlight>>());
    window->set_board_notation(std::make_shared<slint::VectorModel<NotationToken>>());
    window->set_search_matches(std::make_shared<slint::VectorModel<SearchMatchRow>>());
    window->set_search_continuations(std::make_shared<slint::VectorModel<ContinuationRow>>());

    window->set_notation_below(false);
    auto const notation_right = render_layout(*state, "notation-right");

    window->set_notation_below(true);
    auto const notation_below = render_layout(*state, "notation-below");
    auto const mismatch = std::ranges::mismatch(notation_right,
                                                notation_below,
                                                [](slint::Rgb8Pixel const right, slint::Rgb8Pixel const below) -> bool
                                                { return right.r == below.r && right.g == below.g && right.b == below.b; });
    REQUIRE(mismatch.in1 != notation_right.end());
}

TEST_CASE("workspace layout: renders the setup screen without a database", "[motif-slint-app][layout]")
{
    auto const state = use_snapshot_platform();

    auto window = WorkspaceWindow::create();
    window->set_has_database(false);
    window->set_recent_databases(std::make_shared<slint::VectorModel<RecentEntry>>(std::vector<RecentEntry> {
        RecentEntry {.name = slint::SharedString {"Main"}, .path = slint::SharedString {"/tmp/main.motif"}, .available = true},
    }));

    auto const pixels = render_layout(*state, "setup-screen");
    REQUIRE(std::ranges::count_if(pixels, is_visible_non_background_pixel) > 5000);
}

namespace
{

auto make_recent_entries(int const count) -> std::shared_ptr<slint::VectorModel<RecentEntry>>
{
    auto entries = std::vector<RecentEntry> {};
    for (auto index = 0; index < count; ++index) {
        entries.push_back(RecentEntry {
            .name = slint::SharedString {("Database " + std::to_string(index)).c_str()},
            .path = slint::SharedString {("/data/chess/database-" + std::to_string(index)).c_str()},
            .available = index % 3 != 0,
        });
    }
    return std::make_shared<slint::VectorModel<RecentEntry>>(std::move(entries));
}

auto differs(std::vector<slint::Rgb8Pixel> const& lhs, std::vector<slint::Rgb8Pixel> const& rhs) -> bool
{
    return std::ranges::mismatch(lhs,
                                 rhs,
                                 [](slint::Rgb8Pixel const left, slint::Rgb8Pixel const right) -> bool
                                 { return left.r == right.r && left.g == right.g && left.b == right.b; })
               .in1
        != lhs.end();
}

}  // namespace

TEST_CASE("workspace layout: unavailable recents render differently from available ones", "[motif-slint-app][layout]")
{
    auto const state = use_snapshot_platform();
    auto window = WorkspaceWindow::create();
    window->set_has_database(false);

    window->set_recent_databases(std::make_shared<slint::VectorModel<RecentEntry>>(std::vector<RecentEntry> {
        RecentEntry {.name = slint::SharedString {"Main"}, .path = slint::SharedString {"/tmp/main.motif"}, .available = true},
    }));
    auto const available = render_layout(*state, "recent-available");

    window->set_recent_databases(std::make_shared<slint::VectorModel<RecentEntry>>(std::vector<RecentEntry> {
        RecentEntry {.name = slint::SharedString {"Main"}, .path = slint::SharedString {"/tmp/main.motif"}, .available = false},
    }));
    auto const unavailable = render_layout(*state, "recent-unavailable");

    REQUIRE(differs(available, unavailable));
}

TEST_CASE("workspace layout: a long recent list scrolls so later entries become reachable", "[motif-slint-app][layout]")
{
    constexpr auto recent_count = 20;
    auto const state = use_snapshot_platform();
    auto window = WorkspaceWindow::create();
    window->set_has_database(false);
    window->set_recent_databases(make_recent_entries(recent_count));

    auto const top = render_layout(*state, "recent-list-top");

    // The recents card is centered in the setup screen, so the window center is over the list.
    auto const center = slint::LogicalPosition {slint::Point<float> {
        .x = static_cast<float>(snapshot_platform::width) / 2.F,
        .y = (static_cast<float>(snapshot_platform::height) / 2.F) + recents_below_center,
    }};
    window->window().dispatch_pointer_move_event(center);
    window->window().dispatch_pointer_scroll_event(center, 0.F, -scroll_to_end_distance);
    // Scrolling is animated; let the animation clock run to completion.
    std::this_thread::sleep_for(scroll_animation_settle);
    slint::platform::update_timers_and_animations();
    auto const bottom = render_layout(*state, "recent-list-bottom");

    REQUIRE(differs(top, bottom));
}

TEST_CASE("workspace layout: renders the explorer sidebar tab", "[motif-slint-app][layout]")
{
    auto const state = use_snapshot_platform();

    auto window = WorkspaceWindow::create();
    window->set_has_database(true);
    window->set_is_scratch(true);
    window->set_display_name("Scratch");
    window->set_workspace_preset(WorkspacePreset::Explorer);
    window->set_game_rows(std::make_shared<slint::VectorModel<GameRow>>());
    window->set_game_column_widths(std::make_shared<slint::VectorModel<std::int32_t>>());
    window->set_board_square_pieces(std::make_shared<slint::VectorModel<slint::SharedString>>());
    window->set_board_square_highlights(std::make_shared<slint::VectorModel<SquareHighlight>>());
    window->set_board_notation(std::make_shared<slint::VectorModel<NotationToken>>());
    window->set_search_matches(std::make_shared<slint::VectorModel<SearchMatchRow>>());
    window->set_search_continuations(std::make_shared<slint::VectorModel<ContinuationRow>>());
    constexpr auto sample_total_games = 42;
    window->set_search_total_games(sample_total_games);

    auto const pixels = render_layout(*state, "explorer-tab");
    REQUIRE(std::ranges::count_if(pixels, is_visible_non_background_pixel) > 5000);
}

namespace
{

struct board_footprint
{
    int left {0};
    int top {0};
    int right {0};
    int bottom {0};
    bool found {false};
};

constexpr auto light_square_r = std::uint8_t {0xf0};
constexpr auto light_square_g = std::uint8_t {0xd9};
constexpr auto light_square_b = std::uint8_t {0xb5};
constexpr auto dark_square_r = std::uint8_t {0xb5};
constexpr auto dark_square_g = std::uint8_t {0x88};
constexpr auto dark_square_b = std::uint8_t {0x63};
constexpr auto square_color_tolerance = 6;

constexpr auto is_board_pixel(slint::Rgb8Pixel const pixel) -> bool
{
    auto const near = [](std::uint8_t const value, int const target, int const tolerance) -> bool
    { return std::abs(static_cast<int>(value) - target) <= tolerance; };
    // Light squares #f0d9b5 and dark squares #b58863 (board_notation.slint).
    return (near(pixel.r, light_square_r, square_color_tolerance) && near(pixel.g, light_square_g, square_color_tolerance)
            && near(pixel.b, light_square_b, square_color_tolerance))
        || (near(pixel.r, dark_square_r, square_color_tolerance) && near(pixel.g, dark_square_g, square_color_tolerance)
            && near(pixel.b, dark_square_b, square_color_tolerance));
}

auto board_bounding_box(std::span<slint::Rgb8Pixel const> pixels, std::uint32_t const width) -> board_footprint
{
    auto const height = static_cast<std::uint32_t>(pixels.size() / width);
    auto box = board_footprint {};
    for (auto row = 0U; row < height; ++row) {
        for (auto col = 0U; col < width; ++col) {
            if (!is_board_pixel(pixels[(static_cast<std::size_t>(row) * width) + col])) {
                continue;
            }
            if (!box.found) {
                box.left = static_cast<int>(col);
                box.right = static_cast<int>(col);
                box.top = static_cast<int>(row);
                box.bottom = static_cast<int>(row);
                box.found = true;
                continue;
            }
            box.left = std::min(box.left, static_cast<int>(col));
            box.right = std::max(box.right, static_cast<int>(col));
            box.top = std::min(box.top, static_cast<int>(row));
            box.bottom = std::max(box.bottom, static_cast<int>(row));
        }
    }
    return box;
}

// Notation tokens for a main line of `count` plies, numbered the PGN way.
// Node ids start at 1 because node 0 is the starting position.
auto make_moves(int const count) -> std::shared_ptr<slint::VectorModel<NotationToken>>
{
    constexpr auto sample = std::array<std::string_view, 3> {"Nbxd7+", "e4", "Nf6"};
    auto moves = std::vector<NotationToken> {};
    moves.reserve(static_cast<std::size_t>(count));
    for (auto i = 0; i < count; ++i) {
        moves.push_back(NotationToken {
            .lead = slint::SharedString {i % 2 == 0 ? (std::to_string((i / 2) + 1) + ".").c_str() : ""},
            .san = slint::SharedString {sample.at(static_cast<std::size_t>(i) % sample.size())},
            .trail = slint::SharedString {},
            .node = i + 1,
            .depth = 0,
        });
    }
    return std::make_shared<slint::VectorModel<NotationToken>>(std::move(moves));
}

auto render_at_size(snapshot_state const& state, std::uint32_t const width) -> std::vector<slint::Rgb8Pixel>
{
    REQUIRE(state.window_adapter != nullptr);
    state.window_adapter->render();
    auto const pixels = state.window_adapter->pixels();
    REQUIRE(pixels.size() % width == 0);
    return {pixels.begin(), pixels.end()};
}

}  // namespace

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: board footprint is invariant under notation content", "[motif-slint-app][layout]")
{
    auto const state = use_snapshot_platform();

    auto window = WorkspaceWindow::create();
    window->set_has_database(true);
    window->set_is_scratch(true);
    window->set_display_name("Scratch");
    window->set_game_rows(std::make_shared<slint::VectorModel<GameRow>>());
    window->set_game_column_widths(std::make_shared<slint::VectorModel<std::int32_t>>());
    window->set_board_square_pieces(std::make_shared<slint::VectorModel<slint::SharedString>>());
    window->set_board_square_highlights(std::make_shared<slint::VectorModel<SquareHighlight>>());
    window->set_search_matches(std::make_shared<slint::VectorModel<SearchMatchRow>>());
    window->set_search_continuations(std::make_shared<slint::VectorModel<ContinuationRow>>());

    for (auto const notation_below : {false, true}) {
        window->set_notation_below(notation_below);
        auto footprints = std::vector<board_footprint> {};
        for (auto const move_count : {0, 30, 300}) {
            window->set_board_notation(make_moves(move_count));
            window->set_board_white_name("Averylongplayernamethatcouldwrap White");
            window->set_board_black_name("Averylongplayernamethatcouldwrap Black");
            window->set_board_event_name("Long event name that might also wrap when narrow");
            window->set_board_result("1-0");
            auto const name = std::string {"board-footprint-"} + (notation_below ? "below-" : "right-") + std::to_string(move_count);
            footprints.push_back(board_bounding_box(render_layout(*state, name), snapshot_platform::width));
        }

        REQUIRE(footprints[0].found);
        for (auto i = 1U; i < footprints.size(); ++i) {
            REQUIRE(footprints[i].found);
            REQUIRE(footprints[i].left == footprints[0].left);
            REQUIRE(footprints[i].top == footprints[0].top);
            REQUIRE(footprints[i].right == footprints[0].right);
            REQUIRE(footprints[i].bottom == footprints[0].bottom);
        }
    }
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: board footprint is invariant under notation content at a maximized window size", "[motif-slint-app][layout]")
{
    // Reproduces a live-session report: 2518x1377 logical matches a maximized
    // 1650x1050-DPI-scaled desktop window, a very different aspect ratio from
    // the 1440x760 default used by the other snapshot tests in this file.
    constexpr std::uint32_t maximized_width {2518};
    constexpr std::uint32_t maximized_height {1377};

    auto const state = use_snapshot_platform(maximized_width, maximized_height);

    auto window = WorkspaceWindow::create();
    window->set_has_database(true);
    window->set_is_scratch(true);
    window->set_display_name("Scratch");
    window->set_game_rows(std::make_shared<slint::VectorModel<GameRow>>());
    window->set_game_column_widths(std::make_shared<slint::VectorModel<std::int32_t>>());
    window->set_board_square_pieces(std::make_shared<slint::VectorModel<slint::SharedString>>());
    window->set_board_square_highlights(std::make_shared<slint::VectorModel<SquareHighlight>>());
    window->set_search_matches(std::make_shared<slint::VectorModel<SearchMatchRow>>());
    window->set_search_continuations(std::make_shared<slint::VectorModel<ContinuationRow>>());

    for (auto const notation_below : {false, true}) {
        window->set_notation_below(notation_below);
        auto footprints = std::vector<board_footprint> {};
        for (auto const move_count : {0, 30, 300}) {
            window->set_board_notation(make_moves(move_count));
            window->set_board_white_name("Averylongplayernamethatcouldwrap White");
            window->set_board_black_name("Averylongplayernamethatcouldwrap Black");
            window->set_board_event_name("Long event name that might also wrap when narrow");
            window->set_board_result("1-0");
            footprints.push_back(board_bounding_box(render_at_size(*state, maximized_width), maximized_width));
        }

        REQUIRE(footprints[0].found);
        for (auto i = 1U; i < footprints.size(); ++i) {
            REQUIRE(footprints[i].found);
            REQUIRE(footprints[i].left == footprints[0].left);
            REQUIRE(footprints[i].top == footprints[0].top);
            REQUIRE(footprints[i].right == footprints[0].right);
            REQUIRE(footprints[i].bottom == footprints[0].bottom);
        }
    }
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: board footprint is invariant under the loaded-game sidebar title", "[motif-slint-app][layout]")
{
    // Regression test: the "Loaded: <white> - <black> <result>" status text
    // in the game browser's pagination row (game_browser.slint) previously had
    // no width/overflow constraint. Its natural preferred width scaled with
    // player-name length and leaked through the sidebar's pinned width up into
    // the workspace's HorizontalLayout, stealing space from GameReviewPanel
    // (and therefore the board) every time a game with different player-name
    // lengths was opened. Reproduced live at a maximized 2518x1377 window:
    // GameReviewPanel measured 1742px wide with a short/absent title and
    // 1733px with a long one -- the gap does not show at this file's smaller
    // default snapshot size because GameReviewPanel is already pinned near
    // its 620px floor there, so a maximized size is required to reproduce it.
    constexpr std::uint32_t maximized_width {2518};
    constexpr std::uint32_t maximized_height {1377};

    auto const state = use_snapshot_platform(maximized_width, maximized_height);

    auto window = WorkspaceWindow::create();
    window->set_has_database(true);
    window->set_is_scratch(true);
    window->set_display_name("Scratch");
    window->set_game_rows(std::make_shared<slint::VectorModel<GameRow>>());
    window->set_game_column_widths(std::make_shared<slint::VectorModel<std::int32_t>>());
    window->set_board_square_pieces(std::make_shared<slint::VectorModel<slint::SharedString>>());
    window->set_board_square_highlights(std::make_shared<slint::VectorModel<SquareHighlight>>());
    window->set_board_notation(std::make_shared<slint::VectorModel<NotationToken>>());
    window->set_search_matches(std::make_shared<slint::VectorModel<SearchMatchRow>>());
    window->set_search_continuations(std::make_shared<slint::VectorModel<ContinuationRow>>());

    auto footprints = std::vector<board_footprint> {};
    for (auto const* const title : {"", "Ab, C - De, F  1-0", "Averylongfirstname, Averylongsurname - Anotherlongname, X  0-1"}) {
        window->set_game_active_title(slint::SharedString {title});
        footprints.push_back(board_bounding_box(render_at_size(*state, maximized_width), maximized_width));
    }

    REQUIRE(footprints[0].found);
    for (auto i = 1U; i < footprints.size(); ++i) {
        REQUIRE(footprints[i].found);
        REQUIRE(footprints[i].left == footprints[0].left);
        REQUIRE(footprints[i].top == footprints[0].top);
        REQUIRE(footprints[i].right == footprints[0].right);
        REQUIRE(footprints[i].bottom == footprints[0].bottom);
    }
}

TEST_CASE("workspace layout: arrow-key game navigation works immediately after a game loads, without a prior board click",
          "[motif-slint-app][layout]")
{
    // Regression test: GameReviewPanel's keyboard-scope FocusScope (which
    // owns Left/Right/Home/End game navigation) previously only received
    // focus reactively, from inside board-square and notation-move click
    // handlers. Loading a game via the game browser never called it, so
    // arrow-key navigation silently did nothing until the board or a move
    // was clicked at least once -- a keyboard-only-use defect matching
    // AC-9-5-2's still-open keyboard-only check.
    auto const state = use_snapshot_platform();

    auto window = WorkspaceWindow::create();
    window->set_has_database(true);
    window->set_is_scratch(true);
    window->set_display_name("Scratch");
    window->set_game_rows(std::make_shared<slint::VectorModel<GameRow>>());
    window->set_game_column_widths(std::make_shared<slint::VectorModel<std::int32_t>>());
    window->set_board_square_pieces(std::make_shared<slint::VectorModel<slint::SharedString>>());
    window->set_board_square_highlights(std::make_shared<slint::VectorModel<SquareHighlight>>());
    window->set_board_notation(std::make_shared<slint::VectorModel<NotationToken>>());
    window->set_search_matches(std::make_shared<slint::VectorModel<SearchMatchRow>>());
    window->set_search_continuations(std::make_shared<slint::VectorModel<ContinuationRow>>());

    auto navigate_next_calls = 0;
    window->on_board_navigate_next([&navigate_next_calls]() -> void { ++navigate_next_calls; });

    // No game loaded yet: render once so the item tree and window adapter exist.
    render_layout(*state, "arrow-key-before-load");

    // Simulate the game browser activating a game -- board_loaded flips true
    // -- WITHOUT any prior click on the board or a notation move. This is
    // exactly what "select a row, click Open Game" or a double-click
    // produces; a keyboard-only or freshly opened session never clicks the
    // board first.
    window->set_board_notation(make_moves(2));
    window->set_board_loaded(true);
    window->set_board_load_serial(1);
    render_layout(*state, "arrow-key-after-load");

    window->window().dispatch_key_press_event(slint::SharedString {slint::platform::key_codes::RightArrow});
    window->window().dispatch_key_release_event(slint::SharedString {slint::platform::key_codes::RightArrow});

    REQUIRE(navigate_next_calls == 1);
}

TEST_CASE("workspace layout: arrow-key game navigation focuses after interaction re-enables", "[motif-slint-app][layout]")
{
    auto const state = use_snapshot_platform();

    auto window = WorkspaceWindow::create();
    window->set_has_database(true);
    window->set_is_scratch(true);
    window->set_display_name("Scratch");
    window->set_game_rows(std::make_shared<slint::VectorModel<GameRow>>());
    window->set_game_column_widths(std::make_shared<slint::VectorModel<std::int32_t>>());
    window->set_board_square_pieces(std::make_shared<slint::VectorModel<slint::SharedString>>());
    window->set_board_square_highlights(std::make_shared<slint::VectorModel<SquareHighlight>>());
    window->set_board_notation(std::make_shared<slint::VectorModel<NotationToken>>());
    window->set_search_matches(std::make_shared<slint::VectorModel<SearchMatchRow>>());
    window->set_search_continuations(std::make_shared<slint::VectorModel<ContinuationRow>>());

    auto navigate_next_calls = 0;
    window->on_board_navigate_next([&navigate_next_calls]() -> void { ++navigate_next_calls; });

    render_layout(*state, "arrow-key-disabled-before-load");
    window->set_import_active(true);
    window->set_board_loaded(true);
    window->set_board_load_serial(1);
    render_layout(*state, "arrow-key-disabled-after-load");
    window->set_import_active(false);
    render_layout(*state, "arrow-key-after-enable");

    window->window().dispatch_key_press_event(slint::SharedString {slint::platform::key_codes::RightArrow});
    window->window().dispatch_key_release_event(slint::SharedString {slint::platform::key_codes::RightArrow});

    REQUIRE(navigate_next_calls == 1);
}

namespace
{

struct pixel_bounds
{
    int left {0};
    int top {0};
    int right {0};
    int bottom {0};
    bool found {false};

    [[nodiscard]] auto width() const -> int { return found ? right - left + 1 : 0; }

    [[nodiscard]] auto height() const -> int { return found ? bottom - top + 1 : 0; }
};

auto difference_bounds(std::span<slint::Rgb8Pixel const> lhs, std::span<slint::Rgb8Pixel const> rhs, std::uint32_t const width)
    -> pixel_bounds
{
    auto bounds = pixel_bounds {};
    for (auto index = std::size_t {0}; index < lhs.size() && index < rhs.size(); ++index) {
        if (lhs[index].r == rhs[index].r && lhs[index].g == rhs[index].g && lhs[index].b == rhs[index].b) {
            continue;
        }
        auto const col = static_cast<int>(index % width);
        auto const row = static_cast<int>(index / width);
        if (!bounds.found) {
            bounds = pixel_bounds {.left = col, .top = row, .right = col, .bottom = row, .found = true};
            continue;
        }
        bounds.left = std::min(bounds.left, col);
        bounds.top = std::min(bounds.top, row);
        bounds.right = std::max(bounds.right, col);
        bounds.bottom = std::max(bounds.bottom, row);
    }
    return bounds;
}

constexpr auto typical_games = 30;
constexpr auto typical_moves = 20;
constexpr auto few_games = 5;

auto make_game_rows(int const count) -> std::shared_ptr<slint::VectorModel<GameRow>>
{
    auto rows = std::vector<GameRow> {};
    rows.reserve(static_cast<std::size_t>(count));
    for (auto i = 0; i < count; ++i) {
        rows.push_back(GameRow {
            .white = slint::SharedString {("Player" + std::to_string(i) + "LongName").c_str()},
            .black = slint::SharedString {"Opponent"},
            .result = slint::SharedString {"1-0"},
            .event = slint::SharedString {"Test"},
            .date = slint::SharedString {"2020.01.02"},
            .eco = slint::SharedString {"C20"},
        });
    }
    return std::make_shared<slint::VectorModel<GameRow>>(std::move(rows));
}

// What a populated workspace shows: game-list rows and moves in the loaded game.
struct workspace_contents
{
    int games {typical_games};
    int moves {typical_moves};
};

// A workspace with an open database, a loaded game, and a populated game list.
auto make_loaded_workspace(workspace_contents const contents) -> slint::ComponentHandle<WorkspaceWindow>
{
    auto window = WorkspaceWindow::create();
    window->set_has_database(true);
    window->set_is_scratch(true);
    window->set_display_name("Scratch");
    window->set_game_rows(make_game_rows(contents.games));
    window->set_game_total_count(contents.games);
    window->set_game_column_widths(std::make_shared<slint::VectorModel<std::int32_t>>());
    window->set_board_square_pieces(std::make_shared<slint::VectorModel<slint::SharedString>>());
    window->set_board_square_highlights(std::make_shared<slint::VectorModel<SquareHighlight>>());
    window->set_board_notation(make_moves(contents.moves));
    window->set_search_matches(std::make_shared<slint::VectorModel<SearchMatchRow>>());
    window->set_search_continuations(std::make_shared<slint::VectorModel<ContinuationRow>>());
    window->set_board_white_name("Player02LongName");
    window->set_board_black_name("Opponent02");
    window->set_board_event_name("Test 2");
    window->set_board_date("2020.01.03");
    return window;
}

void press_key(WorkspaceWindow& window, std::u8string_view const key)
{
    window.window().dispatch_key_press_event(slint::SharedString {key});
    window.window().dispatch_key_release_event(slint::SharedString {key});
}

// Conditional elements are rebuilt lazily, so render between key presses to
// match what a running event loop does.
void press_key_and_render(snapshot_state const& state, WorkspaceWindow& window, std::u8string_view const key)
{
    press_key(window, key);
    state.window_adapter->render();
}

}  // namespace

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: notation keeps usable room for moves in both layouts", "[motif-slint-app][layout]")
{
    // Reproduces the 2026-10-03 compositor findings: notation was unusable at
    // the 960x560 minimum and degenerate beside the board at 1280x800.
    struct window_case
    {
        std::uint32_t width;
        std::uint32_t height;
        bool notation_below;
    };

    constexpr auto minimum_notation_width = 100;
    constexpr auto minimum_notation_height = 60;
    constexpr auto moves = 70;

    for (auto const& size : {window_case {.width = 960, .height = 560, .notation_below = false},
                             window_case {.width = 960, .height = 560, .notation_below = true},
                             window_case {.width = 1280, .height = 800, .notation_below = false},
                             window_case {.width = 1280, .height = 800, .notation_below = true}})
    {
        INFO("size " << size.width << "x" << size.height << (size.notation_below ? " notation below" : " notation beside"));
        auto const state = use_snapshot_platform(size.width, size.height);
        auto window = make_loaded_workspace({.games = typical_games, .moves = 0});
        window->set_board_loaded(true);
        window->set_notation_below(size.notation_below);

        auto const without_moves = render_at_size(*state, size.width);
        window->set_board_notation(make_moves(moves));
        auto const with_moves = render_at_size(*state, size.width);

        // Only the move list differs between the two renders, so its pixel
        // extent is the room the notation actually gave the moves.
        auto const bounds = difference_bounds(without_moves, with_moves, size.width);
        REQUIRE(bounds.found);
        CHECK(bounds.width() >= minimum_notation_width);
        CHECK(bounds.height() >= minimum_notation_height);
    }
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: sidebar controls are not clipped at the right window edge", "[motif-slint-app][layout]")
{
    // Open Game was clipped at 960x560 and 1100x700 because the games footer
    // forced the sidebar wider than its allocation. The window's right margin
    // stays plain background while every control fits.
    constexpr auto margin = 8;

    struct window_case
    {
        std::uint32_t width;
        std::uint32_t height;
    };

    for (auto const& size : {window_case {.width = 960, .height = 560}, window_case {.width = 1100, .height = 700}}) {
        for (auto const preset : {WorkspacePreset::Review, WorkspacePreset::Explorer, WorkspacePreset::Analysis}) {
            INFO("size " << size.width << "x" << size.height << " preset " << static_cast<int>(preset));
            auto const state = use_snapshot_platform(size.width, size.height);
            auto window = make_loaded_workspace({});
            window->set_game_selected_row(2);
            window->set_game_has_next_page(true);
            window->set_game_active_title("Averylongfirstname, Averylongsurname - Anotherlongname, X  0-1");
            window->set_board_loaded(true);
            window->set_workspace_preset(preset);

            auto const pixels = render_at_size(*state, size.width);
            auto const reference = pixels[size.width - 1];
            auto clipped = 0;
            for (auto row = 0U; row < size.height; ++row) {
                for (auto col = size.width - margin; col < size.width; ++col) {
                    auto const pixel = pixels[(static_cast<std::size_t>(row) * size.width) + col];
                    clipped += (pixel.r != reference.r || pixel.g != reference.g || pixel.b != reference.b) ? 1 : 0;
                }
            }
            CHECK(clipped == 0);
        }
    }
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: a replay button keeps keyboard focus after it is activated", "[motif-slint-app][layout]")
{
    // The review panel pulled focus to an invisible scope after every replay
    // action: directly, and again whenever interaction re-enabled after the
    // auto-search that follows each navigation. A second Return then did
    // nothing and focus appeared lost.
    auto const state = use_snapshot_platform();
    auto window = make_loaded_workspace({});
    window->set_board_loaded(true);
    window->set_board_load_serial(1);

    auto next_calls = 0;
    window->on_board_navigate_next([&next_calls]() -> void { ++next_calls; });
    state->window_adapter->render();

    constexpr auto max_tab_stops = 80;
    for (auto stop = 0; stop < max_tab_stops && next_calls == 0; ++stop) {
        press_key_and_render(*state, *window, slint::platform::key_codes::Tab);
        press_key_and_render(*state, *window, slint::platform::key_codes::Return);
    }
    REQUIRE(next_calls == 1);

    for (auto activation = 2; activation <= 3; ++activation) {
        // Auto-search after a navigation briefly disables interaction.
        window->set_search_busy(true);
        state->window_adapter->render();
        window->set_search_busy(false);
        state->window_adapter->render();
        press_key_and_render(*state, *window, slint::platform::key_codes::Return);
        CHECK(next_calls == activation);
    }
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: the board visibility button keeps keyboard focus across hide and show", "[motif-slint-app][layout]")
{
    // Hide Board and Show Board used to be two conditional buttons, so toggling
    // destroyed the focused element and the next key press went nowhere.
    auto const state = use_snapshot_platform();
    auto window = make_loaded_workspace({});
    window->set_board_loaded(true);

    auto toggles = 0;
    window->on_board_panel_visibility_toggled(
        [&window, &toggles]() -> void
        {
            ++toggles;
            window->set_board_panel_visible(!window->get_board_panel_visible());
        });
    state->window_adapter->render();

    constexpr auto max_tab_stops = 80;
    for (auto stop = 0; stop < max_tab_stops && toggles == 0; ++stop) {
        press_key_and_render(*state, *window, slint::platform::key_codes::Tab);
        press_key_and_render(*state, *window, slint::platform::key_codes::Return);
    }
    REQUIRE(toggles == 1);
    CHECK_FALSE(window->get_board_panel_visible());

    press_key_and_render(*state, *window, slint::platform::key_codes::Return);
    CHECK(toggles == 2);
    CHECK(window->get_board_panel_visible());
    press_key_and_render(*state, *window, slint::platform::key_codes::Return);
    CHECK(toggles == 3);
    CHECK_FALSE(window->get_board_panel_visible());
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: the game table is one tab stop that moves by row, page, and end", "[motif-slint-app][layout]")
{
    constexpr auto game_count = 60;
    auto const state = use_snapshot_platform();
    auto window = make_loaded_workspace({.games = game_count, .moves = 0});

    // Mirror the presenter: requested rows are published back to the window.
    auto selected = std::vector<int> {};
    auto deltas = std::vector<int> {};
    window->on_game_selected(
        [&selected, &window](int const row) -> void
        {
            selected.push_back(row);
            window->set_game_selected_row(row);
        });
    window->on_game_move_selection(
        [&deltas, &window](int const delta) -> void
        {
            deltas.push_back(delta);
            window->set_game_selected_row(std::clamp(window->get_game_selected_row() + delta, 0, game_count - 1));
        });
    state->window_adapter->render();

    // With nothing selected, Down starts at the first row rather than skipping it.
    constexpr auto max_tab_stops = 80;
    for (auto stop = 0; stop < max_tab_stops && selected.empty(); ++stop) {
        press_key_and_render(*state, *window, slint::platform::key_codes::Tab);
        press_key_and_render(*state, *window, slint::platform::key_codes::DownArrow);
    }
    REQUIRE(selected.size() == 1);
    CHECK(selected.front() == 0);
    auto const top = render_at_size(*state, snapshot_platform::width);

    press_key_and_render(*state, *window, slint::platform::key_codes::PageDown);
    REQUIRE(deltas.size() == 1);
    CHECK(deltas.back() > 1);
    press_key_and_render(*state, *window, slint::platform::key_codes::PageUp);
    REQUIRE(deltas.size() == 2);
    CHECK(deltas.back() < -1);
    CHECK(window->get_game_selected_row() == 0);

    // End selects the last row and scrolls it into view; Home must scroll back.
    press_key_and_render(*state, *window, slint::platform::key_codes::End);
    REQUIRE(selected.size() == 2);
    CHECK(selected.back() == game_count - 1);
    CHECK(differs(top, render_at_size(*state, snapshot_platform::width)));
    press_key_and_render(*state, *window, slint::platform::key_codes::Home);
    REQUIRE(selected.size() == 3);
    CHECK(selected.back() == 0);
    CHECK_FALSE(differs(top, render_at_size(*state, snapshot_platform::width)));

    // One tab stop: the next Tab leaves the table, so Down no longer selects.
    press_key_and_render(*state, *window, slint::platform::key_codes::Tab);
    press_key_and_render(*state, *window, slint::platform::key_codes::DownArrow);
    CHECK(selected.size() == 3);
    CHECK(deltas.size() == 2);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: a focused notation move keeps focus when the host republishes the move list", "[motif-slint-app][layout]")
{
    // The host replaces the whole move list with a new model on every
    // navigation. That rebuilt every notation token and silently dropped
    // keyboard focus from the move being operated.
    constexpr auto moves = 20;
    auto const state = use_snapshot_platform();
    auto window = make_loaded_workspace({.games = few_games, .moves = moves});
    window->set_board_loaded(true);
    window->set_board_load_serial(1);

    auto nodes = std::vector<int> {};
    auto next_calls = 0;
    window->on_board_node_activated([&nodes](int const node) -> void { nodes.push_back(node); });
    window->on_board_navigate_next([&next_calls]() -> void { ++next_calls; });
    state->window_adapter->render();

    // Tab until a notation token takes focus and activates.
    constexpr auto max_tab_stops = 80;
    for (auto stop = 0; stop < max_tab_stops && nodes.empty(); ++stop) {
        press_key_and_render(*state, *window, slint::platform::key_codes::Tab);
        press_key_and_render(*state, *window, slint::platform::key_codes::Return);
    }
    REQUIRE(nodes.size() == 1);

    for (auto activation = 2; activation <= 3; ++activation) {
        window->set_board_notation(make_moves(moves));
        window->set_board_current_node(activation);
        window->set_search_busy(true);
        state->window_adapter->render();
        window->set_search_busy(false);
        state->window_adapter->render();
        press_key_and_render(*state, *window, slint::platform::key_codes::Return);
        CHECK(nodes.size() == static_cast<std::size_t>(activation));
    }

    // Keys the token does not use still reach the review region's handler.
    auto const before_arrow = next_calls;
    press_key_and_render(*state, *window, slint::platform::key_codes::RightArrow);
    CHECK(next_calls == before_arrow + 1);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: a focused notation move keeps focus when the window is resized", "[motif-slint-app][layout]")
{
    // Resizing changes how many moves fit per row. That used to rebuild the
    // row repeaters, destroying the focused token.
    constexpr std::uint32_t narrow_width {960};
    constexpr std::uint32_t narrow_height {560};
    constexpr std::uint32_t wide_width {1600};
    constexpr std::uint32_t wide_height {900};
    constexpr auto late_node = 15;
    auto const state = use_snapshot_platform(narrow_width, narrow_height);
    auto window = make_loaded_workspace({});
    window->set_board_loaded(true);
    window->set_board_load_serial(1);

    auto nodes = std::vector<int> {};
    window->on_board_node_activated([&nodes](int const node) -> void { nodes.push_back(node); });
    state->window_adapter->render();

    constexpr auto max_tab_stops = 120;
    for (auto stop = 0; stop < max_tab_stops && (nodes.empty() || nodes.back() < late_node); ++stop) {
        press_key_and_render(*state, *window, slint::platform::key_codes::Tab);
        press_key_and_render(*state, *window, slint::platform::key_codes::Return);
    }
    REQUIRE(!nodes.empty());
    REQUIRE(nodes.back() >= late_node);
    auto const focused_node = nodes.back();
    auto const before = nodes.size();

    window->window().set_size(slint::PhysicalSize {slint::Size<std::uint32_t> {.width = wide_width, .height = wide_height}});
    state->window_adapter->render();
    press_key_and_render(*state, *window, slint::platform::key_codes::Return);
    REQUIRE(nodes.size() == before + 1);
    CHECK(nodes.back() == focused_node);
}

TEST_CASE("workspace layout: a game loaded while the board is hidden takes focus when the board is shown", "[motif-slint-app][layout]")
{
    auto const state = use_snapshot_platform();
    auto window = make_loaded_workspace({});
    window->set_board_loaded(true);
    window->set_board_load_serial(1);
    auto next_calls = 0;
    window->on_board_navigate_next([&next_calls]() -> void { ++next_calls; });
    state->window_adapter->render();

    window->set_board_panel_visible(false);
    state->window_adapter->render();
    window->set_board_load_serial(2);
    state->window_adapter->render();
    window->set_board_panel_visible(true);
    state->window_adapter->render();

    press_key_and_render(*state, *window, slint::platform::key_codes::RightArrow);
    CHECK(next_calls == 1);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: toolbar presets are keyboard reachable and report each selection", "[motif-slint-app][layout]")
{
    auto const state = use_snapshot_platform();
    auto window = make_loaded_workspace({});
    auto selected = std::vector<WorkspacePreset> {};
    window->on_preset_selected([&selected](WorkspacePreset const preset) -> void { selected.push_back(preset); });
    state->window_adapter->render();
    REQUIRE(window->get_workspace_preset() == WorkspacePreset::Review);

    // Activate each of the first tab stops (the toolbar). Only the preset buttons
    // report a selection, so the callback log records which stops were presets,
    // their order, and that the window follows every selection.
    constexpr auto toolbar_stops = 8;
    for (auto stop = 0; stop < toolbar_stops; ++stop) {
        press_key_and_render(*state, *window, slint::platform::key_codes::Tab);
        auto const before = selected.size();
        press_key_and_render(*state, *window, slint::platform::key_codes::Return);
        if (selected.size() != before) {
            REQUIRE(selected.size() == before + 1);
            CHECK(window->get_workspace_preset() == selected.back());
        }
    }

    // Each preset is individually reachable exactly once, in toolbar order.
    REQUIRE(selected.size() == 3);
    CHECK(selected[0] == WorkspacePreset::Review);
    CHECK(selected[1] == WorkspacePreset::Explorer);
    CHECK(selected[2] == WorkspacePreset::Analysis);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: each preset renders a distinct composition at narrow and wide sizes", "[motif-slint-app][layout]")
{
    struct window_case
    {
        std::uint32_t width;
        std::uint32_t height;
    };

    for (auto const& size : {window_case {.width = 960, .height = 560}, window_case {.width = 1600, .height = 900}}) {
        auto renders = std::vector<std::vector<slint::Rgb8Pixel>> {};
        for (auto const preset : {WorkspacePreset::Review, WorkspacePreset::Explorer, WorkspacePreset::Analysis}) {
            INFO("size " << size.width << "x" << size.height << " preset " << static_cast<int>(preset));
            auto const state = use_snapshot_platform(size.width, size.height);
            auto window = make_loaded_workspace({});
            window->set_board_loaded(true);
            window->set_workspace_preset(preset);
            renders.push_back(render_at_size(*state, size.width));
            // Board, notation and the sidebar stay inside the window in every preset.
            auto const reference = renders.back()[size.width - 1];
            auto clipped = 0;
            for (auto row = 0U; row < size.height; ++row) {
                for (auto col = size.width - clip_probe_columns; col < size.width; ++col) {
                    auto const pixel = renders.back()[(static_cast<std::size_t>(row) * size.width) + col];
                    clipped += (pixel.r != reference.r || pixel.g != reference.g || pixel.b != reference.b) ? 1 : 0;
                }
            }
            CHECK(clipped == 0);
        }
        // The active toolbar button and the sidebar panel differ between presets.
        CHECK(differs(renders[0], renders[1]));
        CHECK(differs(renders[0], renders[2]));
        CHECK(differs(renders[1], renders[2]));
    }
}

namespace
{

constexpr auto variation_node = 4;
constexpr auto variation_continuation_node = 5;
constexpr auto all_variation_nodes = std::array {1, 2, 3, variation_node, variation_continuation_node};
constexpr auto idle_tab_stops = 12;
constexpr auto overlay_game_plies = 10;

// 1. e4 e5 (1... c5 2. Nf3) 2. Nf3 as the presenter publishes it: node ids are
// the tree's, and the variation's tokens carry depth 1.
auto make_variation_tokens() -> std::shared_ptr<slint::VectorModel<NotationToken>>
{
    auto const token = [](char const* lead, char const* san, char const* trail, int const node, int const depth) -> NotationToken
    {
        return NotationToken {
            .lead = slint::SharedString {lead},
            .san = slint::SharedString {san},
            .trail = slint::SharedString {trail},
            .node = node,
            .depth = depth,
        };
    };
    return std::make_shared<slint::VectorModel<NotationToken>>(std::vector<NotationToken> {
        token("1.", "e4", "", 1, 0),
        token("", "e5", "", 2, 0),
        token("(1...", "c5", "", 4, 1),
        token("2.", "Nf3", ")", variation_continuation_node, 1),
        token("2.", "Nf3", "", 3, 0),
    });
}

// Tabs (activating each stop) until `done` holds; the stop count is bounded.
template<typename Predicate>
void tab_activating_until(snapshot_state const& state, WorkspaceWindow& window, Predicate const& done)
{
    constexpr auto max_tab_stops = 120;
    for (auto stop = 0; stop < max_tab_stops && !done(); ++stop) {
        press_key_and_render(state, window, slint::platform::key_codes::Tab);
        press_key_and_render(state, window, slint::platform::key_codes::Return);
    }
}

// An open database with no game: the empty, playable board.
auto make_empty_board_workspace() -> slint::ComponentHandle<WorkspaceWindow>
{
    auto window = make_loaded_workspace({.games = few_games, .moves = 0});
    window->set_board_loaded(false);
    return window;
}

}  // namespace

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: variation tokens render and every node is reachable by keyboard at narrow and wide sizes",
          "[motif-slint-app][layout]")
{
    struct window_case
    {
        std::uint32_t width;
        std::uint32_t height;
    };

    for (auto const& size : {window_case {.width = 960, .height = 560}, window_case {.width = 1600, .height = 900}}) {
        INFO("size " << size.width << "x" << size.height);
        auto const state = use_snapshot_platform(size.width, size.height);
        auto window = make_loaded_workspace({.games = few_games, .moves = 0});
        window->set_board_loaded(true);
        window->set_board_load_serial(1);

        auto const without_tokens = render_at_size(*state, size.width);
        window->set_board_notation(make_variation_tokens());
        auto const with_tokens = render_at_size(*state, size.width);
        CHECK(differs(without_tokens, with_tokens));

        auto activated = std::vector<int> {};
        window->on_board_node_activated([&activated](int const node) -> void { activated.push_back(node); });
        tab_activating_until(*state,
                             *window,
                             [&activated]() -> bool
                             {
                                 return std::ranges::all_of(all_variation_nodes,
                                                            [&activated](int const node) -> bool
                                                            { return std::ranges::count(activated, node) > 0; });
                             });
        for (auto const node : all_variation_nodes) {
            CHECK(std::ranges::count(activated, node) > 0);
        }
    }
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: a focused variation move keeps focus across resize and republish", "[motif-slint-app][layout]")
{
    constexpr std::uint32_t narrow_width {960};
    constexpr std::uint32_t narrow_height {560};
    constexpr std::uint32_t wide_width {1600};
    constexpr std::uint32_t wide_height {900};
    auto const state = use_snapshot_platform(narrow_width, narrow_height);
    auto window = make_loaded_workspace({.games = few_games, .moves = 0});
    window->set_board_loaded(true);
    window->set_board_load_serial(1);
    window->set_board_notation(make_variation_tokens());

    auto activated = std::vector<int> {};
    window->on_board_node_activated([&activated](int const node) -> void { activated.push_back(node); });
    state->window_adapter->render();
    tab_activating_until(*state, *window, [&activated]() -> bool { return !activated.empty() && activated.back() == variation_node; });
    REQUIRE(!activated.empty());
    REQUIRE(activated.back() == variation_node);

    // Republish with a new model, as every navigation does.
    window->set_board_notation(make_variation_tokens());
    window->set_board_current_node(variation_node);
    window->set_search_busy(true);
    state->window_adapter->render();
    window->set_search_busy(false);
    state->window_adapter->render();
    auto before = activated.size();
    press_key_and_render(*state, *window, slint::platform::key_codes::Return);
    REQUIRE(activated.size() == before + 1);
    CHECK(activated.back() == variation_node);

    window->window().set_size(slint::PhysicalSize {slint::Size<std::uint32_t> {.width = wide_width, .height = wide_height}});
    state->window_adapter->render();
    before = activated.size();
    press_key_and_render(*state, *window, slint::platform::key_codes::Return);
    REQUIRE(activated.size() == before + 1);
    CHECK(activated.back() == variation_node);

    window->window().set_size(slint::PhysicalSize {slint::Size<std::uint32_t> {.width = narrow_width, .height = narrow_height}});
    state->window_adapter->render();
    before = activated.size();
    press_key_and_render(*state, *window, slint::platform::key_codes::Return);
    REQUIRE(activated.size() == before + 1);
    CHECK(activated.back() == variation_node);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: the empty board takes square input from the keyboard", "[motif-slint-app][layout]")
{
    auto const state = use_snapshot_platform();
    auto window = make_empty_board_workspace();

    auto squares = std::vector<std::pair<int, int>> {};
    auto next_calls = 0;
    window->on_board_square_activated([&squares](int const file, int const rank) -> void { squares.emplace_back(file, rank); });
    window->on_board_navigate_next([&next_calls]() -> void { ++next_calls; });
    state->window_adapter->render();

    tab_activating_until(*state, *window, [&squares]() -> bool { return !squares.empty(); });
    REQUIRE(squares.size() == 1);
    CHECK(squares.front() == std::pair {0, 0});

    // Arrow keys move the square cursor instead of stepping through a game.
    press_key_and_render(*state, *window, slint::platform::key_codes::RightArrow);
    press_key_and_render(*state, *window, slint::platform::key_codes::UpArrow);
    press_key_and_render(*state, *window, slint::platform::key_codes::Return);
    REQUIRE(squares.size() == 2);
    CHECK(squares.back() == std::pair {1, 1});
    CHECK(next_calls == 0);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: the promotion picker takes Q R B N and Escape", "[motif-slint-app][layout]")
{
    auto const state = use_snapshot_platform();
    auto window = make_empty_board_workspace();

    auto squares = 0;
    auto chosen = std::vector<std::string> {};
    auto cancelled = 0;
    window->on_board_square_activated([&squares](int, int) -> void { ++squares; });
    window->on_board_promotion_chosen([&chosen](slint::SharedString const& piece) -> void
                                      { chosen.emplace_back(piece.data(), piece.size()); });
    window->on_board_promotion_cancelled([&cancelled]() -> void { ++cancelled; });
    state->window_adapter->render();
    tab_activating_until(*state, *window, [&squares]() -> bool { return squares > 0; });
    REQUIRE(squares == 1);

    auto const without_picker = render_at_size(*state, snapshot_platform::width);
    window->set_board_promotion_pending(true);
    CHECK(differs(without_picker, render_at_size(*state, snapshot_platform::width)));

    press_key_and_render(*state, *window, u8"k");
    CHECK(chosen.empty());
    for (auto const* letter : {u8"q", u8"R", u8"b", u8"N"}) {
        press_key_and_render(*state, *window, letter);
    }
    CHECK(chosen == std::vector<std::string> {"q", "r", "b", "n"});

    press_key_and_render(*state, *window, slint::platform::key_codes::Escape);
    CHECK(cancelled == 1);
    CHECK(squares == 1);  // keys went to the picker, not to square activation

    // Without a pending promotion the same letters are ignored.
    window->set_board_promotion_pending(false);
    state->window_adapter->render();
    press_key_and_render(*state, *window, u8"q");
    CHECK(chosen.size() == 4);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: New Game is a keyboard reachable toolbar action", "[motif-slint-app][layout]")
{
    auto const state = use_snapshot_platform();
    auto window = make_loaded_workspace({.games = few_games, .moves = 0});
    auto requests = 0;
    window->on_board_new_game_requested([&requests]() -> void { ++requests; });
    state->window_adapter->render();

    tab_activating_until(*state, *window, [&requests]() -> bool { return requests > 0; });
    CHECK(requests == 1);

    // Busy workspaces disable it.
    window->set_search_busy(true);
    state->window_adapter->render();
    auto const before = requests;
    for (auto stop = 0; stop < idle_tab_stops; ++stop) {
        press_key_and_render(*state, *window, slint::platform::key_codes::Tab);
        press_key_and_render(*state, *window, slint::platform::key_codes::Return);
    }
    CHECK(requests == before);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: the replace confirmation overlay blocks the board and answers by keyboard", "[motif-slint-app][layout]")
{
    auto const state = use_snapshot_platform();
    auto window = make_loaded_workspace({.games = few_games, .moves = overlay_game_plies});
    window->set_board_loaded(true);
    window->set_board_load_serial(1);

    auto accepted = 0;
    auto cancelled = 0;
    auto board_actions = 0;
    window->on_confirm_accepted([&accepted]() -> void { ++accepted; });
    window->on_confirm_cancelled([&cancelled]() -> void { ++cancelled; });
    window->on_board_square_activated([&board_actions](int, int) -> void { ++board_actions; });
    window->on_board_new_game_requested([&board_actions]() -> void { ++board_actions; });
    window->on_board_navigate_next([&board_actions]() -> void { ++board_actions; });
    window->on_board_node_activated([&board_actions](int) -> void { ++board_actions; });
    state->window_adapter->render();
    auto const hidden = render_at_size(*state, snapshot_platform::width);

    window->set_confirm_message("This game has moves that are not saved to any database. Replacing it discards them.");
    window->set_confirm_action_label("Load game");
    window->set_confirm_visible(true);
    state->window_adapter->render();
    CHECK(differs(hidden, render_at_size(*state, snapshot_platform::width)));

    // Nothing behind the overlay can be operated while it is up.
    constexpr auto stops = 40;
    for (auto stop = 0; stop < stops; ++stop) {
        press_key_and_render(*state, *window, slint::platform::key_codes::Tab);
        press_key_and_render(*state, *window, slint::platform::key_codes::RightArrow);
        press_key_and_render(*state, *window, slint::platform::key_codes::Return);
    }
    CHECK(board_actions == 0);
    CHECK(cancelled + accepted > 0);

    // Escape cancels from wherever focus is inside the overlay.
    cancelled = 0;
    accepted = 0;
    press_key_and_render(*state, *window, slint::platform::key_codes::Escape);
    CHECK(cancelled == 1);
    CHECK(accepted == 0);
}

// The explorer fixtures are literal sample data; named constants would hide the figures a test asserts.
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
namespace
{

auto explorer_row(std::string_view const san, int const count, int const white, int const draw, int const black) -> ContinuationRow
{
    return ContinuationRow {
        .san = slint::SharedString {san},
        .count = count,
        .share_text = slint::SharedString {"33%"},
        .white_percent = white,
        .draw_percent = draw,
        .black_percent = black,
        .white_elo = slint::SharedString {"2412"},
        .black_elo = slint::SharedString {"2388"},
        .eco = slint::SharedString {"B20"},
        .opening_name = slint::SharedString {"Sicilian Defence"},
        .accessible_label = slint::SharedString {std::string {san} + " row"},
    };
}

auto make_explorer_rows(std::size_t const count) -> std::shared_ptr<slint::VectorModel<ContinuationRow>>
{
    constexpr auto names = std::array<std::string_view, 12> {"e4", "d4", "Nf3", "c4", "g3", "b3", "f4", "Nc3", "a3", "h3", "b4", "e3"};
    auto rows = std::vector<ContinuationRow> {};
    for (auto index = std::size_t {0}; index < count; ++index) {
        rows.push_back(explorer_row(names.at(index % names.size()), 900 - (static_cast<int>(index) * 10), 50, 30, 20));
    }
    return std::make_shared<slint::VectorModel<ContinuationRow>>(std::move(rows));
}

// An Explorer-preset window with `rows` moves, recording every activated SAN.
auto make_explorer_workspace(std::size_t const rows, std::vector<std::string>& activated) -> slint::ComponentHandle<WorkspaceWindow>
{
    auto window = make_loaded_workspace({.games = 0, .moves = 0});
    window->set_board_loaded(true);
    window->set_workspace_preset(WorkspacePreset::Explorer);
    window->set_search_continuations(make_explorer_rows(rows));
    window->set_search_continuation_total(ContinuationTotal {
        .count = 1200, .white_percent = 50, .draw_percent = 30, .black_percent = 20, .accessible_label = slint::SharedString {"Total"}});
    window->set_search_total_games(1200);
    window->on_search_continuation_activated([&activated](slint::SharedString const& san) -> void
                                             { activated.emplace_back(std::string_view {san.data(), san.size()}); });
    return window;
}

// Tabs to the explorer table: the first tab stop where Down changes what is drawn.
// Leaves the first row selected and, when `unselected` is given, stores the
// render taken just before that first selection.
auto focus_explorer_table(snapshot_state const& state,
                          WorkspaceWindow& window,
                          std::uint32_t const width,
                          std::vector<slint::Rgb8Pixel>* const unselected = nullptr) -> bool
{
    constexpr auto max_tab_stops = 80;
    for (auto stop = 0; stop < max_tab_stops; ++stop) {
        press_key_and_render(state, window, slint::platform::key_codes::Tab);
        auto const before = render_at_size(state, width);
        press_key_and_render(state, window, slint::platform::key_codes::DownArrow);
        if (differs(before, render_at_size(state, width))) {
            press_key_and_render(state, window, slint::platform::key_codes::UpArrow);  // stays on the first row
            if (unselected != nullptr) {
                *unselected = before;
            }
            return true;
        }
    }
    return false;
}

// The colour a selected row is filled with: the most common colour among the
// pixels that changed between the two renders.
auto selection_fill(std::span<slint::Rgb8Pixel const> const lhs, std::span<slint::Rgb8Pixel const> const rhs) -> slint::Rgb8Pixel
{
    auto counts = std::map<std::array<std::uint8_t, 3>, int> {};
    for (auto index = std::size_t {0}; index < lhs.size(); ++index) {
        if (lhs[index].r != rhs[index].r || lhs[index].g != rhs[index].g || lhs[index].b != rhs[index].b) {
            ++counts[{rhs[index].r, rhs[index].g, rhs[index].b}];
        }
    }
    auto const best =
        std::ranges::max_element(counts, [](auto const& left, auto const& right) -> bool { return left.second < right.second; });
    if (best == counts.end()) {
        return slint::Rgb8Pixel {};
    }
    return slint::Rgb8Pixel {.r = best->first[0], .g = best->first[1], .b = best->first[2]};
}

struct explorer_size
{
    std::uint32_t width;
    std::uint32_t height;
};

}  // namespace

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: the explorer table is one tab stop; Up, Down and Enter choose and play a move at narrow and wide sizes",
          "[motif-slint-app][layout]")
{
    for (auto const& size : {explorer_size {.width = 960, .height = 560}, explorer_size {.width = 1600, .height = 900}}) {
        INFO("size " << size.width << "x" << size.height);
        auto const state = use_snapshot_platform(size.width, size.height);
        auto activated = std::vector<std::string> {};
        auto window = make_explorer_workspace(3, activated);
        state->window_adapter->render();

        REQUIRE(focus_explorer_table(*state, *window, size.width));
        // Down then Up selected and unselected row 0 or stayed on it; Enter plays the selected row.
        press_key_and_render(*state, *window, slint::platform::key_codes::Return);
        REQUIRE(activated.size() == 1);
        CHECK(activated.back() == "e4");

        press_key_and_render(*state, *window, slint::platform::key_codes::DownArrow);
        press_key_and_render(*state, *window, slint::platform::key_codes::DownArrow);
        press_key_and_render(*state, *window, slint::platform::key_codes::DownArrow);  // clamped at the last row
        press_key_and_render(*state, *window, slint::platform::key_codes::Return);
        REQUIRE(activated.size() == 2);
        CHECK(activated.back() == "Nf3");

        press_key_and_render(*state, *window, slint::platform::key_codes::UpArrow);
        press_key_and_render(*state, *window, slint::platform::key_codes::Return);
        REQUIRE(activated.size() == 3);
        CHECK(activated.back() == "d4");

        // Replacing the rows (a new position) clears the selection: Enter plays nothing.
        window->set_search_continuations(make_explorer_rows(3));
        press_key_and_render(*state, *window, slint::platform::key_codes::Return);
        CHECK(activated.size() == 3);

        // One tab stop: the next Tab leaves the table.
        press_key_and_render(*state, *window, slint::platform::key_codes::Tab);
        press_key_and_render(*state, *window, slint::platform::key_codes::DownArrow);
        press_key_and_render(*state, *window, slint::platform::key_codes::Return);
        CHECK(activated.size() == 3);
    }
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: the explorer table ignores keys while the workspace is busy or a confirmation is shown",
          "[motif-slint-app][layout]")
{
    auto const state = use_snapshot_platform();
    auto activated = std::vector<std::string> {};
    auto window = make_explorer_workspace(3, activated);
    state->window_adapter->render();
    REQUIRE(focus_explorer_table(*state, *window, snapshot_platform::width));
    press_key_and_render(*state, *window, slint::platform::key_codes::Return);
    REQUIRE(activated.size() == 1);

    window->set_search_busy(true);
    press_key_and_render(*state, *window, slint::platform::key_codes::Return);
    CHECK(activated.size() == 1);
    window->set_search_busy(false);

    window->set_browser_busy(true);
    press_key_and_render(*state, *window, slint::platform::key_codes::Return);
    CHECK(activated.size() == 1);
    window->set_browser_busy(false);

    window->set_import_active(true);
    press_key_and_render(*state, *window, slint::platform::key_codes::Return);
    CHECK(activated.size() == 1);
    window->set_import_active(false);

    window->set_busy(true);
    press_key_and_render(*state, *window, slint::platform::key_codes::Return);
    CHECK(activated.size() == 1);
    window->set_busy(false);

    window->set_confirm_visible(true);
    press_key_and_render(*state, *window, slint::platform::key_codes::Return);
    CHECK(activated.size() == 1);
    window->set_confirm_visible(false);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: the explorer table keeps the sidebar inside the window and scrolls a long move list",
          "[motif-slint-app][layout]")
{
    for (auto const& size : {explorer_size {.width = 960, .height = 560}, explorer_size {.width = 1600, .height = 900}}) {
        INFO("size " << size.width << "x" << size.height);
        auto const state = use_snapshot_platform(size.width, size.height);
        auto activated = std::vector<std::string> {};
        auto window = make_explorer_workspace(12, activated);
        auto const top = render_at_size(*state, size.width);

        auto const reference = top[size.width - 1];
        auto clipped = 0;
        for (auto row = 0U; row < size.height; ++row) {
            for (auto col = size.width - 8; col < size.width; ++col) {
                auto const pixel = top[(static_cast<std::size_t>(row) * size.width) + col];
                clipped += (pixel.r != reference.r || pixel.g != reference.g || pixel.b != reference.b) ? 1 : 0;
            }
        }
        CHECK(clipped == 0);

        auto unselected = std::vector<slint::Rgb8Pixel> {};
        REQUIRE(focus_explorer_table(*state, *window, size.width, &unselected));
        auto const first_selected = render_at_size(*state, size.width);
        auto const fill = selection_fill(unselected, first_selected);
        for (auto step = 0; step < 12; ++step) {
            press_key_and_render(*state, *window, slint::platform::key_codes::DownArrow);
        }
        // The last row is selected and scrolled into view: its highlight is drawn.
        press_key_and_render(*state, *window, slint::platform::key_codes::Return);
        REQUIRE(activated.size() == 1);
        CHECK(activated.back() == "e3");
        auto const bottom = render_at_size(*state, size.width);
        constexpr auto min_highlight_pixels = 2000;
        CHECK(std::ranges::count_if(bottom,
                                    [fill](slint::Rgb8Pixel const pixel) -> bool
                                    { return pixel.r == fill.r && pixel.g == fill.g && pixel.b == fill.b; })
              > min_highlight_pixels);
        CHECK(differs(top, bottom));

        // A new position replaces the rows: selection and scroll start over, so the
        // table looks exactly as it did when focus first reached it.
        window->set_search_continuations(make_explorer_rows(12));
        state->window_adapter->render();  // change handlers run while the frame is prepared
        // Only the sidebar half is compared: one pixel column in the left pane differs
        // between these frames and is not part of the table.
        auto const bounds = difference_bounds(unselected, render_at_size(*state, size.width), size.width);
        CHECK((!bounds.found || std::cmp_less(bounds.right, size.width / 2)));
    }
}

namespace
{

// Clicks down the sidebar column and returns the SANs activated, top to bottom.
auto click_down_sidebar(snapshot_state const& state,
                        WorkspaceWindow& window,
                        explorer_size const size,
                        std::vector<std::string> const& activated) -> std::vector<std::string>
{
    constexpr auto step = 6;
    auto seen = std::vector<std::string> {};
    auto const column = static_cast<float>(size.width) - 70.0F;
    for (auto row = 0; std::cmp_less(row, size.height); row += step) {
        auto const position = slint::LogicalPosition {slint::Point<float> {.x = column, .y = static_cast<float>(row)}};
        window.window().dispatch_pointer_move_event(position);
        window.window().dispatch_pointer_press_event(position, slint::PointerEventButton::Left);
        window.window().dispatch_pointer_release_event(position, slint::PointerEventButton::Left);
        state.window_adapter->render();
        if (activated.size() > seen.size()) {
            seen.push_back(activated.back());
        }
    }
    return seen;
}

}  // namespace

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: clicking an explorer row plays it, except while the workspace is busy", "[motif-slint-app][layout]")
{
    for (auto const& size : {explorer_size {.width = 960, .height = 560}, explorer_size {.width = 1600, .height = 900}}) {
        INFO("size " << size.width << "x" << size.height);
        auto const state = use_snapshot_platform(size.width, size.height);
        auto activated = std::vector<std::string> {};
        auto window = make_explorer_workspace(3, activated);
        state->window_adapter->render();

        auto const seen = click_down_sidebar(*state, *window, size, activated);
        REQUIRE_FALSE(seen.empty());
        CHECK(seen.front() == "e4");  // the first row is the first thing clickable in the column

        auto const before = activated.size();
        window->set_search_busy(true);
        state->window_adapter->render();
        (void)click_down_sidebar(*state, *window, size, activated);
        CHECK(activated.size() == before);
        window->set_search_busy(false);
        window->set_confirm_visible(true);
        state->window_adapter->render();
        (void)click_down_sidebar(*state, *window, size, activated);
        CHECK(activated.size() == before);
    }
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: an empty explorer has no table to focus and a searching one says so", "[motif-slint-app][layout]")
{
    auto const state = use_snapshot_platform();
    auto activated = std::vector<std::string> {};
    auto window = make_explorer_workspace(0, activated);
    auto const idle = render_at_size(*state, snapshot_platform::width);

    // No rows: the table is not a tab stop, so nothing can be chosen or played.
    CHECK_FALSE(focus_explorer_table(*state, *window, snapshot_platform::width));
    press_key_and_render(*state, *window, slint::platform::key_codes::Return);
    CHECK(activated.empty());

    window->set_search_searching(true);
    CHECK(differs(idle, render_at_size(*state, snapshot_platform::width)));
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: rows searched for another position are dimmed, unselectable and not playable", "[motif-slint-app][layout]")
{
    for (auto const& size : {explorer_size {.width = 960, .height = 560}, explorer_size {.width = 1600, .height = 900}}) {
        INFO("size " << size.width << "x" << size.height);
        auto const state = use_snapshot_platform(size.width, size.height);
        auto activated = std::vector<std::string> {};
        auto window = make_explorer_workspace(3, activated);

        // Learn the selection colour from a selected row while the rows are current.
        auto unselected = std::vector<slint::Rgb8Pixel> {};
        REQUIRE(focus_explorer_table(*state, *window, size.width, &unselected));
        auto const selected = render_at_size(*state, size.width);
        auto const fill = selection_fill(unselected, selected);
        auto const fill_pixels = [fill](std::span<slint::Rgb8Pixel const> const pixels) -> std::ptrdiff_t
        {
            return std::ranges::count_if(pixels,
                                         [fill](slint::Rgb8Pixel const pixel) -> bool
                                         { return pixel.r == fill.r && pixel.g == fill.g && pixel.b == fill.b; });
        };
        constexpr auto highlight_pixels = 2000;
        REQUIRE(fill_pixels(selected) > highlight_pixels);

        // Going stale clears the selection and dims the rows.
        window->set_search_rows_stale(true);
        auto const stale = render_at_size(*state, size.width);
        CHECK(differs(selected, stale));
        CHECK(fill_pixels(stale) < highlight_pixels);

        // Enter plays nothing, and a click neither plays nor selects.
        press_key_and_render(*state, *window, slint::platform::key_codes::Return);
        CHECK(activated.empty());
        auto const seen = click_down_sidebar(*state, *window, size, activated);
        CHECK(seen.empty());
        CHECK(activated.empty());
        CHECK(fill_pixels(render_at_size(*state, size.width)) < highlight_pixels);

        // A finished search for the board's position makes the rows usable again.
        window->set_search_rows_stale(false);
        state->window_adapter->render();
        auto const resumed = click_down_sidebar(*state, *window, size, activated);
        REQUIRE_FALSE(resumed.empty());
        CHECK(resumed.front() == "e4");
    }
}

namespace
{

struct analysis_size
{
    std::uint32_t width;
    std::uint32_t height;
};

constexpr auto narrow_analysis = analysis_size {.width = 960, .height = 560};
constexpr auto wide_analysis = analysis_size {.width = 1280, .height = 800};

// Writes the frame as a PPM when MOTIF_LAYOUT_SNAPSHOT_DIR is set, at its real size.
void dump_frame(snapshot_state const& state, std::string const& name, analysis_size const size)
{
    // NOLINTNEXTLINE(concurrency-mt-unsafe): the test binary is serial and only reads a caller-supplied path.
    char const* const output = std::getenv("MOTIF_LAYOUT_SNAPSHOT_DIR");
    if (output == nullptr || *output == '\0') {
        return;
    }
    std::filesystem::create_directories(output);
    auto image = std::ofstream {std::filesystem::path {output} / (name + ".ppm"), std::ios::binary | std::ios::trunc};
    image << "P6\n" << size.width << ' ' << size.height << "\n255\n";
    for (auto const pixel : state.window_adapter->pixels()) {
        image.put(static_cast<char>(pixel.r));
        image.put(static_cast<char>(pixel.g));
        image.put(static_cast<char>(pixel.b));
    }
}

// Where two frames differ in the right half of the window, the sidebar column.
// One pixel of the board pane differs between otherwise equal frames.
auto sidebar_difference_bounds(std::span<slint::Rgb8Pixel const> const lhs,
                               std::span<slint::Rgb8Pixel const> const rhs,
                               std::uint32_t const width) -> pixel_bounds
{
    auto left = std::vector<slint::Rgb8Pixel> {lhs.begin(), lhs.end()};
    auto right = std::vector<slint::Rgb8Pixel> {rhs.begin(), rhs.end()};
    for (auto index = std::size_t {0}; index < left.size(); ++index) {
        if (index % width < width / 2) {
            left[index] = right[index] = slint::Rgb8Pixel {};
        }
    }
    return difference_bounds(left, right, width);
}

auto make_analysis_lines(int const count) -> std::shared_ptr<slint::VectorModel<AnalysisLine>>
{
    auto lines = std::vector<AnalysisLine> {};
    for (auto rank = 1; rank <= count; ++rank) {
        lines.push_back(AnalysisLine {
            .rank = rank,
            .depth = 24,
            .score = slint::SharedString {"+0.35"},
            .moves = slint::SharedString {"1.e4 e5 2.Nf3 Nc6 3.Bb5 a6 4.Ba4 Nf6 5.O-O Be7 6.Re1 b5 7.Bb3 d6"},
            .accessible_label = slint::SharedString {"Line"},
        });
    }
    return std::make_shared<slint::VectorModel<AnalysisLine>>(std::move(lines));
}

// An Analysis-preset window with one engine configured, ready to start. A first
// frame is rendered so the window's short-height mode is known before a test
// compares frames.
auto make_analysis_workspace(snapshot_state const& state) -> slint::ComponentHandle<WorkspaceWindow>
{
    auto window = make_loaded_workspace({});
    window->set_board_loaded(true);
    window->set_workspace_preset(WorkspacePreset::Analysis);
    window->set_analysis_engine_names(
        std::make_shared<slint::VectorModel<slint::SharedString>>(std::vector<slint::SharedString> {slint::SharedString {"stockfish"}}));
    window->set_analysis_selected_engine(0);
    window->set_analysis_can_start(true);
    window->set_analysis_status_text("Ready");
    window->set_analysis_lines(make_analysis_lines(0));
    state.window_adapter->render();
    return window;
}

}  // namespace

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: the engine panel fits its controls and a PV line without clipping at narrow and wide sizes",
          "[motif-slint-app][layout][analysis]")
{
    constexpr auto margin = 8;
    constexpr auto min_text_height = 8;

    for (auto const size : {narrow_analysis, wide_analysis}) {
        INFO("size " << size.width << "x" << size.height);
        auto const state = use_snapshot_platform(size.width, size.height);
        auto window = make_analysis_workspace(*state);
        // The first frame lays out before the window's short-height mode is known.
        static_cast<void>(render_at_size(*state, size.width));
        auto const empty = render_at_size(*state, size.width);
        dump_frame(*state, "analysis-ready-" + std::to_string(size.width), size);

        // Start becomes Stop when analysis is on: that difference is the button.
        window->set_analysis_active(true);
        window->set_analysis_status_text("Analysing");
        auto const active = render_at_size(*state, size.width);
        auto const control_bounds = sidebar_difference_bounds(empty, active, size.width);
        REQUIRE(control_bounds.found);
        CHECK(std::cmp_less(control_bounds.right, size.width - margin));
        CHECK(std::cmp_less(control_bounds.bottom, size.height - margin));

        // One PV line must be visible in full, below the controls.
        window->set_analysis_lines(make_analysis_lines(1));
        auto const one_line = render_at_size(*state, size.width);
        dump_frame(*state, "analysis-one-line-" + std::to_string(size.width), size);
        auto const line_bounds = sidebar_difference_bounds(active, one_line, size.width);
        REQUIRE(line_bounds.found);
        CHECK(line_bounds.height() >= min_text_height);
        CHECK(line_bounds.top > control_bounds.bottom);
        CHECK(std::cmp_less(line_bounds.right, size.width - margin));
        CHECK(std::cmp_less(line_bounds.bottom, size.height - margin));

        // Nothing below the panel was pushed out of the window: the import row still draws.
        window->set_import_path_text("x");
        auto const typed = render_at_size(*state, size.width);
        auto const import_bounds = sidebar_difference_bounds(one_line, typed, size.width);
        REQUIRE(import_bounds.found);
        CHECK(import_bounds.top > line_bounds.bottom);
        CHECK(std::cmp_less(import_bounds.bottom, size.height - margin));

        // The right margin stays plain background.
        auto const reference = typed[size.width - 1];
        auto clipped = 0;
        for (auto row = 0U; row < size.height; ++row) {
            for (auto col = size.width - margin; col < size.width; ++col) {
                auto const pixel = typed[(static_cast<std::size_t>(row) * size.width) + col];
                clipped += (pixel.r != reference.r || pixel.g != reference.g || pixel.b != reference.b) ? 1 : 0;
            }
        }
        CHECK(clipped == 0);
    }
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: extra engine lines scroll inside the panel and leave the rest of the window alone",
          "[motif-slint-app][layout][analysis]")
{
    constexpr auto margin = 8;
    for (auto const size : {narrow_analysis, wide_analysis}) {
        INFO("size " << size.width << "x" << size.height);
        auto const state = use_snapshot_platform(size.width, size.height);
        auto window = make_analysis_workspace(*state);
        window->set_analysis_lines(make_analysis_lines(1));
        window->set_import_path_text("x");
        auto const one_line = render_at_size(*state, size.width);

        window->set_analysis_lines(make_analysis_lines(5));
        auto const five_lines = render_at_size(*state, size.width);
        dump_frame(*state, "analysis-five-lines-" + std::to_string(size.width), size);

        // Five lines draw more than one, yet the import row below is where it was.
        auto const added = sidebar_difference_bounds(one_line, five_lines, size.width);
        REQUIRE(added.found);
        window->set_import_path_text("");
        auto const without_import_text = render_at_size(*state, size.width);
        auto const import_bounds = sidebar_difference_bounds(five_lines, without_import_text, size.width);
        REQUIRE(import_bounds.found);
        CHECK(added.bottom < import_bounds.top);
        CHECK(std::cmp_less(added.right, size.width - margin));
    }
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: lines of a stopped session are dimmed", "[motif-slint-app][layout][analysis]")
{
    auto const state = use_snapshot_platform(wide_analysis.width, wide_analysis.height);
    auto window = make_analysis_workspace(*state);
    window->set_analysis_lines(make_analysis_lines(2));
    auto const live = render_at_size(*state, wide_analysis.width);

    window->set_analysis_lines_stale(true);
    auto const stale = render_at_size(*state, wide_analysis.width);

    CHECK(differs(live, stale));
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: the engine panel is reachable by Tab in order and Start and Stop answer Enter and Space",
          "[motif-slint-app][layout][analysis]")
{
    for (auto const size : {narrow_analysis, wide_analysis}) {
        INFO("size " << size.width << "x" << size.height);
        auto const state = use_snapshot_platform(size.width, size.height);
        auto window = make_analysis_workspace(*state);
        window->set_analysis_engine_path_text("/opt/engines/lc0");
        auto activations = std::vector<std::string> {};
        window->on_analysis_engine_removed([&activations]() -> void { activations.emplace_back("remove"); });
        window->on_analysis_engine_added([&activations](slint::SharedString const&) -> void { activations.emplace_back("add"); });
        window->on_analysis_start_requested([&activations]() -> void { activations.emplace_back("start"); });
        window->on_analysis_stop_requested([&activations]() -> void { activations.emplace_back("stop"); });
        state->window_adapter->render();

        // Activate every tab stop in turn (Escape closes a combo box's popup).
        constexpr auto max_tab_stops = 60;
        for (auto stop = 0; stop < max_tab_stops && !std::ranges::contains(activations, "start"); ++stop) {
            press_key_and_render(*state, *window, slint::platform::key_codes::Tab);
            press_key_and_render(*state, *window, slint::platform::key_codes::Return);
            press_key_and_render(*state, *window, slint::platform::key_codes::Escape);
        }
        REQUIRE(std::ranges::contains(activations, "start"));
        // Declaration order: engine list, Remove, path field (Enter adds), Add, options, Start.
        CHECK(activations == std::vector<std::string> {"remove", "add", "add", "start"});

        // The host answers Start with the running state; the same button now stops.
        window->set_analysis_active(true);
        press_key_and_render(*state, *window, slint::platform::key_codes::Return);
        CHECK(activations.back() == "stop");
        window->set_analysis_active(false);
        press_key_and_render(*state, *window, slint::platform::key_codes::Space);
        CHECK(activations.back() == "start");
        window->set_analysis_active(true);
        press_key_and_render(*state, *window, slint::platform::key_codes::Space);
        CHECK(activations == std::vector<std::string> {"remove", "add", "add", "start", "stop", "start", "stop"});
    }
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: the engine panel locks its engine and option controls while analysis is on, and Start needs an engine",
          "[motif-slint-app][layout][analysis]")
{
    auto const state = use_snapshot_platform(wide_analysis.width, wide_analysis.height);
    auto window = make_analysis_workspace(*state);
    auto removed = 0;
    window->on_analysis_engine_removed([&removed]() -> void { ++removed; });
    window->on_analysis_start_requested([&removed]() -> void { ++removed; });
    window->set_analysis_active(true);
    state->window_adapter->render();

    // While on, the only enabled stop of the panel is the Stop button.
    constexpr auto max_tab_stops = 60;
    auto stops_that_acted = 0;
    for (auto stop = 0; stop < max_tab_stops; ++stop) {
        press_key_and_render(*state, *window, slint::platform::key_codes::Tab);
        auto const before = removed;
        press_key_and_render(*state, *window, slint::platform::key_codes::Return);
        stops_that_acted += removed != before ? 1 : 0;
    }
    CHECK(stops_that_acted == 0);

    // No engine: Start is disabled.
    window->set_analysis_active(false);
    window->set_analysis_can_start(false);
    window->set_analysis_selected_engine(-1);
    window->set_analysis_engine_names(std::make_shared<slint::VectorModel<slint::SharedString>>());
    state->window_adapter->render();
    for (auto stop = 0; stop < max_tab_stops; ++stop) {
        press_key_and_render(*state, *window, slint::platform::key_codes::Tab);
        press_key_and_render(*state, *window, slint::platform::key_codes::Return);
    }
    CHECK(removed == 0);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: a scrolled engine line list keeps its position when the host republishes the lines",
          "[motif-slint-app][layout][analysis]")
{
    // The host replaces the whole line model about ten times a second.
    auto const state = use_snapshot_platform(narrow_analysis.width, narrow_analysis.height);
    auto window = make_analysis_workspace(*state);
    window->set_analysis_active(true);
    window->set_analysis_lines(make_analysis_lines(3));
    auto const top = render_at_size(*state, narrow_analysis.width);

    // The list shows one row at this size; scroll to the last one.
    auto const over_list = slint::LogicalPosition {slint::Point<float> {.x = 700.0F, .y = 236.0F}};
    window->window().dispatch_pointer_move_event(over_list);
    window->window().dispatch_pointer_scroll_event(over_list, 0.0F, -200.0F);
    // Scrolling is animated; let the animation clock run to completion.
    std::this_thread::sleep_for(std::chrono::milliseconds {500});
    slint::platform::update_timers_and_animations();
    auto const scrolled = render_at_size(*state, narrow_analysis.width);
    REQUIRE(sidebar_difference_bounds(top, scrolled, narrow_analysis.width).found);

    window->set_analysis_lines(make_analysis_lines(3));
    auto const republished = render_at_size(*state, narrow_analysis.width);

    CHECK_FALSE(sidebar_difference_bounds(scrolled, republished, narrow_analysis.width).found);
}

// ---------------------------------------------------------------------------
// Follow board (Story 13.5)
// ---------------------------------------------------------------------------

namespace
{

constexpr auto follow_margin = 8;
// One table row is 34 px; a fully visible row differs from an empty table by at least this much.
constexpr auto full_row_height = 30;

// A populated workspace in the given preset. Analysis gets its engine panel the way make_analysis_workspace does.
auto make_follow_workspace(snapshot_state const& state, WorkspacePreset const preset, int const games = typical_games)
    -> slint::ComponentHandle<WorkspaceWindow>
{
    auto window = make_loaded_workspace({.games = games, .moves = 0});
    window->set_board_loaded(true);
    window->set_workspace_preset(preset);
    if (preset == WorkspacePreset::Analysis) {
        window->set_analysis_engine_names(std::make_shared<slint::VectorModel<slint::SharedString>>(
            std::vector<slint::SharedString> {slint::SharedString {"stockfish"}}));
        window->set_analysis_selected_engine(0);
        window->set_analysis_can_start(true);
        window->set_analysis_status_text("Ready");
        window->set_analysis_lines(make_analysis_lines(0));
    }
    // The first frame lays out before the window's short-height mode is known.
    state.window_adapter->render();
    state.window_adapter->render();
    return window;
}

auto right_margin_is_plain(std::span<slint::Rgb8Pixel const> const pixels, analysis_size const size) -> bool
{
    auto const reference = pixels[size.width - 1];
    for (auto row = 0U; row < size.height; ++row) {
        for (auto col = size.width - follow_margin; col < size.width; ++col) {
            auto const pixel = pixels[(static_cast<std::size_t>(row) * size.width) + col];
            if (pixel.r != reference.r || pixel.g != reference.g || pixel.b != reference.b) {
                return false;
            }
        }
    }
    return true;
}

auto center_of(pixel_bounds const& bounds) -> slint::LogicalPosition
{
    return slint::LogicalPosition {slint::Point<float> {
        .x = static_cast<float>(bounds.left + bounds.right) / 2.0F,
        .y = static_cast<float>(bounds.top + bounds.bottom) / 2.0F,
    }};
}

void click_at(WorkspaceWindow& window, slint::LogicalPosition const position)
{
    window.window().dispatch_pointer_move_event(position);
    window.window().dispatch_pointer_press_event(position, slint::PointerEventButton::Left);
    window.window().dispatch_pointer_release_event(position, slint::PointerEventButton::Left);
}

}  // namespace

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: the Follow board toggle fits the sidebar with the footer and leaves a table row at narrow and wide sizes",
          "[motif-slint-app][layout][follow]")
{
    for (auto const size : {narrow_analysis, wide_analysis}) {
        for (auto const preset : {WorkspacePreset::Review, WorkspacePreset::Analysis}) {
            INFO("size " << size.width << "x" << size.height << " preset " << static_cast<int>(preset));
            auto const state = use_snapshot_platform(size.width, size.height);
            auto window = make_follow_workspace(*state, preset);
            window->set_game_has_previous_page(true);
            window->set_game_has_next_page(true);
            window->set_game_selected_row(-1);
            window->set_game_active_title("Averylongfirstname, Averylongsurname - Anotherlongname, X  0-1");
            auto const off = render_at_size(*state, size.width);
            dump_frame(
                *state, "follow-off-" + std::to_string(preset == WorkspacePreset::Review ? 0 : 1) + "-" + std::to_string(size.width), size);

            // The toggle: visible when on, inside the window, in the sidebar column.
            window->set_game_following(true);
            auto const checked_frame = render_at_size(*state, size.width);
            dump_frame(
                *state, "follow-on-" + std::to_string(preset == WorkspacePreset::Review ? 0 : 1) + "-" + std::to_string(size.width), size);
            auto const toggle = sidebar_difference_bounds(off, checked_frame, size.width);
            REQUIRE(toggle.found);
            CHECK(std::cmp_less(toggle.right, size.width - follow_margin));
            CHECK(std::cmp_less(toggle.bottom, size.height - follow_margin));
            CHECK(right_margin_is_plain(checked_frame, size));

            // The footer buttons still draw inside the window: Open Game enabled by a selection.
            window->set_game_selected_row(2);
            auto const selected = render_at_size(*state, size.width);
            auto const open_game = sidebar_difference_bounds(checked_frame, selected, size.width);
            REQUIRE(open_game.found);
            CHECK(std::cmp_less(open_game.right, size.width - follow_margin));
            CHECK(std::cmp_less(open_game.bottom, size.height - follow_margin));
            CHECK(open_game.bottom > toggle.bottom);  // the button sits in the row below the counters
            CHECK(right_margin_is_plain(selected, size));

            // At least one whole table row sits above the toggle.
            window->set_game_rows(make_game_rows(0));
            auto const no_rows = render_at_size(*state, size.width);
            window->set_game_rows(make_game_rows(typical_games));
            auto const rows = render_at_size(*state, size.width);
            auto const row_bounds = sidebar_difference_bounds(no_rows, rows, size.width);
            REQUIRE(row_bounds.found);
            CHECK(row_bounds.height() >= full_row_height);
            CHECK(row_bounds.bottom < toggle.top);
        }
    }
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: Follow board is a tab stop operated by Space, Enter and the mouse, and keeps focus through its own refresh",
          "[motif-slint-app][layout][follow]")
{
    for (auto const size : {narrow_analysis, wide_analysis}) {
        for (auto const preset : {WorkspacePreset::Review, WorkspacePreset::Analysis}) {
            INFO("size " << size.width << "x" << size.height << " preset " << static_cast<int>(preset));
            auto const state = use_snapshot_platform(size.width, size.height);
            auto window = make_follow_workspace(*state, preset);
            auto requests = std::vector<bool> {};
            // The host answers by publishing the state it accepted.
            window->on_game_follow_toggled(
                [&requests, &window](bool const enabled) -> void
                {
                    requests.push_back(enabled);
                    window->set_game_following(enabled);
                });
            state->window_adapter->render();

            // Tab reaches it; Space operates it. Space on the toolbar's preset buttons on the
            // way switches the preset, so the layout the toggle is found in may differ from the start.
            constexpr auto max_tab_stops = 80;
            for (auto stop = 0; stop < max_tab_stops && requests.empty(); ++stop) {
                press_key_and_render(*state, *window, slint::platform::key_codes::Tab);
                press_key_and_render(*state, *window, slint::platform::key_codes::Space);
            }
            REQUIRE(requests == std::vector<bool> {true});
            CHECK(window->get_game_following());
            state->window_adapter->render();
            dump_frame(*state,
                       "follow-focus-" + std::to_string(preset == WorkspacePreset::Review ? 0 : 1) + "-" + std::to_string(size.width),
                       size);

            // The refresh it started runs and lands: the toggle is still the focus.
            window->set_browser_busy(true);
            window->set_game_refreshing(true);
            state->window_adapter->render();
            window->set_browser_busy(false);
            window->set_game_refreshing(false);
            state->window_adapter->render();
            press_key_and_render(*state, *window, slint::platform::key_codes::Space);
            CHECK(requests == std::vector<bool> {true, false});
            press_key_and_render(*state, *window, slint::platform::key_codes::Return);
            CHECK(requests == std::vector<bool> {true, false, true});
            CHECK(window->get_game_following());

            // A key that repeats does not flip it again and again.
            auto const before_repeat = requests.size();
            window->window().dispatch_key_press_repeat_event(slint::SharedString {slint::platform::key_codes::Space});
            CHECK(requests.size() == before_repeat);

            // The mouse operates it as well (Slint pointer events delivered to the window).
            window->set_game_following(false);
            state->window_adapter->render();
            // Where the box is drawn in the layout the window has now.
            window->set_game_following(false);
            auto const off = render_at_size(*state, size.width);
            window->set_game_following(true);
            auto const checked_frame = render_at_size(*state, size.width);
            window->set_game_following(false);
            auto const box = sidebar_difference_bounds(off, checked_frame, size.width);
            REQUIRE(box.found);
            state->window_adapter->render();
            click_at(*window, center_of(box));
            CHECK(requests.size() == before_repeat + 1);
            CHECK(requests.back());
        }
    }
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: Follow board shows a keyboard focus ring", "[motif-slint-app][layout][follow]")
{
    auto const state = use_snapshot_platform(narrow_analysis.width, narrow_analysis.height);
    auto window = make_follow_workspace(*state, WorkspacePreset::Review);
    auto requests = 0;
    window->on_game_follow_toggled([&requests](bool) -> void { ++requests; });
    auto const unfocused = render_at_size(*state, narrow_analysis.width);

    constexpr auto max_tab_stops = 80;
    for (auto stop = 0; stop < max_tab_stops && requests == 0; ++stop) {
        press_key_and_render(*state, *window, slint::platform::key_codes::Tab);
        press_key_and_render(*state, *window, slint::platform::key_codes::Space);
    }
    REQUIRE(requests == 1);
    auto const focused = render_at_size(*state, narrow_analysis.width);
    auto const ring = sidebar_difference_bounds(unfocused, focused, narrow_analysis.width);
    REQUIRE(ring.found);
    // The ring surrounds the box and its label: wider than a box, not a bare cursor.
    CHECK(ring.width() > 60);
    CHECK(ring.height() >= 16);
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: rows of a list being refreshed are dimmed and inert", "[motif-slint-app][layout][follow]")
{
    auto const state = use_snapshot_platform(wide_analysis.width, wide_analysis.height);
    auto window = make_follow_workspace(*state, WorkspacePreset::Review);
    window->set_game_following(true);
    auto selected = std::vector<int> {};
    auto activated = 0;
    window->on_game_selected([&selected](int const row) -> void { selected.push_back(row); });
    window->on_game_activate_requested([&activated]() -> void { ++activated; });
    auto const idle = render_at_size(*state, wide_analysis.width);

    window->set_browser_busy(true);
    window->set_game_refreshing(true);
    auto const refreshing = render_at_size(*state, wide_analysis.width);
    dump_frame(*state, "follow-refreshing-1280", wide_analysis);
    CHECK(sidebar_difference_bounds(idle, refreshing, wide_analysis.width).found);

    // Where the first row is: the rows differ from an empty table.
    window->set_game_refreshing(false);
    window->set_browser_busy(false);
    window->set_game_rows(make_game_rows(0));
    auto const no_rows = render_at_size(*state, wide_analysis.width);
    window->set_game_rows(make_game_rows(typical_games));
    auto const rows = render_at_size(*state, wide_analysis.width);
    auto const row_area = sidebar_difference_bounds(no_rows, rows, wide_analysis.width);
    REQUIRE(row_area.found);
    auto const first_row = slint::LogicalPosition {slint::Point<float> {
        .x = static_cast<float>(row_area.left + 20),
        .y = static_cast<float>(row_area.top + 10),
    }};

    window->set_browser_busy(true);
    window->set_game_refreshing(true);
    state->window_adapter->render();
    click_at(*window, first_row);
    click_at(*window, first_row);
    CHECK(selected.empty());
    CHECK(activated == 0);

    window->set_browser_busy(false);
    window->set_game_refreshing(false);
    state->window_adapter->render();
    click_at(*window, first_row);
    CHECK(selected == std::vector<int> {0});
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: the progress bar and the empty-list text draw over the table without moving a control",
          "[motif-slint-app][layout][follow]")
{
    for (auto const size : {narrow_analysis, wide_analysis}) {
        INFO("size " << size.width << "x" << size.height);
        auto const state = use_snapshot_platform(size.width, size.height);
        auto window = make_follow_workspace(*state, WorkspacePreset::Review, few_games);
        window->set_game_following(true);
        window->set_game_rows(make_game_rows(0));
        window->set_game_total_count(0);
        auto const plain = render_at_size(*state, size.width);

        // The bar is a thin band across the table.
        window->set_game_progress_visible(true);
        auto const bar = render_at_size(*state, size.width);
        dump_frame(*state, "follow-progress-" + std::to_string(size.width), size);
        auto const band = sidebar_difference_bounds(plain, bar, size.width);
        REQUIRE(band.found);
        CHECK(band.height() <= 8);
        CHECK(band.width() > 100);
        window->set_game_progress_visible(false);

        // The text of an empty followed list or a failed refresh fills the table body only.
        window->set_game_status_text("No games reach this position");
        auto const empty_text = render_at_size(*state, size.width);
        dump_frame(*state, "follow-empty-" + std::to_string(size.width), size);
        auto const text_area = sidebar_difference_bounds(plain, empty_text, size.width);
        REQUIRE(text_area.found);
        CHECK(text_area.top > band.bottom);
        CHECK(text_area.width() > 100);
        window->set_game_status_text("Could not load the games for this position");
        auto const failed_text = render_at_size(*state, size.width);
        dump_frame(*state, "follow-failed-" + std::to_string(size.width), size);
        CHECK(differs(empty_text, failed_text));
        CHECK(right_margin_is_plain(failed_text, size));

        // While the next result is on its way the text is hidden.
        window->set_game_refreshing(true);
        auto const hidden = render_at_size(*state, size.width);
        CHECK_FALSE(sidebar_difference_bounds(plain, hidden, size.width).found);
    }
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace layout: a new result scrolls the table to the top and reveals a selected game that is still listed",
          "[motif-slint-app][layout][follow]")
{
    constexpr auto game_count = 60;
    auto const state = use_snapshot_platform();
    auto window = make_follow_workspace(*state, WorkspacePreset::Review, game_count);
    window->on_game_selected([&window](int const row) -> void { window->set_game_selected_row(row); });
    window->on_game_move_selection(
        [&window](int const delta) -> void
        { window->set_game_selected_row(std::clamp(window->get_game_selected_row() + delta, 0, game_count - 1)); });

    // Focus the table (its focus ring is part of every later frame), then show it from the top with nothing selected.
    constexpr auto max_tab_stops = 80;
    for (auto stop = 0; stop < max_tab_stops && window->get_game_selected_row() < 0; ++stop) {
        press_key_and_render(*state, *window, slint::platform::key_codes::Tab);
        press_key_and_render(*state, *window, slint::platform::key_codes::DownArrow);
    }
    REQUIRE(window->get_game_selected_row() == 0);
    window->set_game_selected_row(-1);
    auto const top = render_at_size(*state, snapshot_platform::width);

    // Scroll to the end with the keyboard.
    press_key_and_render(*state, *window, slint::platform::key_codes::End);
    auto const bottom = render_at_size(*state, snapshot_platform::width);
    REQUIRE(differs(top, bottom));

    // A new result that no longer lists the selected game: back at the top, nothing selected.
    window->set_game_selected_row(-1);
    window->set_game_rows(make_game_rows(game_count));
    window->set_game_result_serial(window->get_game_result_serial() + 1);
    slint::platform::update_timers_and_animations();
    CHECK_FALSE(differs(top, render_at_size(*state, snapshot_platform::width)));

    // A new result that still lists the selected game far down: that row is brought into view.
    window->set_game_selected_row(game_count - 1);
    window->set_game_rows(make_game_rows(game_count));
    window->set_game_result_serial(window->get_game_result_serial() + 1);
    slint::platform::update_timers_and_animations();
    auto const revealed = render_at_size(*state, snapshot_platform::width);
    CHECK(differs(top, revealed));
    // The selected row's fill (dark teal) is on screen, as it is when the user scrolls there.
    auto const selection_pixels = [](std::span<slint::Rgb8Pixel const> const pixels) -> std::ptrdiff_t
    {
        return std::ranges::count_if(
            pixels, [](slint::Rgb8Pixel const pixel) -> bool { return pixel.r < 40 && pixel.g > pixel.r + 30 && pixel.b > pixel.r + 30; });
    };
    CHECK(selection_pixels(top) < selection_pixels(revealed) - 1'000);
    CHECK(selection_pixels(revealed) > 1'000);
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
