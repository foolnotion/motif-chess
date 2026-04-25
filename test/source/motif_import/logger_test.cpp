#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "motif/import/logger.hpp"

#include <catch2/catch_test_macros.hpp>
#include <spdlog/spdlog.h>

#include "motif/import/error.hpp"

namespace fs = std::filesystem;

namespace
{

auto make_temp_log_dir() -> fs::path
{
    static std::atomic_uint64_t counter {0};

    const auto suffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + std::to_string(counter.fetch_add(1));
    const auto log_path = fs::temp_directory_path() / ("motif_logger_test_" + suffix);
    fs::create_directories(log_path);
    return log_path;
}

}  // namespace

TEST_CASE("logger: named loggers are registered after initialize_logging", "[motif-import]")
{
    const auto log_dir = make_temp_log_dir();
    const auto init_result = motif::import::initialize_logging({.log_dir = log_dir, .json_sink = false});
    REQUIRE(init_result.has_value());

    REQUIRE(spdlog::get("motif.db") != nullptr);
    REQUIRE(spdlog::get("motif.import") != nullptr);
    REQUIRE(spdlog::get("motif.search") != nullptr);

    const auto shutdown_result = motif::import::shutdown_logging();
    REQUIRE(shutdown_result.has_value());
    fs::remove_all(log_dir);
}

TEST_CASE("logger: emitting at all levels does not crash", "[motif-import]")
{
    const auto log_dir = make_temp_log_dir();
    const auto init_result = motif::import::initialize_logging({.log_dir = log_dir, .json_sink = false});
    REQUIRE(init_result.has_value());

    const auto logger = spdlog::get("motif.import");
    REQUIRE(logger != nullptr);

    logger->trace("trace message");
    logger->debug("debug message");
    logger->info("info message");
    logger->warn("warn message");
    logger->error("error message");
    logger->critical("critical message");

    logger->flush();

    const auto shutdown_result = motif::import::shutdown_logging();
    REQUIRE(shutdown_result.has_value());
    fs::remove_all(log_dir);
}

TEST_CASE("logger: json_sink creates two sinks per logger", "[motif-import]")
{
    const auto log_dir = make_temp_log_dir();
    const auto init_result = motif::import::initialize_logging({.log_dir = log_dir, .json_sink = true});
    REQUIRE(init_result.has_value());

    const auto logger = spdlog::get("motif.import");
    REQUIRE(logger != nullptr);
    const auto& sinks = logger->sinks();
    REQUIRE(sinks.size() == 2);

    const auto shutdown_result = motif::import::shutdown_logging();
    REQUIRE(shutdown_result.has_value());
    fs::remove_all(log_dir);
}

TEST_CASE("logger: initialize_logging is idempotent", "[motif-import]")
{
    const auto log_dir = make_temp_log_dir();

    const auto first_init = motif::import::initialize_logging({.log_dir = log_dir, .json_sink = false});
    REQUIRE(first_init.has_value());

    const auto second_init = motif::import::initialize_logging({.log_dir = log_dir, .json_sink = false});
    REQUIRE(second_init.has_value());

    const auto logger = spdlog::get("motif.import");
    REQUIRE(logger != nullptr);
    REQUIRE(logger->sinks().size() == 1);

    const auto shutdown_result = motif::import::shutdown_logging();
    REQUIRE(shutdown_result.has_value());
    fs::remove_all(log_dir);
}

TEST_CASE("logger: initialize_logging rejects incompatible reconfiguration", "[motif-import]")
{
    const auto log_dir = make_temp_log_dir();

    const auto first_init = motif::import::initialize_logging({.log_dir = log_dir, .json_sink = false});
    REQUIRE(first_init.has_value());

    const auto second_init = motif::import::initialize_logging({.log_dir = log_dir, .json_sink = true});
    REQUIRE_FALSE(second_init.has_value());
    REQUIRE(second_init.error() == motif::import::error_code::invalid_state);

    const auto logger = spdlog::get("motif.import");
    REQUIRE(logger != nullptr);
    REQUIRE(logger->sinks().size() == 1);

    const auto shutdown_result = motif::import::shutdown_logging();
    REQUIRE(shutdown_result.has_value());
    fs::remove_all(log_dir);
}

TEST_CASE("logger: initialize_logging rejects partial logger state", "[motif-import]")
{
    const auto log_dir = make_temp_log_dir();

    const auto init_result = motif::import::initialize_logging({.log_dir = log_dir, .json_sink = false});
    REQUIRE(init_result.has_value());

    spdlog::drop("motif.search");

    const auto second_init = motif::import::initialize_logging({.log_dir = log_dir, .json_sink = false});
    REQUIRE_FALSE(second_init.has_value());
    REQUIRE(second_init.error() == motif::import::error_code::invalid_state);

    const auto shutdown_result = motif::import::shutdown_logging();
    REQUIRE(shutdown_result.has_value());
    fs::remove_all(log_dir);
}

TEST_CASE("logger: initialize_logging returns io_failure for bad log path", "[motif-import]")
{
    const auto log_dir = make_temp_log_dir();
    const auto blocking_file = log_dir / "not_a_directory";

    {
        std::ofstream output(blocking_file);
        REQUIRE(output.is_open());
        output << "blocking file";
    }

    const auto init_result = motif::import::initialize_logging({.log_dir = blocking_file, .json_sink = false});
    REQUIRE_FALSE(init_result.has_value());
    REQUIRE(init_result.error() == motif::import::error_code::io_failure);

    REQUIRE(spdlog::get("motif.db") == nullptr);
    REQUIRE(spdlog::get("motif.import") == nullptr);
    REQUIRE(spdlog::get("motif.search") == nullptr);

    const auto shutdown_result = motif::import::shutdown_logging();
    REQUIRE(shutdown_result.has_value());
    fs::remove_all(log_dir);
}

TEST_CASE("logger: json_sink escapes messages as valid json text", "[motif-import]")
{
    const auto log_dir = make_temp_log_dir();

    const auto init_result = motif::import::initialize_logging({.log_dir = log_dir, .json_sink = true});
    REQUIRE(init_result.has_value());

    const auto logger = spdlog::get("motif.import");
    REQUIRE(logger != nullptr);

    logger->info(std::string {"quote \" slash \\ newline\n tab\t nul "} + std::string(1, '\0') + std::string {" bell "}
                 + std::string(1, '\b') + std::string {" formfeed "} + std::string(1, '\f'));
    logger->flush();

    const auto shutdown_result = motif::import::shutdown_logging();
    REQUIRE(shutdown_result.has_value());

    std::ifstream json_file(log_dir / "motif-chess.jsonl");
    REQUIRE(json_file.is_open());

    std::string line;
    REQUIRE(std::getline(json_file, line));
    REQUIRE(line.contains("\\\""));
    REQUIRE(line.contains("\\\\"));
    REQUIRE(line.contains("\\n"));
    REQUIRE(line.contains("\\t"));
    REQUIRE(line.contains("\\u0000"));
    REQUIRE(line.contains("\\u0008"));
    REQUIRE(line.contains("\\u000c"));

    fs::remove_all(log_dir);
}
