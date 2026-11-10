#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "motif/db/game_writer.hpp"

#include <gtl/phmap.hpp>
#include <sqlite3.h>
#include <tl/expected.hpp>
#include <xxhash.h>

#include "motif/db/error.hpp"
#include "motif/db/fault_injection.hpp"
#include "motif/db/schema.hpp"
#include "motif/db/sqlite_impl.hpp"
#include "motif/db/sqlite_util.hpp"
#include "motif/db/types.hpp"

namespace motif::db
{

struct game_writer::impl
{
    explicit impl(sqlite3* conn) noexcept
        : db_ {conn}
    {
    }

    auto find_or_insert_player(player const& plr) -> result<std::int64_t>;
    auto find_or_insert_event(event const& evt) -> result<std::int64_t>;
    auto insert_game_tags(game_id gid, std::vector<std::pair<std::string, std::string>> const& extra_tags) -> result<void>;
    // NOLINTNEXTLINE(readability-make-member-function-const) — modifies DB state through raw ptr
    auto prepare_cached_stmt(detail::unique_stmt& stmt, char const* sql) -> result<sqlite3_stmt*>;
    auto insert(game const& src_game) -> result<game_id>;
    auto insert_raw(game const& src_game) -> result<game_id>;
    // NOLINTNEXTLINE(readability-make-member-function-const) — modifies DB state through raw ptr
    auto drop_identity_index() -> result<void>;
    auto identity_index_exists() -> result<bool>;
    // NOLINTNEXTLINE(readability-make-member-function-const) — modifies DB state through raw ptr
    auto deduplicate() -> result<dedup_summary>;
    // NOLINTNEXTLINE(readability-make-member-function-const) — modifies DB state through raw ptr
    auto begin_transaction() -> result<void>;
    // NOLINTNEXTLINE(readability-make-member-function-const) — modifies DB state through raw ptr
    auto commit_transaction() -> result<void>;
    // NOLINTNEXTLINE(readability-make-member-function-const) — modifies DB state through raw ptr
    auto rollback_transaction() noexcept -> void;
    void clear_insert_caches() noexcept;

    // Mutation-sequence accounting. record_canonical_change() marks an
    // index-relevant canonical change made by the calling operation: inside a
    // writer-owned batch it only sets the pending flag (commit_transaction()
    // advances the sequence once); otherwise it advances the sequence now, in
    // the caller's open transaction. note_failed_operation() poisons a batch.
    // has_pending_canonical_change() is true from the first marked change in a
    // batch until commit or rollback.
    auto record_canonical_change() -> result<void>;
    void note_failed_operation() noexcept;

    [[nodiscard]] auto has_pending_canonical_change() const noexcept -> bool { return canonical_changed_; }

    [[nodiscard]] auto batch_open() const noexcept -> bool { return batch_open_; }

    [[nodiscard]] auto data_revision() const noexcept -> std::uint64_t { return data_revision_.load(std::memory_order_relaxed); }

    void note_data_changed() noexcept { data_revision_.fetch_add(1, std::memory_order_relaxed); }

    auto bump_mutation_seq() -> result<void>;

    // Durable game-ID allocator (bundle_meta.next_game_id). reserve_game_id()
    // returns the next unused ID without consuming it; confirm_game_id()
    // consumes it once the row is actually inserted, so a deduplicated-away
    // insert burns nothing. The counter is cached in memory and written back
    // by persist_game_id_counter() inside the transaction that commits the rows
    // it covers: once per insert outside a batch, once at commit_transaction()
    // inside one. This is the connection's only allocator (game_store owns the
    // single game_writer), so the cache cannot diverge from a sibling's. It is
    // dropped on rollback, on a failed standalone insert, at begin_transaction()
    // and when the table is found to have advanced externally.
    auto reserve_game_id() -> result<std::uint32_t>;
    void confirm_game_id(std::uint32_t assigned_id) noexcept;
    auto persist_game_id_counter() -> result<void>;
    void discard_game_id_counter() noexcept;

    // Drops the cached counter if an insert outside a batch fails after it may
    // have advanced the counter in memory: the insert's transaction rolled
    // back, so the cache would otherwise run ahead of the table. Inside a
    // batch the advance is kept; rollback_transaction() resets it.
    struct allocation_scope
    {
        impl& owner;
        bool succeeded {false};

        explicit allocation_scope(impl& owning_impl) noexcept
            : owner {owning_impl}
        {
        }

        allocation_scope(allocation_scope const&) = delete;
        auto operator=(allocation_scope const&) -> allocation_scope& = delete;
        allocation_scope(allocation_scope&&) = delete;
        auto operator=(allocation_scope&&) -> allocation_scope& = delete;

        ~allocation_scope() noexcept
        {
            if (succeeded) {
                return;
            }
            if (owner.batch_open_) {
                owner.note_failed_operation();
            } else {
                owner.discard_game_id_counter();
            }
        }
    };

  private:
    sqlite3* db_;
    gtl::flat_hash_map<std::string, std::int64_t> player_id_cache_;
    gtl::flat_hash_map<std::string, std::int64_t> event_id_cache_;
    gtl::flat_hash_map<std::string, std::int64_t> tag_id_cache_;

