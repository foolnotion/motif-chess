#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include <spdlog/logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "motif/db/database_manager.hpp"
#include "motif/http/server.hpp"

namespace
{

constexpr std::uint16_t max_valid_port {65535U};

struct cli_args
{
    std::filesystem::path db_path;
    std::uint16_t port {motif::http::server::default_port};
};

auto parse_port(std::string_view text) -> std::optional<std::uint16_t>
{
    std::uint32_t value {};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    auto const [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (ec != std::errc {} || ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    if (value == 0 || value > static_cast<std::uint32_t>(max_valid_port)) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

auto parse_args(std::span<char const* const> argv) -> std::optional<cli_args>
{
    cli_args args;

    for (std::size_t i = 1; i < argv.size(); ++i) {
        std::string_view const arg {argv[i]};
        if (arg == "--db" && i + 1 < argv.size()) {
            args.db_path = argv[++i];
        } else if (arg == "--port" && i + 1 < argv.size()) {
            auto const port_opt = parse_port(std::string_view {argv[++i]});
            if (!port_opt) {
                std::cerr << "error: invalid port value: " << argv[i] << '\n';
                return std::nullopt;
            }
            args.port = *port_opt;
        }
    }

    if (args.db_path.empty()) {
        // NOLINTNEXTLINE(concurrency-mt-unsafe) — called before any threads
        // start
        if (auto const* env_path = std::getenv("MOTIF_DB_PATH")) {
            args.db_path = env_path;
        }
    }

    if (args.port == motif::http::server::default_port) {
        // NOLINTNEXTLINE(concurrency-mt-unsafe) — called before any threads
        // start
        if (auto const* env_port = std::getenv("MOTIF_HTTP_PORT")) {
            if (auto const port_opt = parse_port(std::string_view {env_port})) {
                args.port = *port_opt;
            }
        }
    }

    return args;
}

auto setup_logging() -> std::shared_ptr<spdlog::logger>
{
    auto logger = spdlog::stdout_color_mt("motif.http");
    spdlog::set_default_logger(logger);
    return logger;
}

}  // namespace

auto main(int argc, char** argv) -> int
{
    auto const args_opt = parse_args(
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        std::span<char const* const> {argv, static_cast<std::size_t>(argc)});
    if (!args_opt) {
        return EXIT_FAILURE;
    }
    auto const& args = *args_opt;

    if (args.db_path.empty()) {
        std::cerr << "error: database path required; use --db <path> or set " "MOTIF_DB_PATH\n";
        return EXIT_FAILURE;
    }

    auto logger = setup_logging();
    logger->info("motif-chess HTTP server starting");

    auto db_result = motif::db::database_manager::open(args.db_path);
    if (!db_result) {
        db_result = motif::db::database_manager::create(args.db_path, args.db_path.filename().string());
    }
    if (!db_result) {
        logger->error("failed to open or create database at {}", args.db_path.string());
        return EXIT_FAILURE;
    }

    motif::http::server srv {*db_result};
    logger->info("listening on port {}", args.port);
    auto const start_result = srv.start(args.port);
    if (!start_result) {
        logger->error("server failed to start on port {}", args.port);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
