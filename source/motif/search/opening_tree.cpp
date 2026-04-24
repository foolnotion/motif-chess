#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "motif/search/opening_tree.hpp"

#include <chesslib/board/board.hpp>
#include <chesslib/board/move_codec.hpp>
#include <chesslib/util/san.hpp>
#include <tl/expected.hpp>

#include "motif/db/database_manager.hpp"
#include "motif/db/types.hpp"
#include "motif/search/error.hpp"
#include "motif/search/opening_stats.hpp"

namespace
{

struct occurrence_key
{
    std::uint32_t game_id;
    std::uint16_t root_ply;

    auto operator==(occurrence_key const& other) const -> bool { return game_id == other.game_id && root_ply == other.root_ply; }
};

struct occurrence_key_hash
{
    auto operator()(occurrence_key const& key) const noexcept -> std::size_t
    {
        constexpr std::size_t phi = 0x9e3779b97f4a7c15ULL;
        constexpr std::size_t lshift = 12U;
        constexpr std::size_t rshift = 4U;
        auto seed = std::hash<std::uint32_t> {}(key.game_id);
        seed ^= static_cast<std::size_t>(key.root_ply) + phi + (seed << lshift) + (seed >> rshift);
        return seed;
    }
};

struct node_key
{
    std::uint16_t depth;
    std::uint64_t parent_hash;

    auto operator==(node_key const& other) const -> bool { return depth == other.depth && parent_hash == other.parent_hash; }
};

struct node_key_hash
{
    auto operator()(node_key const& key) const noexcept -> std::size_t
    {
        constexpr std::size_t phi = 0x9e3779b97f4a7c15ULL;
        constexpr std::size_t lshift = 12U;
        constexpr std::size_t rshift = 4U;
        auto seed = std::hash<std::uint64_t> {}(key.parent_hash);
        seed ^= static_cast<std::size_t>(key.depth) + phi + (seed << lshift) + (seed >> rshift);
        return seed;
    }
};

struct tree_continuation_aggregate
{
    std::uint16_t encoded_move {};
    std::uint64_t result_hash {};
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

auto average_elo(std::int64_t const sum, std::uint32_t const count) -> std::optional<double>
{
    if (count == 0U) {
        return std::nullopt;
    }

    return static_cast<double>(sum) / static_cast<double>(count);
}

void note_result(tree_continuation_aggregate& aggregate, std::int8_t const result)
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

void note_elo(tree_continuation_aggregate& aggregate,
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

void note_opening_name(std::map<std::string, std::string, std::less<>>& eco_lookup, std::string const& eco, std::string const& opening_name)
{
    auto const [lookup_it, inserted] = eco_lookup.emplace(eco, opening_name);
    if (!inserted && opening_name.size() > lookup_it->second.size()) {
        lookup_it->second = opening_name;
    }
}

auto dominant_eco(tree_continuation_aggregate const& aggregate) -> std::optional<std::string>
{
    if (aggregate.eco_counts.empty()) {
        return std::nullopt;
    }

    auto best = aggregate.eco_counts.begin();
    for (auto it = std::next(aggregate.eco_counts.begin()); it != aggregate.eco_counts.end(); ++it) {
        if (it->second > best->second) {
            best = it;
        }
    }

    return best->first;
}

auto replay_position(motif::db::game_context const& context, std::uint16_t const ply) -> motif::search::result<chesslib::board>
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

struct node_aggregate
{
    std::map<std::uint16_t, tree_continuation_aggregate> continuations;
    std::map<std::string, std::string, std::less<>> eco_lookup;
    std::uint32_t sample_game_id {};
    std::uint16_t sample_root_ply {};
    bool has_sample {false};
};

auto build_continuation(tree_continuation_aggregate const& aggregate,
                        std::map<std::string, std::string, std::less<>> const& eco_lookup,
                        chesslib::board const& position,
                        bool const is_expanded,
                        std::uint64_t const child_hash) -> motif::search::opening_tree::node_continuation
{
    auto eco = dominant_eco(aggregate);
    auto opening_name = std::optional<std::string> {};
    if (eco.has_value()) {
        if (auto lookup_it = eco_lookup.find(*eco); lookup_it != eco_lookup.end()) {
            opening_name = lookup_it->second;
        }
    }

    auto const cont_move = chesslib::codec::decode(aggregate.encoded_move);
    auto san = chesslib::san::to_string(position, cont_move);

    auto child_node = std::make_unique<motif::search::opening_tree::node>(motif::search::opening_tree::node {
        .zobrist_hash = child_hash,
        .continuations = {},
        .is_expanded = is_expanded,
    });

    return motif::search::opening_tree::node_continuation {
        .san = std::move(san),
        .result_hash = aggregate.result_hash,
        .frequency = aggregate.frequency,
        .white_wins = aggregate.white_wins,
        .draws = aggregate.draws,
        .black_wins = aggregate.black_wins,
        .average_white_elo = average_elo(aggregate.white_elo_sum, aggregate.white_elo_count),
        .average_black_elo = average_elo(aggregate.black_elo_sum, aggregate.black_elo_count),
        .eco = std::move(eco),
        .opening_name = std::move(opening_name),
        .subtree = std::move(child_node),
    };
}

void sort_continuations(std::vector<motif::search::opening_tree::node_continuation>& conts)
{
    std::ranges::sort(
        conts,
        [](motif::search::opening_tree::node_continuation const& left, motif::search::opening_tree::node_continuation const& right) -> bool
        {
            if (left.frequency != right.frequency) {
                return left.frequency > right.frequency;
            }

            return left.san < right.san;
        });
}

}  // namespace

namespace motif::search::opening_tree
{

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto open(motif::db::database_manager const& database, std::uint64_t const root_hash, std::size_t const prefetch_depth) -> result<tree>
{
    if (prefetch_depth == 0U) {
        return tree {
            .root = node {.zobrist_hash = root_hash, .continuations = {}, .is_expanded = false},
            .prefetch_depth = prefetch_depth,
        };
    }

    if (prefetch_depth > static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max())) {
        return tl::unexpected {error_code::invalid_argument};
    }