    detail::unique_stmt select_player_stmt_;
    detail::unique_stmt insert_player_stmt_;
    detail::unique_stmt select_event_stmt_;
    detail::unique_stmt insert_event_stmt_;
    detail::unique_stmt insert_game_stmt_;
    detail::unique_stmt insert_game_raw_stmt_;
    detail::unique_stmt select_tag_stmt_;
    detail::unique_stmt insert_tag_stmt_;
    detail::unique_stmt insert_game_tag_stmt_;
    detail::unique_stmt select_next_game_id_stmt_;
    detail::unique_stmt update_next_game_id_stmt_;
    detail::unique_stmt bump_mutation_seq_stmt_;

    // Next game ID to hand out; empty until loaded (or after being discarded).
    // dirty means it is ahead of the table and persist_game_id_counter() owes
    // an UPDATE.
    std::optional<std::int64_t> next_game_id_;
    bool next_game_id_dirty_ {false};
    bool batch_open_ {false};
    // Writer-owned batch state: an index-relevant change happened (the single
    // sequence advance is owed at commit) / a failed operation means the batch
    // may not commit.
    bool canonical_changed_ {false};
    bool batch_poisoned_ {false};
    // Bumped by every successful canonical change of any kind (indexed or not)
    // and by batch commit/rollback; see game_writer::data_revision().
    std::atomic<std::uint64_t> data_revision_ {0};
};

// ── Implementation details
// ─────────────────────────────────────────────────────
//
// llvm-prefer-static-over-anonymous-namespace and misc-use-anonymous-namespace
// are mutually contradictory: one demands 'static', the other demands anonymous
// namespace.  Anonymous namespace is preferred by the C++ standard; the LLVM
// check is suppressed inline.

namespace
{

using detail::bind_optional_int;
using detail::bind_optional_text;
using detail::mutation_scope;
using detail::unique_stmt;

auto reset_stmt(sqlite3_stmt* stmt) noexcept -> void
{  // NOLINT(llvm-prefer-static-over-anonymous-namespace)
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
}

// Bind parameter positions for the game insert statement. Each is a numbered
// SQLite parameter (?N) reused across the VALUES row and the two identity
// subqueries below, so every value is bound exactly once per insert.
namespace game_ins_param
{
constexpr int white_id = 1;
constexpr int black_id = 2;
constexpr int event_id = 3;
constexpr int date = 4;
constexpr int result = 5;
constexpr int eco = 6;
constexpr int moves = 7;
constexpr int source_type = 8;
constexpr int source_label = 9;
constexpr int review_status = 10;
constexpr int move_hash = 11;
constexpr int white_elo = 12;
constexpr int black_elo = 13;
constexpr int game_row_id = 14;
}  // namespace game_ins_param

// Single atomic statement replacing the former SELECT-then-INSERT pair: the
// WHERE NOT EXISTS clause rejects an exact duplicate (same identity fields
// and move blob) by inserting zero rows, and the identity_collision
// subquery assigns the next collision slot for identity-field matches that
// are NOT exact duplicates (distinct games sharing every indexed field).
// sqlite3_changes() after execution distinguishes "rejected" (0) from
// "inserted" (1) without a separate round trip.
// language=sql
constexpr auto insert_game_sql = R"sql(
    INSERT INTO game(white_id, black_id, event_id, date, result, eco, moves,
                     source_type, source_label, review_status, move_hash, identity_collision,
                     white_elo, black_elo, id)
    SELECT ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11,
           COALESCE((SELECT MAX(identity_collision) + 1 FROM game
                     WHERE white_id = ?1 AND black_id = ?2
                       AND COALESCE(event_id, -1) = COALESCE(?3, -1)
                       AND COALESCE(date, '') = COALESCE(?4, '')
                       AND result = ?5 AND move_hash = ?11), 0),
           ?12, ?13, ?14
    WHERE NOT EXISTS (
        SELECT 1 FROM game
        WHERE white_id = ?1 AND black_id = ?2
          AND COALESCE(event_id, -1) = COALESCE(?3, -1)
          AND COALESCE(date, '') = COALESCE(?4, '')
          AND result = ?5 AND move_hash = ?11 AND moves = ?7
    )
)sql";

// Used only while game_identity_lookup is absent (see game_writer::drop_identity_index()).
// No WHERE NOT EXISTS, no identity_collision subquery: every call inserts a row, even an
// exact duplicate of an existing one. deduplicate() reconciles that afterward.
// language=sql
constexpr auto insert_game_raw_sql = R"sql(
    INSERT INTO game(white_id, black_id, event_id, date, result, eco, moves,
                     source_type, source_label, review_status, move_hash, identity_collision,
                     white_elo, black_elo, id)
    VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, 0, ?12, ?13, ?14)
)sql";

// Deletes exact duplicates among raw-inserted rows: same identity fields, move_hash, and
// moves blob. Keeps the lowest id in each group. game_tag rows for removed games cascade
// via the ON DELETE CASCADE FK (schema.cpp), since foreign_keys is enforced ON.
// language=sql
constexpr auto dedup_delete_sql = R"sql(
    DELETE FROM game WHERE id NOT IN (
        SELECT MIN(id) FROM game
        GROUP BY white_id, black_id, COALESCE(event_id, -1), COALESCE(date, ''), result, move_hash, moves
    )
)sql";

