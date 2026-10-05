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
    std::shared_ptr<query_connection_open_gate> open_gate;
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

[[nodiscard]] auto current_open_gate() -> std::shared_ptr<query_connection_open_gate>
{
    auto& state = test_state();
    std::scoped_lock const lock {state.mutex};
    return state.open_gate;
}

auto progress_callback(void* const context) noexcept -> int
{
    return static_cast<query_connection_blocker*>(context)->progress();
}

}  // namespace

auto query_connection_blocker::wait_until_entered_for(std::chrono::milliseconds const timeout) -> bool
{
    std::unique_lock lock {mutex_};
    return entered_condition_.wait_for(lock, timeout, [this]() -> bool { return entered_; });
}

void query_connection_blocker::release() noexcept
{
    std::scoped_lock const lock {mutex_};
    released_ = true;
    release_condition_.notify_all();
}

void query_connection_blocker::bind(sqlite3* const connection, std::atomic<bool>* const cancellation) noexcept
{
    std::scoped_lock const lock {mutex_};
    connection_ = connection;
    cancellation_ = cancellation;
}

void query_connection_blocker::notify_cancellation() noexcept
{
    std::scoped_lock const lock {mutex_};
    release_condition_.notify_all();
}

auto query_connection_blocker::progress() noexcept -> int
{
    std::unique_lock lock {mutex_};
    entered_ = true;
    entered_condition_.notify_all();
    release_condition_.wait(
        lock,
        [this]() -> bool
        {
            return released_
                || (cancellation_ != nullptr && cancellation_->load(std::memory_order_acquire) && sqlite3_is_interrupted(connection_) != 0);
        });
    auto const interrupted =
        cancellation_ != nullptr && cancellation_->load(std::memory_order_acquire) && sqlite3_is_interrupted(connection_) != 0;
    observed_sqlite_interrupt_.store(interrupted, std::memory_order_release);
    return interrupted ? 1 : 0;
}

auto query_connection_blocker::observed_sqlite_interrupt() const noexcept -> bool
{
    return observed_sqlite_interrupt_.load(std::memory_order_acquire);
}

void set_query_connection_blocker_for_tests(std::shared_ptr<query_connection_blocker> blocker) noexcept
{
    auto& state = test_state();
    std::scoped_lock const lock {state.mutex};
    state.blocker = std::move(blocker);
}

auto query_connection_open_gate::wait_until_entered_for(std::chrono::milliseconds const timeout) -> bool
{
    std::unique_lock lock {mutex_};
    return entered_condition_.wait_for(lock, timeout, [this]() -> bool { return entered_; });
}

void query_connection_open_gate::release() noexcept
{
    std::scoped_lock const lock {mutex_};
    released_ = true;
    release_condition_.notify_all();
}

void query_connection_open_gate::notify_cancellation() noexcept
{
    std::scoped_lock const lock {mutex_};
    release_condition_.notify_all();
}

void query_connection_open_gate::wait(std::atomic<bool> const& cancellation)
{
    std::unique_lock lock {mutex_};
    entered_ = true;
    entered_condition_.notify_all();
    release_condition_.wait(lock, [this, &cancellation]() -> bool { return released_ || cancellation.load(std::memory_order_acquire); });
}

void set_query_connection_open_gate_for_tests(std::shared_ptr<query_connection_open_gate> gate) noexcept
{
    auto& state = test_state();
    std::scoped_lock const lock {state.mutex};
    state.open_gate = std::move(gate);
}

void notify_query_connection_open_gate_cancellation_for_tests() noexcept
{
    if (auto gate = current_open_gate()) {
        gate->notify_cancellation();
    }
}

query_connection::query_connection(sqlite3* const connection,
                                   std::shared_ptr<query_connection_blocker> blocker,
                                   std::shared_ptr<std::atomic<bool>> cancellation) noexcept
    : connection_ {connection}
    , store_ {connection}
    , blocker_ {std::move(blocker)}
    , cancellation_ {std::move(cancellation)}
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
    , cancellation_ {std::move(other.cancellation_)}
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
        cancellation_ = std::move(other.cancellation_);
    }
    return *this;
}

auto query_connection::open(std::filesystem::path const& bundle_dir, std::shared_ptr<std::atomic<bool>> cancellation)
    -> result<query_connection>
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
        blocker->bind(connection, cancellation.get());
        sqlite3_progress_handler(connection, 1, progress_callback, blocker.get());
    }
    if (auto gate = current_open_gate()) {
        gate->wait(*cancellation);
    }
    return query_connection {connection, std::move(blocker), std::move(cancellation)};
}

auto query_connection::open(std::filesystem::path const& bundle_dir) -> result<query_connection>
{
    return open(bundle_dir, std::make_shared<std::atomic<bool>>(false));
}

auto query_connection::store() const noexcept -> game_store const&
{
    // Open construction always emplaces the store before exposing the connection.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return *store_;
}

void query_connection::interrupt() noexcept
{
    cancellation_->store(true, std::memory_order_release);
    if (connection_ != nullptr) {
        sqlite3_interrupt(connection_);
    }
    if (blocker_) {
        blocker_->notify_cancellation();
    }
}

auto query_connection::cancelled() const noexcept -> bool
{
    return cancellation_->load(std::memory_order_acquire);
}
}  // namespace motif::db
