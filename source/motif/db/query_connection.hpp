#pragma once

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
    void wait_until_entered();
    void release() noexcept;
    [[nodiscard]] auto progress() noexcept -> int;

  private:
    friend class query_connection;

    void interrupt() noexcept;

    std::mutex mutex_;
    std::condition_variable entered_condition_;
    std::condition_variable release_condition_;
    bool entered_ {false};
    bool released_ {false};
};

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

    [[nodiscard]] static auto open(std::filesystem::path const& bundle_dir) -> result<query_connection>;
    [[nodiscard]] auto store() const noexcept -> game_store const&;
    void interrupt() noexcept;

  private:
    explicit query_connection(sqlite3* connection, std::shared_ptr<query_connection_blocker> blocker) noexcept;

    sqlite3* connection_ {nullptr};
    std::optional<game_store> store_;
    std::shared_ptr<query_connection_blocker> blocker_;
};

}  // namespace motif::db