// Reassigns identity_collision for what remains: distinct games that share every identity
// field including move_hash (not exact duplicates -- dedup_delete_sql already removed
// those) get 0, 1, 2, ... in insertion order, matching what insert()'s per-row
// MAX(identity_collision)+1 subquery would have produced.
// language=sql
constexpr auto dedup_reassign_collision_sql = R"sql(
    UPDATE game SET identity_collision = ranked.new_collision
    FROM (
        SELECT id, ROW_NUMBER() OVER (
            PARTITION BY white_id, black_id, COALESCE(event_id, -1), COALESCE(date, ''), result, move_hash
            ORDER BY id
        ) - 1 AS new_collision
        FROM game
    ) AS ranked
    WHERE ranked.id = game.id AND game.identity_collision <> ranked.new_collision
)sql";

}  // namespace

// ── game_writer
// ───────────────────────────────────────────────────────────────

game_writer::game_writer(sqlite3* conn) noexcept
    : impl_ {std::make_unique<impl>(conn)}
{
}

auto game_writer::make(sqlite3* const conn) -> std::unique_ptr<game_writer>
{
    // The constructor is private to everything but game_store, so make_unique cannot be used.
    return std::unique_ptr<game_writer> {new game_writer(conn)};  // NOLINT(cppcoreguidelines-owning-memory)
}

game_writer::~game_writer() = default;

game_writer::game_writer(game_writer&& other) noexcept = default;

auto game_writer::operator=(game_writer&& other) noexcept -> game_writer& = default;

void game_writer::clear_insert_caches() noexcept
{
    impl_->clear_insert_caches();
}

auto game_writer::begin_transaction() -> result<void>
{
    return impl_->begin_transaction();
}

auto game_writer::commit_transaction() -> result<void>
{
    return impl_->commit_transaction();
}

auto game_writer::rollback_transaction() noexcept -> void
{
    impl_->rollback_transaction();
}

auto game_writer::insert(game const& src_game) -> result<game_id>
{
    return impl_->insert(src_game);
}

auto game_writer::insert_raw(game const& src_game) -> result<game_id>
{
    return impl_->insert_raw(src_game);
}

auto game_writer::drop_identity_index() -> result<void>
{
    return impl_->drop_identity_index();
}

auto game_writer::identity_index_exists() -> result<bool>
{
    return impl_->identity_index_exists();
}

auto game_writer::deduplicate() -> result<dedup_summary>
{
    return impl_->deduplicate();
}

auto game_writer::has_pending_canonical_change() const noexcept -> bool
{
    return impl_->has_pending_canonical_change();
}

auto game_writer::owns_open_batch() const noexcept -> bool
{
    return impl_->batch_open();
}

auto game_writer::data_revision() const noexcept -> std::uint64_t
{
    return impl_->data_revision();
}

void game_writer::note_data_changed() noexcept
{
    impl_->note_data_changed();
}

auto game_writer::record_canonical_change() -> result<void>
{
    return impl_->record_canonical_change();
}

void game_writer::note_failed_operation() noexcept
{
    impl_->note_failed_operation();
}

void game_writer::impl::clear_insert_caches() noexcept
{
    player_id_cache_.clear();
    event_id_cache_.clear();
    tag_id_cache_.clear();
    player_id_cache_.rehash(0);
    event_id_cache_.rehash(0);
    tag_id_cache_.rehash(0);
}

auto game_writer::impl::prepare_cached_stmt(detail::unique_stmt& stmt, char const* sql) -> result<sqlite3_stmt*>
{
    if (!stmt) {
        sqlite3_stmt* raw = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &raw, nullptr) != SQLITE_OK) {
            return tl::unexpected {error_code::io_failure};
        }
        stmt.reset(raw);
    }
    reset_stmt(stmt.get());
    return stmt.get();
}

auto game_writer::impl::reserve_game_id() -> result<std::uint32_t>
{
    if (!next_game_id_) {
        auto sel = prepare_cached_stmt(select_next_game_id_stmt_, "SELECT next_game_id FROM bundle_meta WHERE id = 1");
        if (!sel) {
            return tl::unexpected {sel.error()};
        }
        auto const step_rc = sqlite3_step(*sel);
        if (step_rc != SQLITE_ROW) {
            reset_stmt(*sel);
            return tl::unexpected {error {error_code::io_failure, "bundle_meta allocator row is missing"}};
        }
        auto const persisted = sqlite3_column_int64(*sel, 0);
        reset_stmt(*sel);
        if (persisted < 1) {
            return tl::unexpected {error {error_code::io_failure, "bundle_meta.next_game_id is corrupt"}};
        }
        next_game_id_ = persisted;
    }

    // Public IDs are uint32_t and 0 is invalid: never wrap or narrow.
    if (std::cmp_greater(*next_game_id_, std::numeric_limits<std::uint32_t>::max())) {
        return tl::unexpected {error {error_code::io_failure, "game id space exhausted"}};
    }
    return static_cast<std::uint32_t>(*next_game_id_);
}

