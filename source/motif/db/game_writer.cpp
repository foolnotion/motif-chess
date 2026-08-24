#include <cstddef>
#include <cstdint>
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
using detail::txn_guard;
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
                     source_type, source_label, review_status, move_hash, identity_collision)
    SELECT ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11,
           COALESCE((SELECT MAX(identity_collision) + 1 FROM game
                     WHERE white_id = ?1 AND black_id = ?2
                       AND COALESCE(event_id, -1) = COALESCE(?3, -1)
                       AND COALESCE(date, '') = COALESCE(?4, '')
                       AND result = ?5 AND move_hash = ?11), 0)
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
                     source_type, source_label, review_status, move_hash, identity_collision)
    VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, 0)
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
    WHERE ranked.id = game.id
)sql";

}  // namespace

// ── game_writer
// ───────────────────────────────────────────────────────────────

game_writer::game_writer(sqlite3* conn) noexcept
    : impl_ {std::make_unique<impl>(conn)}
{
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

auto game_writer::impl::begin_transaction() -> result<void>
{
    if (sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return tl::unexpected {error_code::io_failure};
    }
    return {};
}

auto game_writer::impl::commit_transaction() -> result<void>
{
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return tl::unexpected {error_code::io_failure};
    }
    return {};
}

auto game_writer::impl::rollback_transaction() noexcept -> void
{
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
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
    txn_guard txn {db_};
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

    int const ins_rc = sqlite3_step(*game_ins);
    if (ins_rc != SQLITE_DONE) {
        return tl::unexpected {error_code::io_failure};
    }
    if (sqlite3_changes(db_) == 0) {
        // WHERE NOT EXISTS matched an existing row with the same identity
        // fields and move blob -- zero rows inserted, not an error.
        return tl::unexpected {error_code::duplicate};
    }

    auto const new_game_id = game_id {static_cast<std::uint32_t>(sqlite3_last_insert_rowid(db_))};

    auto tags_rc = insert_game_tags(new_game_id, src_game.extra_tags);
    if (!tags_rc) {
        return tl::unexpected {tags_rc.error()};
    }

    if (!txn.commit()) {
        return tl::unexpected {error_code::io_failure};
    }
    cache_guard.committed = true;
    return new_game_id;
}

auto game_writer::impl::insert_raw(game const& src_game) -> result<game_id>
{
    txn_guard txn {db_};
    if (!txn.began()) {
        return tl::unexpected {error_code::io_failure};
    }

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

    if (sqlite3_step(*game_ins) != SQLITE_DONE) {
        return tl::unexpected {error_code::io_failure};
    }

    auto const new_game_id = game_id {static_cast<std::uint32_t>(sqlite3_last_insert_rowid(db_))};

    auto tags_rc = insert_game_tags(new_game_id, src_game.extra_tags);
    if (!tags_rc) {
        return tl::unexpected {tags_rc.error()};
    }

    if (!txn.commit()) {
        return tl::unexpected {error_code::io_failure};
    }
    cache_guard.committed = true;
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
    txn_guard txn {db_};
    if (!txn.began()) {
        return tl::unexpected {error_code::io_failure};
    }

    if (sqlite3_exec(db_, dedup_delete_sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const removed = static_cast<std::size_t>(sqlite3_changes(db_));

    if (sqlite3_exec(db_, dedup_reassign_collision_sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        return tl::unexpected {error_code::io_failure};
    }

    if (auto index_res = schema::create_identity_index(db_); !index_res) {
        return tl::unexpected {index_res.error()};
    }

    if (!txn.commit()) {
        return tl::unexpected {error_code::io_failure};
    }
    return dedup_summary {.removed = removed};
}

}  // namespace motif::db
