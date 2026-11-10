#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
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
#include "motif/db/published_snapshot.hpp"
#include "motif/db/schema.hpp"
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
using detail::mutation_scope;
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
//   w.name, g.white_elo, w.title, w.country,       (0-3)
//   b.name, g.black_elo, b.title, b.country,       (4-7)
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

namespace find_filter_param
{
constexpr int player_name_is_null = 1;
constexpr int player_color = 2;
constexpr int white_player_name = 3;
constexpr int black_player_name = 4;
constexpr int white_player_name_fallback = 5;
constexpr int black_player_name_fallback = 6;
constexpr int result_is_null = 7;
constexpr int result = 8;
constexpr int eco_is_null = 9;
constexpr int eco_prefix = 10;
constexpr int min_elo_is_null = 11;
constexpr int min_elo_white = 12;
constexpr int min_elo_black = 13;
constexpr int max_elo_is_null = 14;
constexpr int max_elo_white = 15;
constexpr int max_elo_black = 16;
constexpr int limit = 17;
constexpr int offset = 18;
}  // namespace find_filter_param

constexpr std::size_t max_game_list_reserve {501};

auto bind_find_filter(sqlite3_stmt* stmt, search_filter const& filter) -> result<void>
{
    // Checked wrappers — bind_optional_text/int discard the SQLite return code
    // (they are used as void helpers elsewhere), so we inline checked calls here.
    auto const bind_text = [stmt](int col, std::optional<std::string> const& val) -> int
    {
        if (val) {
            return sqlite3_bind_text(stmt, col, val->c_str(), static_cast<int>(val->size()), SQLITE_TRANSIENT);
        }
        return sqlite3_bind_null(stmt, col);
    };
    auto const bind_int32 = [stmt](int col, std::optional<std::int32_t> const& val) -> int
    {
        if (val) {
            return sqlite3_bind_int(stmt, col, *val);
        }
        return sqlite3_bind_null(stmt, col);
    };

    if (bind_text(find_filter_param::player_name_is_null, filter.player_name) != SQLITE_OK
        || sqlite3_bind_int(stmt, find_filter_param::player_color, static_cast<int>(filter.player_color)) != SQLITE_OK
        || bind_text(find_filter_param::white_player_name, filter.player_name) != SQLITE_OK
        || bind_text(find_filter_param::black_player_name, filter.player_name) != SQLITE_OK
        || bind_text(find_filter_param::white_player_name_fallback, filter.player_name) != SQLITE_OK
        || bind_text(find_filter_param::black_player_name_fallback, filter.player_name) != SQLITE_OK
        || bind_text(find_filter_param::result_is_null, filter.result) != SQLITE_OK
        || bind_text(find_filter_param::result, filter.result) != SQLITE_OK
        || bind_text(find_filter_param::eco_is_null, filter.eco_prefix) != SQLITE_OK
        || bind_text(find_filter_param::eco_prefix, filter.eco_prefix) != SQLITE_OK
        || bind_int32(find_filter_param::min_elo_is_null, filter.min_elo) != SQLITE_OK
        || bind_int32(find_filter_param::min_elo_white, filter.min_elo) != SQLITE_OK
        || bind_int32(find_filter_param::min_elo_black, filter.min_elo) != SQLITE_OK
        || bind_int32(find_filter_param::max_elo_is_null, filter.max_elo) != SQLITE_OK
        || bind_int32(find_filter_param::max_elo_white, filter.max_elo) != SQLITE_OK
        || bind_int32(find_filter_param::max_elo_black, filter.max_elo) != SQLITE_OK)
    {
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(sqlite3_db_handle(stmt))}};
    }
    return {};
}

auto has_metadata_filter(search_filter const& filter) noexcept -> bool
{
    return filter.player_name.has_value() || filter.result.has_value() || filter.eco_prefix.has_value() || filter.min_elo.has_value()
        || filter.max_elo.has_value();
}

auto game_sort_expression(game_sort_column const column) noexcept -> std::string_view
{
    switch (column) {
        case game_sort_column::id:
            return "g.id";
        case game_sort_column::white:
            return "w.name COLLATE NOCASE";
        case game_sort_column::black:
            return "b.name COLLATE NOCASE";
        case game_sort_column::result:
            return "g.result COLLATE NOCASE";
        case game_sort_column::event:
            return "e.name COLLATE NOCASE";
        case game_sort_column::date:
            return "g.date COLLATE NOCASE";
        case game_sort_column::eco:
            return "g.eco COLLATE NOCASE";
    }
    std::unreachable();
}

auto game_order_clause(search_filter const& filter) -> std::string
{
    auto order = std::string {game_sort_expression(filter.sort_column)};
    order += filter.sort_ascending ? " ASC" : " DESC";
    if (filter.sort_column != game_sort_column::id) {
        order += filter.sort_ascending ? ", g.id ASC" : ", g.id DESC";
    }
    return order;
}

// NOLINT(llvm-prefer-static-over-anonymous-namespace): conflicts with
// misc-use-anonymous-namespace
[[nodiscard]] auto prepare(sqlite3* conn, char const* sql) -> result<unique_stmt>
{  // NOLINT(llvm-prefer-static-over-anonymous-namespace)
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(conn, sql, -1, &raw, nullptr) != SQLITE_OK) {
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(conn)}};
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

auto populate_position_temp_table(sqlite3* conn, std::vector<game_id> const& game_ids) -> result<void>
{
    if (sqlite3_exec(
            conn, "CREATE TEMP TABLE IF NOT EXISTS _position_game_ids (id INTEGER PRIMARY KEY NOT NULL)", nullptr, nullptr, nullptr)
            != SQLITE_OK
        || sqlite3_exec(conn, "DELETE FROM _position_game_ids", nullptr, nullptr, nullptr) != SQLITE_OK)
    {
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(conn)}};
    }

    auto const max_params = static_cast<std::size_t>(sqlite3_limit(conn, SQLITE_LIMIT_VARIABLE_NUMBER, -1));
    auto const batch_size = max_params > 0U ? max_params : std::size_t {999};

    for (std::size_t batch_start = 0; batch_start < game_ids.size(); batch_start += batch_size) {
        auto const batch_end = std::min(batch_start + batch_size, game_ids.size());
        auto const batch_count = batch_end - batch_start;

        auto sql = std::string {"INSERT INTO _position_game_ids (id) VALUES (?)"};
        for (std::size_t i = 1; i < batch_count; ++i) {
            sql += ",(?)";
        }

        auto stmt = prepare(conn, sql.c_str());
        if (!stmt) {
            return tl::unexpected {stmt.error()};
        }

        for (std::size_t i = 0; i < batch_count; ++i) {
            if (sqlite3_bind_int64(stmt->get(), static_cast<int>(i + 1U), static_cast<sqlite3_int64>(game_ids[batch_start + i].value))
                != SQLITE_OK)
            {
                return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(conn)}};
            }
        }

        if (sqlite3_step(stmt->get()) != SQLITE_DONE) {
            return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(conn)}};
        }
    }
    return {};
}

