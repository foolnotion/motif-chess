#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "motif/import/import_worker.hpp"

#include <chesslib/board/board.hpp>  // NOLINT(misc-include-cleaner)
#include <chesslib/board/move_codec.hpp>
#include <chesslib/util/san.hpp>
#include <pgnlib/types.hpp>  // NOLINT(misc-include-cleaner)
#include <tl/expected.hpp>

#include "motif/db/database_manager.hpp"
#include "motif/db/error.hpp"
#include "motif/db/game_store.hpp"
#include "motif/db/position_store.hpp"
#include "motif/db/types.hpp"
#include "motif/import/error.hpp"
#include "motif/import/pgn_helpers.hpp"

namespace motif::import
{

import_worker::import_worker(motif::db::database_manager& database) noexcept
    : db_ {database}
{
}

auto import_worker::process(pgn::game const& pgn_game) -> result<process_result>
{
    if (pgn_game.moves.empty()) {
        return tl::unexpected(error_code::empty_game);
    }

    if (pgn_game.moves.size() > std::numeric_limits<std::uint16_t>::max()) {
        return tl::unexpected(error_code::io_failure);
    }

    auto db_game = pgn_to_game(pgn_game);

    auto const white_elo = db_game.white.elo ? std::optional<std::int16_t> {static_cast<std::int16_t>(*db_game.white.elo)} : std::nullopt;
    auto const black_elo = db_game.black.elo ? std::optional<std::int16_t> {static_cast<std::int16_t>(*db_game.black.elo)} : std::nullopt;
    auto const result_int = pgn_result_to_int8(pgn_game.result);

    // Process all moves before any DB write — any SAN failure aborts entire
    // game
    auto board = chesslib::board {};
    std::vector<std::uint16_t> encoded_moves;
    std::vector<motif::db::position_row> position_rows;
    encoded_moves.reserve(pgn_game.moves.size());
    position_rows.reserve(pgn_game.moves.size() + 1);

    // Starting position row (ply = 0) so root-hash queries find data
    position_rows.push_back(motif::db::position_row {
        .zobrist_hash = board.hash(),
        .game_id = 0,
        .ply = 0,
        .result = result_int,
        .white_elo = white_elo,
        .black_elo = black_elo,
    });

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
    auto insert_res = db_.store().insert(db_game);
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
        auto batch_res = db_.positions().insert_batch(position_rows);
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