    auto const max_depth = static_cast<std::uint16_t>(prefetch_depth);

    auto rows_res = database.positions().query_tree_slice(root_hash, max_depth);
    if (!rows_res) {
        return tl::unexpected {error_code::io_failure};
    }

    if (rows_res->empty()) {
        return tree {
            .root = node {.zobrist_hash = root_hash, .continuations = {}, .is_expanded = true},
            .prefetch_depth = prefetch_depth,
        };
    }

    auto game_ids = std::vector<std::uint32_t> {};
    game_ids.reserve(rows_res->size());
    for (auto const& row : *rows_res) {
        game_ids.push_back(row.game_id);
    }
    std::ranges::sort(game_ids);
    auto const unique_end = std::ranges::unique(game_ids);
    game_ids.erase(unique_end.begin(), unique_end.end());

    auto contexts_res = database.store().get_game_contexts(game_ids);
    if (!contexts_res) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const& contexts = *contexts_res;

    auto rows_by_occurrence = std::unordered_map<occurrence_key, std::vector<motif::db::tree_position_row>, occurrence_key_hash> {};
    for (auto const& row : *rows_res) {
        rows_by_occurrence[occurrence_key {
                               .game_id = row.game_id,
                               .root_ply = row.root_ply,
                           }]
            .push_back(row);
    }

    auto node_aggregates = std::unordered_map<node_key, node_aggregate, node_key_hash> {};

    // Process each root occurrence independently so repeated visits to the
    // same root position inside one game contribute separate counts and build
    // the correct descendant chain.
    for (auto& [occurrence, occurrence_rows] : rows_by_occurrence) {
        std::ranges::sort(occurrence_rows,
                          [](motif::db::tree_position_row const& left, motif::db::tree_position_row const& right) -> bool
                          { return left.depth < right.depth; });

        auto const ctx_it = contexts.find(occurrence.game_id);
        if (ctx_it == contexts.end()) {
            return tl::unexpected {error_code::io_failure};
        }
        auto const& context = ctx_it->second;

        auto root_board = replay_position(context, occurrence.root_ply);
        if (!root_board || root_board->hash() != root_hash) {
            return tl::unexpected {error_code::io_failure};
        }

        auto parent_hash = root_hash;
        for (auto const& row : occurrence_rows) {
            auto const move_ply = static_cast<std::size_t>(occurrence.root_ply + row.depth - 1U);
            if (move_ply >= context.moves.size()) {
                return tl::unexpected {error_code::io_failure};
            }

            auto const encoded_move = context.moves[move_ply];
            auto const nkey = node_key {.depth = row.depth, .parent_hash = parent_hash};
            auto& nag = node_aggregates.try_emplace(nkey, node_aggregate {}).first->second;
            if (!nag.has_sample) {
                nag.sample_game_id = occurrence.game_id;
                nag.sample_root_ply = occurrence.root_ply;
                nag.has_sample = true;
            }

            auto& aggregate = nag.continuations.try_emplace(encoded_move, tree_continuation_aggregate {}).first->second;
            aggregate.encoded_move = encoded_move;
            if (aggregate.frequency == 0U) {
                aggregate.result_hash = row.child_hash;
            }
            ++aggregate.frequency;
            note_result(aggregate, row.result);
            note_elo(aggregate, row.white_elo, row.black_elo);

            auto const eco = context.eco;
            if (eco.has_value()) {
                auto const& eco_value = eco.value();
                ++aggregate.eco_counts[eco_value];
                auto const& opening_name = context.opening_name;
                if (opening_name.has_value()) {
                    note_opening_name(nag.eco_lookup, eco_value, *opening_name);
                }
            }

            auto replayed = replay_position(context, static_cast<std::uint16_t>(occurrence.root_ply + row.depth));
            if (!replayed || replayed->hash() != row.child_hash) {
                return tl::unexpected {error_code::io_failure};
            }

            parent_hash = row.child_hash;
        }
    }

