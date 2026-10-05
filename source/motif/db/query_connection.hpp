#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>

#include "motif/db/error.hpp"
#include "motif/db/game_store.hpp"

struct sqlite3;

namespace motif::db
{

class query_connection_blocker
{
  public:
    [[nodiscard]] auto wait_until_entered_for(std::chrono::milliseconds timeout) -> bool;
    void release() noexcept;
    [[nodiscard]] auto progress() noexcept -> int;
    [[nodiscard]] auto observed_sqlite_interrupt() const noexcept -> bool;

  private:
    friend class query_connection;

    void bind(sqlite3* connection, std::atomic<bool>* cancellation) noexcept;
    void notify_cancellation() noexcept;

    std::mutex mutex_;
    std::condition_variable entered_condition_;
    std::condition_variable release_condition_;
    bool entered_ {false};
    bool released_ {false};
    sqlite3* connection_ {nullptr};
    std::atomic<bool>* cancellation_ {nullptr};
    std::atomic<bool> observed_sqlite_interrupt_ {false};
};

class query_connection_open_gate
{
  public:
    [[nodiscard]] auto wait_until_entered_for(std::chrono::milliseconds timeout) -> bool;
    void release() noexcept;
    void notify_cancellation() noexcept;

  private:
    friend class query_connection;

    void wait(std::atomic<bool> const& cancellation);

    std::mutex mutex_;
    std::condition_variable entered_condition_;
    std::condition_variable release_condition_;
    bool entered_ {false};
    bool released_ {false};
};

void set_query_connection_open_gate_for_tests(std::shared_ptr<query_connection_open_gate> gate) noexcept;
void notify_query_connection_open_gate_cancellation_for_tests() noexcept;

// Installs the deterministic blocking seam for connections opened after this
// call. Test binaries run serially, so one process-wide hook is sufficient.
void set_query_connection_blocker_for_tests(std::shared_ptr<query_connection_blocker> blocker) noexcept;

// A worker-owned, read-only connection to a bundle's SQLite metadata. Its
// game_store is deliberately independent from database_manager::store(), so a
// worker can be interrupted without disturbing imports or manager operations.
class query_connection
{
  public:
    ~query_connection();

    query_connection(query_connection const&) = delete;
    auto operator=(query_connection const&) -> query_connection& = delete;
    query_connection(query_connection&& other) noexcept;
    auto operator=(query_connection&& other) noexcept -> query_connection&;
    [[nodiscard]] static auto open(std::filesystem::path const& bundle_dir, std::shared_ptr<std::atomic<bool>> cancellation)
        -> result<query_connection>;

    [[nodiscard]] static auto open(std::filesystem::path const& bundle_dir) -> result<query_connection>;
    [[nodiscard]] auto store() const noexcept -> game_store const&;
    void interrupt() noexcept;
    [[nodiscard]] auto cancelled() const noexcept -> bool;

  private:
    explicit query_connection(sqlite3* connection,
                              std::shared_ptr<query_connection_blocker> blocker,
                              std::shared_ptr<std::atomic<bool>> cancellation) noexcept;

    sqlite3* connection_ {nullptr};
    std::optional<game_store> store_;
    std::shared_ptr<query_connection_blocker> blocker_;
    std::shared_ptr<std::atomic<bool>> cancellation_;
};
}  // namespace motif::db
