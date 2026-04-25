#pragma once

#include <cstdint>
#include <memory>

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

    explicit server(motif::db::database_manager& database);
    ~server();

    server(server const&) = delete;
    auto operator=(server const&) -> server& = delete;
    server(server&&) = delete;
    auto operator=(server&&) -> server& = delete;

    // Starts listening on `port`. Blocks until stop() is called.
    [[nodiscard]] auto start(std::uint16_t port = default_port) -> result<void>;

    // Signals the server to stop; safe to call from any thread.
    auto stop() -> void;

    [[nodiscard]] auto is_running() const -> bool;

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

}  // namespace motif::http
