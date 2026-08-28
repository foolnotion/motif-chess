#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
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

struct ui_preferences
{
    std::string board_theme {"classic"};
    std::string piece_set {"standard"};
    int prefetch_depth {default_prefetch_depth};
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
