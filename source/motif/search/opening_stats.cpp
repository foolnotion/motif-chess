#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "motif/search/opening_stats.hpp"

#include <chesslib/board/board.hpp>
#include <chesslib/board/move_codec.hpp>
#include <chesslib/util/san.hpp>
#include <tl/expected.hpp>

#include "motif/db/database_manager.hpp"
#include "motif/db/types.hpp"
#include "motif/search/error.hpp"

namespace
{

auto replay_position(std::span<std::uint16_t const> moves, std::uint16_t const ply) -> motif::search::result<chesslib::board>
{
    auto board = chesslib::board {};

    for (std::size_t index = 0; index < ply; ++index) {
        if (index >= moves.size()) {
            return tl::unexpected {motif::search::error_code::io_failure};
        }

        auto const move = chesslib::codec::decode(moves[index]);
        chesslib::move_maker maker {board, move};
        maker.make();
    }

    return board;
}

using context_map = std::unordered_map<std::uint32_t, motif::db::game_context>;

auto find_root_board(std::vector<motif::db::opening_stat_agg_row> const& rows, context_map const& contexts)
    -> motif::search::result<chesslib::board>
{
    auto const root_ply = rows.front().root_ply;
    for (auto const& row : rows) {
        for (auto const candidate_id : {row.eco_sample_min, row.eco_sample_max}) {
            auto const ctx_it = contexts.find(candidate_id);
            if (ctx_it == contexts.end()) {
                continue;
            }
            auto replayed = replay_position(ctx_it->second.moves, root_ply);
            if (replayed) {
                return *replayed;
            }
            break;
        }
    }
    return tl::unexpected {motif::search::error_code::io_failure};
}

auto resolve_eco(motif::db::opening_stat_agg_row const& row, context_map const& contexts)
    -> std::optional<std::pair<std::optional<std::string>, std::optional<std::string>>>
{
    for (auto const candidate_id : {row.eco_sample_min, row.eco_sample_max}) {
        auto const ctx_it = contexts.find(candidate_id);
        if (ctx_it == contexts.end()) {
            continue;
        }
        return std::make_pair(ctx_it->second.eco, ctx_it->second.opening_name);
    }
    return std::nullopt;
}

}  // namespace

namespace motif::search::opening_stats
{

auto query(motif::db::database_manager const& database, std::uint64_t const zobrist_hash) -> result<stats>
{
    auto total_count_res = database.positions().count_distinct_games_by_zobrist(zobrist_hash);
    if (!total_count_res) {
        return tl::unexpected {error_code::io_failure};
    }
    if (*total_count_res == 0) {
        return stats {};
    }

    auto rows_res = database.positions().query_opening_stats(zobrist_hash);
    if (!rows_res) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const& rows = *rows_res;

    auto candidate_ids = std::vector<std::uint32_t> {};
    candidate_ids.reserve(rows.size() * 2);
    for (auto const& row : rows) {
        candidate_ids.push_back(row.eco_sample_min);
        if (row.eco_sample_max != row.eco_sample_min) {
            candidate_ids.push_back(row.eco_sample_max);
        }
    }
    std::ranges::sort(candidate_ids);
    auto const tail = std::ranges::unique(candidate_ids);
    candidate_ids.erase(tail.begin(), tail.end());

    auto contexts_res = database.store().get_game_contexts(candidate_ids);
    if (!contexts_res) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const& contexts = *contexts_res;

    auto position = chesslib::board {};
    if (!rows.empty() && rows.front().root_ply > 0U) {
        auto board_res = find_root_board(rows, contexts);
        if (!board_res) {
            return stats {};
        }
        position = *board_res;
    }

    auto output = stats {};
    output.total_games = static_cast<std::uint32_t>(*total_count_res);
    output.continuations.reserve(rows.size());

    for (auto const& row : rows) {
        auto eco_res = resolve_eco(row, contexts);
        if (!eco_res) {
            continue;
        }
        auto [eco, opening_name] = std::move(*eco_res);

        auto const cont_move = chesslib::codec::decode(row.cont_encoded_move);
        auto const san = chesslib::san::to_string(position, cont_move);

        output.continuations.push_back(continuation {
            .san = san,
            .result_hash = row.cont_hash,
            .frequency = row.frequency,
            .white_wins = row.white_wins,
            .draws = row.draws,
            .black_wins = row.black_wins,
            .average_white_elo = row.avg_white_elo,
            .average_black_elo = row.avg_black_elo,
            .eco = std::move(eco),
            .opening_name = std::move(opening_name),
        });
    }

    std::ranges::sort(output.continuations,
                      [](continuation const& left, continuation const& right) -> bool
                      {
                          if (left.frequency != right.frequency) {
                              return left.frequency > right.frequency;
                          }

                          return left.san < right.san;
                      });

    return output;
}

}  // namespace motif::search::opening_stats
