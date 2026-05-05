#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "motif/db/game_store.hpp"

#include <gtl/phmap.hpp>
#include <sqlite3.h>
#include <tl/expected.hpp>

#include "motif/db/error.hpp"
#include "motif/db/game_writer.hpp"
#include "motif/db/sqlite_impl.hpp"
#include "motif/db/sqlite_util.hpp"
#include "motif/db/types.hpp"

namespace motif::db
{

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

auto finalize_stmt(sqlite3_stmt*& stmt) noexcept -> void
{
    if (stmt != nullptr) {
        sqlite3_finalize(stmt);
        stmt = nullptr;
    }
}

auto reset_stmt(sqlite3_stmt* stmt) noexcept -> void
{
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
}

// Column positions in the JOIN query used by get():
//   w.name, w.elo, w.title, w.country,             (0-3)
//   b.name, b.elo, b.title, b.country,             (4-7)
//   e.name, e.site, e.date,                        (8-10)
//   g.date, g.result, g.eco, g.moves,              (11-14)
//   g.source_type, g.source_label, g.review_status (15-17)
namespace get_col
{
constexpr int white_name = 0;
constexpr int white_elo = 1;
constexpr int white_title = 2;
constexpr int white_country = 3;
constexpr int black_name = 4;
constexpr int black_elo = 5;
constexpr int black_title = 6;
constexpr int black_country = 7;
constexpr int event_name = 8;
constexpr int event_site = 9;
constexpr int event_date = 10;
constexpr int game_date = 11;
constexpr int game_result = 12;
constexpr int game_eco = 13;
constexpr int game_moves = 14;
constexpr int game_source_type = 15;
constexpr int game_source_label = 16;
constexpr int game_review_status = 17;
}  // namespace get_col

namespace list_col
{
constexpr int game_id = 0;
constexpr int white = 1;
constexpr int black = 2;
constexpr int result = 3;
constexpr int event = 4;
constexpr int date = 5;
constexpr int eco = 6;
constexpr int source_type = 7;
constexpr int source_label = 8;
constexpr int review_status = 9;
constexpr int white_elo = 10;
constexpr int black_elo = 11;
}  // namespace list_col

namespace list_param
{
constexpr int player_is_null = 1;
constexpr int white_player = 2;
constexpr int black_player = 3;
constexpr int result_is_null = 4;
constexpr int result = 5;
constexpr int eco_is_null = 6;
constexpr int eco_prefix = 7;
constexpr int date_from_is_null = 8;
constexpr int date_from = 9;
constexpr int date_to_is_null = 10;
constexpr int date_to = 11;
constexpr int min_elo_is_null = 12;
constexpr int min_elo_white = 13;
constexpr int min_elo_black = 14;
constexpr int max_elo_is_null = 15;
constexpr int max_elo_white = 16;
constexpr int max_elo_black = 17;
constexpr int limit = 18;
constexpr int offset = 19;
}  // namespace list_param

// NOLINT(llvm-prefer-static-over-anonymous-namespace): conflicts with
// misc-use-anonymous-namespace
[[nodiscard]] auto prepare(sqlite3* conn, char const* sql) -> result<unique_stmt>
{  // NOLINT(llvm-prefer-static-over-anonymous-namespace)
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(conn, sql, -1, &raw, nullptr) != SQLITE_OK) {
        return tl::unexpected {error_code::io_failure};
    }
    return unique_stmt {raw};
}

// sqlite3_column_text returns const unsigned char*; adapts it to char const*
// safely.
[[nodiscard]] auto column_text(sqlite3_stmt* stmt, int col) -> char const*
{  // NOLINT(llvm-prefer-static-over-anonymous-namespace)
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<char const*>(sqlite3_column_text(stmt, col));
}

[[nodiscard]] auto column_optional_text(  // NOLINT(llvm-prefer-static-over-anonymous-namespace)
    sqlite3_stmt* stmt,
    int col) -> std::optional<std::string>
{
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) {
        return std::nullopt;
    }
    auto const* text = column_text(stmt, col);
    if (text == nullptr) {
        return std::nullopt;
    }
    return std::string {text};
}

[[nodiscard]] auto column_optional_int32(  // NOLINT(llvm-prefer-static-over-anonymous-namespace)
    sqlite3_stmt* stmt,
    int col) -> std::optional<std::int32_t>
{
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(sqlite3_column_int(stmt, col));
}

