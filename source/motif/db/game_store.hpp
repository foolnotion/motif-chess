// NOLINTNEXTLINE(portability-avoid-pragma-once)
#pragma once

#include <cstdint>

#include "motif/db/error.hpp"
#include "motif/db/types.hpp"

// Forward declaration — callers own the connection; game_store is non-owning.
struct sqlite3;

namespace motif::db {

class game_store {
public:
    explicit game_store(sqlite3* conn) noexcept;

    // Create the five tables and supporting indexes.
    // Must be called once per connection before any other method.
    // Duplicate key: (white_id, black_id, coalesced event_id, coalesced date, result, moves).
    auto create_schema() -> result<void>;

    // Insert a game.  Returns the new game row id on success.
    // Returns error_code::duplicate when the identity key already exists.
    // Callers decide whether to treat a duplicate as fatal.
    auto insert(game const& src_game) -> result<std::uint32_t>;

    // Retrieve a game by id.  Returns error_code::not_found if absent.
    auto get(std::uint32_t game_id) -> result<game>;

    // Delete the game row and all associated game_tag rows.
    // Player and event rows are preserved.
    // Returns error_code::not_found if the id does not exist.
    auto remove(std::uint32_t game_id) -> result<void>;

private:
    sqlite3* db_;

    auto find_or_insert_player(player const& plr) -> result<std::int64_t>;
    auto find_or_insert_event(event const& evt) -> result<std::int64_t>;
};

} // namespace motif::db
