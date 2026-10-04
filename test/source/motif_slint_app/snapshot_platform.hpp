#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <slint-platform.h>
#include <slint.h>

// Headless software-rendered Slint platform shared by the layout and host
// tests. Slint allows exactly one platform per process, so every test file
// goes through use_snapshot_platform().
namespace snapshot_test
{

class snapshot_window_adapter;

struct snapshot_state
{
    snapshot_window_adapter* window_adapter {nullptr};
    // Size of the next window adapter the platform creates.
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

// Slint allows exactly one platform per process: a second set_platform call
// aborts. Install it once and hand every test the same state, reconfigured
// with the window size that test needs. Running the test binary directly
// (without ctest's one-process-per-case isolation) therefore stays safe.
// Width and height are swappable by type only; every caller passes a named pair.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
inline auto use_snapshot_platform(std::uint32_t const width = snapshot_platform::width,
                                  std::uint32_t const height = snapshot_platform::height) -> std::shared_ptr<snapshot_state>
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

}  // namespace snapshot_test
