#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace motif::app
{

struct pgn_path_result
{
    std::string raw;
    std::filesystem::path resolved;
    bool valid {false};
    std::string error_message;
};

struct pgn_launch_queue
{
    std::vector<std::filesystem::path> valid_paths;
    std::vector<pgn_path_result> invalid_paths;

    [[nodiscard]] auto empty() const noexcept -> bool { return valid_paths.empty(); }

    [[nodiscard]] auto size() const noexcept -> std::size_t { return valid_paths.size(); }
};

// Parse positional arguments for .pgn file paths (case-insensitive extension check).
// Non-.pgn args, missing paths, and directories land in invalid_paths.
[[nodiscard]] auto parse_pgn_args(std::vector<std::string> const& args) -> pgn_launch_queue;

}  // namespace motif::app
