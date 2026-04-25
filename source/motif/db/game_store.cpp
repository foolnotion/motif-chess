#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <cstring>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "motif/db/game_store.hpp"

#include <sqlite3.h>
#include <tl/expected.hpp>

#include "motif/db/error.hpp"
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

struct stmt_deleter
{
    auto operator()(sqlite3_stmt* stmt) const noexcept -> void { sqlite3_finalize(stmt); }
};

using unique_stmt = std::unique_ptr<sqlite3_stmt, stmt_deleter>;

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

// Wraps BEGIN/COMMIT/ROLLBACK so early returns automatically issue ROLLBACK.
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

// Bind parameter positions for:
//   INSERT INTO game(white_id, black_id, event_id, date, result, eco, moves)
namespace game_ins_param
{
constexpr int white_id = 1;
constexpr int black_id = 2;
constexpr int event_id = 3;
constexpr int date = 4;
constexpr int result = 5;
constexpr int eco = 6;
constexpr int moves = 7;
}  // namespace game_ins_param

// Column positions in the JOIN query used by get():
//   w.name, w.elo, w.title, w.country,   (0-3)
//   b.name, b.elo, b.title, b.country,   (4-7)
//   e.name, e.site, e.date,              (8-10)
//   g.date, g.result, g.eco, g.moves     (11-14)
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
}  // namespace list_col