[[nodiscard]] auto exec(sqlite3* conn, char const* sql) -> result<void>
{  // NOLINT(llvm-prefer-static-over-anonymous-namespace)
    char* err_msg = nullptr;
    int const exec_result = sqlite3_exec(conn, sql, nullptr, nullptr, &err_msg);
    if (err_msg != nullptr) {
        sqlite3_free(err_msg);
    }
    if (exec_result != SQLITE_OK) {
        return tl::unexpected {error_code::io_failure};
    }
    return {};
}

[[nodiscard]] auto foreign_keys_enabled(sqlite3* conn) -> result<bool>
{  // NOLINT(llvm-prefer-static-over-anonymous-namespace)
    auto stmt = prepare(conn, "PRAGMA foreign_keys");
    if (!stmt) {
        return tl::unexpected {stmt.error()};
    }
    if (sqlite3_step(stmt->get()) != SQLITE_ROW) {
        return tl::unexpected {error_code::io_failure};
    }
    return sqlite3_column_int(stmt->get(), 0) == 1;
}

}  // namespace

// ── game_store
// ────────────────────────────────────────────────────────────────

game_store::game_store(sqlite3* conn) noexcept
    : db_ {conn}
    , writer_ {std::make_unique<game_writer>(conn)}
{
}

game_store::~game_store() noexcept
{
    finalize_stmt(select_player_stmt_);
    finalize_stmt(insert_player_stmt_);
    finalize_stmt(select_event_stmt_);
    finalize_stmt(insert_event_stmt_);
    finalize_stmt(select_tag_stmt_);
    finalize_stmt(insert_tag_stmt_);
    finalize_stmt(insert_game_tag_stmt_);
}

game_store::game_store(game_store&& other) noexcept
    : db_ {std::exchange(other.db_, nullptr)}
    , player_id_cache_ {std::move(other.player_id_cache_)}
    , event_id_cache_ {std::move(other.event_id_cache_)}
    , tag_id_cache_ {std::move(other.tag_id_cache_)}
    , select_player_stmt_ {std::exchange(other.select_player_stmt_, nullptr)}
    , insert_player_stmt_ {std::exchange(other.insert_player_stmt_, nullptr)}
    , select_event_stmt_ {std::exchange(other.select_event_stmt_, nullptr)}
    , insert_event_stmt_ {std::exchange(other.insert_event_stmt_, nullptr)}
    , select_tag_stmt_ {std::exchange(other.select_tag_stmt_, nullptr)}
    , insert_tag_stmt_ {std::exchange(other.insert_tag_stmt_, nullptr)}
    , insert_game_tag_stmt_ {std::exchange(other.insert_game_tag_stmt_, nullptr)}
    , writer_ {std::move(other.writer_)}
{
}

auto game_store::operator=(game_store&& other) noexcept -> game_store&
{
    if (this != &other) {
        finalize_stmt(select_player_stmt_);
        finalize_stmt(insert_player_stmt_);
        finalize_stmt(select_event_stmt_);
        finalize_stmt(insert_event_stmt_);
        finalize_stmt(select_tag_stmt_);
        finalize_stmt(insert_tag_stmt_);
        finalize_stmt(insert_game_tag_stmt_);

        db_ = std::exchange(other.db_, nullptr);
        player_id_cache_ = std::move(other.player_id_cache_);
        event_id_cache_ = std::move(other.event_id_cache_);
        tag_id_cache_ = std::move(other.tag_id_cache_);
        select_player_stmt_ = std::exchange(other.select_player_stmt_, nullptr);
        insert_player_stmt_ = std::exchange(other.insert_player_stmt_, nullptr);
        select_event_stmt_ = std::exchange(other.select_event_stmt_, nullptr);
        insert_event_stmt_ = std::exchange(other.insert_event_stmt_, nullptr);
        select_tag_stmt_ = std::exchange(other.select_tag_stmt_, nullptr);
        insert_tag_stmt_ = std::exchange(other.insert_tag_stmt_, nullptr);
        insert_game_tag_stmt_ = std::exchange(other.insert_game_tag_stmt_, nullptr);
        writer_ = std::move(other.writer_);
    }
    return *this;
}

void game_store::clear_insert_caches() noexcept
{
    writer_->clear_insert_caches();
}

auto game_store::begin_transaction() -> result<void>
{
    return writer_->begin_transaction();
}

