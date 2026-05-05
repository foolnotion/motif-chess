#pragma once

#include <memory>
#include <string>

#include "motif/db/error.hpp"
#include "motif/db/types.hpp"

struct sqlite3;

namespace motif::db
{

// Owns the bulk-insert path for the SQLite game store: entity deduplication
// via cached prepared statements and transaction control for batched imports.
// The companion game_store class handles schema creation and read/edit operations.
class game_writer
{
  public:
    explicit game_writer(sqlite3* conn) noexcept;
    ~game_writer();

    game_writer(game_writer const&) = delete;
    auto operator=(game_writer const&) -> game_writer& = delete;
    game_writer(game_writer&& other) noexcept;
    auto operator=(game_writer&& other) noexcept -> game_writer&;

    // Insert a game and return its new row id.
    // Returns error_code::duplicate when the identity key already exists.
    // Callers decide whether to treat a duplicate as fatal.
    auto insert(game const& src_game) -> result<game_id>;

    // Transaction control for batched import paths. insert() joins an
    // already-open transaction instead of opening its own.
    auto begin_transaction() -> result<void>;
    auto commit_transaction() -> result<void>;
    auto rollback_transaction() noexcept -> void;

    // Release player, event, and tag id caches. Call after a bulk import to
    // reclaim memory; subsequent inserts will re-populate the caches on demand.
    void clear_insert_caches() noexcept;

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

}  // namespace motif::db
