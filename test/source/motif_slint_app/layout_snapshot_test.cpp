#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <slint-platform.h>
#include <slint.h>

#include "workspace.h"

namespace
{
class snapshot_window_adapter;

struct snapshot_state
{
    snapshot_window_adapter* window_adapter {nullptr};
    // Size of the next window adapter the process-wide platform creates.
    std::uint32_t width {0};
    std::uint32_t height {0};
};

class snapshot_window_adapter final : public slint::platform::WindowAdapter
{
  public:
    explicit snapshot_window_adapter(slint::PhysicalSize const size)
        : size_ {size}
        , pixels_(static_cast<std::size_t>(size.width) * static_cast<std::size_t>(size.height))
        , renderer_ {slint::platform::SoftwareRenderer::RepaintBufferType::ReusedBuffer}
    {
    }

    [[nodiscard]] auto size() -> slint::PhysicalSize override { return size_; }

    [[nodiscard]] auto renderer() -> slint::platform::AbstractRenderer& override { return renderer_; }

    void set_size(slint::PhysicalSize const size) override
    {
        size_ = size;
        pixels_.assign(static_cast<std::size_t>(size.width) * static_cast<std::size_t>(size.height), {});
        window().dispatch_resize_event(slint::LogicalSize {slint::Size<float> {
            .width = static_cast<float>(size.width),
            .height = static_cast<float>(size.height),
        }});
    }

    void render() { renderer_.render(std::span {pixels_}, size_.width); }

    [[nodiscard]] auto pixels() const -> std::span<slint::Rgb8Pixel const> { return pixels_; }

  private:
    slint::PhysicalSize size_;
    std::vector<slint::Rgb8Pixel> pixels_;
    slint::platform::SoftwareRenderer renderer_;
};

class snapshot_platform final : public slint::platform::Platform
{
  public:
    explicit snapshot_platform(std::shared_ptr<snapshot_state> state)
        : state_ {std::move(state)}
    {
    }

    [[nodiscard]] auto create_window_adapter() -> std::unique_ptr<slint::platform::WindowAdapter> override
    {
        auto adapter = std::make_unique<snapshot_window_adapter>(slint::PhysicalSize {slint::Size<std::uint32_t> {
            .width = state_->width,
            .height = state_->height,
        }});
        state_->window_adapter = adapter.get();
        return adapter;
    }

    static constexpr std::uint32_t width {1440};
    static constexpr std::uint32_t height {760};

  private:
    std::shared_ptr<snapshot_state> state_;
};

// Slint allows exactly one platform per process. Test discovery runs one case
// per process, but direct Catch2 execution does not; install once in either
// mode and reconfigure the shared state for each test window.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- callers pass named width/height locals, not literals.
auto use_snapshot_platform(std::uint32_t const width = snapshot_platform::width, std::uint32_t const height = snapshot_platform::height)
    -> std::shared_ptr<snapshot_state>
{
    static auto const state = []() -> std::shared_ptr<snapshot_state>
    {
        auto shared = std::make_shared<snapshot_state>();
        slint::platform::set_platform(std::make_unique<snapshot_platform>(shared));
        return shared;
    }();
    state->window_adapter = nullptr;
    state->width = width;
    state->height = height;
    return state;
}

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
    window->set_board_san_moves(std::make_shared<slint::VectorModel<slint::SharedString>>());
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
        RecentEntry {.name = slint::SharedString {"Main"}, .path = slint::SharedString {"/tmp/main.motif"}},
    }));

    auto const pixels = render_layout(*state, "setup-screen");
    REQUIRE(std::ranges::count_if(pixels, is_visible_non_background_pixel) > 5000);
}

TEST_CASE("workspace layout: renders the explorer sidebar tab", "[motif-slint-app][layout]")
{
    auto const state = use_snapshot_platform();

    auto window = WorkspaceWindow::create();
    window->set_has_database(true);
    window->set_is_scratch(true);
    window->set_display_name("Scratch");
    window->set_sidebar_tab(1);
    window->set_game_rows(std::make_shared<slint::VectorModel<GameRow>>());
    window->set_game_column_widths(std::make_shared<slint::VectorModel<std::int32_t>>());
    window->set_board_square_pieces(std::make_shared<slint::VectorModel<slint::SharedString>>());
    window->set_board_square_highlights(std::make_shared<slint::VectorModel<SquareHighlight>>());
    window->set_board_san_moves(std::make_shared<slint::VectorModel<slint::SharedString>>());
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
            auto moves = std::vector<slint::SharedString> {};
            moves.reserve(static_cast<std::size_t>(move_count));
            for (auto i = 0; i < move_count; ++i) {
                moves.emplace_back("Nf6");
            }
            window->set_board_san_moves(std::make_shared<slint::VectorModel<slint::SharedString>>(std::move(moves)));
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
            auto moves = std::vector<slint::SharedString> {};
            moves.reserve(static_cast<std::size_t>(move_count));
            for (auto i = 0; i < move_count; ++i) {
                moves.emplace_back("Nf6");
            }
            window->set_board_san_moves(std::make_shared<slint::VectorModel<slint::SharedString>>(std::move(moves)));
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
    window->set_board_san_moves(std::make_shared<slint::VectorModel<slint::SharedString>>());
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
    window->set_board_san_moves(std::make_shared<slint::VectorModel<slint::SharedString>>());
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
    auto moves = std::vector<slint::SharedString> {slint::SharedString {"e4"}, slint::SharedString {"e5"}};
    window->set_board_san_moves(std::make_shared<slint::VectorModel<slint::SharedString>>(std::move(moves)));
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
    window->set_board_san_moves(std::make_shared<slint::VectorModel<slint::SharedString>>());
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