auto game_store::commit_transaction() -> result<void>
{
    return writer_->commit_transaction();
}

auto game_store::rollback_transaction() noexcept -> void
{
    writer_->rollback_transaction();
}

auto game_store::create_schema() -> result<void>
{
    auto pragma_rc = exec(db_, "PRAGMA foreign_keys = ON;");
    if (!pragma_rc) {
        return tl::unexpected {pragma_rc.error()};
    }

    auto fk_enabled = foreign_keys_enabled(db_);
    if (!fk_enabled) {
        return tl::unexpected {fk_enabled.error()};
    }
    if (!*fk_enabled) {
        return tl::unexpected {error_code::io_failure};
    }

    // language=sql
    static constexpr char const* sql = R"sql(
        CREATE TABLE IF NOT EXISTS player (
            id      INTEGER PRIMARY KEY,
            name    TEXT    NOT NULL UNIQUE,
            elo     INTEGER,
            title   TEXT,
            country TEXT
        );

        CREATE TABLE IF NOT EXISTS event (
            id   INTEGER PRIMARY KEY,
            name TEXT NOT NULL UNIQUE,
            site TEXT,
            date TEXT
        );

        CREATE TABLE IF NOT EXISTS tag (
            id   INTEGER PRIMARY KEY,
            name TEXT NOT NULL UNIQUE
        );

        CREATE TABLE IF NOT EXISTS game (
            id            INTEGER PRIMARY KEY,
            white_id      INTEGER NOT NULL REFERENCES player(id),
            black_id      INTEGER NOT NULL REFERENCES player(id),
            event_id      INTEGER REFERENCES event(id),
            date          TEXT,
            result        TEXT NOT NULL,
            eco           TEXT,
            moves         BLOB NOT NULL,
            source_type   TEXT NOT NULL DEFAULT 'imported',
            source_label  TEXT,
            review_status TEXT NOT NULL DEFAULT 'new'
        );

        CREATE UNIQUE INDEX IF NOT EXISTS ux_game_identity ON game(
            white_id,
            black_id,
            COALESCE(event_id, -1),
            COALESCE(date, ''),
            result,
            moves
        );

        CREATE TABLE IF NOT EXISTS game_tag (
            game_id INTEGER NOT NULL REFERENCES game(id) ON DELETE CASCADE,
            tag_id  INTEGER NOT NULL REFERENCES tag(id),
            value   TEXT    NOT NULL,
            PRIMARY KEY (game_id, tag_id)
        );
    )sql";

    return exec(db_, sql);
}

auto game_store::insert(game const& src_game) -> result<game_id>
{
    return writer_->insert(src_game);
}

auto game_store::prepare_cached_stmt(sqlite3_stmt*& stmt, char const* sql) -> result<sqlite3_stmt*>
{
    if (stmt == nullptr) {
        sqlite3_stmt* raw = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &raw, nullptr) != SQLITE_OK) {
            return tl::unexpected {error_code::io_failure};
        }
        stmt = raw;
    }
    reset_stmt(stmt);
    return stmt;
}

auto game_store::find_or_insert_player(player const& plr) -> result<std::int64_t>
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

auto game_store::find_or_insert_event(event const& evt) -> result<std::int64_t>
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

auto game_store::insert_game_tags(game_id const game_id, std::vector<std::pair<std::string, std::string>> const& extra_tags) -> result<void>
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
        sqlite3_bind_int64(*game_tag_ins, 1, static_cast<std::int64_t>(game_id.value));
        sqlite3_bind_int64(*game_tag_ins, 2, tag_id);
        sqlite3_bind_text(*game_tag_ins, 3, tag_value.c_str(), static_cast<int>(tag_value.size()), SQLITE_TRANSIENT);
        if (sqlite3_step(*game_tag_ins) != SQLITE_DONE) {
            return tl::unexpected {error_code::io_failure};
        }
    }

    return {};
}

auto game_store::get(game_id const game_id) -> result<game>
{
    return const_cast<game_store const&>(*this).get(game_id);
}

