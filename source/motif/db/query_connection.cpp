#include <utility>

#include "motif/db/query_connection.hpp"

#include <sqlite3.h>

namespace motif::db
{
namespace
{

struct blocker_test_state
{
    std::mutex mutex;
    std::shared_ptr<query_connection_blocker> blocker;
};

auto test_state() -> blocker_test_state&
{
    static auto state = blocker_test_state {};
    return state;
}

[[nodiscard]] auto current_test_blocker() -> std::shared_ptr<query_connection_blocker>
{
    auto& state = test_state();
    std::scoped_lock const lock {state.mutex};
    return state.blocker;
}

auto progress_callback(void* const context) noexcept -> int
{
    return static_cast<query_connection_blocker*>(context)->progress();
}

}  // namespace

void query_connection_blocker::wait_until_entered()
{
    std::unique_lock lock {mutex_};
    entered_condition_.wait(lock, [this]() -> bool { return entered_; });
}

void query_connection_blocker::release() noexcept
{
    std::scoped_lock const lock {mutex_};
    released_ = true;
    release_condition_.notify_all();
}

void query_connection_blocker::interrupt() noexcept
{
    release();
}

auto query_connection_blocker::progress() noexcept -> int
{
    std::unique_lock lock {mutex_};
    entered_ = true;
    entered_condition_.notify_all();
    release_condition_.wait(lock, [this]() -> bool { return released_; });
    return 0;
}

void set_query_connection_blocker_for_tests(std::shared_ptr<query_connection_blocker> blocker) noexcept
{
    auto& state = test_state();
    std::scoped_lock const lock {state.mutex};
    state.blocker = std::move(blocker);
}

query_connection::query_connection(sqlite3* const connection, std::shared_ptr<query_connection_blocker> blocker) noexcept
    : connection_ {connection}
    , store_ {connection}
    , blocker_ {std::move(blocker)}
{
}

query_connection::~query_connection()
{
    store_.reset();
    if (connection_ != nullptr) {
        sqlite3_close(connection_);
    }
}

query_connection::query_connection(query_connection&& other) noexcept
    : connection_ {std::exchange(other.connection_, nullptr)}
    , store_ {std::move(other.store_)}
    , blocker_ {std::move(other.blocker_)}
{
}

auto query_connection::operator=(query_connection&& other) noexcept -> query_connection&
{
    if (this != &other) {
        store_.reset();
        if (connection_ != nullptr) {
            sqlite3_close(connection_);
        }
        connection_ = std::exchange(other.connection_, nullptr);
        store_ = std::move(other.store_);
        blocker_ = std::move(other.blocker_);
    }
    return *this;
}

auto query_connection::open(std::filesystem::path const& bundle_dir) -> result<query_connection>
{
    sqlite3* connection = nullptr;
    auto const database_path = bundle_dir / "games.db";
    auto const flags = SQLITE_OPEN_READONLY;
    if (sqlite3_open_v2(database_path.c_str(), &connection, flags, nullptr) != SQLITE_OK) {
        if (connection != nullptr) {
            sqlite3_close(connection);
        }
        return tl::unexpected {error_code::io_failure};
    }

    auto blocker = current_test_blocker();
    if (blocker) {
        sqlite3_progress_handler(connection, 1, progress_callback, blocker.get());
    }
    return query_connection {connection, std::move(blocker)};
}

auto query_connection::store() const noexcept -> game_store const&
{
    // Open construction always emplaces the store before exposing the connection.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return *store_;
}

void query_connection::interrupt() noexcept
{
    if (blocker_) {
        blocker_->interrupt();
    }
    if (connection_ != nullptr) {
        sqlite3_interrupt(connection_);
    }
}

}  // namespace motif::db