void game_writer::impl::confirm_game_id(std::uint32_t const assigned_id) noexcept
{
    next_game_id_ = static_cast<std::int64_t>(assigned_id) + 1;
    next_game_id_dirty_ = true;
}

auto game_writer::impl::persist_game_id_counter() -> result<void>
{
    if (!next_game_id_dirty_ || !next_game_id_.has_value()) {
        return {};
    }
    auto const counter = *next_game_id_;
    auto upd =
        prepare_cached_stmt(update_next_game_id_stmt_, "UPDATE bundle_meta SET next_game_id = ?1 WHERE id = 1 AND next_game_id < ?1");
    if (!upd) {
        return tl::unexpected {upd.error()};
    }
    sqlite3_bind_int64(*upd, 1, counter);
    auto const step_rc = sqlite3_step(*upd);
    reset_stmt(*upd);
    if (step_rc != SQLITE_DONE) {
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(db_)}};
    }
    if (sqlite3_changes(db_) == 0) {
        // The table is already at or past the value we meant to write, so it
        // advanced behind our back (a legitimate sequence never gets here: the
        // cached value always exceeds the persisted one it was loaded from).
        // The row just inserted is unaffected; stop trusting the cache.
        discard_game_id_counter();
        return {};
    }
    next_game_id_dirty_ = false;
    return {};
}

void game_writer::impl::discard_game_id_counter() noexcept
{
    next_game_id_.reset();
    next_game_id_dirty_ = false;
}

auto game_writer::impl::begin_transaction() -> result<void>
{
    if (sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return tl::unexpected {error_code::io_failure};
    }
    // Reload once per batch rather than trusting the standalone-insert cache:
    // this bounds how long an undetected external edit of bundle_meta can
    // matter to one batch, at the cost of a single SELECT per batch.
    discard_game_id_counter();
    batch_open_ = true;
    canonical_changed_ = false;
    batch_poisoned_ = false;
    return {};
}

auto game_writer::impl::bump_mutation_seq() -> result<void>
{
    // The bound keeps the increment from overflowing int64; exhaustion fails
    // the whole canonical operation instead of wrapping.
    auto upd = prepare_cached_stmt(
        bump_mutation_seq_stmt_,
        "UPDATE mutation_state SET mutation_seq = mutation_seq + 1 WHERE id = 1 AND mutation_seq < 9223372036854775807");
    if (!upd) {
        return tl::unexpected {upd.error()};
    }
    auto const step_rc = sqlite3_step(*upd);
    reset_stmt(*upd);
    if (step_rc != SQLITE_DONE) {
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(db_)}};
    }
    if (sqlite3_changes(db_) != 1) {
        return tl::unexpected {error {error_code::io_failure, "mutation sequence is missing or exhausted"}};
    }
    return {};
}

auto game_writer::impl::record_canonical_change() -> result<void>
{
    if (batch_open_) {
        // One advance per transaction: commit_transaction() bumps once for
        // however many changes the batch made.
        canonical_changed_ = true;
        return {};
    }
    // Standalone operation, or one joined to a transaction the caller owns
    // outside this writer: no commit hook will ever run, so advance now, in
    // that same transaction.
    return bump_mutation_seq();
}

void game_writer::impl::note_failed_operation() noexcept
{
    // A failed operation inside a writer-owned batch may have left partial
    // rows in the batch's transaction without the success-path sequence mark.
    // Committing them would be a silent unindexed mutation, so the batch can
    // only roll back. Standalone operations roll back their own transaction.
    if (batch_open_) {
        batch_poisoned_ = true;
    }
}

auto game_writer::impl::commit_transaction() -> result<void>
{
    // The batch ends with this call whether or not COMMIT succeeds. Persisting
    // the high-water mark and the sequence here, before COMMIT, ties both to
    // the same transaction as the rows they cover. After a successful COMMIT
    // the in-memory counter equals the table, so it stays cached; after any
    // failure it is dropped and reloaded from the table.
    if (batch_poisoned_) {
        rollback_transaction();
        return tl::unexpected {error {error_code::io_failure, "batch contained a failed operation and can only be rolled back"}};
    }
    auto const fail_commit = [this](error err) -> result<void>
    {
        // A COMMIT that fails can leave the transaction open; roll it back
        // before clearing pending state so nothing can query half-state.
        rollback_transaction();
        return tl::unexpected {std::move(err)};
    };
    auto const persisted = persist_game_id_counter();
    if (!persisted) {
        return fail_commit(persisted.error());
    }
    if (canonical_changed_) {
        if (auto bumped = bump_mutation_seq(); !bumped) {
            return fail_commit(bumped.error());
        }
    }
    if (fault::inject(fault::point::before_canonical_commit, fault::artifact_kind::none, 0, db_) == fault::fault_action::fail_io) {
        return fail_commit(error {error_code::io_failure, "injected canonical commit failure"});
    }
    auto const commit_rc = sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
    if (commit_rc != SQLITE_OK) {
        return fail_commit(error {error_code::io_failure, sqlite3_errmsg(db_)});
    }
    batch_open_ = false;
    note_data_changed();
    canonical_changed_ = false;
    // A hook cannot undo a committed transaction; the point exists for crash tests.
    static_cast<void>(fault::inject(fault::point::after_canonical_commit));
    return {};
}

