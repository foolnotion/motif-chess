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
//
// Ownership: there is exactly ONE game_writer per SQLite connection, and
// game_store owns it (game_store::writer(); database_manager::writer() returns
// that same object). The constructor is private to enforce this, because the
// writer holds the connection's game-ID allocator state -- the cached
// next_game_id counter, whether a batch is open, and the player/event/tag id
// caches. Two writers on one connection would each hand out the same id and
// each be unaware of the other's open batch. A connection must likewise have
// at most one game_store. Everything that inserts games on a connection
// (game_store::insert, database_manager::insert_game, the import pipeline)
// goes through this one object, so an insert made mid-batch uses the batch's
// allocator and never persists the counter on its own.
class game_writer
{
  public:
    ~game_writer();

    game_writer(game_writer const&) = delete;
    auto operator=(game_writer const&) -> game_writer& = delete;
    game_writer(game_writer&& other) noexcept;
    auto operator=(game_writer&& other) noexcept -> game_writer&;

    // Insert a game and return its new row id. The id comes from the durable
    // allocator (bundle_meta.next_game_id), never from SQLite's rowid reuse, so
    // an id freed by deleting the highest game is not handed out again. Fails
    // with io_failure ("game id space exhausted") rather than wrapping once the
    // next id would exceed uint32 max.
    // Returns error_code::duplicate when the identity key already exists; a
    // duplicate consumes no id.
    // Callers decide whether to treat a duplicate as fatal.
    //
    // A standalone insert (no open batch) keeps the counter cached across
    // calls -- there is no per-insert SELECT -- and issues exactly one
    // `UPDATE bundle_meta ... WHERE next_game_id < ?` inside its own
    // transaction, before COMMIT. The cache is dropped (and reloaded from the
    // table on next use) after a failed insert, after rollback_transaction(),
    // at begin_transaction(), and when that UPDATE finds the table already at
    // or past the cached value (an external advance). NOT detected: SQL run on
    // the raw connection that edits bundle_meta or inserts/deletes game rows
    // with explicit ids while the cache is live, a rollback of a transaction
    // opened outside this writer (the cache then merely runs ahead of the
    // table, leaving a gap), and any writer in another process or connection
    // (the bundle lock forbids that).
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
    // the same end state insert()'s per-row check would have produced. Advances
    // the mutation sequence once if it deleted a row or changed a collision slot
    // (see the sequence note below); recreating the index alone does not.
    auto deduplicate() -> result<dedup_summary>;

    // Transaction control for batched import paths. insert() joins an
    // already-open transaction instead of opening its own. Inside
    // begin_transaction()/commit_transaction() the allocator's next id lives in
    // memory (no per-row UPDATE) and commit_transaction() persists its
    // high-water mark just before COMMIT, in the same SQLite transaction as the
    // rows it covers. The counter is reloaded from the table at begin and after
    // a failed commit.
    //
    // rollback_transaction() discards the in-memory counter and reloads it from
    // the table, so ids handed out by the aborted batch ARE reissued. That is
    // deliberate: the durable contract is only that a *committed* id is never
    // reused (the table counter always exceeds every committed id and never
    // decreases); nothing durable can reference an id from an aborted batch.
    // Callers must not retain ids returned inside a batch until it commits.
    //
    // Mutation sequence: mutation_state.mutation_seq counts index-relevant
    // canonical TRANSACTIONS, never games or statements. An inserted game
    // (checked or raw), a removal, a dedup that deletes rows or reassigns
    // collision slots, and a patch that really changes the result or a rating
    // each advance it exactly once per transaction: a batch of 100,000 accepted
    // inserts advances it once, at commit, inside the same SQLite transaction as
    // the rows. Rollback, a duplicate-only or empty batch, a missing-id removal,
    // a rejected patch and a patch that restates the stored values advance it
    // zero times. Metadata the indexes do not embed (names, event, date, eco,
    // notes, provenance) never advance it. The counter lives in the table, never
    // in a cache, so a rolled-back transaction cannot leave it falsely advanced.
    //
    // A non-duplicate failure inside a batch poisons it: commit_transaction()
    // then rolls back and fails, because partially applied rows would reach
    // the table without the success-path sequence mark. A failed COMMIT rolls
    // back a still-open transaction before clearing pending state.
    //
    // Atomicity of one operation: insert, insert_raw, deduplicate and the
    // game_store mutations (remove, patch_metadata) are multi-statement. Each
    // runs in its own transaction when none is open; when it joins a
    // transaction the CALLER owns (plain BEGIN outside this writer) it runs
    // behind an operation SAVEPOINT that any failure rolls back to and that is
    // released only after the sequence update (and allocator write) succeeded.
    // A failed operation therefore leaves no partial change for the caller's
    // later COMMIT, and the caller's transaction stays usable. Inside a
    // writer-owned batch the failure poisons the batch instead (no savepoint
    // per row on the bulk path); deduplicate() additionally always savepoints.
    auto begin_transaction() -> result<void>;
    auto commit_transaction() -> result<void>;
    auto rollback_transaction() noexcept -> void;

    // Release player, event, and tag id caches. Call after a bulk import to
    // reclaim memory; subsequent inserts will re-populate the caches on demand.
    void clear_insert_caches() noexcept;

    // True from the first index-relevant canonical change made inside an open
    // writer-owned batch until that batch commits or rolls back. The sequence
    // advance for such a batch is deferred to commit_transaction(), so until
    // then a reader on this connection can see new rows under the old
    // sequence; position queries must treat pending as "not covered".
    [[nodiscard]] auto has_pending_canonical_change() const noexcept -> bool;

    // In-process change counter over ALL canonical data a reader may cache:
    // advanced by every successful insert (checked or raw), removal,
    // deduplication, metadata patch (indexed or not) and provenance change,
    // and by batch commit and rollback. Unlike the mutation sequence it is not
    // persisted and is not the index-coverage key: it exists so an in-process
    // consumer (the desktop search cache) can tell that something it copied out
    // of the store -- names, event, date, ECO, notes, provenance included --
    // may have changed. It only ever grows, it is conservative (a rolled-back
    // change may still advance it), and it makes no promise across processes
    // (the bundle lock forbids a second writer).
    [[nodiscard]] auto data_revision() const noexcept -> std::uint64_t;

  private:
    friend class game_store;

    // True while begin_transaction() ... commit/rollback is open on this
    // writer, i.e. the transaction is writer-owned rather than the caller's.
    [[nodiscard]] auto owns_open_batch() const noexcept -> bool;

    // Called by game_store for the canonical changes it owns (remove, patch,
    // provenance) so data_revision() covers them.
    void note_data_changed() noexcept;

    // Called by game_store for the mutations it owns (remove, patch). See the
    // sequence note above: record_canonical_change() marks (batch) or advances
    // (standalone or foreign transaction) the sequence; note_failed_operation()
    // poisons an open batch.
    auto record_canonical_change() -> result<void>;
    void note_failed_operation() noexcept;

    // Only game_store constructs a writer (see the ownership note above); the
    // constructor is private, so std::make_unique cannot be used.
    [[nodiscard]] static auto make(sqlite3* conn) -> std::unique_ptr<game_writer>;
    explicit game_writer(sqlite3* conn) noexcept;

    struct impl;
    std::unique_ptr<impl> impl_;
};

}  // namespace motif::db
