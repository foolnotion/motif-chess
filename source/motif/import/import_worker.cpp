#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "motif/import/import_worker.hpp"

#include <pgnlib/types.hpp>  // NOLINT(misc-include-cleaner)
#include <tl/expected.hpp>

#include "motif/chess/chess.hpp"
#include "motif/db/database_manager.hpp"
#include "motif/db/error.hpp"
#include "motif/db/game_store.hpp"
#include "motif/db/types.hpp"
#include "motif/import/error.hpp"
#include "motif/import/pgn_helpers.hpp"

namespace motif::import
{

namespace
{

constexpr std::array<std::string_view, 11> known_tag_keys = {
    "White",
    "Black",
    "WhiteElo",
    "BlackElo",
    "WhiteTitle",
    "BlackTitle",
    "Event",
    "Site",
    "Date",
    "Result",
    "ECO",
};

auto is_known_tag(std::string_view key) noexcept -> bool
{
    return std::ranges::any_of(known_tag_keys, [&](std::string_view known) -> bool { return key == known; });
}

auto extract_game(pgn::game const& pgn_game) -> motif::db::game
{
    auto const& tags = pgn_game.tags;

    auto const white_elo = parse_elo(find_tag(tags, "WhiteElo"));
    auto const black_elo = parse_elo(find_tag(tags, "BlackElo"));

    auto const white_title_raw = find_tag(tags, "WhiteTitle");
    auto const black_title_raw = find_tag(tags, "BlackTitle");
    auto const event_name = find_tag(tags, "Event");
    auto const site_raw = find_tag(tags, "Site");
    auto const date_raw = find_tag(tags, "Date");
    auto const eco_raw = find_tag(tags, "ECO");

    auto const valid_date = (!date_raw.empty() && date_raw != "????.??.??") ? std::optional<std::string> {date_raw} : std::nullopt;

    motif::db::game dbg;
    dbg.white.name = find_tag(tags, "White");
    dbg.white.elo = white_elo ? std::optional<std::int32_t> {*white_elo} : std::nullopt;
    dbg.white.title = white_title_raw.empty() ? std::nullopt : std::optional<std::string> {white_title_raw};

    dbg.black.name = find_tag(tags, "Black");
    dbg.black.elo = black_elo ? std::optional<std::int32_t> {*black_elo} : std::nullopt;
    dbg.black.title = black_title_raw.empty() ? std::nullopt : std::optional<std::string> {black_title_raw};

    if (!event_name.empty()) {
        dbg.event_details = motif::db::event {
            .name = event_name,
            .site = site_raw.empty() ? std::nullopt : std::optional<std::string> {site_raw},
            .date = valid_date,
        };
    }

    dbg.date = valid_date;
    dbg.eco = eco_raw.empty() ? std::nullopt : std::optional<std::string> {eco_raw};
    dbg.result = pgn_result_to_string(pgn_game.result);

    for (auto const& tag : tags) {
        if (!is_known_tag(tag.key)) {
            dbg.extra_tags.emplace_back(tag.key, tag.value);
        }
    }

    return dbg;
}

}  // namespace

import_worker::import_worker(motif::db::database_manager& database) noexcept
    : db_ {database}
{
}

auto import_worker::process(pgn::game const& pgn_game) -> result<process_result>
{
    auto const generation_lock = db_.lock_generation();
    if (pgn_game.moves.empty()) {
        return tl::unexpected {error_code::empty_game};
    }

    if (pgn_game.moves.size() > std::numeric_limits<std::uint16_t>::max()) {
        return tl::unexpected {error_code::io_failure};
    }

    auto db_game = extract_game(pgn_game);

    // Process all moves before any DB write — any SAN failure aborts entire
    // game
    auto board = motif::chess::board {};
    std::vector<std::uint16_t> encoded_moves;
    encoded_moves.reserve(pgn_game.moves.size());

    for (auto const& node : pgn_game.moves) {
        auto move_res = motif::chess::apply_san(board, node.san);
        if (!move_res) {
            return tl::unexpected {error_code::parse_error};
        }
        encoded_moves.push_back(*move_res);
    }

    db_game.moves = std::move(encoded_moves);

    if (auto stale_res = db_.prepare_canonical_mutation(); !stale_res) {
        return tl::unexpected {error_code::io_failure};
    }

    // Insert into SQLite game store
    auto insert_res = db_.writer().insert(db_game);
    if (!insert_res) {
        if (insert_res.error() == motif::db::error_code::duplicate) {
            return tl::unexpected {error_code::duplicate};
        }
        return tl::unexpected {error_code::io_failure};
    }
    auto const game_id = *insert_res;

    return process_result {
        .game_id = game_id,
        .positions_inserted = db_game.moves.size() + 1,
    };
}

}  // namespace motif::import
