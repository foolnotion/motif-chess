#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "motif/search/opening_stats.hpp"

#include <gtl/phmap.hpp>
#include <tl/expected.hpp>

#include "motif/chess/chess.hpp"
#include "motif/db/database_manager.hpp"
#include "motif/db/types.hpp"
#include "motif/search/error.hpp"

namespace
{

using context_map = gtl::flat_hash_map<motif::db::game_id, motif::db::game_context>;

auto find_root_board(std::vector<motif::db::opening_stat_agg_row> const& rows,
                     motif::db::zobrist_hash const zobrist_hash,
                     context_map const& contexts) -> motif::search::result<motif::chess::board>
{
    auto const root_ply = rows.front().root_ply;
    for (auto const& row : rows) {
        for (auto const candidate_id : {row.eco_sample_min, row.eco_sample_max}) {
            auto const ctx_it = contexts.find(candidate_id);
            if (ctx_it == contexts.end()) {
                continue;
            }
            auto replayed = motif::chess::replay(ctx_it->second.moves, root_ply);
            if (replayed && motif::db::zobrist_hash {replayed->hash()} == zobrist_hash) {
                return *replayed;
            }
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

namespace
{

auto build_stats(motif::db::database_manager const& database,
                 motif::db::zobrist_hash const hash,
                 std::vector<motif::db::opening_stat_agg_row> const& rows,
                 std::uint32_t const total_games) -> motif::search::result<motif::search::opening_stats::stats>
{
    if (rows.empty()) {
        motif::search::opening_stats::stats out;
        out.total_games = total_games;
        return out;
    }

    auto candidate_ids = std::vector<motif::db::game_id> {};
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
        return tl::unexpected {motif::search::error_code::io_failure};
    }
    auto const& contexts = *contexts_res;

    auto position = motif::chess::board {};
    if (rows.front().root_ply > 0U) {
        auto board_res = find_root_board(rows, hash, contexts);
        if (!board_res) {
            return motif::search::opening_stats::stats {};
        }
        position = *board_res;
    }

    motif::search::opening_stats::stats output;
    output.total_games = total_games;
    output.continuations.reserve(rows.size());

    for (auto const& row : rows) {
        if (!contexts.contains(row.eco_sample_min) && !contexts.contains(row.eco_sample_max)) {
            continue;
        }

        auto eco_res = resolve_eco(row, contexts);
        auto [eco, opening_name] =
            eco_res ? std::move(*eco_res) : std::make_pair(std::optional<std::string> {}, std::optional<std::string> {});

        auto const san = motif::chess::san(position, row.cont_encoded_move);

        output.continuations.push_back(motif::search::opening_stats::continuation {
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
            .elo_weighted_score = row.elo_weighted_score.value_or(0.0),
        });
    }

    std::ranges::sort(
        output.continuations,
        [](motif::search::opening_stats::continuation const& left, motif::search::opening_stats::continuation const& right) -> bool
        {
            if (left.frequency != right.frequency) {
                return left.frequency > right.frequency;
            }
            return left.san < right.san;
        });

    return output;
}

}  // namespace

namespace motif::search::opening_stats
{

auto query(motif::db::database_manager const& database, motif::db::zobrist_hash const hash) -> result<stats>
{
    auto total_count_res = database.positions().count_distinct_games_by_zobrist(hash);
    if (!total_count_res) {
        return tl::unexpected {error_code::io_failure};
    }
    if (*total_count_res == 0) {
        return stats {};
    }

    auto rows_res = database.positions().query_opening_stats(hash);
    if (!rows_res) {
        return tl::unexpected {error_code::io_failure};
    }

    return build_stats(database, hash, *rows_res, static_cast<std::uint32_t>(*total_count_res));
}

auto query_elo_distribution(motif::db::database_manager const& database,
                            motif::db::zobrist_hash const hash,
                            motif::db::search_filter const& filter,
                            int const bucket_width) -> result<std::vector<motif::db::elo_distribution_row>>
{
    auto res = database.query_elo_distribution(hash, filter, bucket_width);
    if (!res) {
        return tl::unexpected {error_code::io_failure};
    }
    return std::move(*res);
}

auto query(motif::db::database_manager const& database, motif::db::zobrist_hash const hash, motif::db::search_filter const& filter)
    -> result<stats>
{
    bool const has_metadata = filter.player_name.has_value() || filter.min_elo.has_value() || filter.max_elo.has_value()
        || filter.result.has_value() || filter.eco_prefix.has_value();

    if (!has_metadata) {
        return query(database, hash);
    }

    auto all_ids_res = database.positions().distinct_game_ids_by_zobrist(hash);
    if (!all_ids_res) {
        return tl::unexpected {error_code::io_failure};
    }
    if (all_ids_res->empty()) {
        return stats {};
    }

    auto meta_filter = filter;
    meta_filter.position = std::nullopt;

    auto filtered_ids_res = database.store().find_game_ids_with_filter(*all_ids_res, meta_filter);
    if (!filtered_ids_res) {
        return tl::unexpected {error_code::io_failure};
    }
    if (filtered_ids_res->empty()) {
        return stats {};
    }
    auto const& filtered_ids = *filtered_ids_res;

    auto rows_res = database.positions().query_opening_stats(hash, filtered_ids);
    if (!rows_res) {
        return tl::unexpected {error_code::io_failure};
    }

    return build_stats(database, hash, *rows_res, static_cast<std::uint32_t>(filtered_ids.size()));
}

}  // namespace motif::search::opening_stats
