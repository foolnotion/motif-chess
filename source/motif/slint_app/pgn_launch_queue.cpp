#include <algorithm>
#include <cctype>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "motif/slint_app/pgn_launch_queue.hpp"

#include <fmt/format.h>

namespace motif::slint_app
{

namespace
{

auto has_pgn_extension(std::filesystem::path const& path) -> bool
{
    auto extension = path.extension().string();
    std::ranges::transform(extension,
                           extension.begin(),
                           [](char const chr) -> char { return static_cast<char>(std::tolower(static_cast<unsigned char>(chr))); });
    return extension == ".pgn";
}

// Returns the rejection reason, or nullopt when the path names a regular file.
auto check_regular_file(std::filesystem::path const& path) -> std::optional<std::string>
{
    std::error_code fs_error;
    auto const status = std::filesystem::status(path, fs_error);
    if (fs_error == std::errc::no_such_file_or_directory || status.type() == std::filesystem::file_type::not_found) {
        return "path does not exist";
    }
    if (fs_error) {
        return "path cannot be read";
    }
    if (!std::filesystem::is_regular_file(status)) {
        return "not a regular file";
    }
    return std::nullopt;
}

}  // namespace

auto pgn_launch_queue::from_arguments(std::span<std::string const> const arguments) -> pgn_launch_queue
{
    auto queue = pgn_launch_queue {};
    for (auto const& argument : arguments) {
        if (argument.empty()) {
            queue.rejected_.push_back({.argument = argument, .reason = "empty path"});
            continue;
        }

        auto const candidate = std::filesystem::path {argument};
        if (!has_pgn_extension(candidate)) {
            queue.rejected_.push_back({.argument = argument, .reason = "not a .pgn file"});
            continue;
        }

        std::error_code fs_error;
        auto absolute = std::filesystem::absolute(candidate, fs_error);
        if (fs_error) {
            absolute = candidate;
        }
        if (auto reason = check_regular_file(absolute)) {
            queue.rejected_.push_back({.argument = argument, .reason = std::move(*reason)});
            continue;
        }
        queue.paths_.push_back(std::move(absolute));
    }
    return queue;
}

auto pgn_launch_queue::rejection_message() const -> std::string
{
    if (rejected_.empty()) {
        return {};
    }
    auto message = fmt::format("Skipped {} launch argument{}: ", rejected_.size(), rejected_.size() == 1 ? "" : "s");
    auto separator = std::string_view {};
    for (auto const& entry : rejected_) {
        message += fmt::format("{}'{}' ({})", separator, entry.argument, entry.reason);
        separator = "; ";
    }
    return message;
}

auto pgn_launch_queue::take_next() -> std::optional<std::filesystem::path>
{
    if (paths_.empty()) {
        return std::nullopt;
    }
    auto next = std::move(paths_.front());
    paths_.pop_front();
    return next;
}

auto pgn_launch_queue::clear() noexcept -> std::size_t
{
    auto const dropped = paths_.size();
    paths_.clear();
    return dropped;
}

}  // namespace motif::slint_app
