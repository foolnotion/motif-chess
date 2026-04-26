// NOLINTNEXTLINE(portability-avoid-pragma-once)
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace motif::db
{

struct player
{
    std::string name;
    std::optional<std::int32_t> elo;
    std::optional<std::string> title;
    std::optional<std::string> country;
};

struct event
{
    std::string name;
    std::optional<std::string> site;
    std::optional<std::string> date;
};

struct position
{
    std::uint64_t zobrist_hash {};
    std::uint32_t game_id {};
    std::uint16_t ply {};
};

struct position_row
{
    std::uint64_t zobrist_hash {};
    std::uint32_t game_id {};
    std::uint16_t ply {};
    std::int8_t result {};
    std::optional<std::int16_t> white_elo;
    std::optional<std::int16_t> black_elo;
};

struct position_match
{
    std::uint32_t game_id {};
    std::uint16_t ply {};
    std::int8_t result {};
    std::optional<std::int16_t> white_elo;
    std::optional<std::int16_t> black_elo;
};

struct opening_move_stat
{
    std::uint32_t game_id {};
    std::uint16_t ply {};
    std::int8_t result {};
    std::optional<std::int16_t> white_elo;
    std::optional<std::int16_t> black_elo;
};

struct tree_position_row
{
    std::uint32_t game_id {};
    std::uint16_t root_ply {};
    std::uint16_t depth {};
    std::uint64_t child_hash {};
    std::int8_t result {};
    std::optional<std::int16_t> white_elo;
    std::optional<std::int16_t> black_elo;
};

struct game_context
{
    std::optional<std::string> eco;
    std::optional<std::string> opening_name;
    std::vector<std::uint16_t> moves;
};

struct game_continuation_context
{
    std::uint32_t game_id {};
    std::uint16_t ply {};
    std::uint16_t encoded_move {};
    std::int8_t result {};
    std::optional<std::int16_t> white_elo;
    std::optional<std::int16_t> black_elo;
    std::optional<std::string> eco;
    std::optional<std::string> opening_name;
};

struct opening_context
{
    std::optional<std::int32_t> white_elo;
    std::optional<std::int32_t> black_elo;
    std::optional<std::string> eco;
    std::optional<std::string> opening_name;
    std::vector<std::uint16_t> moves;
};

// source_type values: "manual" (user-added via API), "imported" (bulk import).
// Only "manual" games are editable or deletable.
// Existing imported games without a stored source_type default to "imported".
struct game_provenance
{
    std::string source_type {"imported"};
    std::optional<std::string> source_label;
    // review_status: "new" | "needs_review" | "studied" | "archived"
    std::string review_status {"new"};
};

struct game_list_entry
{
    std::uint32_t id {};
    std::string white;
    std::string black;
    std::string result;
    std::string event;
    std::string date;
    std::string eco;
    std::string source_type {"imported"};
    std::optional<std::string> source_label;
    std::string review_status {"new"};
};

struct game_list_query
{
    std::optional<std::string> player;
    std::optional<std::string> result;
    std::size_t limit {50};  // NOLINT(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
    std::size_t offset {0};
};

struct game
{
    player white;
    player black;
    std::optional<event> event_details;
    std::optional<std::string> date;
    std::string result;
    std::optional<std::string> eco;
    std::vector<std::uint16_t> moves;
    std::vector<std::pair<std::string, std::string>> extra_tags;
    game_provenance provenance;
};

// Fields that may be updated via PATCH /api/games/{id}.
// Only user-added (manual) games can be patched.
// nullopt fields are left unchanged.
struct game_patch
{
    std::optional<std::string> white_name;
    std::optional<std::int32_t> white_elo;
    std::optional<std::string> black_name;
    std::optional<std::int32_t> black_elo;
    std::optional<std::string> event;
    std::optional<std::string> site;
    std::optional<std::string> date;
    std::optional<std::string> result;
    std::optional<std::string> eco;
    std::optional<std::string> source_label;
    std::optional<std::string> review_status;
    std::optional<std::string> notes;
};

}  // namespace motif::db
