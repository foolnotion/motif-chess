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

#include <chesslib/board/board.hpp>  // NOLINT(misc-include-cleaner)
#include <chesslib/board/move_codec.hpp>
#include <chesslib/util/san.hpp>
#include <pgnlib/types.hpp>  // NOLINT(misc-include-cleaner)
#include <tl/expected.hpp>

#include "motif/db/error.hpp"
#include "motif/db/game_store.hpp"
#include "motif/db/position_store.hpp"
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
    return std::ranges::any_of(known_tag_keys,
                               [&](std::string_view known) -> bool
                               { return key == known; });
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

    auto const valid_date = (!date_raw.empty() && date_raw != "????.??.??")
        ? std::optional<std::string> {date_raw}
        : std::nullopt;

    motif::db::game dbg;
    dbg.white.name = find_tag(tags, "White");
    dbg.white.elo =
        white_elo ? std::optional<std::int32_t> {*white_elo} : std::nullopt;
    dbg.white.title = white_title_raw.empty()
        ? std::nullopt
        : std::optional<std::string> {white_title_raw};

    dbg.black.name = find_tag(tags, "Black");
    dbg.black.elo =
        black_elo ? std::optional<std::int32_t> {*black_elo} : std::nullopt;
    dbg.black.title = black_title_raw.empty()
        ? std::nullopt
        : std::optional<std::string> {black_title_raw};

    if (!event_name.empty()) {
        dbg.event_details = motif::db::event {
            .name = event_name,
            .site = site_raw.empty() ? std::nullopt
                                     : std::optional<std::string> {site_raw},
            .date = valid_date,
        };
    }

    dbg.date = valid_date;
    dbg.eco =
        eco_raw.empty() ? std::nullopt : std::optional<std::string> {eco_raw};
    dbg.result = pgn_result_to_string(pgn_game.result);

    for (auto const& tag : tags) {
        if (!is_known_tag(tag.key)) {
            dbg.extra_tags.emplace_back(tag.key, tag.value);
        }
    }

    return dbg;
}

}  // namespace

import_worker::import_worker(motif::db::game_store& store,
                             motif::db::position_store& positions) noexcept
    : store_ {store}
    , positions_ {positions}
{
}

auto import_worker::process(pgn::game const& pgn_game) -> result<process_result>
{
    if (pgn_game.moves.size() > std::numeric_limits<std::uint16_t>::max()) {
        return tl::unexpected(error_code::io_failure);
    }

    auto db_game = extract_game(pgn_game);

    auto const& tags = pgn_game.tags;
    auto const white_elo = parse_elo(find_tag(tags, "WhiteElo"));
    auto const black_elo = parse_elo(find_tag(tags, "BlackElo"));
    auto const result_int = pgn_result_to_int8(pgn_game.result);

    // Process all moves before any DB write — any SAN failure aborts entire
    // game
    auto board = chesslib::board {};
    std::vector<std::uint16_t> encoded_moves;
    std::vector<motif::db::position_row> position_rows;
    encoded_moves.reserve(pgn_game.moves.size());
    position_rows.reserve(pgn_game.moves.size());

    for (auto const& node : pgn_game.moves) {
        auto move_res = chesslib::san::from_string(board, node.san);
        if (!move_res) {
            return tl::unexpected(error_code::parse_error);
        }
        encoded_moves.push_back(chesslib::codec::encode(*move_res));
        chesslib::move_maker mmaker {board, *move_res};
        mmaker.make();

        position_rows.push_back(motif::db::position_row {
            .zobrist_hash = board.hash(),
            .game_id = 0,
            .ply = static_cast<std::uint16_t>(encoded_moves.size()),
            .result = result_int,
            .white_elo = white_elo,
            .black_elo = black_elo,
        });
    }

    db_game.moves = std::move(encoded_moves);

    // Insert into SQLite game store
    auto insert_res = store_.insert(db_game);
    if (!insert_res) {
        if (insert_res.error() == motif::db::error_code::duplicate) {
            return tl::unexpected(error_code::duplicate);
        }
        return tl::unexpected(error_code::io_failure);
    }
    auto const game_id = *insert_res;

    for (auto& row : position_rows) {
        row.game_id = game_id;
    }

    if (!position_rows.empty()) {
        auto batch_res = positions_.insert_batch(position_rows);
        if (!batch_res) {
            return tl::unexpected(error_code::io_failure);
        }
    }

    return process_result {
        .game_id = game_id,
        .positions_inserted = position_rows.size(),
    };
}

}  // namespace motif::import