auto read_game_list_entry(sqlite3_stmt* stmt) -> game_list_entry
{
    auto const* src_type_raw = column_text(stmt, list_col::source_type);
    auto const* rev_status_raw = column_text(stmt, list_col::review_status);
    return game_list_entry {
        .id = motif::db::game_id {static_cast<std::uint32_t>(sqlite3_column_int64(stmt, list_col::game_id))},
        .white = column_text(stmt, list_col::white),
        .black = column_text(stmt, list_col::black),
        .white_elo = column_optional_int32(stmt, list_col::white_elo),
        .black_elo = column_optional_int32(stmt, list_col::black_elo),
        .result = column_text(stmt, list_col::result),
        .event = column_text(stmt, list_col::event),
        .date = column_text(stmt, list_col::date),
        .eco = column_text(stmt, list_col::eco),
        .source_type = (src_type_raw != nullptr) ? std::string {src_type_raw} : "imported",
        .source_label = column_optional_text(stmt, list_col::source_label),
        .review_status = (rev_status_raw != nullptr) ? std::string {rev_status_raw} : "new",
    };
}

// Shared SELECT column list, join/filter predicate, and LIMIT/OFFSET tail for
// find_games and find_games_with_ids.
// language=sql
constexpr auto game_list_select_sql = R"sql(
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
        g.white_elo,
        g.black_elo
    FROM game g
)sql";

// `extra_from_join` supplies any join clause the caller needs between
// `FROM game g` and the player/event joins below (e.g. find_games_with_ids'
// `_position_game_ids` restriction); pass "" when none is needed. The actual
// `ORDER BY` clause is appended separately via `game_order_clause(filter)`.
// language=sql
constexpr auto game_list_where_sql = R"sql(
    JOIN player w ON w.id = g.white_id
    JOIN player b ON b.id = g.black_id
    LEFT JOIN event e ON e.id = g.event_id
    WHERE (? IS NULL
           OR (CASE ?
                 WHEN 1 THEN instr(lower(w.name), lower(?)) > 0
                 WHEN 2 THEN instr(lower(b.name), lower(?)) > 0
                 ELSE instr(lower(w.name), lower(?)) > 0
                      OR instr(lower(b.name), lower(?)) > 0
                 END))
      AND (? IS NULL OR g.result = ?)
      AND (? IS NULL OR g.eco LIKE ? || '%')
      AND (? IS NULL OR (g.white_elo >= ? AND g.black_elo >= ?))
      AND (? IS NULL OR (g.white_elo <= ? AND g.black_elo <= ?))
    ORDER BY
)sql";

// language=sql
constexpr auto game_list_limit_offset_sql = R"sql(
    LIMIT ? OFFSET ?
)sql";

[[nodiscard]] auto fetch_game_list_page(sqlite3* conn,
                                        std::string_view extra_from_join,
                                        search_filter const& filter,
                                        std::size_t const effective_limit) -> result<std::vector<game_list_entry>>
{
    auto const data_sql = std::string {game_list_select_sql} + std::string {extra_from_join} + std::string {game_list_where_sql}
        + game_order_clause(filter) + std::string {game_list_limit_offset_sql};

    auto data_stmt = prepare(conn, data_sql.c_str());
    if (!data_stmt) {
        return tl::unexpected {data_stmt.error()};
    }
    if (auto bind_res = bind_find_filter(data_stmt->get(), filter); !bind_res) {
        return tl::unexpected {bind_res.error()};
    }
    if (sqlite3_bind_int64(data_stmt->get(), find_filter_param::limit, static_cast<sqlite3_int64>(effective_limit)) != SQLITE_OK
        || sqlite3_bind_int64(data_stmt->get(), find_filter_param::offset, static_cast<sqlite3_int64>(filter.offset)) != SQLITE_OK)
    {
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(conn)}};
    }

    auto entries = std::vector<game_list_entry> {};
    entries.reserve(std::min(effective_limit + 1, max_game_list_reserve));

    int step_rc {};
    while ((step_rc = sqlite3_step(data_stmt->get())) == SQLITE_ROW) {
        entries.push_back(read_game_list_entry(data_stmt->get()));
    }
    if (step_rc != SQLITE_DONE) {
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(conn)}};
    }
    return entries;
}

[[nodiscard]] auto exec(sqlite3* conn, char const* sql) -> result<void>
{  // NOLINT(llvm-prefer-static-over-anonymous-namespace)
    char* err_msg = nullptr;
    int const exec_result = sqlite3_exec(conn, sql, nullptr, nullptr, &err_msg);
    if (err_msg != nullptr) {
        sqlite3_free(err_msg);
    }
    if (exec_result != SQLITE_OK) {
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(conn)}};
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
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(conn)}};
    }
    return sqlite3_column_int(stmt->get(), 0) == 1;
}

}  // namespace

// ── game_store
// ────────────────────────────────────────────────────────────────

game_store::game_store(sqlite3* conn) noexcept
    : db_ {conn}
    , writer_ {game_writer::make(conn)}
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
    player_id_cache_.clear();
    event_id_cache_.clear();
    tag_id_cache_.clear();
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
    player_id_cache_.clear();
    event_id_cache_.clear();
    tag_id_cache_.clear();
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
        return tl::unexpected {error {error_code::io_failure, "foreign keys could not be enabled"}};
    }

    return schema::create_tables(db_);
}

auto game_store::writer() noexcept -> game_writer&
{
    return *writer_;
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
            return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(db_)}};
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
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(db_)}};
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
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(db_)}};
    }
    auto const event_id = sqlite3_last_insert_rowid(db_);
    event_id_cache_.emplace(evt.name, event_id);
    return event_id;
}

auto game_store::insert_game_tags(game_id const game_key, std::vector<std::pair<std::string, std::string>> const& extra_tags)
    -> result<void>
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
                    return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(db_)}};
                }
                tag_id = sqlite3_last_insert_rowid(db_);
            }
            tag_id_cache_.emplace(tag_name, tag_id);
        }

        auto game_tag_ins = prepare_cached_stmt(insert_game_tag_stmt_, "INSERT INTO game_tag(game_id, tag_id, value) VALUES(?, ?, ?)");
        if (!game_tag_ins) {
            return tl::unexpected {game_tag_ins.error()};
        }
        sqlite3_bind_int64(*game_tag_ins, 1, static_cast<std::int64_t>(game_key.value));
        sqlite3_bind_int64(*game_tag_ins, 2, tag_id);
        sqlite3_bind_text(*game_tag_ins, 3, tag_value.c_str(), static_cast<int>(tag_value.size()), SQLITE_TRANSIENT);
        if (sqlite3_step(*game_tag_ins) != SQLITE_DONE) {
            return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(db_)}};
        }
    }

    return {};
}

auto game_store::get(game_id const game_key) -> result<game>
{
    return const_cast<game_store const&>(*this).get(game_key);
}

auto game_store::get(game_id const game_key) const -> result<game>
{
    // language=sql
    static constexpr char const* sql = R"sql(
        SELECT
            w.name, g.white_elo, w.title, w.country,
            b.name, g.black_elo, b.title, b.country,
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
    sqlite3_bind_int64(stmt->get(), 1, static_cast<std::int64_t>(game_key.value));

    int const step_rc = sqlite3_step(stmt->get());
    if (step_rc == SQLITE_DONE) {
        return tl::unexpected {error_code::not_found};
    }
    if (step_rc != SQLITE_ROW) {
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(db_)}};
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
        return tl::unexpected {error {error_code::io_failure, "blob size not a multiple of uint16"}};
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
    sqlite3_bind_int64(tags_stmt->get(), 1, static_cast<std::int64_t>(game_key.value));
    int tag_rc = SQLITE_ROW;
    while ((tag_rc = sqlite3_step(tags_stmt->get())) == SQLITE_ROW) {
        out.extra_tags.emplace_back(std::string {column_text(tags_stmt->get(), 0)}, std::string {column_text(tags_stmt->get(), 1)});
    }
    if (tag_rc != SQLITE_DONE) {
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(db_)}};
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
                return tl::unexpected {error {error_code::io_failure, "blob size not a multiple of uint16"}};
            }

            auto const move_count = static_cast<std::size_t>(blob_bytes) / sizeof(std::uint16_t);
            context.moves.resize(move_count);
            if (move_count > 0 && blob != nullptr) {
                std::memcpy(context.moves.data(), blob, static_cast<std::size_t>(blob_bytes));
            }

            out.emplace(game_id, std::move(context));
        }

        if (step_rc != SQLITE_DONE) {
            return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(db_)}};
        }
    }

    return out;
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
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(db_)}};
    }
    return sqlite3_column_int64(stmt->get(), 0);
}