auto game_writer::impl::rollback_transaction() noexcept -> void
{
    if (sqlite3_get_autocommit(db_) == 0) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
    // The ids this batch allocated in memory are dropped with it and the
    // counter is reloaded from the table (which still holds the pre-batch
    // value), so those ids will be reissued. Only committed ids must be
    // monotonic; nothing durable references an aborted one.
    batch_open_ = false;
    // Rows a reader on this connection could see mid-batch are gone.
    note_data_changed();
    canonical_changed_ = false;
    batch_poisoned_ = false;
    discard_game_id_counter();
    // The rolled-back transaction may have inserted (and cached the id of) a
    // player/event/tag row that no longer exists; a later insert reusing
    // that stale id would fail its foreign-key check.
    clear_insert_caches();
}

auto game_writer::impl::find_or_insert_player(player const& plr) -> result<std::int64_t>
{
    if (auto const cached = player_id_cache_.find(plr.name); cached != player_id_cache_.end()) {
        return cached->second;
    }

    {
        auto sel = prepare_cached_stmt(select_player_stmt_, "SELECT id FROM player WHERE name = ?");
        if (!sel) {
            return tl::unexpected {sel.error()};
        }
        sqlite3_bind_text(*sel, 1, plr.name.c_str(), static_cast<int>(plr.name.size()), SQLITE_TRANSIENT);
        if (sqlite3_step(*sel) == SQLITE_ROW) {
            auto const player_id = sqlite3_column_int64(*sel, 0);
            reset_stmt(*sel);
            player_id_cache_.emplace(plr.name, player_id);
            return player_id;
        }
    }

    auto ins = prepare_cached_stmt(insert_player_stmt_, "INSERT INTO player(name, elo, title, country) VALUES(?, ?, ?, ?)");
    if (!ins) {
        return tl::unexpected {ins.error()};
    }
    sqlite3_bind_text(*ins, 1, plr.name.c_str(), static_cast<int>(plr.name.size()), SQLITE_TRANSIENT);
    bind_optional_int(*ins, 2, plr.elo);
    bind_optional_text(*ins, 3, plr.title);
    bind_optional_text(*ins, 4, plr.country);

    if (sqlite3_step(*ins) != SQLITE_DONE) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const player_id = sqlite3_last_insert_rowid(db_);
    player_id_cache_.emplace(plr.name, player_id);
    return player_id;
}

auto game_writer::impl::find_or_insert_event(event const& evt) -> result<std::int64_t>
{
    if (auto const cached = event_id_cache_.find(evt.name); cached != event_id_cache_.end()) {
        return cached->second;
    }

    {
        auto sel = prepare_cached_stmt(select_event_stmt_, "SELECT id FROM event WHERE name = ?");
        if (!sel) {
            return tl::unexpected {sel.error()};
        }
        sqlite3_bind_text(*sel, 1, evt.name.c_str(), static_cast<int>(evt.name.size()), SQLITE_TRANSIENT);
        if (sqlite3_step(*sel) == SQLITE_ROW) {
            auto const event_id = sqlite3_column_int64(*sel, 0);
            reset_stmt(*sel);
            event_id_cache_.emplace(evt.name, event_id);
            return event_id;
        }
    }

    auto ins = prepare_cached_stmt(insert_event_stmt_, "INSERT INTO event(name, site, date) VALUES(?, ?, ?)");
    if (!ins) {
        return tl::unexpected {ins.error()};
    }
    sqlite3_bind_text(*ins, 1, evt.name.c_str(), static_cast<int>(evt.name.size()), SQLITE_TRANSIENT);
    bind_optional_text(*ins, 2, evt.site);
    bind_optional_text(*ins, 3, evt.date);

    if (sqlite3_step(*ins) != SQLITE_DONE) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const event_id = sqlite3_last_insert_rowid(db_);
    event_id_cache_.emplace(evt.name, event_id);
    return event_id;
}