auto game_store::get(game_id const game_id) const -> result<game>
{
    // language=sql
    static constexpr char const* sql = R"sql(
        SELECT
            w.name, w.elo, w.title, w.country,
            b.name, b.elo, b.title, b.country,
            e.name, e.site, e.date,
            g.date, g.result, g.eco, g.moves,
            g.source_type, g.source_label, g.review_status
        FROM game g
        JOIN player w ON w.id = g.white_id
        JOIN player b ON b.id = g.black_id
        LEFT JOIN event e ON e.id = g.event_id
        WHERE g.id = ?
    )sql";

    auto stmt = prepare(db_, sql);
    if (!stmt) {
        return tl::unexpected {stmt.error()};
    }
    sqlite3_bind_int64(stmt->get(), 1, static_cast<std::int64_t>(game_id.value));

    int const step_rc = sqlite3_step(stmt->get());
    if (step_rc == SQLITE_DONE) {
        return tl::unexpected {error_code::not_found};
    }
    if (step_rc != SQLITE_ROW) {
        return tl::unexpected {error_code::io_failure};
    }

    game out;

    out.white.name = column_text(stmt->get(), get_col::white_name);
    out.white.elo = column_optional_int32(stmt->get(), get_col::white_elo);
    out.white.title = column_optional_text(stmt->get(), get_col::white_title);
    out.white.country = column_optional_text(stmt->get(), get_col::white_country);

    out.black.name = column_text(stmt->get(), get_col::black_name);
    out.black.elo = column_optional_int32(stmt->get(), get_col::black_elo);
    out.black.title = column_optional_text(stmt->get(), get_col::black_title);
    out.black.country = column_optional_text(stmt->get(), get_col::black_country);

    if (sqlite3_column_type(stmt->get(), get_col::event_name) != SQLITE_NULL) {
        event evt_row;
        evt_row.name = column_text(stmt->get(), get_col::event_name);
        evt_row.site = column_optional_text(stmt->get(), get_col::event_site);
        evt_row.date = column_optional_text(stmt->get(), get_col::event_date);
        out.event_details = std::move(evt_row);
    }

    out.date = column_optional_text(stmt->get(), get_col::game_date);
    out.result = column_text(stmt->get(), get_col::game_result);
    out.eco = column_optional_text(stmt->get(), get_col::game_eco);

    {
        auto const* src_type_raw = column_text(stmt->get(), get_col::game_source_type);
        out.provenance.source_type = (src_type_raw != nullptr) ? std::string {src_type_raw} : "imported";
    }
    out.provenance.source_label = column_optional_text(stmt->get(), get_col::game_source_label);
    {
        auto const* rev_status_raw = column_text(stmt->get(), get_col::game_review_status);
        out.provenance.review_status = (rev_status_raw != nullptr) ? std::string {rev_status_raw} : "new";
    }

    // Decode move blob — raw uint16_t array stored by insert().
    auto const* blob = static_cast<std::uint8_t const*>(sqlite3_column_blob(stmt->get(), get_col::game_moves));
    int const blob_bytes = sqlite3_column_bytes(stmt->get(), get_col::game_moves);
    if (blob_bytes % static_cast<int>(sizeof(std::uint16_t)) != 0) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const move_count = static_cast<std::size_t>(blob_bytes) / sizeof(std::uint16_t);
    out.moves.resize(move_count);
    if (move_count > 0 && blob != nullptr) {
        std::memcpy(out.moves.data(), blob, static_cast<std::size_t>(blob_bytes));
    }

    // Extra tags
    auto tags_stmt =
        prepare(db_,
                "SELECT t.name, gt.value FROM game_tag gt " "JOIN tag t ON t.id = gt.tag_id WHERE gt.game_id = ? ORDER BY gt.rowid");
    if (!tags_stmt) {
        return tl::unexpected {tags_stmt.error()};
    }
    sqlite3_bind_int64(tags_stmt->get(), 1, static_cast<std::int64_t>(game_id.value));
    int tag_rc = SQLITE_ROW;
    while ((tag_rc = sqlite3_step(tags_stmt->get())) == SQLITE_ROW) {
        out.extra_tags.emplace_back(std::string {column_text(tags_stmt->get(), 0)}, std::string {column_text(tags_stmt->get(), 1)});
    }
    if (tag_rc != SQLITE_DONE) {
        return tl::unexpected {error_code::io_failure};
    }

    return out;
}

auto game_store::get_game_contexts(std::vector<game_id> const& game_ids) -> result<gtl::flat_hash_map<game_id, game_context>>
{
    return const_cast<game_store const&>(*this).get_game_contexts(game_ids);
}