namespace list_param
{
constexpr int player_is_null = 1;
constexpr int white_player = 2;
constexpr int black_player = 3;
constexpr int result_is_null = 4;
constexpr int result = 5;
constexpr int limit = 6;
constexpr int offset = 7;
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

auto bind_optional_text(  // NOLINT(llvm-prefer-static-over-anonymous-namespace)
    sqlite3_stmt* stmt,
    int col,
    std::optional<std::string> const& val) -> void
{
    if (val) {
        sqlite3_bind_text(stmt, col, val->c_str(), static_cast<int>(val->size()), SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, col);
    }
}

auto bind_optional_int(  // NOLINT(llvm-prefer-static-over-anonymous-namespace)
    sqlite3_stmt* stmt,
    int col,
    std::optional<std::int32_t> const& val) -> void
{
    if (val) {
        sqlite3_bind_int(stmt, col, *val);
    } else {
        sqlite3_bind_null(stmt, col);
    }
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
{
}

game_store::~game_store() noexcept
{
    finalize_stmt(select_player_stmt_);
    finalize_stmt(insert_player_stmt_);
    finalize_stmt(select_event_stmt_);
    finalize_stmt(insert_event_stmt_);
    finalize_stmt(insert_game_stmt_);
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
    , insert_game_stmt_ {std::exchange(other.insert_game_stmt_, nullptr)}
    , select_tag_stmt_ {std::exchange(other.select_tag_stmt_, nullptr)}
    , insert_tag_stmt_ {std::exchange(other.insert_tag_stmt_, nullptr)}
    , insert_game_tag_stmt_ {std::exchange(other.insert_game_tag_stmt_, nullptr)}
{
}

auto game_store::operator=(game_store&& other) noexcept -> game_store&
{
    if (this != &other) {
        finalize_stmt(select_player_stmt_);
        finalize_stmt(insert_player_stmt_);
        finalize_stmt(select_event_stmt_);
        finalize_stmt(insert_event_stmt_);
        finalize_stmt(insert_game_stmt_);
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
        insert_game_stmt_ = std::exchange(other.insert_game_stmt_, nullptr);
        select_tag_stmt_ = std::exchange(other.select_tag_stmt_, nullptr);
        insert_tag_stmt_ = std::exchange(other.insert_tag_stmt_, nullptr);
        insert_game_tag_stmt_ = std::exchange(other.insert_game_tag_stmt_, nullptr);
    }
    return *this;
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

auto game_store::begin_transaction() -> result<void>
{
    if (sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return tl::unexpected {error_code::io_failure};
    }
    return {};
}

auto game_store::commit_transaction() -> result<void>
{
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return tl::unexpected {error_code::io_failure};
    }
    return {};
}

auto game_store::rollback_transaction() noexcept -> void
{
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
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
            id       INTEGER PRIMARY KEY,
            white_id INTEGER NOT NULL REFERENCES player(id),
            black_id INTEGER NOT NULL REFERENCES player(id),
            event_id INTEGER REFERENCES event(id),
            date     TEXT,
            result   TEXT NOT NULL,
            eco      TEXT,
            moves    BLOB NOT NULL
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

auto game_store::insert_game_tags(std::uint32_t game_id, std::vector<std::pair<std::string, std::string>> const& extra_tags) -> result<void>
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
        sqlite3_bind_int64(*game_tag_ins, 1, static_cast<std::int64_t>(game_id));
        sqlite3_bind_int64(*game_tag_ins, 2, tag_id);
        sqlite3_bind_text(*game_tag_ins, 3, tag_value.c_str(), static_cast<int>(tag_value.size()), SQLITE_TRANSIENT);
        if (sqlite3_step(*game_tag_ins) != SQLITE_DONE) {
            return tl::unexpected {error_code::io_failure};
        }
    }

    return {};
}

auto game_store::insert(game const& src_game) -> result<std::uint32_t>
{
    txn_guard txn {db_};
    if (!txn.began()) {
        return tl::unexpected {error_code::io_failure};
    }
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

    auto game_ins = prepare_cached_stmt(
        insert_game_stmt_,
        "INSERT INTO game(white_id, black_id, event_id, date, result, eco, "
        "moves) " "VALUES(?, ?, ?, ?, ?, ?, ?)");
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

    int const ins_rc = sqlite3_step(*game_ins);
    if (ins_rc == SQLITE_CONSTRAINT) {
        return tl::unexpected {error_code::duplicate};
    }
    if (ins_rc != SQLITE_DONE) {
        return tl::unexpected {error_code::io_failure};
    }

    auto const new_game_id = static_cast<std::uint32_t>(sqlite3_last_insert_rowid(db_));

    auto tags_rc = insert_game_tags(new_game_id, src_game.extra_tags);
    if (!tags_rc) {
        return tl::unexpected {tags_rc.error()};
    }

    if (!txn.commit()) {
        return tl::unexpected {error_code::io_failure};
    }
    return new_game_id;
}

auto game_store::get(std::uint32_t const game_id) -> result<game>
{
    return const_cast<game_store const&>(*this).get(game_id);
}

auto game_store::get(std::uint32_t const game_id) const -> result<game>
{
    // language=sql
    static constexpr char const* sql = R"sql(
        SELECT
            w.name, w.elo, w.title, w.country,
            b.name, b.elo, b.title, b.country,
            e.name, e.site, e.date,
            g.date, g.result, g.eco, g.moves
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
    sqlite3_bind_int64(stmt->get(), 1, static_cast<std::int64_t>(game_id));

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
    sqlite3_bind_int64(tags_stmt->get(), 1, static_cast<std::int64_t>(game_id));
    int tag_rc = SQLITE_ROW;
    while ((tag_rc = sqlite3_step(tags_stmt->get())) == SQLITE_ROW) {
        out.extra_tags.emplace_back(std::string {column_text(tags_stmt->get(), 0)}, std::string {column_text(tags_stmt->get(), 1)});
    }
    if (tag_rc != SQLITE_DONE) {
        return tl::unexpected {error_code::io_failure};
    }

    return out;
}

auto game_store::get_opening_context(std::uint32_t const game_id) -> result<opening_context>
{
    return const_cast<game_store const&>(*this).get_opening_context(game_id);
}

auto game_store::get_opening_context(std::uint32_t const game_id) const -> result<opening_context>
{
    // language=sql
    static constexpr char const* sql = R"sql(
        SELECT
            w.elo,
            b.elo,
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
        JOIN player w ON w.id = g.white_id
        JOIN player b ON b.id = g.black_id
        WHERE g.id = ?
    )sql";

    auto stmt = prepare(db_, sql);
    if (!stmt) {
        return tl::unexpected {stmt.error()};
    }
    sqlite3_bind_int64(stmt->get(), 1, static_cast<std::int64_t>(game_id));

    int const step_rc = sqlite3_step(stmt->get());
    if (step_rc == SQLITE_DONE) {
        return tl::unexpected {error_code::not_found};
    }
    if (step_rc != SQLITE_ROW) {
        return tl::unexpected {error_code::io_failure};
    }

    auto out = opening_context {};
    out.white_elo = column_optional_int32(stmt->get(), 0);
    out.black_elo = column_optional_int32(stmt->get(), 1);
    out.eco = column_optional_text(stmt->get(), 2);
    out.opening_name = column_optional_text(stmt->get(), 3);

    auto const* blob = static_cast<std::uint8_t const*>(sqlite3_column_blob(stmt->get(), 4));
    int const blob_bytes = sqlite3_column_bytes(stmt->get(), 4);
    if (blob_bytes % static_cast<int>(sizeof(std::uint16_t)) != 0) {
        return tl::unexpected {error_code::io_failure};
    }

    auto const move_count = static_cast<std::size_t>(blob_bytes) / sizeof(std::uint16_t);
    out.moves.resize(move_count);
    if (move_count > 0 && blob != nullptr) {
        std::memcpy(out.moves.data(), blob, static_cast<std::size_t>(blob_bytes));
    }

    return out;
}

auto game_store::get_game_contexts(std::vector<std::uint32_t> const& game_ids) -> result<std::unordered_map<std::uint32_t, game_context>>
{
    return const_cast<game_store const&>(*this).get_game_contexts(game_ids);
}

auto game_store::get_game_contexts(std::vector<std::uint32_t> const& game_ids) const
    -> result<std::unordered_map<std::uint32_t, game_context>>
{
    auto out = std::unordered_map<std::uint32_t, game_context> {};
    if (game_ids.empty()) {
        return out;
    }

    out.reserve(game_ids.size());

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

    for (std::size_t index = 0; index < game_ids.size(); ++index) {
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

    for (std::size_t index = 0; index < game_ids.size(); ++index) {
        sqlite3_bind_int64(stmt->get(), static_cast<int>(index + 1U), static_cast<std::int64_t>(game_ids[index]));
    }

    int step_rc = SQLITE_ROW;
    while ((step_rc = sqlite3_step(stmt->get())) == SQLITE_ROW) {
        auto const game_id = static_cast<std::uint32_t>(sqlite3_column_int64(stmt->get(), 0));
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
            COALESCE(g.eco, '')
        FROM game g
        JOIN player w ON w.id = g.white_id
        JOIN player b ON b.id = g.black_id
        LEFT JOIN event e ON e.id = g.event_id
        WHERE (? IS NULL OR instr(lower(w.name), lower(?)) > 0 OR instr(lower(b.name), lower(?)) > 0)
          AND (? IS NULL OR g.result = ?)
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
    if (sqlite3_bind_int64(stmt->get(), list_param::limit, static_cast<sqlite3_int64>(query.limit)) != SQLITE_OK
        || sqlite3_bind_int64(stmt->get(), list_param::offset, static_cast<sqlite3_int64>(query.offset)) != SQLITE_OK) {
        return tl::unexpected {error_code::io_failure};
    }

    auto entries = std::vector<game_list_entry> {};
    entries.reserve(std::min(query.limit, std::size_t {256}));

    int step_rc = SQLITE_ROW;
    while ((step_rc = sqlite3_step(stmt->get())) == SQLITE_ROW) {
        entries.push_back(game_list_entry {
            .id = static_cast<std::uint32_t>(sqlite3_column_int64(stmt->get(), list_col::game_id)),
            .white = column_text(stmt->get(), list_col::white),
            .black = column_text(stmt->get(), list_col::black),
            .result = column_text(stmt->get(), list_col::result),
            .event = column_text(stmt->get(), list_col::event),
            .date = column_text(stmt->get(), list_col::date),
            .eco = column_text(stmt->get(), list_col::eco),
        });
    }

    if (step_rc != SQLITE_DONE) {
        return tl::unexpected {error_code::io_failure};
    }

    return entries;
}

auto game_store::get_continuation_contexts(std::vector<opening_move_stat> const& move_stats)
    -> result<std::vector<game_continuation_context>>
{
    return const_cast<game_store const&>(*this).get_continuation_contexts(move_stats);
}

auto game_store::get_continuation_contexts(std::vector<opening_move_stat> const& move_stats) const
    -> result<std::vector<game_continuation_context>>
{
    auto out = std::vector<game_continuation_context> {};
    if (move_stats.empty()) {
        return out;
    }

    constexpr std::size_t max_lookup_batch {300};
    constexpr int row_index_col {0};
    constexpr int game_id_col {1};
    constexpr int ply_col {2};
    constexpr int eco_col {3};
    constexpr int opening_name_col {4};
    constexpr int encoded_move_col {5};
    out.reserve(move_stats.size());

    for (std::size_t batch_begin = 0; batch_begin < move_stats.size(); batch_begin += max_lookup_batch) {
        auto const batch_end = std::min(batch_begin + max_lookup_batch, move_stats.size());

        auto sql = std::ostringstream {};
        sql << R"sql(
            WITH lookup(row_index, game_id, ply) AS (VALUES
        )sql";
        for (std::size_t index = batch_begin; index < batch_end; ++index) {
            if (index > batch_begin) {
                sql << ',';
            }
            sql << "(?, ?, ?)";
        }
        sql << R"sql(
            )
            SELECT
                lookup.row_index,
                lookup.game_id,
                lookup.ply,
                g.eco,
                (
                    SELECT gt.value
                    FROM game_tag gt
                    JOIN tag t ON t.id = gt.tag_id
                    WHERE gt.game_id = g.id AND t.name = 'Opening'
                    ORDER BY gt.rowid
                    LIMIT 1
                ) AS opening_name,
                substr(g.moves, (CAST(lookup.ply AS INTEGER) * 2) + 1, 2) AS encoded_move
            FROM lookup
            JOIN game g ON g.id = lookup.game_id
        )sql";

        auto stmt = prepare(db_, sql.str().c_str());
        if (!stmt) {
            return tl::unexpected {stmt.error()};
        }

        int bind_index = 1;
        for (std::size_t index = batch_begin; index < batch_end; ++index) {
            auto const& move_stat = move_stats[index];
            sqlite3_bind_int64(stmt->get(), bind_index++, static_cast<std::int64_t>(index));
            sqlite3_bind_int64(stmt->get(), bind_index++, static_cast<std::int64_t>(move_stat.game_id));
            sqlite3_bind_int64(stmt->get(), bind_index++, static_cast<std::int64_t>(move_stat.ply));
        }

        int step_rc = SQLITE_ROW;
        while ((step_rc = sqlite3_step(stmt->get())) == SQLITE_ROW) {
            auto const row_index = static_cast<std::size_t>(sqlite3_column_int64(stmt->get(), row_index_col));
            if (row_index >= move_stats.size()) {
                return tl::unexpected {error_code::io_failure};
            }

            auto const* blob = static_cast<std::uint8_t const*>(sqlite3_column_blob(stmt->get(), encoded_move_col));
            int const blob_bytes = sqlite3_column_bytes(stmt->get(), encoded_move_col);
            if (blob_bytes == 0) {
                continue;
            }
            if (std::cmp_not_equal(blob_bytes, sizeof(std::uint16_t)) || blob == nullptr) {
                return tl::unexpected {error_code::io_failure};
            }

            std::uint16_t encoded_move {};
            std::memcpy(&encoded_move, blob, sizeof(encoded_move));

            auto const& move_stat = move_stats[row_index];
            out.push_back(game_continuation_context {
                .game_id = static_cast<std::uint32_t>(sqlite3_column_int64(stmt->get(), game_id_col)),
                .ply = static_cast<std::uint16_t>(sqlite3_column_int(stmt->get(), ply_col)),
                .encoded_move = encoded_move,
                .result = move_stat.result,
                .white_elo = move_stat.white_elo,
                .black_elo = move_stat.black_elo,
                .eco = column_optional_text(stmt->get(), eco_col),
                .opening_name = column_optional_text(stmt->get(), opening_name_col),
            });
        }

        if (step_rc != SQLITE_DONE) {
            return tl::unexpected {error_code::io_failure};
        }
    }

    return out;
}

auto game_store::remove(std::uint32_t game_id) -> result<void>
{
    // ON DELETE CASCADE in game_tag handles tag row cleanup automatically.
    auto stmt = prepare(db_, "DELETE FROM game WHERE id = ?");
    if (!stmt) {
        return tl::unexpected {stmt.error()};
    }
    sqlite3_bind_int64(stmt->get(), 1, static_cast<std::int64_t>(game_id));

    if (sqlite3_step(stmt->get()) != SQLITE_DONE) {
        return tl::unexpected {error_code::io_failure};
    }
    if (sqlite3_changes(db_) == 0) {
        return tl::unexpected {error_code::not_found};
    }
    return {};
}

}  // namespace motif::db