auto game_writer::impl::insert_game_tags(game_id gid, std::vector<std::pair<std::string, std::string>> const& extra_tags) -> result<void>
{
    for (auto const& [tag_name, tag_value] : extra_tags) {
        std::int64_t tag_id = 0;

        if (auto const cached = tag_id_cache_.find(tag_name); cached != tag_id_cache_.end()) {
            tag_id = cached->second;
        } else {
            auto tag_sel = prepare_cached_stmt(select_tag_stmt_, "SELECT id FROM tag WHERE name = ?");
            if (!tag_sel) {
                return tl::unexpected {tag_sel.error()};
            }
            sqlite3_bind_text(*tag_sel, 1, tag_name.c_str(), static_cast<int>(tag_name.size()), SQLITE_TRANSIENT);

            if (sqlite3_step(*tag_sel) == SQLITE_ROW) {
                tag_id = sqlite3_column_int64(*tag_sel, 0);
                reset_stmt(*tag_sel);
            } else {
                auto tag_ins = prepare_cached_stmt(insert_tag_stmt_, "INSERT INTO tag(name) VALUES(?)");
                if (!tag_ins) {
                    return tl::unexpected {tag_ins.error()};
                }
                sqlite3_bind_text(*tag_ins, 1, tag_name.c_str(), static_cast<int>(tag_name.size()), SQLITE_TRANSIENT);
                if (sqlite3_step(*tag_ins) != SQLITE_DONE) {
                    return tl::unexpected {error_code::io_failure};
                }
                tag_id = sqlite3_last_insert_rowid(db_);
            }
            tag_id_cache_.emplace(tag_name, tag_id);
        }

        auto game_tag_ins = prepare_cached_stmt(insert_game_tag_stmt_, "INSERT INTO game_tag(game_id, tag_id, value) VALUES(?, ?, ?)");
        if (!game_tag_ins) {
            return tl::unexpected {game_tag_ins.error()};
        }
        sqlite3_bind_int64(*game_tag_ins, 1, static_cast<std::int64_t>(gid.value));
        sqlite3_bind_int64(*game_tag_ins, 2, tag_id);
        sqlite3_bind_text(*game_tag_ins, 3, tag_value.c_str(), static_cast<int>(tag_value.size()), SQLITE_TRANSIENT);
        if (sqlite3_step(*game_tag_ins) != SQLITE_DONE) {
            return tl::unexpected {error_code::io_failure};
        }
    }

    return {};
}

auto game_writer::impl::insert(game const& src_game) -> result<game_id>
{
    mutation_scope txn {db_, batch_open_};
    allocation_scope id_scope {*this};
    if (!txn.began()) {
        return tl::unexpected {error_code::io_failure};
    }

    // txn's destructor rolls back on any early return below via a raw SQL
    // ROLLBACK, which doesn't know about player_id_cache_/event_id_cache_ --
    // an early return (including the routine "duplicate game" case) after
    // find_or_insert_player/event cached a just-inserted (and now
    // rolled-back) id would leave that id cached, and a later insert reusing
    // it would fail its foreign-key check.
    struct cache_rollback_guard
    {
        game_writer::impl* self;
        bool committed {false};

        explicit cache_rollback_guard(game_writer::impl* owner) noexcept
            : self {owner}
        {
        }

        cache_rollback_guard(cache_rollback_guard const&) = delete;
        auto operator=(cache_rollback_guard const&) -> cache_rollback_guard& = delete;
        cache_rollback_guard(cache_rollback_guard&&) = delete;
        auto operator=(cache_rollback_guard&&) -> cache_rollback_guard& = delete;

        ~cache_rollback_guard() noexcept
        {
            if (!committed) {
                self->clear_insert_caches();
            }
        }
    } cache_guard {this};

    auto white_id = find_or_insert_player(src_game.white);
    if (!white_id) {
        return tl::unexpected {white_id.error()};
    }

    auto black_id = find_or_insert_player(src_game.black);
    if (!black_id) {
        return tl::unexpected {black_id.error()};
    }

    std::optional<std::int64_t> event_id_val;
    if (src_game.event_details.has_value()) {
        auto evt_id = find_or_insert_event(*src_game.event_details);
        if (!evt_id) {
            return tl::unexpected {evt_id.error()};
        }
        event_id_val = *evt_id;
    }

    auto const& moves = src_game.moves;
    auto const blob_bytes = moves.size() * sizeof(std::uint16_t);
    auto const move_hash = XXH3_64bits(moves.data(), blob_bytes);
    auto const reserved_id = reserve_game_id();
    if (!reserved_id) {
        return tl::unexpected {reserved_id.error()};
    }

    auto game_ins = prepare_cached_stmt(insert_game_stmt_, insert_game_sql);
    if (!game_ins) {
        return tl::unexpected {game_ins.error()};
    }

    sqlite3_bind_int64(*game_ins, game_ins_param::white_id, *white_id);
    sqlite3_bind_int64(*game_ins, game_ins_param::black_id, *black_id);
    if (event_id_val) {
        sqlite3_bind_int64(*game_ins, game_ins_param::event_id, *event_id_val);
    } else {
        sqlite3_bind_null(*game_ins, game_ins_param::event_id);
    }
    bind_optional_text(*game_ins, game_ins_param::date, src_game.date);
    sqlite3_bind_text(
        *game_ins, game_ins_param::result, src_game.result.c_str(), static_cast<int>(src_game.result.size()), SQLITE_TRANSIENT);
    bind_optional_text(*game_ins, game_ins_param::eco, src_game.eco);
    if (blob_bytes == 0) {
        sqlite3_bind_zeroblob(*game_ins, game_ins_param::moves, 0);
    } else {
        sqlite3_bind_blob(*game_ins, game_ins_param::moves, moves.data(), static_cast<int>(blob_bytes), SQLITE_TRANSIENT);
    }
    sqlite3_bind_text(*game_ins,
                      game_ins_param::source_type,
                      src_game.provenance.source_type.c_str(),
                      static_cast<int>(src_game.provenance.source_type.size()),
                      SQLITE_TRANSIENT);
    bind_optional_text(*game_ins, game_ins_param::source_label, src_game.provenance.source_label);
    sqlite3_bind_text(*game_ins,
                      game_ins_param::review_status,
                      src_game.provenance.review_status.c_str(),
                      static_cast<int>(src_game.provenance.review_status.size()),
                      SQLITE_TRANSIENT);
    sqlite3_bind_blob(*game_ins, game_ins_param::move_hash, &move_hash, static_cast<int>(sizeof(move_hash)), SQLITE_TRANSIENT);
    bind_optional_int(*game_ins, game_ins_param::white_elo, src_game.white.elo);
    bind_optional_int(*game_ins, game_ins_param::black_elo, src_game.black.elo);
    sqlite3_bind_int64(*game_ins, game_ins_param::game_row_id, static_cast<std::int64_t>(*reserved_id));

    int const ins_rc = sqlite3_step(*game_ins);
    if (ins_rc != SQLITE_DONE) {
        return tl::unexpected {error_code::io_failure};
    }
    if (sqlite3_changes(db_) == 0) {
        // WHERE NOT EXISTS matched an existing row with the same identity
        // fields and move blob -- zero rows inserted, not an error. Nothing
        // was reserved or consumed, so the cached counter stays valid.
        id_scope.succeeded = true;
        return tl::unexpected {error_code::duplicate};
    }

    confirm_game_id(*reserved_id);
    auto const new_game_id = game_id {*reserved_id};

    auto tags_rc = insert_game_tags(new_game_id, src_game.extra_tags);
    if (!tags_rc) {
        return tl::unexpected {tags_rc.error()};
    }

    // Standalone insert (or one joined to a foreign transaction): the counter
    // must reach the table in this same transaction. Inside a writer-owned
    // batch, commit_transaction() writes the high-water mark once instead.
    if (auto record_res = record_canonical_change(); !record_res) {
        return tl::unexpected {record_res.error()};
    }

    if (!batch_open_) {
        if (auto persist_res = persist_game_id_counter(); !persist_res) {
            return tl::unexpected {persist_res.error()};
        }
    }

    if (!txn.commit()) {
        return tl::unexpected {error_code::io_failure};
    }
    cache_guard.committed = true;
    id_scope.succeeded = true;
    note_data_changed();
    return new_game_id;
}

