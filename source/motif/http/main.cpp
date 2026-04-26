#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fmt/base.h>
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
    std::string host {motif::http::server::default_host};
    std::uint16_t port {motif::http::server::default_port};
    std::vector<std::string> allowed_origins;
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
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
                fmt::print(stderr, "error: invalid port value: {}\n", argv[i]);
                return std::nullopt;
            }
            args.port = *port_opt;
        } else if (arg == "--host" && i + 1 < argv.size()) {
            args.host = argv[++i];
        } else if (arg == "--cors-origin" && i + 1 < argv.size()) {
            args.allowed_origins.emplace_back(argv[++i]);
        }
    }

    if (args.db_path.empty()) {
        // Called before any threads start.
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        if (auto const* env_path = std::getenv("MOTIF_DB_PATH")) {
            args.db_path = env_path;
        }
    }

    if (args.port == motif::http::server::default_port) {
        // Called before any threads start.
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        if (auto const* env_port = std::getenv("MOTIF_HTTP_PORT")) {
            if (auto const port_opt = parse_port(std::string_view {env_port})) {
                args.port = *port_opt;
            }
        }
    }

    if (std::string(motif::http::server::default_host) == args.host) {
        // Called before any threads start.
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        if (auto const* env_host = std::getenv("MOTIF_HTTP_HOST")) {
            args.host = env_host;
        }
    }

    if (args.allowed_origins.empty()) {
        // Called before any threads start.
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        if (auto const* env_origins = std::getenv("MOTIF_HTTP_CORS_ORIGINS")) {
            // Comma-separated list of allowed origins.
            std::string_view origins_sv {env_origins};
            while (!origins_sv.empty()) {
                auto const comma = origins_sv.find(',');
                auto const origin = origins_sv.substr(0, comma);
                if (!origin.empty()) {
                    args.allowed_origins.emplace_back(origin);
                }
                if (comma == std::string_view::npos) {
                    break;
                }
                origins_sv = origins_sv.substr(comma + 1);
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
        fmt::print(stderr, "error: database path required; use --db <path> or set MOTIF_DB_PATH\n");
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
    logger->info("listening on {}:{}", args.host, args.port);
    if (!args.allowed_origins.empty()) {
        logger->info("CORS allowed origins: {} configured", args.allowed_origins.size());
    }
    auto const start_result = srv.start(args.host, args.port, args.allowed_origins);
    if (!start_result) {
        logger->error("server failed to start on {}:{}", args.host, args.port);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