auto game_store::all_game_ids() const -> result<std::vector<game_id>>
{
    // language=sql
    static constexpr char const* sql = R"sql(SELECT id FROM game ORDER BY id)sql";
    auto stmt = prepare(db_, sql);
    if (!stmt) {
        return tl::unexpected {stmt.error()};
    }

    std::vector<game_id> ids;
    int step_rc = SQLITE_ROW;
    while ((step_rc = sqlite3_step(stmt->get())) == SQLITE_ROW) {
        ids.push_back(game_id {static_cast<std::uint32_t>(sqlite3_column_int(stmt->get(), 0))});
    }
    if (step_rc != SQLITE_DONE) {
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(db_)}};
    }

    return ids;
}

auto game_store::mutation_seq() const -> result<std::uint64_t>
{
    return read_mutation_seq(db_);
}

auto game_store::next_game_id() const -> result<std::int64_t>
{
    // language=sql
    static constexpr char const* sql = R"sql(SELECT next_game_id FROM bundle_meta WHERE id = 1)sql";
    auto stmt = prepare(db_, sql);
    if (!stmt) {
        return tl::unexpected {stmt.error()};
    }
    if (sqlite3_step(stmt->get()) != SQLITE_ROW) {
        return tl::unexpected {error {error_code::io_failure, "bundle_meta allocator row is missing"}};
    }
    return sqlite3_column_int64(stmt->get(), 0);
}

auto game_store::data_revision() const noexcept -> std::uint64_t
{
    return writer_ ? writer_->data_revision() : 0U;
}

auto game_store::has_pending_canonical_change() const noexcept -> bool
{
    return writer_ && writer_->has_pending_canonical_change();
}

auto game_store::for_each_replay_game(std::function<result<void>(replay_game_record const&)> const& visitor) const -> result<void>
{
    // language=sql
    constexpr auto replay_games_sql = R"sql(
        SELECT g.id, g.result, g.white_elo, g.black_elo, g.moves
        FROM game AS g
        ORDER BY g.id
    )sql";
    auto stmt = prepare(db_, replay_games_sql);
    if (!stmt) {
        return tl::unexpected {stmt.error()};
    }

    int step_rc = SQLITE_ROW;
    while ((step_rc = sqlite3_step(stmt->get())) == SQLITE_ROW) {
        auto const move_bytes = sqlite3_column_bytes(stmt->get(), 4);
        auto const* moves_blob = sqlite3_column_blob(stmt->get(), 4);
        auto const* result_text =
            reinterpret_cast<char const*>(sqlite3_column_text(stmt->get(), 1));  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        if (result_text == nullptr || move_bytes < 0 || move_bytes % static_cast<int>(sizeof(std::uint16_t)) != 0
            || (move_bytes > 0 && moves_blob == nullptr))
        {
            return tl::unexpected {error {error_code::io_failure, "invalid replay row"}};
        }

        replay_game_record record {
            .id = game_id {static_cast<std::uint32_t>(sqlite3_column_int64(stmt->get(), 0))},
            .result = result_text,
            .white_elo = sqlite3_column_type(stmt->get(), 2) == SQLITE_NULL
                ? std::optional<std::int32_t> {}
                : std::optional<std::int32_t> {sqlite3_column_int(stmt->get(), 2)},
            .black_elo = sqlite3_column_type(stmt->get(), 3) == SQLITE_NULL
                ? std::optional<std::int32_t> {}
                : std::optional<std::int32_t> {sqlite3_column_int(stmt->get(), 3)},
            .moves = {},
        };
        record.moves.resize(static_cast<std::size_t>(move_bytes) / sizeof(std::uint16_t));
        if (!record.moves.empty()) {
            std::memcpy(record.moves.data(), moves_blob, static_cast<std::size_t>(move_bytes));
        }
        if (auto visit_res = visitor(record); !visit_res) {
            return visit_res;
        }
    }
    if (step_rc != SQLITE_DONE) {
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(db_)}};
    }
    return {};
}

auto game_store::for_each_metadata_row(std::function<result<void>(canonical_metadata_row const&)> const& visitor) const -> result<void>
{
    // language=sql
    constexpr auto metadata_rows_sql = R"sql(
        SELECT g.id, g.content_revision, g.result, g.white_elo, g.black_elo
        FROM game AS g
        ORDER BY g.id
    )sql";
    auto stmt = prepare(db_, metadata_rows_sql);
    if (!stmt) {
        return tl::unexpected {stmt.error()};
    }
    int step_rc = SQLITE_ROW;
    while ((step_rc = sqlite3_step(stmt->get())) == SQLITE_ROW) {
        auto const* result_text =
            reinterpret_cast<char const*>(sqlite3_column_text(stmt->get(), 2));  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        if (result_text == nullptr || sqlite3_column_int64(stmt->get(), 0) <= 0 || sqlite3_column_int64(stmt->get(), 1) <= 0) {
            return tl::unexpected {error {error_code::io_failure, "invalid metadata row"}};
        }
        canonical_metadata_row const row {
            .id = game_id {static_cast<std::uint32_t>(sqlite3_column_int64(stmt->get(), 0))},
            .content_revision = static_cast<std::uint64_t>(sqlite3_column_int64(stmt->get(), 1)),
            .result = result_text,
            .white_elo = sqlite3_column_type(stmt->get(), 3) == SQLITE_NULL
                ? std::optional<std::int32_t> {}
                : std::optional<std::int32_t> {sqlite3_column_int(stmt->get(), 3)},
            .black_elo = sqlite3_column_type(stmt->get(), 4) == SQLITE_NULL
                ? std::optional<std::int32_t> {}
                : std::optional<std::int32_t> {sqlite3_column_int(stmt->get(), 4)},
        };
        if (auto visit_res = visitor(row); !visit_res) {
            return visit_res;
        }
    }
    if (step_rc != SQLITE_DONE) {
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(db_)}};
    }
    return {};
}

