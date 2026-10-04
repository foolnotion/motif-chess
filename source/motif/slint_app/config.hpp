#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <tl/expected.hpp>

namespace motif::slint_app
{

enum class config_error_code : std::uint8_t
{
    io_failure,
    malformed_config,
};

struct config_error
{
    config_error_code code;
    std::string message;

    [[nodiscard]] friend auto operator==(config_error const& lhs, config_error_code rhs) noexcept -> bool { return lhs.code == rhs; }
};

template<typename ValueType>
using config_result = tl::expected<ValueType, config_error>;

struct recent_entry
{
    std::string name;
    std::string path;
};

constexpr int default_prefetch_depth {5};

// Fixed workspace compositions selectable from the toolbar (Story 13.1). Stored
// by name so that a newer or hand-edited file with an unknown value degrades to
// the default instead of failing to load.
enum class workspace_preset_kind : std::uint8_t
{
    review,
    explorer,
    analysis,
};

[[nodiscard]] auto to_string(workspace_preset_kind kind) noexcept -> std::string_view;
[[nodiscard]] auto workspace_preset_from_string(std::string_view name) noexcept -> workspace_preset_kind;

// The single bounded limit of an engine analysis (Story 13.3): search to a depth
// or for a fixed time. The motif_engine API rejects a request with neither or both.
enum class analysis_limit_kind : std::uint8_t
{
    depth,
    movetime,
};

[[nodiscard]] auto to_string(analysis_limit_kind kind) noexcept -> std::string_view;
[[nodiscard]] auto analysis_limit_from_string(std::string_view name) noexcept -> analysis_limit_kind;

// What the engine panel lets a user choose. Values outside these ranges (a
// hand-edited file, a stale control) are clamped, never rejected.
constexpr int min_engine_multipv {1};
constexpr int max_engine_multipv {5};
constexpr int default_engine_multipv {3};
constexpr int min_engine_depth {1};
constexpr int max_engine_depth {99};
constexpr int default_engine_depth {20};
constexpr int min_engine_movetime_ms {100};
constexpr int max_engine_movetime_ms {600'000};
constexpr int default_engine_movetime_ms {1000};

[[nodiscard]] constexpr auto clamp_engine_multipv(int const value) noexcept -> int
{
    return std::clamp(value, min_engine_multipv, max_engine_multipv);
}

[[nodiscard]] constexpr auto clamp_engine_depth(int const value) noexcept -> int
{
    return std::clamp(value, min_engine_depth, max_engine_depth);
}

[[nodiscard]] constexpr auto clamp_engine_movetime_ms(int const value) noexcept -> int
{
    return std::clamp(value, min_engine_movetime_ms, max_engine_movetime_ms);
}

// The options an analysis is started with: MultiPV, which limit applies, and
// the stored value of each limit. Set together because they are saved together.
struct engine_options
{
    int multipv {default_engine_multipv};
    analysis_limit_kind limit {analysis_limit_kind::depth};
    int depth {default_engine_depth};
    int movetime_ms {default_engine_movetime_ms};
};

// The engine panel's persisted choices (Story 13.3), normalised: `selected` is
// always one of `paths` (the first when the stored choice is gone) and every
// number is inside its range.
struct engine_settings
{
    std::vector<std::string> paths;
    std::string selected;
    int multipv {default_engine_multipv};
    analysis_limit_kind limit {analysis_limit_kind::depth};
    int depth {default_engine_depth};
    int movetime_ms {default_engine_movetime_ms};
};

struct ui_preferences
{
    std::string board_theme {"classic"};
    std::string piece_set {"standard"};
    int prefetch_depth {default_prefetch_depth};
    std::string notation_layout {"right"};
    std::string workspace_preset {"review"};
    // Engine panel choices (Story 13.3). The engine itself is the path stored in
    // workspace_config::engine_paths; an empty or no longer listed selection
    // means the first engine. A running analysis is never stored.
    std::string engine_selected;
    int engine_multipv {default_engine_multipv};
    std::string engine_limit {"depth"};
    int engine_depth {default_engine_depth};
    int engine_movetime_ms {default_engine_movetime_ms};
};

struct workspace_config
{
    std::string database_directory;
    std::vector<recent_entry> recent_databases;
    std::vector<std::string> engine_paths;
    ui_preferences ui_preferences;
};

[[nodiscard]] auto config_path() -> std::filesystem::path;
[[nodiscard]] auto load_config(std::filesystem::path const& path) -> config_result<workspace_config>;
[[nodiscard]] auto load_config() -> config_result<workspace_config>;
[[nodiscard]] auto save_config(workspace_config const& config, std::filesystem::path const& path) -> config_result<void>;
[[nodiscard]] auto save_config(workspace_config const& config) -> config_result<void>;
void push_recent(workspace_config& config, std::string const& name, std::string const& path);

}  // namespace motif::slint_app