auto game_writer::impl::insert_raw(game const& src_game) -> result<game_id>
{
    mutation_scope txn {db_, batch_open_};
    if (!txn.began()) {
        return tl::unexpected {error_code::io_failure};
    }
    allocation_scope id_scope {*this};

    // Same reasoning as insert()'s guard: an early return below rolls back
    // any player/event/tag row find_or_insert_player/event/tag already
    // cached the id of. In production this call is always nested inside
    // import_pipeline's own transaction (txn_guard joins it and this
    // rollback is a no-op; rollback_sqlite_batch()'s own rollback_transaction()
    // clears the caches instead), but insert_raw is also callable standalone
    // (as the tests do), where this guard is the only thing that clears them.
    struct cache_rollback_guard
    {
        game_writer::impl* self;
        bool committed {false};

        explicit cache_rollback_guard(game_writer::impl* owner) noexcept
            : self {owner}
        {
        }

        cache_rollback_guard(cache_rollback_guard const&) = delete;
        auto operator=(cache_rollback_guard const&) -> cache_rollback_guard& = delete;
        cache_rollback_guard(cache_rollback_guard&&) = delete;
        auto operator=(cache_rollback_guard&&) -> cache_rollback_guard& = delete;

        ~cache_rollback_guard() noexcept
        {
            if (!committed) {
                self->clear_insert_caches();
            }
        }
    } cache_guard {this};

    auto white_id = find_or_insert_player(src_game.white);
    if (!white_id) {
        return tl::unexpected {white_id.error()};
    }

    auto black_id = find_or_insert_player(src_game.black);
    if (!black_id) {
        return tl::unexpected {black_id.error()};
    }

    std::optional<std::int64_t> event_id_val;
    if (src_game.event_details.has_value()) {
        auto evt_id = find_or_insert_event(*src_game.event_details);
        if (!evt_id) {
            return tl::unexpected {evt_id.error()};
        }
        event_id_val = *evt_id;
    }

    auto const& moves = src_game.moves;
    auto const blob_bytes = moves.size() * sizeof(std::uint16_t);
    auto const move_hash = XXH3_64bits(moves.data(), blob_bytes);
    auto const reserved_id = reserve_game_id();
    if (!reserved_id) {
        return tl::unexpected {reserved_id.error()};
    }

    auto game_ins = prepare_cached_stmt(insert_game_raw_stmt_, insert_game_raw_sql);
    if (!game_ins) {
        return tl::unexpected {game_ins.error()};
    }

    sqlite3_bind_int64(*game_ins, game_ins_param::white_id, *white_id);
    sqlite3_bind_int64(*game_ins, game_ins_param::black_id, *black_id);
    if (event_id_val) {
        sqlite3_bind_int64(*game_ins, game_ins_param::event_id, *event_id_val);
    } else {
        sqlite3_bind_null(*game_ins, game_ins_param::event_id);
    }
    bind_optional_text(*game_ins, game_ins_param::date, src_game.date);
    sqlite3_bind_text(
        *game_ins, game_ins_param::result, src_game.result.c_str(), static_cast<int>(src_game.result.size()), SQLITE_TRANSIENT);
    bind_optional_text(*game_ins, game_ins_param::eco, src_game.eco);
    if (blob_bytes == 0) {
        sqlite3_bind_zeroblob(*game_ins, game_ins_param::moves, 0);
    } else {
        sqlite3_bind_blob(*game_ins, game_ins_param::moves, moves.data(), static_cast<int>(blob_bytes), SQLITE_TRANSIENT);
    }
    sqlite3_bind_text(*game_ins,
                      game_ins_param::source_type,
                      src_game.provenance.source_type.c_str(),
                      static_cast<int>(src_game.provenance.source_type.size()),
                      SQLITE_TRANSIENT);
    bind_optional_text(*game_ins, game_ins_param::source_label, src_game.provenance.source_label);
    sqlite3_bind_text(*game_ins,
                      game_ins_param::review_status,
                      src_game.provenance.review_status.c_str(),
                      static_cast<int>(src_game.provenance.review_status.size()),
                      SQLITE_TRANSIENT);
    sqlite3_bind_blob(*game_ins, game_ins_param::move_hash, &move_hash, static_cast<int>(sizeof(move_hash)), SQLITE_TRANSIENT);
    bind_optional_int(*game_ins, game_ins_param::white_elo, src_game.white.elo);
    bind_optional_int(*game_ins, game_ins_param::black_elo, src_game.black.elo);
    sqlite3_bind_int64(*game_ins, game_ins_param::game_row_id, static_cast<std::int64_t>(*reserved_id));

    if (sqlite3_step(*game_ins) != SQLITE_DONE) {
        return tl::unexpected {error_code::io_failure};
    }

    confirm_game_id(*reserved_id);
    auto const new_game_id = game_id {*reserved_id};

    auto tags_rc = insert_game_tags(new_game_id, src_game.extra_tags);
    if (!tags_rc) {
        return tl::unexpected {tags_rc.error()};
    }

    if (auto record_res = record_canonical_change(); !record_res) {
        return tl::unexpected {record_res.error()};
    }

    if (!batch_open_) {
        if (auto persist_res = persist_game_id_counter(); !persist_res) {
            return tl::unexpected {persist_res.error()};
        }
    }

    if (!txn.commit()) {
        return tl::unexpected {error_code::io_failure};
    }
    cache_guard.committed = true;
    id_scope.succeeded = true;
    note_data_changed();
    return new_game_id;
}