auto game_store::find_games(search_filter const& filter) const -> result<game_list_result>
{
    auto const effective_limit = std::min(filter.limit, max_search_limit);
    constexpr auto sqlite_max = static_cast<std::size_t>(std::numeric_limits<sqlite3_int64>::max());
    if (effective_limit > sqlite_max || filter.offset > sqlite_max) {
        return tl::unexpected {error {error_code::io_failure, "limit or offset exceeds sqlite3_int64 max"}};
    }

    // Wrap COUNT + SELECT in a single read transaction so total_count is
    // consistent with the rows returned even under concurrent writes.
    txn_guard txn {db_};
    if (!txn.began()) {
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(db_)}};
    }

    auto total_count = std::int64_t {};
    if (!has_metadata_filter(filter)) {
        auto count = count_games();
        if (!count) {
            return tl::unexpected {count.error()};
        }
        total_count = *count;
    } else {
        // language=sql
        constexpr auto count_sql = R"sql(
            SELECT COUNT(*)
            FROM game g
            JOIN player w ON w.id = g.white_id
            JOIN player b ON b.id = g.black_id
            LEFT JOIN event e ON e.id = g.event_id
            WHERE (? IS NULL
                   OR (CASE ?
                         WHEN 1 THEN instr(lower(w.name), lower(?)) > 0
                         WHEN 2 THEN instr(lower(b.name), lower(?)) > 0
                         ELSE instr(lower(w.name), lower(?)) > 0
                              OR instr(lower(b.name), lower(?)) > 0
                         END))
              AND (? IS NULL OR g.result = ?)
              AND (? IS NULL OR g.eco LIKE ? || '%')
              AND (? IS NULL OR (g.white_elo >= ? AND g.black_elo >= ?))
              AND (? IS NULL OR (g.white_elo <= ? AND g.black_elo <= ?))
        )sql";

        auto count_stmt = prepare(db_, count_sql);
        if (!count_stmt) {
            return tl::unexpected {count_stmt.error()};
        }
        if (auto bind_res = bind_find_filter(count_stmt->get(), filter); !bind_res) {
            return tl::unexpected {bind_res.error()};
        }

        auto const count_step_rc = sqlite3_step(count_stmt->get());
        if (count_step_rc != SQLITE_ROW) {
            return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(db_)}};
        }
        total_count = sqlite3_column_int64(count_stmt->get(), 0);
    }

    if (total_count == 0 || effective_limit == 0) {
        return game_list_result {.games = {}, .total_count = total_count};
    }

    auto entries = fetch_game_list_page(db_, "", filter, effective_limit);
    if (!entries) {
        return tl::unexpected {entries.error()};
    }

    if (!txn.commit()) {
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(db_)}};
    }
    return game_list_result {.games = std::move(*entries), .total_count = total_count};
}

auto game_store::find_games_with_ids(std::vector<game_id> const& game_ids, search_filter const& filter) const -> result<game_list_result>
{
    if (game_ids.empty()) {
        return game_list_result {.games = {}, .total_count = 0};
    }

    auto const lock = std::scoped_lock {position_game_ids_mutex_};

    auto const effective_limit = std::min(filter.limit, max_search_limit);
    constexpr auto sqlite_max = static_cast<std::size_t>(std::numeric_limits<sqlite3_int64>::max());
    if (effective_limit > sqlite_max || filter.offset > sqlite_max) {
        return tl::unexpected {error {error_code::io_failure, "limit or offset exceeds sqlite3_int64 max"}};
    }

    // Rebuild the temporary ID set and read its page atomically. Keeping the
    // batched inserts in this transaction avoids one SQLite commit per batch
    // for high-frequency positions before the read-only COUNT and page query.
    txn_guard txn {db_};
    if (!txn.began()) {
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(db_)}};
    }
    if (auto res = populate_position_temp_table(db_, game_ids); !res) {
        return tl::unexpected {res.error()};
    }

    // language=sql
    constexpr auto count_sql = R"sql(
        SELECT COUNT(*)
        FROM game g
        JOIN _position_game_ids pgi ON pgi.id = g.id
        JOIN player w ON w.id = g.white_id
        JOIN player b ON b.id = g.black_id
        LEFT JOIN event e ON e.id = g.event_id
        WHERE (? IS NULL
               OR (CASE ?
                     WHEN 1 THEN instr(lower(w.name), lower(?)) > 0
                     WHEN 2 THEN instr(lower(b.name), lower(?)) > 0
                     ELSE instr(lower(w.name), lower(?)) > 0
                          OR instr(lower(b.name), lower(?)) > 0
                     END))
          AND (? IS NULL OR g.result = ?)
          AND (? IS NULL OR g.eco LIKE ? || '%')
          AND (? IS NULL OR (g.white_elo >= ? AND g.black_elo >= ?))
          AND (? IS NULL OR (g.white_elo <= ? AND g.black_elo <= ?))
    )sql";

    auto count_stmt = prepare(db_, count_sql);
    if (!count_stmt) {
        return tl::unexpected {count_stmt.error()};
    }
    if (auto bind_res = bind_find_filter(count_stmt->get(), filter); !bind_res) {
        return tl::unexpected {bind_res.error()};
    }

    int const step_rc = sqlite3_step(count_stmt->get());
    if (step_rc != SQLITE_ROW) {
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(db_)}};
    }
    auto const total_count = sqlite3_column_int64(count_stmt->get(), 0);

    if (total_count == 0 || effective_limit == 0) {
        return game_list_result {.games = {}, .total_count = total_count};
    }

    auto entries = fetch_game_list_page(db_, "JOIN _position_game_ids pgi ON pgi.id = g.id\n", filter, effective_limit);
    if (!entries) {
        return tl::unexpected {entries.error()};
    }

    if (!txn.commit()) {
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(db_)}};
    }
    return game_list_result {.games = std::move(*entries), .total_count = total_count};
}

auto game_store::find_game_ids_with_filter(std::vector<game_id> const& game_ids, search_filter const& filter) const
    -> result<std::vector<game_id>>
{
    if (game_ids.empty()) {
        return std::vector<game_id> {};
    }

    auto const lock = std::scoped_lock {position_game_ids_mutex_};

    if (auto res = populate_position_temp_table(db_, game_ids); !res) {
        return tl::unexpected {res.error()};
    }

    // language=sql
    constexpr auto ids_sql = R"sql(
        SELECT g.id
        FROM game g
        JOIN _position_game_ids pgi ON pgi.id = g.id
        JOIN player w ON w.id = g.white_id
        JOIN player b ON b.id = g.black_id
        LEFT JOIN event e ON e.id = g.event_id
        WHERE (? IS NULL
               OR (CASE ?
                     WHEN 1 THEN instr(lower(w.name), lower(?)) > 0
                     WHEN 2 THEN instr(lower(b.name), lower(?)) > 0
                     ELSE instr(lower(w.name), lower(?)) > 0
                          OR instr(lower(b.name), lower(?)) > 0
                     END))
          AND (? IS NULL OR g.result = ?)
          AND (? IS NULL OR g.eco LIKE ? || '%')
          AND (? IS NULL OR (g.white_elo >= ? AND g.black_elo >= ?))
          AND (? IS NULL OR (g.white_elo <= ? AND g.black_elo <= ?))
        ORDER BY g.id ASC
    )sql";

    auto stmt = prepare(db_, ids_sql);
    if (!stmt) {
        return tl::unexpected {stmt.error()};
    }
    if (auto bind_res = bind_find_filter(stmt->get(), filter); !bind_res) {
        return tl::unexpected {bind_res.error()};
    }

    auto out = std::vector<game_id> {};
    int step_rc {};
    while ((step_rc = sqlite3_step(stmt->get())) == SQLITE_ROW) {
        out.push_back(game_id {static_cast<std::uint32_t>(sqlite3_column_int64(stmt->get(), 0))});
    }
    if (step_rc != SQLITE_DONE) {
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(db_)}};
    }
    return out;
}