auto game_store::get_game_contexts(std::vector<game_id> const& game_ids) const -> result<gtl::flat_hash_map<game_id, game_context>>
{
    auto out = gtl::flat_hash_map<game_id, game_context> {};
    if (game_ids.empty()) {
        return out;
    }

    out.reserve(game_ids.size());

    // SQLite limits the number of host parameters per statement; batch to stay within it.
    auto const max_params = static_cast<std::size_t>(sqlite3_limit(db_, SQLITE_LIMIT_VARIABLE_NUMBER, -1));
    auto const batch_size = max_params > 0U ? max_params : std::size_t {999};

    for (std::size_t batch_start = 0; batch_start < game_ids.size(); batch_start += batch_size) {
        auto const batch_end = std::min(batch_start + batch_size, game_ids.size());
        auto const batch_count = batch_end - batch_start;

        auto sql = std::ostringstream {};
        sql << R"sql(
            SELECT
                g.id,
                g.eco,
                (
                    SELECT gt.value
                    FROM game_tag gt
                    JOIN tag t ON t.id = gt.tag_id
                    WHERE gt.game_id = g.id AND t.name = 'Opening'
                    ORDER BY gt.rowid
                    LIMIT 1
                ) AS opening_name,
                g.moves
            FROM game g
            WHERE g.id IN (
        )sql";

        for (std::size_t index = 0; index < batch_count; ++index) {
            if (index > 0U) {
                sql << ',';
            }
            sql << '?';
        }
        sql << ')';

        auto stmt = prepare(db_, sql.str().c_str());
        if (!stmt) {
            return tl::unexpected {stmt.error()};
        }

        for (std::size_t index = 0; index < batch_count; ++index) {
            sqlite3_bind_int64(stmt->get(), static_cast<int>(index + 1U), static_cast<std::int64_t>(game_ids[batch_start + index].value));
        }

        int step_rc = SQLITE_ROW;
        while ((step_rc = sqlite3_step(stmt->get())) == SQLITE_ROW) {
            auto const game_id = motif::db::game_id {static_cast<std::uint32_t>(sqlite3_column_int64(stmt->get(), 0))};
            auto context = game_context {};
            context.eco = column_optional_text(stmt->get(), 1);
            context.opening_name = column_optional_text(stmt->get(), 2);

            auto const* blob = static_cast<std::uint8_t const*>(sqlite3_column_blob(stmt->get(), 3));
            int const blob_bytes = sqlite3_column_bytes(stmt->get(), 3);
            if (blob_bytes % static_cast<int>(sizeof(std::uint16_t)) != 0) {
                return tl::unexpected {error_code::io_failure};
            }

            auto const move_count = static_cast<std::size_t>(blob_bytes) / sizeof(std::uint16_t);
            context.moves.resize(move_count);
            if (move_count > 0 && blob != nullptr) {
                std::memcpy(context.moves.data(), blob, static_cast<std::size_t>(blob_bytes));
            }

            out.emplace(game_id, std::move(context));
        }

        if (step_rc != SQLITE_DONE) {
            return tl::unexpected {error_code::io_failure};
        }
    }

    return out;
}

