#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ios>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "motif/import/logger.hpp"

#include <spdlog/async_logger.h>
#include <spdlog/common.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>
#include <tl/expected.hpp>

#include "motif/import/error.hpp"

namespace
{

constexpr std::size_t async_queue_capacity =
    8192;  // NOLINT(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)
constexpr std::size_t rotate_file_count = 3;
constexpr std::uintmax_t rotate_max_bytes = 5ULL * 1024ULL
    * 1024ULL;  // NOLINT(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)
constexpr std::array<std::string_view, 3> logger_names {
    "motif.db", "motif.import", "motif.search"};
constexpr unsigned char control_character_limit = 0x20U;

auto logging_mutex() -> std::mutex&
{
    static std::mutex mutex;
    return mutex;
}

auto logging_thread_pool() -> std::shared_ptr<spdlog::details::thread_pool>&
{
    static std::shared_ptr<spdlog::details::thread_pool> thread_pool;
    return thread_pool;
}

auto current_config() -> std::optional<motif::import::log_config>&
{
    static std::optional<motif::import::log_config> config;
    return config;
}

auto to_std_string_view(const spdlog::string_view_t value) -> std::string_view
{
    return {value.data(), value.size()};
}

auto escape_json_string(std::string_view value) -> std::string
{
    std::string escaped;
    escaped.reserve(value.size());

    for (const auto character : value) {
        const auto code = static_cast<unsigned char>(character);
        switch (character) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                if (code < control_character_limit) {
                    std::ostringstream unicode_escape;
                    unicode_escape << "\\u" << std::hex << std::setw(4)
                                   << std::setfill('0')
                                   << static_cast<int>(code);
                    escaped += unicode_escape.str();
                } else {
                    escaped += character;
                }
                break;
        }
    }

    return escaped;
}

auto format_timestamp(const std::chrono::system_clock::time_point time_point)
    -> std::string
{
    const auto time = std::chrono::system_clock::to_time_t(time_point);
    const auto millisec = std::chrono::duration_cast<std::chrono::milliseconds>(
                              time_point.time_since_epoch())
        % 1000;

    std::tm timestamp {};
#ifdef _WIN32
    localtime_s(&timestamp, &time);
#else
    localtime_r(&time, &timestamp);
#endif

    std::ostringstream stream;
    stream << std::put_time(&timestamp, "%Y-%m-%dT%H:%M:%S") << '.'
           << std::setw(3) << std::setfill('0') << millisec.count();

    return stream.str();
}

// NOLINTNEXTLINE(portability-template-virtual-member-function)
class jsonl_file_sink final : public spdlog::sinks::base_sink<std::mutex>
{
  public:
    explicit jsonl_file_sink(const std::filesystem::path& path)
        : stream_(path, std::ios::app)
    {
        if (!stream_.is_open()) {
            throw spdlog::spdlog_ex("failed to open json log file");
        }
    }

  protected:
    void sink_it_(const spdlog::details::log_msg& msg) override
    {
        stream_ << R"({"ts":")" << format_timestamp(msg.time)
                << R"(","level":")"
                << to_std_string_view(spdlog::level::to_string_view(msg.level))
                << R"(","logger":")"
                << escape_json_string(to_std_string_view(msg.logger_name))
                << R"(","msg":")"
                << escape_json_string(to_std_string_view(msg.payload))
                << R"("})" << '\n';

        if (!stream_) {
            throw spdlog::spdlog_ex("failed to write json log file");
        }
    }

    void flush_() override
    {
        stream_.flush();
        if (!stream_) {
            throw spdlog::spdlog_ex("failed to flush json log file");
        }
    }

  private:
    std::ofstream stream_;
};

using jsonl_file_sink_mt = jsonl_file_sink;

auto config_matches(const motif::import::log_config& lhs,
                    const motif::import::log_config& rhs) -> bool
{
    return lhs.log_dir == rhs.log_dir && lhs.json_sink == rhs.json_sink;
}