auto game_store::remove(game_id const game_key) -> result<void>
{
    // The DELETE and the sequence advance are one atomic operation: in a
    // transaction of its own, or an operation savepoint when joining the
    // caller's (a writer batch is poisoned instead).
    mutation_scope txn {db_, writer_->owns_open_batch()};
    if (!txn.began()) {
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(db_)}};
    }
    // ON DELETE CASCADE in game_tag handles tag row cleanup automatically.
    auto stmt = prepare(db_, "DELETE FROM game WHERE id = ?");
    if (!stmt) {
        writer_->note_failed_operation();
        return tl::unexpected {stmt.error()};
    }
    sqlite3_bind_int64(stmt->get(), 1, static_cast<std::int64_t>(game_key.value));

    if (sqlite3_step(stmt->get()) != SQLITE_DONE) {
        writer_->note_failed_operation();
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(db_)}};
    }
    // Read before the sequence UPDATE overwrites the per-connection change count.
    // sqlite3_changes() excludes ON DELETE CASCADE row changes, so this is the
    // number of game rows removed.
    if (sqlite3_changes(db_) == 0) {
        return tl::unexpected {error_code::not_found};
    }
    if (auto record_res = writer_->record_canonical_change(); !record_res) {
        writer_->note_failed_operation();
        return tl::unexpected {record_res.error()};
    }
    if (!txn.commit()) {
        writer_->note_failed_operation();
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(db_)}};
    }
    writer_->note_data_changed();
    return {};
}

auto game_store::get_provenance(game_id const game_key) const -> result<game_provenance>
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
    sqlite3_bind_int64(stmt->get(), 1, static_cast<std::int64_t>(game_key.value));

    int const step_rc = sqlite3_step(stmt->get());
    if (step_rc == SQLITE_DONE) {
        return tl::unexpected {error_code::not_found};
    }
    if (step_rc != SQLITE_ROW) {
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(db_)}};
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

// ── Metadata patching ────────────────────────────────────────────────────────
//
// Elo is stored per game (game.white_elo/black_elo), not on the shared player
// row, so a rating edit is an UPDATE of this game's own row and can never touch
// another game that happens to name the same player. A name edit repoints
// white_id/black_id to a find-or-inserted player row (player.name is UNIQUE, so
// a renamed player reuses an existing row or gets a fresh one).

struct game_store::identity_row
{
    std::int64_t white_id {};
    std::int64_t black_id {};
    std::optional<std::int64_t> event_id;
    std::optional<std::string> date;
    std::string result;

    // The unique index compares COALESCE(event_id, -1) and COALESCE(date, '').
    [[nodiscard]] auto same_identity(identity_row const& other) const -> bool
    {
        return white_id == other.white_id && black_id == other.black_id && event_id.value_or(-1) == other.event_id.value_or(-1)
            && date.value_or("") == other.date.value_or("") && result == other.result;
    }
};

namespace
{

auto is_valid_game_result(std::string_view const result) noexcept -> bool
{
    return result == "1-0" || result == "0-1" || result == "1/2-1/2" || result == "*";
}

auto rating_in_domain(rating_patch const& patch) noexcept -> bool
{
    return patch.kind != rating_patch::action::set
        || (patch.value >= std::numeric_limits<std::int16_t>::min() && patch.value <= std::numeric_limits<std::int16_t>::max());
}

auto step_error(sqlite3* const connection) -> tl::unexpected<error>
{
    return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(connection)}};
}

// language=sql
constexpr auto identity_duplicate_sql = R"sql(
    SELECT 1 FROM game o
    WHERE o.id <> ?1 AND o.white_id = ?2 AND o.black_id = ?3
      AND COALESCE(o.event_id, -1) = COALESCE(?4, -1)
      AND COALESCE(o.date, '') = COALESCE(?5, '')
      AND o.result = ?6
      AND o.move_hash = (SELECT move_hash FROM game WHERE id = ?1)
      AND o.moves = (SELECT moves FROM game WHERE id = ?1)
    LIMIT 1
)sql";

// language=sql
constexpr auto identity_next_slot_sql = R"sql(
    SELECT COALESCE(MAX(o.identity_collision) + 1, 0) FROM game o
    WHERE o.id <> ?1 AND o.white_id = ?2 AND o.black_id = ?3
      AND COALESCE(o.event_id, -1) = COALESCE(?4, -1)
      AND COALESCE(o.date, '') = COALESCE(?5, '')
      AND o.result = ?6
      AND o.move_hash = (SELECT move_hash FROM game WHERE id = ?1)
)sql";

}  // namespace

auto game_store::read_identity_row(game_id const game_key) const -> result<identity_row>
{
    auto stmt = prepare(db_, "SELECT white_id, black_id, event_id, date, result FROM game WHERE id = ?");
    if (!stmt) {
        return tl::unexpected {stmt.error()};
    }
    sqlite3_bind_int64(stmt->get(), 1, static_cast<std::int64_t>(game_key.value));
    auto const step_rc = sqlite3_step(stmt->get());
    if (step_rc == SQLITE_DONE) {
        return tl::unexpected {error_code::not_found};
    }
    if (step_rc != SQLITE_ROW) {
        return step_error(db_);
    }
    auto row = identity_row {};
    row.white_id = sqlite3_column_int64(stmt->get(), 0);
    row.black_id = sqlite3_column_int64(stmt->get(), 1);
    if (sqlite3_column_type(stmt->get(), 2) != SQLITE_NULL) {
        row.event_id = sqlite3_column_int64(stmt->get(), 2);
    }
    row.date = column_optional_text(stmt->get(), 3);
    row.result = column_text(stmt->get(), 4);
    return row;
}

auto game_store::read_indexed_metadata(game_id const game_key) const -> result<std::pair<indexed_game_metadata, std::uint64_t>>
{
    auto stmt = prepare(db_, "SELECT result, white_elo, black_elo, content_revision FROM game WHERE id = ?");
    if (!stmt) {
        return tl::unexpected {stmt.error()};
    }
    sqlite3_bind_int64(stmt->get(), 1, static_cast<std::int64_t>(game_key.value));
    auto const step_rc = sqlite3_step(stmt->get());
    if (step_rc == SQLITE_DONE) {
        return tl::unexpected {error_code::not_found};
    }
    if (step_rc != SQLITE_ROW) {
        return step_error(db_);
    }
    auto const* result_text = column_text(stmt->get(), 0);
    if (result_text == nullptr || sqlite3_column_int64(stmt->get(), 3) <= 0) {
        return tl::unexpected {error {error_code::io_failure, "invalid game metadata row"}};
    }
    return std::pair {
        indexed_game_metadata {
            .result = result_text, .white_elo = column_optional_int32(stmt->get(), 1), .black_elo = column_optional_int32(stmt->get(), 2)},
        static_cast<std::uint64_t>(sqlite3_column_int64(stmt->get(), 3))};
}

auto game_store::get_replay_record(game_id const game_key) const -> result<replay_game_record>
{
    auto stmt = prepare(db_, "SELECT result, white_elo, black_elo, moves FROM game WHERE id = ?");
    if (!stmt) {
        return tl::unexpected {stmt.error()};
    }
    sqlite3_bind_int64(stmt->get(), 1, static_cast<std::int64_t>(game_key.value));
    auto const step_rc = sqlite3_step(stmt->get());
    if (step_rc == SQLITE_DONE) {
        return tl::unexpected {error_code::not_found};
    }
    if (step_rc != SQLITE_ROW) {
        return step_error(db_);
    }
    auto const move_bytes = sqlite3_column_bytes(stmt->get(), 3);
    auto const* moves_blob = sqlite3_column_blob(stmt->get(), 3);
    auto const* result_text = column_text(stmt->get(), 0);
    if (result_text == nullptr || move_bytes < 0 || move_bytes % static_cast<int>(sizeof(std::uint16_t)) != 0
        || (move_bytes > 0 && moves_blob == nullptr))
    {
        return tl::unexpected {error {error_code::io_failure, "invalid replay row"}};
    }
    auto record = replay_game_record {.id = game_key,
                                      .result = result_text,
                                      .white_elo = column_optional_int32(stmt->get(), 1),
                                      .black_elo = column_optional_int32(stmt->get(), 2),
                                      .moves = {}};
    record.moves.resize(static_cast<std::size_t>(move_bytes) / sizeof(std::uint16_t));
    if (!record.moves.empty()) {
        std::memcpy(record.moves.data(), moves_blob, static_cast<std::size_t>(move_bytes));
    }
    return record;
}

