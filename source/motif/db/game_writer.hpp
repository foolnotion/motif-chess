#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <gtl/phmap.hpp>

#include "motif/db/error.hpp"
#include "motif/db/sqlite_util.hpp"
#include "motif/db/types.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace motif::db
{

// Owns the bulk-insert path for the SQLite game store: entity deduplication
// via cached prepared statements and transaction control for batched imports.
// The companion game_store class handles schema creation and read/edit operations.
class game_writer
{
  public:
    explicit game_writer(sqlite3* conn) noexcept;
    ~game_writer() = default;

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
    sqlite3* db_;
    gtl::flat_hash_map<std::string, std::int64_t> player_id_cache_;
    gtl::flat_hash_map<std::string, std::int64_t> event_id_cache_;
    gtl::flat_hash_map<std::string, std::int64_t> tag_id_cache_;

    detail::unique_stmt select_player_stmt_;
    detail::unique_stmt insert_player_stmt_;
    detail::unique_stmt select_event_stmt_;
    detail::unique_stmt insert_event_stmt_;
    detail::unique_stmt insert_game_stmt_;
    detail::unique_stmt select_tag_stmt_;
    detail::unique_stmt insert_tag_stmt_;
    detail::unique_stmt insert_game_tag_stmt_;

    auto find_or_insert_player(player const& plr) -> result<std::int64_t>;
    auto find_or_insert_event(event const& evt) -> result<std::int64_t>;
    auto insert_game_tags(game_id gid, std::vector<std::pair<std::string, std::string>> const& extra_tags) -> result<void>;
    auto prepare_cached_stmt(detail::unique_stmt& stmt, char const* sql) -> result<sqlite3_stmt*>;
};

}  // namespace motif::db
