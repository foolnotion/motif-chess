#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
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

struct continuation_key
{
    std::uint32_t game_id;
    std::uint16_t encoded_move;

    auto operator==(continuation_key const& other) const -> bool
    {
        return game_id == other.game_id && encoded_move == other.encoded_move;
    }
};

struct continuation_key_hash
{
    auto operator()(continuation_key const& key) const noexcept -> std::size_t
    {
        auto const h1 = std::hash<std::uint32_t> {}(key.game_id);
        auto const h2 = std::hash<std::uint16_t> {}(key.encoded_move);
        return h1 ^ (h2 << 1U);
    }
};

struct continuation_aggregate
{
    std::uint16_t encoded_move {};
    std::uint32_t frequency {};
    std::uint32_t white_wins {};
    std::uint32_t draws {};
    std::uint32_t black_wins {};
    std::int64_t white_elo_sum {};
    std::uint32_t white_elo_count {};
    std::int64_t black_elo_sum {};
    std::uint32_t black_elo_count {};
    std::map<std::string, std::uint32_t, std::less<>> eco_counts;
};

auto average_elo(std::int64_t const sum, std::uint32_t const count)
    -> std::optional<double>
{
    if (count == 0U) {
        return std::nullopt;
    }

    return static_cast<double>(sum) / static_cast<double>(count);
}

void note_result(continuation_aggregate& aggregate, std::int8_t const result)
{
    if (result > 0) {
        ++aggregate.white_wins;
        return;
    }
    if (result < 0) {
        ++aggregate.black_wins;
        return;
    }

    ++aggregate.draws;
}

void note_elo(continuation_aggregate& aggregate,
              std::optional<std::int16_t> const& white_elo,
              std::optional<std::int16_t> const& black_elo)
{
    if (white_elo.has_value()) {
        aggregate.white_elo_sum += static_cast<std::int64_t>(*white_elo);
        ++aggregate.white_elo_count;
    }
    if (black_elo.has_value()) {
        aggregate.black_elo_sum += static_cast<std::int64_t>(*black_elo);
        ++aggregate.black_elo_count;
    }
}

void note_opening_name(
    std::map<std::string, std::string, std::less<>>& eco_lookup,
    std::string const& eco,
    std::string const& opening_name)
{
    auto const [lookup_it, inserted] = eco_lookup.emplace(eco, opening_name);
    if (!inserted && opening_name.size() > lookup_it->second.size()) {
        lookup_it->second = opening_name;
    }
}

auto dominant_eco(continuation_aggregate const& aggregate)
    -> std::optional<std::string>
{
    if (aggregate.eco_counts.empty()) {
        return std::nullopt;
    }

    auto best = aggregate.eco_counts.begin();
    for (auto it = std::next(aggregate.eco_counts.begin());
         it != aggregate.eco_counts.end();
         ++it)
    {
        if (it->second > best->second) {
            best = it;
        }
    }

    return best->first;
}

auto replay_position(motif::db::game_context const& context,
                     std::uint16_t const ply)
    -> motif::search::result<chesslib::board>
{
    auto board = chesslib::board {};

    for (std::size_t index = 0; index < ply; ++index) {
        if (index >= context.moves.size()) {
            return tl::unexpected {motif::search::error_code::io_failure};
        }

        auto const move = chesslib::codec::decode(context.moves[index]);
        chesslib::move_maker maker {board, move};
        maker.make();
    }

    return board;
}

}  // namespace

namespace motif::search::opening_stats
{

auto query(motif::db::database_manager const& database,
           std::uint64_t const zobrist_hash) -> result<stats>
{
    auto opening_moves = database.positions().query_opening_moves(zobrist_hash);
    if (!opening_moves) {
        return tl::unexpected {error_code::io_failure};
    }

    if (opening_moves->empty()) {
        return stats {};
    }

    auto game_ids = std::vector<std::uint32_t> {};
    game_ids.reserve(opening_moves->size());
    for (auto const& move_row : *opening_moves) {
        game_ids.push_back(move_row.game_id);
    }
    std::ranges::sort(game_ids);
    game_ids.erase(std::unique(game_ids.begin(), game_ids.end()),
                   game_ids.end());

    auto contexts = database.store().get_game_contexts(game_ids);
    if (!contexts) {
        return tl::unexpected {error_code::io_failure};
    }

    auto position = chesslib::board {};
    bool position_set = false;
    for (auto const& move_row : *opening_moves) {
        auto const ctx_it = contexts->find(move_row.game_id);
        if (ctx_it == contexts->end()) {
            continue;
        }
        if (static_cast<std::size_t>(move_row.ply) >= ctx_it->second.moves.size()) {
            continue;
        }
        auto replayed = replay_position(ctx_it->second, move_row.ply);
        if (replayed) {
            position = std::move(*replayed);
            position_set = true;
            break;
        }
    }

    if (!position_set) {
        return stats {};
    }

    auto grouped = std::map<std::uint16_t, continuation_aggregate> {};
    auto eco_lookup = std::map<std::string, std::string, std::less<>> {};
    auto seen = std::unordered_map<continuation_key, bool, continuation_key_hash> {};

    for (auto const& move_row : *opening_moves) {
        auto const ctx_it = contexts->find(move_row.game_id);
        if (ctx_it == contexts->end()) {
            continue;
        }
        auto const& context = ctx_it->second;

        if (static_cast<std::size_t>(move_row.ply) >= context.moves.size()) {
            continue;
        }

        auto const encoded_continuation =
            context.moves[static_cast<std::size_t>(move_row.ply)];

        auto const key = continuation_key {move_row.game_id, encoded_continuation};
        if (seen.contains(key)) {
            continue;
        }
        seen.emplace(key, true);

        auto& aggregate =
            grouped.try_emplace(encoded_continuation, continuation_aggregate {})
                .first->second;
        aggregate.encoded_move = encoded_continuation;
        ++aggregate.frequency;
        note_result(aggregate, move_row.result);
        note_elo(aggregate, move_row.white_elo, move_row.black_elo);

        auto const eco = context.eco;
        if (eco.has_value()) {
            auto const& eco_value = eco.value();
            ++aggregate.eco_counts[eco_value];

            auto opening_name = context.opening_name;
            if (opening_name.has_value()) {
                note_opening_name(eco_lookup, eco_value, *opening_name);
            }
        }
    }

    auto output = stats {};
    output.continuations.reserve(grouped.size());

    for (auto const& [encoded_move, aggregate] : grouped) {
        auto eco = dominant_eco(aggregate);
        auto opening_name = std::optional<std::string> {};
        if (eco.has_value()) {
            if (auto lookup_it = eco_lookup.find(*eco);
                lookup_it != eco_lookup.end())
            {
                opening_name = lookup_it->second;
            }
        }

        auto const continuation_move = chesslib::codec::decode(encoded_move);
        auto const san = chesslib::san::to_string(position, continuation_move);

        output.continuations.push_back(continuation {
            .san = san,
            .frequency = aggregate.frequency,
            .white_wins = aggregate.white_wins,
            .draws = aggregate.draws,
            .black_wins = aggregate.black_wins,
            .average_white_elo =
                average_elo(aggregate.white_elo_sum, aggregate.white_elo_count),
            .average_black_elo =
                average_elo(aggregate.black_elo_sum, aggregate.black_elo_count),
            .eco = std::move(eco),
            .opening_name = std::move(opening_name),
        });
    }

    std::ranges::sort(
        output.continuations,
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