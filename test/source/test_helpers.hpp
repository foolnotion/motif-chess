#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace test_helpers
{

inline constexpr bool is_sanitized_build = []() -> bool
{
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_UNDEFINED__)
    return true;
#else
#    ifdef __has_feature
#        if __has_feature(address_sanitizer)
    return true;
#        elif __has_feature(undefined_behavior_sanitizer)
    return true;
#        else
    return false;
#        endif
#    else
    return false;
#    endif
#endif
}();

#ifdef __linux__
inline auto read_rss_bytes() -> std::size_t
{
    static constexpr std::string_view vmrss_key = "VmRSS:";
    static constexpr std::size_t bytes_per_kb = 1024;

    std::ifstream status_file("/proc/self/status");
    std::string line;
    while (std::getline(status_file, line)) {
        if (line.starts_with(vmrss_key)) {
            std::size_t size_kb = 0;
            std::istringstream iss(line.substr(vmrss_key.size()));
            iss >> size_kb;
            return size_kb * bytes_per_kb;
        }
    }
    return 0;
}
#else
inline auto read_rss_bytes() -> std::size_t
{
    return 0;
}
#endif

class peak_rss_sampler
{
  public:
    peak_rss_sampler()
        : thread_([this]() -> void { sample_loop(); })
    {
    }

    ~peak_rss_sampler()
    {
        running_.store(false, std::memory_order_relaxed);
        thread_.join();
    }

    peak_rss_sampler(peak_rss_sampler const&) = delete;
    peak_rss_sampler(peak_rss_sampler&&) = delete;
    auto operator=(peak_rss_sampler const&) -> peak_rss_sampler& = delete;
    auto operator=(peak_rss_sampler&&) -> peak_rss_sampler& = delete;

    auto peak() const -> std::size_t { return peak_.load(std::memory_order_relaxed); }

  private:
    void sample_loop()
    {
        static constexpr auto poll_interval = std::chrono::milliseconds(50);
        while (running_.load(std::memory_order_relaxed)) {
            auto const rss = read_rss_bytes();
            auto cur = peak_.load(std::memory_order_relaxed);
            while (rss > cur && !peak_.compare_exchange_weak(cur, rss, std::memory_order_relaxed)) {}
            std::this_thread::sleep_for(poll_interval);
        }
    }

    std::atomic<bool> running_ {true};
    std::atomic<std::size_t> peak_ {0};
    std::thread thread_;
};

}  // namespace test_helpers
