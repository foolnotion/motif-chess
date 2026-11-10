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
        began_ = sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK;
        started_local_ = began_;
    }

    ~txn_guard() noexcept
    {
        if (started_local_ && !committed_) {
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        }
    }

    [[nodiscard]] auto began() const noexcept -> bool { return began_; }

    [[nodiscard]] auto started_local() const noexcept -> bool { return started_local_; }

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

// Atomic scope for one multi-statement canonical mutation. It owns a
// transaction when none is open (txn_guard) and, when it merely JOINS one the
// caller owns, an operation SAVEPOINT as well: a failed operation then rolls
// back to that savepoint so no partial indexed change can reach the caller's
// later COMMIT without the matching mutation-sequence advance. commit() is the
// release/commit point and must be called only after the last statement of
// the operation, the sequence update included, succeeded; any early return
// before it undoes the operation.
//
// A transaction owned by the game_writer's batch needs no savepoint: its
// failure poisons the batch (game_writer::note_failed_operation), which can
// then only roll back as a whole, and skipping the savepoint keeps the bulk
// import hot path free of a sub-journal per row. Pass in_writer_batch=true
// for that case.
class mutation_scope
{
  public:
    mutation_scope(sqlite3* conn, bool in_writer_batch) noexcept
        : db_ {conn}
        , txn_ {conn}
    {
        if (!txn_.began()) {
            return;
        }
        if (txn_.started_local() || in_writer_batch) {
            // Local transaction: its ROLLBACK is the undo. Writer batch: poison.
            ready_ = true;
            return;
        }
        ready_ = sqlite3_exec(db_, "SAVEPOINT motif_operation;", nullptr, nullptr, nullptr) == SQLITE_OK;
        savepoint_open_ = ready_;
    }

    ~mutation_scope() noexcept
    {
        if (savepoint_open_) {
            // ROLLBACK TO keeps the savepoint on the stack; RELEASE pops it.
            sqlite3_exec(db_, "ROLLBACK TO motif_operation;", nullptr, nullptr, nullptr);
            sqlite3_exec(db_, "RELEASE motif_operation;", nullptr, nullptr, nullptr);
        }
    }

    [[nodiscard]] auto began() const noexcept -> bool { return ready_; }

    [[nodiscard]] auto commit() noexcept -> bool
    {
        if (savepoint_open_) {
            if (sqlite3_exec(db_, "RELEASE motif_operation;", nullptr, nullptr, nullptr) != SQLITE_OK) {
                return false;  // the destructor still rolls the operation back
            }
            savepoint_open_ = false;
        }
        return txn_.commit();
    }

    mutation_scope(mutation_scope const&) = delete;
    auto operator=(mutation_scope const&) -> mutation_scope& = delete;
    mutation_scope(mutation_scope&&) = delete;
    auto operator=(mutation_scope&&) -> mutation_scope& = delete;

  private:
    sqlite3* db_;
    txn_guard txn_;
    bool ready_ {false};
    bool savepoint_open_ {false};
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
