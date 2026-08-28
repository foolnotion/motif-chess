#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <system_error>

#include "motif/slint_app/config.hpp"

#include <glaze/json/read.hpp>
#include <glaze/json/write.hpp>

namespace motif::slint_app
{

namespace
{

auto io_error() -> config_error
{
    return {.code = config_error_code::io_failure, .message = "Could not read or write workspace configuration."};
}

auto temporary_path(std::filesystem::path const& path) -> std::filesystem::path
{
    auto const nonce = std::random_device {}();
    return path.parent_path() / (path.filename().string() + ".tmp-" + std::to_string(nonce));
}

}  // namespace

auto config_path() -> std::filesystem::path
{
    char const* const xdg = std::getenv("XDG_CONFIG_HOME");  // NOLINT(concurrency-mt-unsafe)
    if (xdg != nullptr && *xdg != '\0') {
        return std::filesystem::path {xdg} / "motif-chess" / "config.json";
    }

    char const* const home = std::getenv("HOME");  // NOLINT(concurrency-mt-unsafe)
    auto const base = home != nullptr ? std::filesystem::path {home} / ".config" : std::filesystem::temp_directory_path();
    return base / "motif-chess" / "config.json";
}

auto load_config(std::filesystem::path const& path) -> config_result<workspace_config>
{
    std::error_code fs_error;
    auto const exists = std::filesystem::exists(path, fs_error);
    if (fs_error) {
        return tl::unexpected {io_error()};
    }
    if (!exists) {
        return workspace_config {};
    }

    auto file = std::ifstream {path};
    if (!file.is_open()) {
        return tl::unexpected {io_error()};
    }
    auto const contents = std::string {std::istreambuf_iterator<char> {file}, {}};
    if (!file.good() && !file.eof()) {
        return tl::unexpected {io_error()};
    }

    auto config = workspace_config {};
    auto const read_error = glz::read_json(config, contents);
    if (read_error) {
        return tl::unexpected {
            config_error {.code = config_error_code::malformed_config, .message = glz::format_error(read_error, contents)}};
    }
    return config;
}

auto load_config() -> config_result<workspace_config>
{
    return load_config(config_path());
}

auto save_config(workspace_config const& config, std::filesystem::path const& path) -> config_result<void>
{
    auto const parent_path = path.parent_path();
    if (parent_path.empty()) {
        return tl::unexpected {io_error()};
    }

    std::error_code fs_error;
    std::filesystem::create_directories(parent_path, fs_error);
    if (fs_error) {
        return tl::unexpected {io_error()};
    }

    auto contents = std::string {};
    if (glz::write_json(config, contents)) {
        return tl::unexpected {io_error()};
    }

    auto const temp_path = temporary_path(path);
    {
        auto file = std::ofstream {temp_path, std::ios::binary | std::ios::trunc};
        if (!file.is_open()) {
            return tl::unexpected {io_error()};
        }
        file << contents;
        if (!file) {
            std::filesystem::remove(temp_path, fs_error);
            return tl::unexpected {io_error()};
        }
    }

    std::filesystem::rename(temp_path, path, fs_error);
    if (fs_error) {
        std::filesystem::remove(temp_path, fs_error);
        return tl::unexpected {io_error()};
    }
    return {};
}

auto save_config(workspace_config const& config) -> config_result<void>
{
    return save_config(config, config_path());
}

void push_recent(workspace_config& config, std::string const& name, std::string const& path)
{
    auto& recent = config.recent_databases;
    std::erase_if(recent, [&path](recent_entry const& entry) -> bool { return entry.path == path; });
    recent.insert(recent.begin(), {.name = name, .path = path});
    constexpr std::size_t max_recent_count {20};
    if (recent.size() > max_recent_count) {
        recent.resize(max_recent_count);
    }
}

}  // namespace motif::slint_app
