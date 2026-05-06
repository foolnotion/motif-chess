#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace motif::db
{

struct game_id
{
    std::uint32_t value {};

    constexpr explicit operator std::uint32_t() const noexcept { return value; }

    constexpr auto operator<=>(game_id const&) const noexcept = default;
};

struct zobrist_hash
{
    std::uint64_t value {};

    constexpr explicit operator std::uint64_t() const noexcept { return value; }

    constexpr auto operator<=>(zobrist_hash const&) const noexcept = default;
};

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

struct position_row
{
    motif::db::zobrist_hash zobrist_hash {};
    motif::db::game_id game_id {};
    std::uint16_t ply {};
    std::uint16_t encoded_move {};  // move that reached this position; 0 for ply == 0
    std::int8_t result {};
    std::optional<std::int16_t> white_elo;
    std::optional<std::int16_t> black_elo;
};

struct position_match
{
    motif::db::game_id game_id {};
    std::uint16_t ply {};
    std::int8_t result {};
    std::optional<std::int16_t> white_elo;
    std::optional<std::int16_t> black_elo;
};

struct tree_position_row
{
    motif::db::game_id game_id {};
    std::uint16_t root_ply {};
    std::uint16_t depth {};
    std::uint16_t encoded_move {};  // move that reached child_hash
    motif::db::zobrist_hash child_hash {};
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

// One row per distinct continuation from a given position, with counts and
// averages pre-aggregated in DuckDB. eco_sample_{min,max} are two candidate
// game_ids for ECO attribution (the game with the lowest and highest id
// among all games playing this continuation).
struct opening_stat_agg_row
{
    std::uint16_t cont_encoded_move {};
    motif::db::zobrist_hash cont_hash {};
    std::uint16_t root_ply {};  // MIN(p_root.ply) — for board reconstruction
    std::uint32_t frequency {};
    std::uint32_t white_wins {};
    std::uint32_t draws {};
    std::uint32_t black_wins {};
    std::optional<double> avg_white_elo;
    std::optional<double> avg_black_elo;
    motif::db::game_id eco_sample_min {};
    motif::db::game_id eco_sample_max {};
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
    motif::db::game_id id {};
    std::string white;
    std::string black;
    std::optional<std::int32_t> white_elo;
    std::optional<std::int32_t> black_elo;
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
    // ECO prefix: "C4" matches C40, C41, … (case-sensitive, PGN convention).
    std::optional<std::string> eco_prefix;
    // Date strings in YYYY-MM-DD format (ISO 8601); inclusive.
    // Note: games store dates as YYYY.MM.DD (PGN convention) — callers must
    // convert before filtering or queries will silently return wrong results.
    std::optional<std::string> date_from;
    std::optional<std::string> date_to;
    // Elo filters: at least one player must satisfy each bound.
    // Games with no recorded Elo are excluded when a bound is set.
    std::optional<std::int32_t> min_elo;
    std::optional<std::int32_t> max_elo;
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

template<>
struct std::hash<motif::db::game_id>
{
    auto operator()(motif::db::game_id const game_key) const noexcept -> std::size_t { return std::hash<std::uint32_t> {}(game_key.value); }
};

template<>
struct std::hash<motif::db::zobrist_hash>
{
    auto operator()(motif::db::zobrist_hash const hash) const noexcept -> std::size_t { return std::hash<std::uint64_t> {}(hash.value); }
};
