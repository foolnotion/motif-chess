#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "motif/app/pgn_launch_queue.hpp"

namespace motif::app
{

namespace
{

// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto has_pgn_extension(std::filesystem::path const& path) -> bool
{
    auto ext = path.extension().string();
    for (auto& chr : ext) {
        chr = static_cast<char>(std::tolower(static_cast<unsigned char>(chr)));
    }
    return ext == ".pgn";
}

}  // namespace

auto parse_pgn_args(std::vector<std::string> const& args) -> pgn_launch_queue
{
    pgn_launch_queue queue;
    for (auto const& raw : args) {
        std::filesystem::path const candidate {raw};

        if (!has_pgn_extension(candidate)) {
            queue.invalid_paths.push_back(pgn_path_result {
                .raw = raw,
                .resolved = candidate,
                .valid = false,
                .error_message = "not a .pgn file",
            });
            continue;
        }

        std::error_code fs_err;
        auto const abs = std::filesystem::absolute(candidate, fs_err);
        auto const resolved = fs_err ? candidate : abs;

        auto const exists = std::filesystem::exists(resolved, fs_err);
        if (fs_err || !exists) {
            queue.invalid_paths.push_back(pgn_path_result {
                .raw = raw,
                .resolved = resolved,
                .valid = false,
                .error_message = "path does not exist",
            });
            continue;
        }

        auto const is_file = std::filesystem::is_regular_file(resolved, fs_err);
        if (fs_err || !is_file) {
            queue.invalid_paths.push_back(pgn_path_result {
                .raw = raw,
                .resolved = resolved,
                .valid = false,
                .error_message = "not a regular file",
            });
            continue;
        }

        queue.valid_paths.push_back(resolved);
    }
    return queue;
}

}  // namespace motif::app
