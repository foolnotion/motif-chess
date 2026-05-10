#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "motif/app/error.hpp"

namespace motif::app
{

static constexpr int default_prefetch_depth = 5;

struct ui_preferences
{
    std::string board_theme {"classic"};
    std::string piece_set {"standard"};
    int prefetch_depth {default_prefetch_depth};
};

struct recent_database_entry
{
    std::string name;
    std::string path;
};

struct app_config
{
    std::string database_directory;
    std::vector<recent_database_entry> recent_databases;
    std::vector<std::string> engine_paths;
    ui_preferences ui_preferences;
};

// Returns $XDG_CONFIG_HOME/motif-chess/config.json or
// $HOME/.config/motif-chess/config.json when XDG_CONFIG_HOME is unset.
[[nodiscard]] auto config_path() -> std::filesystem::path;

[[nodiscard]] auto default_config() -> app_config;

// Load from canonical config_path(). Creates and writes defaults if none exists.
// Returns malformed_config if the file exists but cannot be parsed; the
// malformed file is left untouched.
[[nodiscard]] auto load_config() -> result<app_config>;

// Load from an explicit path (for testing with injected XDG root).
[[nodiscard]] auto load_config(std::filesystem::path const& path) -> result<app_config>;

// Save to canonical config_path().
[[nodiscard]] auto save_config(app_config const& config) -> result<void>;

// Save to an explicit path (for testing).
[[nodiscard]] auto save_config(app_config const& config, std::filesystem::path const& path) -> result<void>;

// Add or promote path to front of recent list, deduplicated, max 20 entries.
void push_recent(app_config& config, std::string const& name, std::string const& path);

// Remove all entries with the given path.
void remove_recent(app_config& config, std::string const& path);

}  // namespace motif::app