auto game_store::list_games(game_list_query const& query) const -> result<std::vector<game_list_entry>>
{
    // language=sql
    static constexpr char const* sql = R"sql(
        SELECT
            g.id,
            w.name,
            b.name,
            COALESCE(g.result, ''),
            COALESCE(e.name, ''),
            COALESCE(g.date, ''),
            COALESCE(g.eco, ''),
            COALESCE(g.source_type, 'imported'),
            g.source_label,
            COALESCE(g.review_status, 'new'),
            w.elo,
            b.elo
        FROM game g
        JOIN player w ON w.id = g.white_id
        JOIN player b ON b.id = g.black_id
        LEFT JOIN event e ON e.id = g.event_id
        WHERE (? IS NULL OR instr(lower(w.name), lower(?)) > 0 OR instr(lower(b.name), lower(?)) > 0)
          AND (? IS NULL OR g.result = ?)
          AND (? IS NULL OR g.eco LIKE ? || '%')
          AND (? IS NULL OR g.date >= ?)
          AND (? IS NULL OR g.date <= ?)
          AND (? IS NULL OR w.elo >= ? OR b.elo >= ?)
          AND (? IS NULL OR w.elo <= ? OR b.elo <= ?)
        ORDER BY g.id ASC
        LIMIT ? OFFSET ?
    )sql";

    auto stmt = prepare(db_, sql);
    if (!stmt) {
        return tl::unexpected {stmt.error()};
    }

    constexpr auto sqlite_max = static_cast<std::size_t>(std::numeric_limits<sqlite3_int64>::max());
    if (query.limit > sqlite_max || query.offset > sqlite_max) {
        return tl::unexpected {error_code::io_failure};
    }

    bind_optional_text(stmt->get(), list_param::player_is_null, query.player);
    bind_optional_text(stmt->get(), list_param::white_player, query.player);
    bind_optional_text(stmt->get(), list_param::black_player, query.player);
    bind_optional_text(stmt->get(), list_param::result_is_null, query.result);
    bind_optional_text(stmt->get(), list_param::result, query.result);
    bind_optional_text(stmt->get(), list_param::eco_is_null, query.eco_prefix);
    bind_optional_text(stmt->get(), list_param::eco_prefix, query.eco_prefix);
    bind_optional_text(stmt->get(), list_param::date_from_is_null, query.date_from);
    bind_optional_text(stmt->get(), list_param::date_from, query.date_from);
    bind_optional_text(stmt->get(), list_param::date_to_is_null, query.date_to);
    bind_optional_text(stmt->get(), list_param::date_to, query.date_to);
    bind_optional_int(stmt->get(), list_param::min_elo_is_null, query.min_elo);
    bind_optional_int(stmt->get(), list_param::min_elo_white, query.min_elo);
    bind_optional_int(stmt->get(), list_param::min_elo_black, query.min_elo);
    bind_optional_int(stmt->get(), list_param::max_elo_is_null, query.max_elo);
    bind_optional_int(stmt->get(), list_param::max_elo_white, query.max_elo);
    bind_optional_int(stmt->get(), list_param::max_elo_black, query.max_elo);
    if (sqlite3_bind_int64(stmt->get(), list_param::limit, static_cast<sqlite3_int64>(query.limit)) != SQLITE_OK
        || sqlite3_bind_int64(stmt->get(), list_param::offset, static_cast<sqlite3_int64>(query.offset)) != SQLITE_OK)
    {
        return tl::unexpected {error_code::io_failure};
    }

    auto entries = std::vector<game_list_entry> {};
    static constexpr std::size_t max_reserve {256};
    entries.reserve(std::min(query.limit, max_reserve));

    int step_rc = SQLITE_ROW;
    while ((step_rc = sqlite3_step(stmt->get())) == SQLITE_ROW) {
        auto const* src_type_raw = column_text(stmt->get(), list_col::source_type);
        auto const* rev_status_raw = column_text(stmt->get(), list_col::review_status);
        entries.push_back(game_list_entry {
            .id = motif::db::game_id {static_cast<std::uint32_t>(sqlite3_column_int64(stmt->get(), list_col::game_id))},
            .white = column_text(stmt->get(), list_col::white),
            .black = column_text(stmt->get(), list_col::black),
            .white_elo = column_optional_int32(stmt->get(), list_col::white_elo),
            .black_elo = column_optional_int32(stmt->get(), list_col::black_elo),
            .result = column_text(stmt->get(), list_col::result),
            .event = column_text(stmt->get(), list_col::event),
            .date = column_text(stmt->get(), list_col::date),
            .eco = column_text(stmt->get(), list_col::eco),
            .source_type = (src_type_raw != nullptr) ? std::string {src_type_raw} : "imported",
            .source_label = column_optional_text(stmt->get(), list_col::source_label),
            .review_status = (rev_status_raw != nullptr) ? std::string {rev_status_raw} : "new",
        });
    }

    if (step_rc != SQLITE_DONE) {
        return tl::unexpected {error_code::io_failure};
    }

    return entries;
}

auto game_store::count_games() const -> result<std::int64_t>
{
    // language=sql
    static constexpr char const* sql = R"sql(SELECT COUNT(*) FROM game)sql";
    auto stmt = prepare(db_, sql);
    if (!stmt) {
        return tl::unexpected {stmt.error()};
    }
    int const step_rc = sqlite3_step(stmt->get());
    if (step_rc != SQLITE_ROW) {
        return tl::unexpected {error_code::io_failure};
    }
    return sqlite3_column_int64(stmt->get(), 0);
}

