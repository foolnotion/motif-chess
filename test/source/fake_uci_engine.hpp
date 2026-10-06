#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <fmt/format.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Shell-script UCI engines for tests. The script answers the handshake, logs every
// command it receives and every process id it runs under, and replies to `go`
// with a caller supplied body. Real engines never run in unit tests.
//
// A script can also stall one handshake phase: its first process receives `uci`
// (or `isready`), logs its pid, announces the arrival on a FIFO and then stays
// silent, still reading its input so `quit` ends it at once. Later processes of
// the same script answer normally. A test waits for the announcement with poll(2)
// on the FIFO, so it needs no sleep to know the handshake is blocked.
namespace fake_uci
{

inline auto unique_stamp() -> std::string
{
    static std::atomic<unsigned> counter {0};
    auto const stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return fmt::format("{}_{}", stamp, counter.fetch_add(1));
}

// The handshake command a fake engine stalls on, for its first process.
enum class blocked_phase : std::uint8_t
{
    none,
    uci,
    isready,
};

constexpr auto phase_name(blocked_phase const phase) -> std::string_view
{
    switch (phase) {
        case blocked_phase::uci:
            return "uci";
        case blocked_phase::isready:
            return "isready";
        case blocked_phase::none:
            break;
    }
    return "none";
}

// A fake engine on disk. The script, its command log and its FIFO are removed with it.
class fake_engine
{
  public:
    // `go_body` is shell text executed for each `go` command; every line needs
    // its own indentation and trailing newline.
    explicit fake_engine(std::string_view go_body, blocked_phase const phase = blocked_phase::none)
        : path_ {std::filesystem::temp_directory_path() / fmt::format("motif_fake_uci_engine_{}.sh", unique_stamp())}
        , log_ {path_.string() + ".log"}
        , gate_ {path_.string() + ".gate"}
        , once_ {path_.string() + ".once"}
    {
        // The script appends to the log only while it exists, so a process that
        // outlives this object (its owner quits it later) cannot recreate it.
        std::ofstream {log_}.close();
        if (phase != blocked_phase::none) {
            // O_RDWR keeps open(2) from blocking on either side; the script's
            // announcement waits in the pipe until wait_blocked() sees it.
            if (::mkfifo(gate_.c_str(), S_IRUSR | S_IWUSR) != 0) {
                throw std::runtime_error {"fake_engine: mkfifo failed"};
            }
            // open(2) is variadic in its declaration; there is no other way to get a descriptor to poll.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
            gate_fd_ = ::open(gate_.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (gate_fd_ < 0) {
                throw std::runtime_error {"fake_engine: cannot open the gate"};
            }
        }
        auto script = std::ofstream {path_};
        script << "#!/usr/bin/env sh\n"
                  "LOG='"
               << log_.string() << "'\nGATE='" << gate_.string() << "'\nONCE='" << once_.string() << "'\nPHASE='" << phase_name(phase)
               << "'\n"
                  // Stalls the first process on phase `$1`: succeeds once, and only while the gate exists.
                  "stall() {\n"
                  "  [ \"$PHASE\" = \"$1\" ] && [ ! -e \"$ONCE\" ] && [ -e \"$GATE\" ] || return 1\n"
                  "  : > \"$ONCE\"\n"
                  "  printf '%s\\n' \"$1\" > \"$GATE\"\n"
                  "}\n"
                  "while IFS= read -r line; do\n"
                  "  [ -e \"$LOG\" ] && printf '%s\\n' \"$line\" >> \"$LOG\"\n"
                  "  case \"$line\" in\n"
                  "    uci)\n"
                  "      [ -e \"$LOG\" ] && printf '%s\\n' \"pid $$\" >> \"$LOG\"\n"
                  "      stall uci && continue\n"
                  "      printf '%s\\n' 'id name MotifFake'\n"
                  "      printf '%s\\n' 'uciok'\n"
                  "      ;;\n"
                  "    isready)\n"
                  "      stall isready && continue\n"
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
        if (gate_fd_ >= 0) {
            ::close(gate_fd_);
        }
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(log_, ignored);
        std::filesystem::remove(gate_, ignored);
        std::filesystem::remove(once_, ignored);
    }

    fake_engine(fake_engine const&) = delete;
    auto operator=(fake_engine const&) -> fake_engine& = delete;
    fake_engine(fake_engine&&) = delete;
    auto operator=(fake_engine&&) -> fake_engine& = delete;

    [[nodiscard]] auto path() const -> std::string { return path_.string(); }

    // Waits until the first process has reached its stalled handshake phase; its
    // process id is in the command log by then. Blocks in poll(2) rather than in a
    // loop with sleeps. One-shot: it consumes the announcement, so a second call
    // waits for a further one (which never comes, the script stalls once). False if
    // nothing arrived within `timeout` or the engine stalls no phase.
    [[nodiscard]] auto wait_blocked(std::chrono::milliseconds const timeout = std::chrono::seconds {10}) const -> bool
    {
        if (gate_fd_ < 0) {
            return false;
        }
        auto descriptor = pollfd {.fd = gate_fd_, .events = POLLIN, .revents = 0};
        if (::poll(&descriptor, 1, static_cast<int>(timeout.count())) != 1 || descriptor.revents != POLLIN) {
            return false;
        }
        constexpr auto drain_size = std::size_t {64};
        auto drained = std::array<char, drain_size> {};
        return ::read(gate_fd_, drained.data(), drained.size()) > 0;
    }

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
    std::filesystem::path gate_;
    std::filesystem::path once_;
    int gate_fd_ {-1};
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

constexpr auto poll_interval = std::chrono::milliseconds {5};

// Polls `done` until it holds or the timeout passes.
template<typename Predicate>
auto wait_for(Predicate const& done, std::chrono::milliseconds const timeout = std::chrono::seconds {10}) -> bool
{
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return done();
        }
        std::this_thread::sleep_for(poll_interval);
    }
    return true;
}

}  // namespace fake_uci
