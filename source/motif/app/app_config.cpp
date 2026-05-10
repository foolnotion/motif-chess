#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "motif/app/app_config.hpp"

#include <glaze/core/reflect.hpp>
#include <glaze/json/read.hpp>
#include <glaze/json/write.hpp>
#include <tl/expected.hpp>

#include "motif/app/error.hpp"

namespace motif::app
{

namespace
{

// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto read_file(std::filesystem::path const& path) -> result<std::string>
{
    std::ifstream file {path};
    if (!file.is_open()) {
        return tl::unexpected {error_code::io_failure};
    }
    std::ostringstream oss;
    oss << file.rdbuf();
    if (!file) {
        return tl::unexpected {error_code::io_failure};
    }
    return oss.str();
}

// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto write_file(std::filesystem::path const& path, std::string const& content) -> result<void>
{
    std::ofstream file {path};
    if (!file.is_open()) {
        return tl::unexpected {error_code::io_failure};
    }
    file << content;
    if (!file) {
        return tl::unexpected {error_code::io_failure};
    }
    return {};
}

}  // namespace

auto config_path() -> std::filesystem::path
{
    char const* xdg = std::getenv("XDG_CONFIG_HOME");  // NOLINT(concurrency-mt-unsafe)
    std::filesystem::path base;
    if (xdg != nullptr && *xdg != '\0') {
        base = xdg;
    } else {
        char const* home = std::getenv("HOME");  // NOLINT(concurrency-mt-unsafe)
        base = (home != nullptr) ? std::filesystem::path {home} / ".config" : std::filesystem::temp_directory_path();
    }
    return base / "motif-chess" / "config.json";
}

auto default_config() -> app_config
{
    return app_config {
        .database_directory = "",
        .recent_databases = {},
        .engine_paths = {},
        .ui_preferences = {.board_theme = "classic", .piece_set = "standard", .prefetch_depth = default_prefetch_depth},
    };
}

auto load_config(std::filesystem::path const& path) -> result<app_config>
{
    std::error_code fs_err;
    auto const exists = std::filesystem::exists(path, fs_err);
    if (fs_err) {
        return tl::unexpected {error_code::io_failure};
    }

    if (!exists) {
        auto cfg = default_config();
        if (auto save_res = save_config(cfg, path); !save_res) {
            return tl::unexpected {save_res.error()};
        }
        return cfg;
    }

    auto content_res = read_file(path);
    if (!content_res) {
        return tl::unexpected {content_res.error()};
    }

    app_config cfg;
    auto const read_err = glz::read_json(cfg, *content_res);
    if (read_err) {
        return tl::unexpected {error {error_code::malformed_config, glz::format_error(read_err, *content_res)}};
    }
    return cfg;
}

auto load_config() -> result<app_config>
{
    return load_config(config_path());
}

auto save_config(app_config const& config, std::filesystem::path const& path) -> result<void>
{
    std::error_code fs_err;
    std::filesystem::create_directories(path.parent_path(), fs_err);
    if (fs_err) {
        return tl::unexpected {error_code::io_failure};
    }

    std::string buffer;
    auto const write_err = glz::write_json(config, buffer);
    if (write_err) {
        return tl::unexpected {error_code::io_failure};
    }
    return write_file(path, buffer);
}

auto save_config(app_config const& config) -> result<void>
{
    return save_config(config, config_path());
}

void push_recent(app_config& config, std::string const& name, std::string const& path)
{
    auto& list = config.recent_databases;
    std::erase_if(list, [&path](recent_database_entry const& entry) -> bool { return entry.path == path; });
    list.insert(list.begin(), recent_database_entry {.name = name, .path = path});
    static constexpr std::size_t max_recent = 20;
    if (list.size() > max_recent) {
        list.resize(max_recent);
    }
}

void remove_recent(app_config& config, std::string const& path)
{
    std::erase_if(config.recent_databases, [&path](recent_database_entry const& entry) -> bool { return entry.path == path; });
}

}  // namespace motif::app