auto game_store::remove(game_id const game_id) -> result<void>
{
    // ON DELETE CASCADE in game_tag handles tag row cleanup automatically.
    auto stmt = prepare(db_, "DELETE FROM game WHERE id = ?");
    if (!stmt) {
        return tl::unexpected {stmt.error()};
    }
    sqlite3_bind_int64(stmt->get(), 1, static_cast<std::int64_t>(game_id.value));

    if (sqlite3_step(stmt->get()) != SQLITE_DONE) {
        return tl::unexpected {error_code::io_failure};
    }
    if (sqlite3_changes(db_) == 0) {
        return tl::unexpected {error_code::not_found};
    }
    return {};
}

auto game_store::get_provenance(game_id const game_id) const -> result<game_provenance>
{
    // language=sql
    static constexpr char const* sql = R"sql(
        SELECT source_type, source_label, review_status
        FROM game
        WHERE id = ?
    )sql";

    auto stmt = prepare(db_, sql);
    if (!stmt) {
        return tl::unexpected {stmt.error()};
    }
    sqlite3_bind_int64(stmt->get(), 1, static_cast<std::int64_t>(game_id.value));

    int const step_rc = sqlite3_step(stmt->get());
    if (step_rc == SQLITE_DONE) {
        return tl::unexpected {error_code::not_found};
    }
    if (step_rc != SQLITE_ROW) {
        return tl::unexpected {error_code::io_failure};
    }

    game_provenance prov;
    {
        auto const* raw = column_text(stmt->get(), 0);
        prov.source_type = (raw != nullptr) ? std::string {raw} : "imported";
    }
    prov.source_label = column_optional_text(stmt->get(), 1);
    {
        auto const* raw = column_text(stmt->get(), 2);
        prov.review_status = (raw != nullptr) ? std::string {raw} : "new";
    }
    return prov;
}

