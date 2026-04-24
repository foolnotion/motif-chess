// NOLINTNEXTLINE(portability-avoid-pragma-once)
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtl/phmap.hpp>

#include "motif/db/error.hpp"
#include "motif/db/types.hpp"

// Forward declaration — callers own the connection; game_store is non-owning.
struct sqlite3;
struct sqlite3_stmt;

namespace motif::db
{

class game_store
{
  public:
    explicit game_store(sqlite3* conn) noexcept;
    ~game_store() noexcept;

    game_store(game_store const&) = delete;
    auto operator=(game_store const&) -> game_store& = delete;
    game_store(game_store&& other) noexcept;
    auto operator=(game_store&& other) noexcept -> game_store&;

    // Create the five tables and supporting indexes.
    // Must be called once per connection before any other method.
    // Duplicate key: (white_id, black_id, coalesced event_id, coalesced date,
    // result, moves).
    auto create_schema() -> result<void>;

    // Insert a game.  Returns the new game row id on success.
    // Returns error_code::duplicate when the identity key already exists.
    // Callers decide whether to treat a duplicate as fatal.
    auto insert(game const& src_game) -> result<std::uint32_t>;

    // Transaction control for batched import paths. Safe to use around multiple
    // insert() calls; insert() will join an already-open transaction.
    auto begin_transaction() -> result<void>;
    auto commit_transaction() -> result<void>;
    auto rollback_transaction() noexcept -> void;

    // Retrieve a game by id.  Returns error_code::not_found if absent.
    auto get(std::uint32_t game_id) -> result<game>;
    auto get(std::uint32_t game_id) const -> result<game>;
    auto get_opening_context(std::uint32_t game_id) -> result<opening_context>;
    auto get_opening_context(std::uint32_t game_id) const -> result<opening_context>;
    // Precondition: game_ids contains no duplicates (duplicate entries are
    // silently dropped by the unordered_map; deduplicate before calling).
    auto get_game_contexts(std::vector<std::uint32_t> const& game_ids) -> result<std::unordered_map<std::uint32_t, game_context>>;
    auto get_game_contexts(std::vector<std::uint32_t> const& game_ids) const -> result<std::unordered_map<std::uint32_t, game_context>>;

    // Delete the game row and all associated game_tag rows.
    // Player and event rows are preserved.
    // Returns error_code::not_found if the id does not exist.
    auto remove(std::uint32_t game_id) -> result<void>;

  private:
    sqlite3* db_;
    gtl::flat_hash_map<std::string, std::int64_t> player_id_cache_;
    gtl::flat_hash_map<std::string, std::int64_t> event_id_cache_;
    gtl::flat_hash_map<std::string, std::int64_t> tag_id_cache_;

    sqlite3_stmt* select_player_stmt_ {nullptr};
    sqlite3_stmt* insert_player_stmt_ {nullptr};
    sqlite3_stmt* select_event_stmt_ {nullptr};
    sqlite3_stmt* insert_event_stmt_ {nullptr};
    sqlite3_stmt* insert_game_stmt_ {nullptr};
    sqlite3_stmt* select_tag_stmt_ {nullptr};
    sqlite3_stmt* insert_tag_stmt_ {nullptr};
    sqlite3_stmt* insert_game_tag_stmt_ {nullptr};

    auto find_or_insert_player(player const& plr) -> result<std::int64_t>;
    auto find_or_insert_event(event const& evt) -> result<std::int64_t>;
    auto insert_game_tags(std::uint32_t game_id, std::vector<std::pair<std::string, std::string>> const& extra_tags) -> result<void>;
    auto prepare_cached_stmt(sqlite3_stmt*& stmt, char const* sql) -> result<sqlite3_stmt*>;
};

}  // namespace motif::db