auto drop_registered_loggers() -> bool
{
    auto flush_failed = false;

    for (const auto name : logger_names) {
        if (const auto logger = spdlog::get(std::string {name});
            logger != nullptr)
        {
            try {
                logger->flush();
            } catch (const spdlog::spdlog_ex&) {
                flush_failed = true;
            }
        }
        spdlog::drop(std::string {name});
    }

    return flush_failed;
}

[[nodiscard]] auto all_loggers_registered() -> bool
{
    return std::ranges::all_of(
        logger_names,
        [](const auto name) -> bool
        { return spdlog::get(std::string {name}) != nullptr; });
}

[[nodiscard]] auto any_logger_registered() -> bool
{
    return std::ranges::any_of(
        logger_names,
        [](const auto name) -> bool
        { return spdlog::get(std::string {name}) != nullptr; });
}

}  // namespace

namespace motif::import
{

auto initialize_logging(log_config const& cfg) -> result<void>
{
    namespace fs = std::filesystem;

    const std::scoped_lock lock(logging_mutex());

    if (all_loggers_registered()) {
        if (const auto& existing_config = current_config();
            existing_config.has_value())
        {
            if (config_matches(existing_config.value(), cfg)) {
                return {};
            }
        }

        return tl::unexpected<error_code> {error_code::invalid_state};
    }

    if (any_logger_registered()) {
        return tl::unexpected<error_code> {error_code::invalid_state};
    }

    if (const auto& existing_config = current_config();
        existing_config.has_value())
    {
        if (config_matches(existing_config.value(), cfg)) {
            return tl::unexpected<error_code> {error_code::invalid_state};
        }

        return tl::unexpected<error_code> {error_code::invalid_state};
    }

    if (logging_thread_pool() != nullptr) {
        return tl::unexpected<error_code> {error_code::invalid_state};
    }

    try {
        fs::create_directories(cfg.log_dir);

        logging_thread_pool() = std::make_shared<spdlog::details::thread_pool>(
            async_queue_capacity, 1);

        auto text_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            (cfg.log_dir / "motif-chess.log").string(),
            rotate_max_bytes,
            rotate_file_count);
        text_sink->set_pattern("[%Y-%m-%dT%H:%M:%S.%e] [%l] [%n] %v");

        std::vector<spdlog::sink_ptr> sinks {text_sink};

        if (cfg.json_sink) {
            sinks.push_back(std::make_shared<jsonl_file_sink_mt>(
                cfg.log_dir / "motif-chess.jsonl"));
        }

        for (const auto name : logger_names) {
            auto logger = std::make_shared<spdlog::async_logger>(
                std::string {name},
                sinks.begin(),
                sinks.end(),
                logging_thread_pool(),
                spdlog::async_overflow_policy::block);
            logger->set_level(spdlog::level::trace);
            spdlog::register_logger(logger);
        }

        current_config() = cfg;
    } catch (const fs::filesystem_error&) {
        static_cast<void>(drop_registered_loggers());
        logging_thread_pool().reset();
        current_config().reset();
        return tl::unexpected<error_code> {error_code::io_failure};
    } catch (const spdlog::spdlog_ex&) {
        static_cast<void>(drop_registered_loggers());
        logging_thread_pool().reset();
        current_config().reset();
        return tl::unexpected<error_code> {error_code::io_failure};
    } catch (const std::exception&) {
        static_cast<void>(drop_registered_loggers());
        logging_thread_pool().reset();
        current_config().reset();
        return tl::unexpected<error_code> {error_code::io_failure};
    }

    return {};
}

auto shutdown_logging() -> result<void>
{
    const std::scoped_lock lock(logging_mutex());

    if (drop_registered_loggers()) {
        logging_thread_pool().reset();
        current_config().reset();
        return tl::unexpected<error_code> {error_code::io_failure};
    }

    logging_thread_pool().reset();
    current_config().reset();

    return {};
}

}  // namespace motif::import