    // Now build the tree iteratively using BFS.
    // We'll build node objects from the aggregated data.
    // First, build all nodes at each depth and link them.
    // We need: for each (depth, parent_hash), build a node with
    // continuations, where each continuation's subtree points to the
    // child node at (depth+1, result_hash) if depth < max_depth,
    // or to a leaf node if depth == max_depth.

    // Collect all node_keys sorted by depth (ascending) for BFS order.
    auto all_keys = std::vector<node_key> {};
    all_keys.reserve(node_aggregates.size());
    for (auto const& [nkey, nag] : node_aggregates) {
        all_keys.push_back(nkey);
    }
    std::ranges::sort(all_keys,
                      [](node_key const& left, node_key const& right) -> bool
                      {
                          if (left.depth != right.depth) {
                              return left.depth < right.depth;
                          }
                          return left.parent_hash < right.parent_hash;
                      });

    // Map from (depth, parent_hash) → built node
    auto built_nodes = std::unordered_map<node_key, std::unique_ptr<node>, node_key_hash> {};

    // Build nodes bottom-up (from max_depth to 1)
    for (auto const& nkey : all_keys | std::views::reverse) {
        auto const nag_it = node_aggregates.find(nkey);
        if (nag_it == node_aggregates.end()) {
            continue;
        }
        auto const& nag = nag_it->second;

        auto const ctx_it = contexts.find(nag.sample_game_id);
        if (ctx_it == contexts.end()) {
            return tl::unexpected {error_code::io_failure};
        }

        auto const parent_ply = static_cast<std::uint16_t>(nag.sample_root_ply + nkey.depth - 1U);
        auto parent_board = replay_position(ctx_it->second, parent_ply);
        if (!parent_board || parent_board->hash() != nkey.parent_hash) {
            return tl::unexpected {error_code::io_failure};
        }

        auto current_node = std::make_unique<node>(node {.zobrist_hash = nkey.parent_hash, .continuations = {}, .is_expanded = true});

        for (auto const& [encoded_move, aggregate] : nag.continuations) {
            auto const child_key = node_key {.depth = static_cast<std::uint16_t>(nkey.depth + 1U), .parent_hash = aggregate.result_hash};
            auto child_it = built_nodes.find(child_key);
            auto const is_boundary = nkey.depth >= max_depth;
            auto cont = build_continuation(aggregate, nag.eco_lookup, *parent_board, !is_boundary, aggregate.result_hash);

            if (child_it != built_nodes.end()) {
                // Attach pre-built child subtree
                cont.subtree = std::move(child_it->second);
            }

            current_node->continuations.push_back(std::move(cont));
        }

        sort_continuations(current_node->continuations);
        built_nodes[nkey] = std::move(current_node);
    }

    // The root node is at depth 0, but it's built from depth-1
    // aggregates. The root node key doesn't match the pattern —
    // we need to special-case it.
    // Actually the root is: its continuations come from depth=1
    // nodes whose parent_hash == root_hash.
    auto result_tree = tree {};
    result_tree.root.zobrist_hash = root_hash;
    result_tree.root.is_expanded = true;
    result_tree.prefetch_depth = prefetch_depth;

    // Find the root's children: depth=1, parent_hash=root_hash
    auto const root_key = node_key {.depth = 1U, .parent_hash = root_hash};
    auto root_node_it = built_nodes.find(root_key);
    if (root_node_it == built_nodes.end()) {
        return tl::unexpected {error_code::io_failure};
    }

    result_tree.root = std::move(*root_node_it->second);
    result_tree.root.zobrist_hash = root_hash;
    result_tree.root.is_expanded = true;

    return result_tree;
}

auto expand(motif::db::database_manager const& database, node& n) -> result<void>
{
    if (n.is_expanded) {
        return {};
    }

    auto stats_res = opening_stats::query(database, n.zobrist_hash);
    if (!stats_res) {
        return tl::unexpected {stats_res.error()};
    }

    n.continuations.reserve(stats_res->continuations.size());
    for (auto const& cont : stats_res->continuations) {
        n.continuations.push_back(node_continuation {
            .san = cont.san,
            .result_hash = cont.result_hash,
            .frequency = cont.frequency,
            .white_wins = cont.white_wins,
            .draws = cont.draws,
            .black_wins = cont.black_wins,
            .average_white_elo = cont.average_white_elo,
            .average_black_elo = cont.average_black_elo,
            .eco = cont.eco,
            .opening_name = cont.opening_name,
            .subtree = std::make_unique<node>(node {
                .zobrist_hash = cont.result_hash,
                .continuations = {},
                .is_expanded = false,
            }),
        });
    }
    n.is_expanded = true;
    return {};
}

}  // namespace motif::search::opening_tree
