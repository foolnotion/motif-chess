#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "motif/db/error.hpp"
#include "motif/db/types.hpp"

struct sqlite3;

namespace motif::db
{

// Result of game_writer::deduplicate(): how many raw-ingested rows turned
// out to be exact duplicates and were removed.
struct dedup_summary
{
    std::size_t removed {};
};

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

    // Insert a game unconditionally, with no identity check -- always
    // succeeds (barring I/O failure) even for an exact duplicate of an
    // existing row, and always assigns identity_collision = 0. Only valid to
    // call while the game_identity_lookup index is absent (see
    // drop_identity_index()); every raw-inserted row must be reconciled by a
    // later deduplicate() call before the index is recreated, or duplicate
    // rows and wrong identity_collision values persist.
    auto insert_raw(game const& src_game) -> result<game_id>;

    // Drop game_identity_lookup so insert_raw() isn't paying index-maintenance
    // cost on every row during a bulk raw ingest. Must be paired with a
    // subsequent deduplicate() call, which recreates the index.
    auto drop_identity_index() -> result<void>;

    // Whether game_identity_lookup is present. A missing index means a raw
    // import was interrupted before deduplicate() completed.
    auto identity_index_exists() -> result<bool>;

    // Reconcile rows inserted via insert_raw(): delete exact duplicates
    // (matching identity fields, move_hash, and moves), reassign
    // identity_collision for the remaining identity-field collisions that are
    // genuinely distinct games, then recreate game_identity_lookup. Restores
    // the same end state insert()'s per-row check would have produced.
    auto deduplicate() -> result<dedup_summary>;

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