auto game_writer::impl::drop_identity_index() -> result<void>
{
    return schema::drop_identity_index(db_);
}

auto game_writer::impl::identity_index_exists() -> result<bool>
{
    return schema::identity_index_exists(db_);
}

auto game_writer::impl::deduplicate() -> result<dedup_summary>
{
    // Dedup is several statements (delete, reassign, recreate index, sequence
    // advance). Any failure must undo all of them: the scope rolls back a
    // local transaction or the operation savepoint of a joined one (always
    // taken here, even in a writer-owned batch, because dedup is rare and a
    // savepoint leaves the batch consistent), and a writer-owned batch is
    // additionally poisoned so it can only roll back as a whole.
    mutation_scope txn {db_, /*in_writer_batch=*/false};
    if (!txn.began()) {
        return tl::unexpected {error_code::io_failure};
    }

    struct failure_scope
    {
        impl& owner;
        bool succeeded {false};

        explicit failure_scope(impl& owning_impl) noexcept
            : owner {owning_impl}
        {
        }

        failure_scope(failure_scope const&) = delete;
        auto operator=(failure_scope const&) -> failure_scope& = delete;
        failure_scope(failure_scope&&) = delete;
        auto operator=(failure_scope&&) -> failure_scope& = delete;

        ~failure_scope() noexcept
        {
            if (!succeeded) {
                owner.note_failed_operation();
            }
        }
    } failure_guard {*this};

    if (sqlite3_exec(db_, dedup_delete_sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const removed = static_cast<std::size_t>(sqlite3_changes(db_));

    if (sqlite3_exec(db_, dedup_reassign_collision_sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const reassigned = static_cast<std::size_t>(sqlite3_changes(db_));

    if (auto index_res = schema::create_identity_index(db_); !index_res) {
        return tl::unexpected {index_res.error()};
    }

    // Dropping and recreating the identity index is structural maintenance and
    // does not advance the sequence. Deleted rows or changed collision slots
    // change the canonical game set the indexes cover, so they advance it once.
    if (removed != 0U || reassigned != 0U) {
        if (auto record_res = record_canonical_change(); !record_res) {
            return tl::unexpected {record_res.error()};
        }
    }

    if (!txn.commit()) {
        return tl::unexpected {error_code::io_failure};
    }
    failure_guard.succeeded = true;
    note_data_changed();
    return dedup_summary {.removed = removed};
}

}  // namespace motif::db
