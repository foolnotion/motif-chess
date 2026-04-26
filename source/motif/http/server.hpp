#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "motif/http/error.hpp"

namespace motif::db
{
class database_manager;
}  // namespace motif::db

namespace motif::http
{

// HTTP server wrapping cpp-httplib. httplib.h is confined to server.cpp via
// pImpl so the platform network headers don't propagate through this interface.
class server
{
  public:
    static constexpr std::uint16_t default_port {8080};
    static constexpr std::string_view default_host {"localhost"};

    explicit server(motif::db::database_manager& database);
    ~server();

    server(server const&) = delete;
    auto operator=(server const&) -> server& = delete;
    server(server&&) = delete;
    auto operator=(server&&) -> server& = delete;

    // Starts listening on `host`:`port`. Blocks until stop() is called.
    // `allowed_origins` controls the CORS Access-Control-Allow-Origin header.
    // An empty vector allows all origins ("*"); a non-empty vector echoes only
    // the request's Origin header if it matches, otherwise omits the header.
    [[nodiscard]] auto start(std::string const& host = std::string {default_host},
                             std::uint16_t port = default_port,
                             std::vector<std::string> const& allowed_origins = {}) -> result<void>;

    // Convenience overload that binds to default_host with configurable port.
    [[nodiscard]] auto start(std::uint16_t port) -> result<void>;

    // Signals the server to stop; safe to call from any thread.
    auto stop() -> void;

    [[nodiscard]] auto is_running() const -> bool;

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

}  // namespace motif::http