auto game_store::lookup_player_id(std::string const& name) const -> result<std::optional<std::int64_t>>
{
    auto stmt = prepare(db_, "SELECT id FROM player WHERE name = ?");
    if (!stmt) {
        return tl::unexpected {stmt.error()};
    }
    sqlite3_bind_text(stmt->get(), 1, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
    auto const step_rc = sqlite3_step(stmt->get());
    if (step_rc == SQLITE_ROW) {
        return std::optional<std::int64_t> {sqlite3_column_int64(stmt->get(), 0)};
    }
    if (step_rc != SQLITE_DONE) {
        return step_error(db_);
    }
    return std::optional<std::int64_t> {};
}

auto game_store::lookup_event_id(std::string const& name) const -> result<std::optional<std::int64_t>>
{
    auto stmt = prepare(db_, "SELECT id FROM event WHERE name = ?");
    if (!stmt) {
        return tl::unexpected {stmt.error()};
    }
    sqlite3_bind_text(stmt->get(), 1, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
    auto const step_rc = sqlite3_step(stmt->get());
    if (step_rc == SQLITE_ROW) {
        return std::optional<std::int64_t> {sqlite3_column_int64(stmt->get(), 0)};
    }
    if (step_rc != SQLITE_DONE) {
        return step_error(db_);
    }
    return std::optional<std::int64_t> {};
}

namespace
{

void bind_identity(sqlite3_stmt* stmt,
                   std::int64_t const game_key,
                   std::int64_t const white_id,
                   std::int64_t const black_id,
                   std::optional<std::int64_t> const& event_id,
                   std::optional<std::string> const& date,
                   std::string const& result_text)
{
    constexpr auto date_param = 5;
    constexpr auto result_param = 6;
    sqlite3_bind_int64(stmt, 1, game_key);
    sqlite3_bind_int64(stmt, 2, white_id);
    sqlite3_bind_int64(stmt, 3, black_id);
    if (event_id) {
        sqlite3_bind_int64(stmt, 4, *event_id);
    } else {
        sqlite3_bind_null(stmt, 4);
    }
    bind_optional_text(stmt, date_param, date);
    sqlite3_bind_text(stmt, result_param, result_text.c_str(), static_cast<int>(result_text.size()), SQLITE_TRANSIENT);
}

}  // namespace

// An exact duplicate shares every identity field AND the move list. A different
// move list with the same move_hash is a hash collision, not a duplicate; it
// takes the next collision slot instead (see next_identity_collision).
auto game_store::identity_duplicate_exists(game_id const game_key, identity_row const& identity) const -> result<bool>
{
    auto stmt = prepare(db_, identity_duplicate_sql);
    if (!stmt) {
        return tl::unexpected {stmt.error()};
    }
    bind_identity(stmt->get(),
                  static_cast<std::int64_t>(game_key.value),
                  identity.white_id,
                  identity.black_id,
                  identity.event_id,
                  identity.date,
                  identity.result);
    auto const step_rc = sqlite3_step(stmt->get());
    if (step_rc == SQLITE_ROW) {
        return true;
    }
    if (step_rc != SQLITE_DONE) {
        return step_error(db_);
    }
    return false;
}

auto game_store::next_identity_collision(game_id const game_key, identity_row const& identity) const -> result<std::int64_t>
{
    auto stmt = prepare(db_, identity_next_slot_sql);
    if (!stmt) {
        return tl::unexpected {stmt.error()};
    }
    bind_identity(stmt->get(),
                  static_cast<std::int64_t>(game_key.value),
                  identity.white_id,
                  identity.black_id,
                  identity.event_id,
                  identity.date,
                  identity.result);
    if (sqlite3_step(stmt->get()) != SQLITE_ROW) {
        return step_error(db_);
    }
    return sqlite3_column_int64(stmt->get(), 0);
}

// The columns one metadata patch rewrites, applied as a single UPDATE so no
// intermediate identity can collide with another game.
class game_store::column_assignments
{
  public:
    void set_int(std::string_view const column, std::int64_t const value)
    {
        items_.emplace_back(column, [value](sqlite3_stmt* stmt, int const index) -> void { sqlite3_bind_int64(stmt, index, value); });
    }

    void set_text(std::string_view const column, std::string const& value)
    {
        items_.emplace_back(column,
                            [value](sqlite3_stmt* stmt, int const index) -> void
                            { sqlite3_bind_text(stmt, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT); });
    }

    void set_optional_int(std::string_view const column, std::optional<std::int64_t> const& value)
    {
        items_.emplace_back(column,
                            [value](sqlite3_stmt* stmt, int const index) -> void
                            {
                                if (value) {
                                    sqlite3_bind_int64(stmt, index, *value);
                                } else {
                                    sqlite3_bind_null(stmt, index);
                                }
                            });
    }

    [[nodiscard]] auto empty() const noexcept -> bool { return items_.empty(); }

    [[nodiscard]] auto update_sql() const -> std::string
    {
        auto sql = std::string {"UPDATE game SET "};
        for (std::size_t index = 0; index < items_.size(); ++index) {
            sql += index == 0 ? "" : ", ";
            sql += items_[index].first;
            sql += " = ?";
        }
        sql += " WHERE id = ?";
        return sql;
    }

    // Binds every value in order, then the key of the game being updated.
    void bind_to(sqlite3_stmt* const stmt, game_id const game_key) const
    {
        for (std::size_t index = 0; index < items_.size(); ++index) {
            items_[index].second(stmt, static_cast<int>(index) + 1);
        }
        sqlite3_bind_int64(stmt, static_cast<int>(items_.size()) + 1, static_cast<std::int64_t>(game_key.value));
    }

  private:
    std::vector<std::pair<std::string_view, std::function<void(sqlite3_stmt*, int)>>> items_;
};

// Applies one patched player name to target. False when the player does not
// exist and create is unset: the patched identity cannot collide with any game.
auto game_store::resolve_patched_player(std::optional<std::string> const& name,
                                        player const* const base,
                                        bool const create,
                                        std::int64_t& target) -> result<bool>
{
    if (!name) {
        return true;
    }
    if (!create) {
        auto existing = lookup_player_id(*name);
        if (!existing) {
            return tl::unexpected {existing.error()};
        }
        auto const& slot = *existing;
        if (!slot) {
            return false;
        }
        target = *slot;
        return true;
    }
    // Created from the game's current player, keeping its title and country.
    auto updated = base != nullptr ? *base : player {};
    updated.name = *name;
    auto inserted = find_or_insert_player(updated);
    if (!inserted) {
        return tl::unexpected {inserted.error()};
    }
    target = *inserted;
    return true;
}

// Applies the patched event to target. An empty event name leaves the game's
// event untouched; the site only describes a newly created event row, so it
// never changes the identity of a game whose event already exists. False when
// the event does not exist and create is unset.
auto game_store::resolve_patched_event(game_patch const& patch, game const& current, bool const create, std::optional<std::int64_t>& target)
    -> result<bool>
{
    auto const& existing = current.event_details;
    auto const event_name = patch.event.value_or(existing ? existing->name : std::string {});
    if (event_name.empty()) {
        return true;
    }
    if (!create) {
        auto found = lookup_event_id(event_name);
        if (!found) {
            return tl::unexpected {found.error()};
        }
        auto const& slot = *found;
        if (!slot) {
            return false;
        }
        target = slot;
        return true;
    }
    std::optional<std::string> const inherited_site = existing ? existing->site : std::nullopt;
    std::optional<std::string> const site = patch.site.has_value() ? patch.site : inherited_site;
    std::optional<std::string> const date = existing ? existing->date : std::nullopt;
    auto inserted = find_or_insert_event({.name = event_name, .site = site, .date = date});
    if (!inserted) {
        return tl::unexpected {inserted.error()};
    }
    target = *inserted;
    return true;
}

auto game_store::resolve_identity(game_id const game_key,
                                  game_patch const& patch,
                                  identity_row const& stored,
                                  game const* const current,
                                  bool const create) -> result<std::optional<identity_row>>
{
    static_cast<void>(game_key);
    auto resolved = stored;

    auto white_known = resolve_patched_player(patch.white_name, current != nullptr ? &current->white : nullptr, create, resolved.white_id);
    if (!white_known) {
        return tl::unexpected {white_known.error()};
    }
    if (!*white_known) {
        return std::optional<identity_row> {};
    }
    auto black_known = resolve_patched_player(patch.black_name, current != nullptr ? &current->black : nullptr, create, resolved.black_id);
    if (!black_known) {
        return tl::unexpected {black_known.error()};
    }
    if (!*black_known) {
        return std::optional<identity_row> {};
    }
    if ((patch.event || patch.site) && current != nullptr) {
        auto event_known = resolve_patched_event(patch, *current, create, resolved.event_id);
        if (!event_known) {
            return tl::unexpected {event_known.error()};
        }
        if (!*event_known) {
            return std::optional<identity_row> {};
        }
    }
    if (patch.date) {
        resolved.date = patch.date;
    }
    if (patch.result) {
        resolved.result = *patch.result;
    }
    return std::optional<identity_row> {std::move(resolved)};
}

// The full game, loaded only when the patch touches names, event or site (the
// fields needed to build a new player or event row); nullopt otherwise.
auto game_store::load_patch_context(game_id const game_key, game_patch const& patch) -> result<std::optional<game>>
{
    if (!(patch.white_name || patch.black_name || patch.event || patch.site)) {
        return std::optional<game> {};
    }
    auto loaded = get(game_key);
    if (!loaded) {
        return tl::unexpected {loaded.error()};
    }
    return std::optional<game> {std::move(*loaded)};
}

// An exact-duplicate identity is refused before anything is written. A player
// or event that does not exist yet cannot collide with any other game.
auto game_store::reject_duplicate_identity(game_id const game_key, game_patch const& patch, std::string const& before_result)
    -> result<void>
{
    auto const touches_identity =
        patch.white_name || patch.black_name || patch.event || patch.site || patch.date || (patch.result && *patch.result != before_result);
    if (!touches_identity) {
        return {};
    }
    auto identity = read_identity_row(game_key);
    if (!identity) {
        return tl::unexpected {identity.error()};
    }
    auto context = load_patch_context(game_key, patch);
    if (!context) {
        return tl::unexpected {context.error()};
    }
    auto const& current = *context;
    auto resolved = resolve_identity(game_key, patch, *identity, current ? &*current : nullptr, /*create=*/false);
    if (!resolved) {
        return tl::unexpected {resolved.error()};
    }
    auto const& candidate = *resolved;
    if (candidate && !candidate->same_identity(*identity)) {
        auto duplicate = identity_duplicate_exists(game_key, *candidate);
        if (!duplicate) {
            return tl::unexpected {duplicate.error()};
        }
        if (*duplicate) {
            return tl::unexpected {error_code::duplicate};
        }
    }
    return {};
}

auto game_store::prepare_metadata_patch(game_id const game_key, game_patch const& patch) -> result<prepared_metadata_patch>
{
    auto provenance = get_provenance(game_key);
    if (!provenance) {
        return tl::unexpected {provenance.error()};
    }
    if (patch.result && !is_valid_game_result(*patch.result)) {
        return tl::unexpected {error {error_code::invalid_argument, "result must be one of 1-0, 0-1, 1/2-1/2, *"}};
    }
    if (!rating_in_domain(patch.white_elo) || !rating_in_domain(patch.black_elo)) {
        return tl::unexpected {error {error_code::invalid_argument, "rating outside the signed 16-bit domain"}};
    }
    auto stored = read_indexed_metadata(game_key);
    if (!stored) {
        return tl::unexpected {stored.error()};
    }

    auto plan = prepared_metadata_patch {};
    plan.id = game_key;
    plan.patch = patch;
    plan.content_revision = stored->second;
    plan.before = stored->first;
    plan.after = indexed_game_metadata {.result = patch.result.value_or(plan.before.result),
                                        .white_elo = patch.white_elo.applied_to(plan.before.white_elo),
                                        .black_elo = patch.black_elo.applied_to(plan.before.black_elo)};
    plan.indexed_change = plan.after != plan.before;
    plan.other_fields = patch.white_name || patch.black_name || patch.event || patch.site || patch.date || patch.eco || patch.source_label
        || patch.review_status || patch.notes;

    // The editability policy: result and ratings are correctable on any game;
    // everything else only on manual games. All or nothing.
    if (plan.other_fields && provenance->source_type != "manual") {
        return tl::unexpected {error_code::not_editable};
    }

    if (auto accepted = reject_duplicate_identity(game_key, patch, plan.before.result); !accepted) {
        return tl::unexpected {accepted.error()};
    }
    return plan;
}

// The identity-bearing columns of the patch: resolved player and event ids,
// the date and result, and the collision slot a changed identity takes. An exact
// duplicate is refused; a hash collision between distinct move lists takes the
// next slot.
auto game_store::collect_identity_assignments(prepared_metadata_patch const& plan, column_assignments& assignments) -> result<void>
{
    auto const& patch = plan.patch;
    auto const game_key = plan.id;

    auto stored_identity = read_identity_row(game_key);
    if (!stored_identity) {
        return tl::unexpected {stored_identity.error()};
    }
    auto context = load_patch_context(game_key, patch);
    if (!context) {
        return tl::unexpected {context.error()};
    }
    auto const& current = *context;
    auto resolved = resolve_identity(game_key, patch, *stored_identity, current ? &*current : nullptr, /*create=*/true);
    if (!resolved) {
        return tl::unexpected {resolved.error()};
    }
    auto const& resolved_row = *resolved;
    if (!resolved_row) {
        return tl::unexpected {error {error_code::io_failure, "the patched identity did not resolve"}};  // create=true always resolves
    }
    auto const& final_identity = *resolved_row;

    if (final_identity.white_id != stored_identity->white_id) {
        assignments.set_int("white_id", final_identity.white_id);
    }
    if (final_identity.black_id != stored_identity->black_id) {
        assignments.set_int("black_id", final_identity.black_id);
    }
    if (final_identity.event_id != stored_identity->event_id) {
        assignments.set_optional_int("event_id", final_identity.event_id);
    }
    if (patch.date) {
        assignments.set_text("date", *patch.date);
    }
    if (patch.result) {
        assignments.set_text("result", *patch.result);
    }
    // A changed identity may collide on the unique index; the exact duplicate is
    // refused, a hash collision between distinct move lists takes the next slot.
    // Everything lands in the single UPDATE, so no half-edited identity is ever
    // compared against another game.
    if (!final_identity.same_identity(*stored_identity)) {
        auto duplicate = identity_duplicate_exists(game_key, final_identity);
        if (!duplicate) {
            return tl::unexpected {duplicate.error()};
        }
        if (*duplicate) {
            return tl::unexpected {error_code::duplicate};
        }
        auto slot = next_identity_collision(game_key, final_identity);
        if (!slot) {
            return tl::unexpected {slot.error()};
        }
        assignments.set_int("identity_collision", *slot);
    }
    return {};
}

// The columns that never take part in the unique identity index.
void game_store::collect_value_assignments(prepared_metadata_patch const& plan, column_assignments& assignments)
{
    auto const& patch = plan.patch;
    if (patch.eco) {
        assignments.set_text("eco", *patch.eco);
    }
    if (patch.source_label) {
        assignments.set_text("source_label", *patch.source_label);
    }
    if (patch.review_status) {
        assignments.set_text("review_status", *patch.review_status);
    }
    if (patch.white_elo.changes()) {
        assignments.set_optional_int("white_elo",
                                     plan.after.white_elo ? std::optional<std::int64_t> {*plan.after.white_elo} : std::nullopt);
    }
    if (patch.black_elo.changes()) {
        assignments.set_optional_int("black_elo",
                                     plan.after.black_elo ? std::optional<std::int64_t> {*plan.after.black_elo} : std::nullopt);
    }
}

auto game_store::run_column_update(game_id const game_key, column_assignments const& assignments) -> result<void>
{
    auto const sql = assignments.update_sql();
    auto upd = prepare(db_, sql.c_str());
    if (!upd) {
        return tl::unexpected {upd.error()};
    }
    assignments.bind_to(upd->get(), game_key);
    if (sqlite3_step(upd->get()) != SQLITE_DONE) {
        // A unique-index rejection here means a concurrent identity change that
        // the checks above could not see; report it as the conflict it is.
        if (sqlite3_extended_errcode(db_) == SQLITE_CONSTRAINT_UNIQUE) {
            return tl::unexpected {error_code::duplicate};
        }
        return step_error(db_);
    }
    if (sqlite3_changes(db_) != 1) {
        return tl::unexpected {error {error_code::io_failure, "metadata patch updated an unexpected number of rows"}};
    }
    return {};
}

auto game_store::replace_notes(game_id const game_key, std::string const& notes) -> result<void>
{
    // notes is stored as a reserved game tag with key "_notes"; replace if present
    auto del = prepare(db_, R"sql(DELETE FROM game_tag WHERE game_id = ? AND tag_id = (SELECT id FROM tag WHERE name = '_notes'))sql");
    if (!del) {
        return tl::unexpected {del.error()};
    }
    sqlite3_bind_int64(del->get(), 1, static_cast<std::int64_t>(game_key.value));
    if (sqlite3_step(del->get()) != SQLITE_DONE) {
        return step_error(db_);
    }
    if (!notes.empty()) {
        return insert_game_tags(game_key, {{"_notes", notes}});
    }
    return {};
}

auto game_store::apply_metadata_patch(prepared_metadata_patch const& plan) -> result<void>
{
    auto const& patch = plan.patch;
    auto const game_key = plan.id;

    // The prepared before-image must still be the stored one: nothing else may
    // have written this game between prepare and the transaction.
    auto live = read_indexed_metadata(game_key);
    if (!live) {
        return tl::unexpected {live.error()};
    }
    if (live->first != plan.before || live->second != plan.content_revision) {
        return tl::unexpected {error {error_code::io_failure, "the game changed after the patch was prepared"}};
    }

    auto assignments = column_assignments {};
    if (auto collected = collect_identity_assignments(plan, assignments); !collected) {
        return collected;
    }
    collect_value_assignments(plan, assignments);
    if (!assignments.empty()) {
        if (auto updated = run_column_update(game_key, assignments); !updated) {
            return updated;
        }
    }
    if (patch.notes) {
        return replace_notes(game_key, *patch.notes);
    }
    return {};
}

auto game_store::advance_mutation_seq() -> result<void>
{
    return writer_->record_canonical_change();
}

void game_store::discard_id_caches() noexcept
{
    player_id_cache_.clear();
    event_id_cache_.clear();
    tag_id_cache_.clear();
}

void game_store::note_data_changed() noexcept
{
    writer_->note_data_changed();
}

auto game_store::commit_metadata_patch(prepared_metadata_patch const& plan) -> result<void>
{
    mutation_scope txn {db_, writer_->owns_open_batch()};
    if (!txn.began()) {
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(db_)}};
    }

    // txn's destructor rolls back on any early return below via a raw SQL
    // ROLLBACK, which doesn't know about these in-memory id caches -- an
    // early return after find_or_insert_player/event/tag cached a
    // just-inserted (and now rolled-back) id would leave that id cached,
    // causing a later insert reusing it to fail its foreign-key check.
    struct cache_rollback_guard
    {
        game_store* self;
        bool committed {false};

        explicit cache_rollback_guard(game_store* owner) noexcept
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
                self->discard_id_caches();
            }
        }
    } cache_guard {this};

    // A failed patch inside a writer-owned batch may have applied some of its
    // field writes already; the batch must then roll back rather than commit
    // them without the sequence mark.
    struct failure_scope
    {
        game_store* self;
        bool succeeded {false};

        explicit failure_scope(game_store* owner) noexcept
            : self {owner}
        {
        }

        failure_scope(failure_scope const&) = delete;
        auto operator=(failure_scope const&) -> failure_scope& = delete;
        failure_scope(failure_scope&&) = delete;
        auto operator=(failure_scope&&) -> failure_scope& = delete;

        ~failure_scope() noexcept
        {
            if (!succeeded) {
                self->writer_->note_failed_operation();
            }
        }
    } failure_guard {this};

    if (auto applied = apply_metadata_patch(plan); !applied) {
        return applied;
    }
    // Only the fields the derived indexes embed (result, white_elo, black_elo;
    // moves and the game set are the others) change what an index covers. Names,
    // event, site, date, eco, notes and provenance are read live from SQLite by
    // every query, so patching them leaves the published snapshot valid and never
    // advances the sequence. One advance covers the whole patch.
    if (plan.indexed_change) {
        if (auto advanced = advance_mutation_seq(); !advanced) {
            return advanced;
        }
    }
    if (!txn.commit()) {
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(db_)}};
    }
    cache_guard.committed = true;
    failure_guard.succeeded = true;
    note_data_changed();
    return {};
}

auto game_store::patch_metadata(game_id const game_key, game_patch const& patch) -> result<void>
{
    auto plan = prepare_metadata_patch(game_key, patch);
    if (!plan) {
        return tl::unexpected {plan.error()};
    }
    if (plan->is_noop()) {
        return {};
    }
    return commit_metadata_patch(*plan);
}

auto game_store::set_manual_provenance(game_id const game_key,
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
    sqlite3_bind_int64(stmt->get(), 3, static_cast<std::int64_t>(game_key.value));

    if (sqlite3_step(stmt->get()) != SQLITE_DONE) {
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(db_)}};
    }
    if (sqlite3_changes(db_) == 0) {
        return tl::unexpected {error_code::not_found};
    }
    // Provenance never advances the mutation sequence, but a reader that
    // copied it out of the store must still be able to tell it changed.
    writer_->note_data_changed();
    return {};
}

}  // namespace motif::db
