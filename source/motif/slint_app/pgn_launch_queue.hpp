#pragma once

#include <cstddef>
#include <deque>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace motif::slint_app
{

struct rejected_launch_argument
{
    std::string argument;
    std::string reason;
};

// PGN files named on the command line (or passed by a file manager through the
// desktop entry's %F). Valid paths wait here until a database is open; the
// application then feeds them, one at a time, to the existing import service.
class pgn_launch_queue
{
  public:
    pgn_launch_queue() = default;

    // Accepts regular files with a case-insensitive ".pgn" extension. Every
    // other argument (wrong extension, missing path, directory, ...) is
    // recorded in rejected() with a reason instead of being dropped.
    [[nodiscard]] static auto from_arguments(std::span<std::string const> arguments) -> pgn_launch_queue;

    // Validates and queues one path received after startup, such as a macOS
    // open-document Apple event. Invalid paths remain inspectable as rejected().
    void enqueue(std::string argument);

    [[nodiscard]] auto pending() const noexcept -> std::size_t { return paths_.size(); }

    [[nodiscard]] auto empty() const noexcept -> bool { return paths_.empty(); }

    [[nodiscard]] auto rejected() const noexcept -> std::vector<rejected_launch_argument> const& { return rejected_; }

    // One user-visible line describing every rejected argument; empty when
    // nothing was rejected.
    [[nodiscard]] auto rejection_message() const -> std::string;

    // Removes and returns the oldest pending path, or nullopt when empty.
    [[nodiscard]] auto take_next() -> std::optional<std::filesystem::path>;

    // Drops every pending path and returns how many were dropped.
    auto clear() noexcept -> std::size_t;

  private:
    std::deque<std::filesystem::path> paths_;
    std::vector<rejected_launch_argument> rejected_;
};

}  // namespace motif::slint_app
