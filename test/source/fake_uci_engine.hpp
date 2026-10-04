#pragma once

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <sys/types.h>

// Shell-script UCI engines for tests. The script answers the handshake, logs every
// command it receives and every process id it runs under, and replies to `go`
// with a caller supplied body. Real engines never run in unit tests.
namespace fake_uci
{

inline auto unique_stamp() -> std::string
{
    static std::atomic<unsigned> counter {0};
    auto const stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return fmt::format("{}_{}", stamp, counter.fetch_add(1));
}

// A fake engine on disk. The script and its command log are removed with it.
class fake_engine
{
  public:
    // `go_body` is shell text executed for each `go` command; every line needs
    // its own indentation and trailing newline.
    explicit fake_engine(std::string_view go_body)
        : path_ {std::filesystem::temp_directory_path() / fmt::format("motif_fake_uci_engine_{}.sh", unique_stamp())}
        , log_ {path_.string() + ".log"}
    {
        // The script appends to the log only while it exists, so a process that
        // outlives this object (its owner quits it later) cannot recreate it.
        std::ofstream {log_}.close();
        auto script = std::ofstream {path_};
        script << "#!/usr/bin/env sh\n"
                  "LOG='"
               << log_.string()
               << "'\n"
                  "while IFS= read -r line; do\n"
                  "  [ -e \"$LOG\" ] && printf '%s\\n' \"$line\" >> \"$LOG\"\n"
                  "  case \"$line\" in\n"
                  "    uci)\n"
                  "      [ -e \"$LOG\" ] && printf '%s\\n' \"pid $$\" >> \"$LOG\"\n"
                  "      printf '%s\\n' 'id name MotifFake'\n"
                  "      printf '%s\\n' 'uciok'\n"
                  "      ;;\n"
                  "    isready)\n"
                  "      printf '%s\\n' 'readyok'\n"
                  "      ;;\n"
                  "    setoption*)\n"
                  "      ;;\n"
                  "    position*)\n"
                  "      ;;\n"
                  "    go*)\n"
               << go_body
               << "      ;;\n"
                  "    stop)\n"
                  "      printf '%s\\n' 'bestmove e2e4'\n"
                  "      ;;\n"
                  "    quit)\n"
                  "      exit 0\n"
                  "      ;;\n"
                  "  esac\n"
                  "done\n";
        script.close();
        std::filesystem::permissions(
            path_, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec);
    }

    ~fake_engine()
    {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(log_, ignored);
    }

    fake_engine(fake_engine const&) = delete;
    auto operator=(fake_engine const&) -> fake_engine& = delete;
    fake_engine(fake_engine&&) = delete;
    auto operator=(fake_engine&&) -> fake_engine& = delete;

    [[nodiscard]] auto path() const -> std::string { return path_.string(); }

    [[nodiscard]] auto commands() const -> std::vector<std::string>
    {
        auto lines = std::vector<std::string> {};
        auto file = std::ifstream {log_};
        for (auto line = std::string {}; std::getline(file, line);) {
            lines.push_back(line);
        }
        return lines;
    }

    // Commands starting with `prefix`, in arrival order across all processes.
    [[nodiscard]] auto commands_starting_with(std::string_view prefix) const -> std::vector<std::string>
    {
        auto matching = std::vector<std::string> {};
        for (auto& line : commands()) {
            if (line.starts_with(prefix)) {
                matching.push_back(std::move(line));
            }
        }
        return matching;
    }

    // One entry per process the script has run as, oldest first.
    [[nodiscard]] auto process_ids() const -> std::vector<pid_t>
    {
        auto ids = std::vector<pid_t> {};
        for (auto const& line : commands_starting_with("pid ")) {
            ids.push_back(static_cast<pid_t>(std::stol(line.substr(4))));
        }
        return ids;
    }

  private:
    std::filesystem::path path_;
    std::filesystem::path log_;
};

constexpr auto info_line = "      printf '%s\\n' 'info depth 1 seldepth 1 multipv 1 score cp 13 nodes 42 nps 1000 time 1 pv e2e4 e7e5'\n";

// Reports one line and finishes at once.
inline auto fast_complete_body() -> std::string
{
    return std::string {info_line} + "      printf '%s\\n' 'bestmove e2e4 ponder e7e5'\n";
}

// Reports one line and then searches until it is told to stop.
inline auto wait_for_stop_body() -> std::string
{
    return std::string {info_line};
}

// Dies mid-search.
inline auto crash_body() -> std::string
{
    return "      exit 1\n";
}

// Reports 400 numbered lines (score cp 1..400, rank 1 and, for two ranks, rank 2
// with cp 1001..1400) back to back: a burst. It then searches until told to stop.
inline auto burst_body(int const ranks = 1) -> std::string
{
    auto body = std::string {
        "      n=1\n"
        "      while [ \"$n\" -le 400 ]; do\n"
        "        printf '%s\\n' \"info depth $((n % 40 + 1)) multipv 1 score cp $n pv e2e4 e7e5\"\n"};
    if (ranks > 1) {
        body += "        printf '%s\\n' \"info depth $((n % 40 + 1)) multipv 2 score cp $((n + 1000)) pv d2d4 d7d5\"\n";
    }
    body += "        n=$((n + 1))\n"
            "      done\n";
    return body;
}

// The same burst, ending with a bestmove.
inline auto burst_complete_body() -> std::string
{
    return burst_body() + "      printf '%s\\n' 'bestmove e2e4'\n";
}

inline auto process_alive(pid_t const process) -> bool
{
    return ::kill(process, 0) == 0;
}

// Polls `done` until it holds or the timeout passes.
template<typename Predicate>
auto wait_for(Predicate const& done, std::chrono::milliseconds const timeout = std::chrono::seconds {10}) -> bool
{
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return done();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {5});
    }
    return true;
}

}  // namespace fake_uci
