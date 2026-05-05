#pragma once

// Internal SQLite utility types shared between game_store.cpp and
// game_writer.cpp. Include only from .cpp files — not part of the public API.

#include <cstdint>
#include <optional>
#include <string>

#include <sqlite3.h>

namespace motif::db::detail
{

// RAII wrapper for SQLite BEGIN / COMMIT / ROLLBACK.
// Joins an already-open transaction when SQLite is not in autocommit mode so
// that insert() participates in a caller-owned batch transaction without
// issuing a nested BEGIN.
class txn_guard
{
  public:
    explicit txn_guard(sqlite3* conn) noexcept
        : db_ {conn}
    {
        if (sqlite3_get_autocommit(db_) == 0) {
            began_ = true;
            started_local_ = false;
            return;
        }
        began_ = sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, nullptr) == SQLITE_OK;
        started_local_ = began_;
    }

    ~txn_guard() noexcept
    {
        if (started_local_ && !committed_) {
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        }
    }

    [[nodiscard]] auto began() const noexcept -> bool { return began_; }

    [[nodiscard]] auto commit() noexcept -> bool
    {
        if (!started_local_) {
            committed_ = true;
            return true;
        }
        int const commit_rc = sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
        if (commit_rc == SQLITE_OK) {
            committed_ = true;
        }
        return commit_rc == SQLITE_OK;
    }

    txn_guard(txn_guard const&) = delete;
    auto operator=(txn_guard const&) -> txn_guard& = delete;
    txn_guard(txn_guard&&) = delete;
    auto operator=(txn_guard&&) -> txn_guard& = delete;

  private:
    sqlite3* db_;
    bool began_ {false};
    bool started_local_ {false};
    bool committed_ {false};
};

inline auto bind_optional_text(sqlite3_stmt* stmt, int col, std::optional<std::string> const& val) -> void
{
    if (val) {
        sqlite3_bind_text(stmt, col, val->c_str(), static_cast<int>(val->size()), SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, col);
    }
}

inline auto bind_optional_int(sqlite3_stmt* stmt, int col, std::optional<std::int32_t> const& val) -> void
{
    if (val) {
        sqlite3_bind_int(stmt, col, *val);
    } else {
        sqlite3_bind_null(stmt, col);
    }
}

}  // namespace motif::db::detail