auto game_store::update_text_field(game_id const game_id, std::string_view column, std::string const& value) -> result<void>
{
    auto const sql = "UPDATE game SET " + std::string(column) + " = ? WHERE id = ?";
    auto upd = prepare(db_, sql.c_str());
    if (!upd) {
        return tl::unexpected {upd.error()};
    }
    sqlite3_bind_text(upd->get(), 1, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(upd->get(), 2, static_cast<std::int64_t>(game_id.value));
    if (sqlite3_step(upd->get()) != SQLITE_DONE) {
        return tl::unexpected {error_code::io_failure};
    }
    return {};
}

// Update player rows via find-or-insert/repoint semantics to avoid
// mutating shared player rows used by other games.
auto game_store::patch_player(game_id const game_id,
                              std::string_view id_col,
                              player const& current,
                              std::optional<std::string> const& name_patch,
                              std::optional<std::int32_t> const& elo_patch) -> result<void>
{
    auto updated = current;
    if (name_patch) {
        updated.name = *name_patch;
    }
    if (elo_patch) {
        updated.elo = elo_patch;
    }
    auto id_res = find_or_insert_player(updated);
    if (!id_res) {
        return tl::unexpected {id_res.error()};
    }
    auto const sql = "UPDATE game SET " + std::string(id_col) + " = ? WHERE id = ?";
    auto upd = prepare(db_, sql.c_str());
    if (!upd) {
        return tl::unexpected {upd.error()};
    }
    sqlite3_bind_int64(upd->get(), 1, *id_res);
    sqlite3_bind_int64(upd->get(), 2, static_cast<std::int64_t>(game_id.value));
    if (sqlite3_step(upd->get()) != SQLITE_DONE) {
        return tl::unexpected {error_code::io_failure};
    }
    return {};
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
auto game_store::patch_event(game_id const game_id,
                             game const& current,
                             std::optional<std::string> const& event_patch,
                             std::optional<std::string> const& site_patch) -> result<void>
{
    auto const& existing = current.event_details;
    auto const event_name = event_patch.value_or(existing ? existing->name : std::string {});
    if (event_name.empty()) {
        return {};
    }
    std::optional<std::string> const inherited_site = existing ? existing->site : std::nullopt;
    std::optional<std::string> const site = site_patch.has_value() ? site_patch : inherited_site;
    std::optional<std::string> const date = existing ? existing->date : std::nullopt;
    auto id_res = find_or_insert_event({.name = event_name, .site = site, .date = date});
    if (!id_res) {
        return tl::unexpected {id_res.error()};
    }
    auto upd = prepare(db_, "UPDATE game SET event_id = ? WHERE id = ?");
    if (!upd) {
        return tl::unexpected {upd.error()};
    }
    sqlite3_bind_int64(upd->get(), 1, *id_res);
    sqlite3_bind_int64(upd->get(), 2, static_cast<std::int64_t>(game_id.value));
    if (sqlite3_step(upd->get()) != SQLITE_DONE) {
        return tl::unexpected {error_code::io_failure};
    }
    return {};
}

// NOLINTEND(bugprone-easily-swappable-parameters)

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto game_store::patch_metadata(game_id const game_id, game_patch const& patch) -> result<void>
{
    auto prov_res = get_provenance(game_id);
    if (!prov_res) {
        return tl::unexpected {prov_res.error()};
    }
    if (prov_res->source_type != "manual") {
        return tl::unexpected {error_code::not_editable};
    }

    txn_guard txn {db_};
    if (!txn.began()) {
        return tl::unexpected {error_code::io_failure};
    }

    if (patch.white_name || patch.white_elo || patch.black_name || patch.black_elo || patch.event || patch.site) {
        auto cur = get(game_id);
        if (!cur) {
            return tl::unexpected {cur.error()};
        }
        if (patch.white_name || patch.white_elo) {
            if (auto res = patch_player(game_id, "white_id", cur->white, patch.white_name, patch.white_elo); !res) {
                return res;
            }
        }
        if (patch.black_name || patch.black_elo) {
            if (auto res = patch_player(game_id, "black_id", cur->black, patch.black_name, patch.black_elo); !res) {
                return res;
            }
        }
        if (patch.event || patch.site) {
            if (auto res = patch_event(game_id, *cur, patch.event, patch.site); !res) {
                return res;
            }
        }
    }

    using field_ptr = std::optional<std::string> game_patch::*;
    static constexpr std::array<std::pair<std::string_view, field_ptr>, 5> scalar_fields = {{
        {"date", &game_patch::date},
        {"result", &game_patch::result},
        {"eco", &game_patch::eco},
        {"source_label", &game_patch::source_label},
        {"review_status", &game_patch::review_status},
    }};
    for (auto const& [col, field] : scalar_fields) {
        if (auto const& val = patch.*field; val) {
            if (auto res = update_text_field(game_id, col, *val); !res) {
                return res;
            }
        }
    }

    if (patch.notes) {
        // notes is stored as a reserved game tag with key "_notes"; replace if present
        auto del = prepare(db_, R"sql(DELETE FROM game_tag WHERE game_id = ? AND tag_id = (SELECT id FROM tag WHERE name = '_notes'))sql");
        if (!del) {
            return tl::unexpected {del.error()};
        }
        sqlite3_bind_int64(del->get(), 1, static_cast<std::int64_t>(game_id.value));
        if (sqlite3_step(del->get()) != SQLITE_DONE) {
            return tl::unexpected {error_code::io_failure};
        }
        if (!patch.notes->empty()) {
            if (auto res = insert_game_tags(game_id, {{"_notes", *patch.notes}}); !res) {
                return res;
            }
        }
    }

    if (!txn.commit()) {
        return tl::unexpected {error_code::io_failure};
    }
    return {};
}

auto game_store::remove_user_game(game_id const game_id) -> result<void>
{
    auto prov_res = get_provenance(game_id);
    if (!prov_res) {
        return tl::unexpected {prov_res.error()};
    }
    if (prov_res->source_type != "manual") {
        return tl::unexpected {error_code::not_editable};
    }
    return remove(game_id);
}

auto game_store::set_manual_provenance(game_id const game_id,
                                       std::optional<std::string> const& source_label,
                                       std::string const& review_status) -> result<void>
{
    // language=sql
    static constexpr char const* sql = R"sql(
        UPDATE game
        SET source_type = 'manual', source_label = ?, review_status = ?
        WHERE id = ?
    )sql";

    auto stmt = prepare(db_, sql);
    if (!stmt) {
        return tl::unexpected {stmt.error()};
    }

    bind_optional_text(stmt->get(), 1, source_label);
    sqlite3_bind_text(stmt->get(), 2, review_status.c_str(), static_cast<int>(review_status.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt->get(), 3, static_cast<std::int64_t>(game_id.value));

    if (sqlite3_step(stmt->get()) != SQLITE_DONE) {
        return tl::unexpected {error_code::io_failure};
    }
    if (sqlite3_changes(db_) == 0) {
        return tl::unexpected {error_code::not_found};
    }
    return {};
}

}  // namespace motif::db
