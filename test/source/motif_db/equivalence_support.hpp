#pragma once

// Shared equivalence machinery for the PR 6 tests: comparison helpers, independent
// canonical-replay oracles (no shared helper code with the indexes under test) and
// the "patched bundle answers exactly like a fresh rebuild of the same canonical
// data" checker used by the composite-snapshot and the edit-publication tests.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "composite_test_support.hpp"
#include "motif/chess/chess.hpp"
#include "motif/db/database_manager.hpp"
#include "motif/db/opening_tree_index.hpp"

namespace motif_test
{

using namespace motif::db;  // NOLINT(google-build-using-namespace) -- the helpers touch most of motif::db; qualifying every name buries the
                            // checks.

// ── Comparison helpers ───────────────────────────────────────────────────────

inline auto rows_equal(std::vector<opening_stat_agg_row> const& left, std::vector<opening_stat_agg_row> const& right) -> bool
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        auto const& lhs = left[index];
        auto const& rhs = right[index];
        if (lhs.cont_encoded_move != rhs.cont_encoded_move || lhs.cont_hash != rhs.cont_hash || lhs.root_ply != rhs.root_ply
            || lhs.frequency != rhs.frequency || lhs.transposition_frequency != rhs.transposition_frequency
            || lhs.white_wins != rhs.white_wins || lhs.draws != rhs.draws || lhs.black_wins != rhs.black_wins
            || lhs.avg_white_elo != rhs.avg_white_elo || lhs.avg_black_elo != rhs.avg_black_elo || lhs.eco_sample_min != rhs.eco_sample_min
            || lhs.eco_sample_max != rhs.eco_sample_max || lhs.elo_weighted_score != rhs.elo_weighted_score)
        {
            return false;
        }
    }
    return true;
}

inline auto distribution_equal(std::vector<elo_distribution_row> const& left, std::vector<elo_distribution_row> const& right) -> bool
{
    return left.size() == right.size()
        && std::ranges::equal(left,
                              right,
                              [](elo_distribution_row const& lhs, elo_distribution_row const& rhs) -> bool
                              {
                                  return lhs.encoded_move == rhs.encoded_move && lhs.elo_bucket_floor == rhs.elo_bucket_floor
                                      && lhs.white_wins == rhs.white_wins && lhs.draws == rhs.draws && lhs.black_wins == rhs.black_wins
                                      && lhs.game_count == rhs.game_count;
                              });
}

// ── Independent canonical-replay oracles (no shared helper code) ─────────────

struct oracle_edge
{
    std::int64_t frequency {};
    std::int64_t white_wins {};
    std::int64_t draws {};
    std::int64_t black_wins {};
    std::int64_t white_sum {};
    std::int64_t white_count {};
    std::int64_t black_sum {};
    std::int64_t black_count {};
    std::int64_t numerator2 {};
    std::int64_t weight2 {};
};

inline auto result_sign(std::string const& result) -> int
{
    if (result == "1-0") {
        return 1;
    }
    if (result == "0-1") {
        return -1;
    }
    return 0;
}

// Index of a result sign in a {white, draw, black} triple.
inline auto result_slot(int const sign) -> std::size_t
{
    if (sign > 0) {
        return 0U;
    }
    if (sign == 0) {
        return 1U;
    }
    return 2U;
}

// Per-game distinct (move, child) edges leaving `root`, at any ply.
inline auto oracle_edges(std::vector<model_game> const& corpus, std::uint64_t const root)
    -> std::map<std::pair<std::uint16_t, std::uint64_t>, oracle_edge>
{
    auto edges = std::map<std::pair<std::uint16_t, std::uint64_t>, oracle_edge> {};
    for (auto const& game : corpus) {
        auto const white_elo = game.white_elo;
        auto const black_elo = game.black_elo;
        auto board = motif::chess::board {};
        auto seen = std::set<std::pair<std::uint16_t, std::uint64_t>> {};
        for (auto const move : game.moves) {
            auto const here = board.hash();
            motif::chess::apply_encoded_move(board, move);
            if (here != root || !seen.emplace(move, board.hash()).second) {
                continue;
            }
            auto& edge = edges[{move, board.hash()}];
            auto const sign = result_sign(game.result);
            edge.frequency += 1;
            edge.white_wins += static_cast<std::int64_t>(sign > 0);
            edge.draws += static_cast<std::int64_t>(sign == 0);
            edge.black_wins += static_cast<std::int64_t>(sign < 0);
            if (white_elo) {
                edge.white_sum += *white_elo;
                edge.white_count += 1;
            }
            if (black_elo) {
                edge.black_sum += *black_elo;
                edge.black_count += 1;
            }
            if (white_elo && black_elo) {
                auto const rating_sum = *white_elo + *black_elo;
                edge.weight2 += rating_sum;
                edge.numerator2 += static_cast<std::int64_t>(sign) * rating_sum;
            }
        }
    }
    return edges;
}

// (move, bucket) -> {white, draw, black}; the manager's documented arithmetic:
// integer average toward zero, bucket = (avg / width) * width, one count per
// distinct (game, move) at the root, both ratings required; interior buckets of a
// move are zero-filled.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- a root hash and a bucket width; every caller passes them in this order.
inline auto oracle_distribution(std::vector<model_game> const& corpus, std::uint64_t const root, int const width)
    -> std::vector<elo_distribution_row>
{
    auto buckets = std::map<std::pair<std::uint16_t, std::int32_t>, std::array<std::uint32_t, 3>> {};
    for (auto const& game : corpus) {
        auto const white_elo = game.white_elo;
        auto const black_elo = game.black_elo;
        if (!white_elo || !black_elo) {
            continue;
        }
        auto board = motif::chess::board {};
        auto seen = std::set<std::uint16_t> {};
        for (auto const move : game.moves) {
            auto const here = board.hash();
            motif::chess::apply_encoded_move(board, move);
            if (here != root || !seen.insert(move).second) {
                continue;
            }
            auto const average = (*white_elo + *black_elo) / 2;  // C++ division: toward zero
            auto const bucket = (average / width) * width;
            auto& counts = buckets[{move, bucket}];
            auto const sign = result_sign(game.result);
            counts.at(result_slot(sign)) += 1;
        }
    }
    auto rows = std::vector<elo_distribution_row> {};
    auto moves = std::set<std::uint16_t> {};
    for (auto const& [key, counts] : buckets) {
        moves.insert(key.first);
    }
    for (auto const move : moves) {
        auto low = std::numeric_limits<std::int32_t>::max();
        auto high = std::numeric_limits<std::int32_t>::min();
        for (auto const& [key, counts] : buckets) {
            if (key.first == move) {
                low = std::min(low, key.second);
                high = std::max(high, key.second);
            }
        }
        for (auto bucket = low; bucket <= high; bucket += width) {
            auto const found = buckets.find({move, bucket});
            auto const counts = found == buckets.end() ? std::array<std::uint32_t, 3> {} : found->second;
            rows.push_back(elo_distribution_row {.encoded_move = move,
                                                 .elo_bucket_floor = bucket,
                                                 .white_wins = counts[0],
                                                 .draws = counts[1],
                                                 .black_wins = counts[2],
                                                 .game_count = counts[0] + counts[1] + counts[2]});
        }
    }
    return rows;
}

// Hashes worth probing: the start, positions along a few games (within and past the
// horizon), and an absent one.
inline auto probe_hashes(std::vector<model_game> const& corpus, std::mt19937_64& rng) -> std::vector<std::uint64_t>
{
    constexpr auto absent_hash = std::uint64_t {0xDEADBEEF};
    constexpr auto probed_games = 6;
    constexpr auto probe_ply_limit = std::size_t {26};
    auto hashes = std::set<std::uint64_t> {start_hash(), absent_hash};
    for (auto round = 0; round < probed_games; ++round) {
        auto const& game = corpus[std::uniform_int_distribution<std::size_t> {0, corpus.size() - 1}(rng)];
        for (std::size_t ply = 0; ply <= game.moves.size() && ply <= probe_ply_limit; ply += 2) {
            hashes.insert(hash_after(game.moves, ply));
        }
    }
    return {hashes.begin(), hashes.end()};
}

// The live game ids a patched bundle reports, one per corpus game (ascending). A fresh rebuild numbers the same corpus 1..N, so its ids
// are translated back through this list before comparing.
inline auto remap_matches(std::vector<position_match> matches, std::vector<game_id> const& ids) -> std::vector<position_match>
{
    for (auto& match : matches) {
        match.game_id = ids.at(match.game_id.value - 1U);
    }
    return matches;
}

inline auto contiguous_ids(std::size_t const count) -> std::vector<game_id>
{
    auto ids = std::vector<game_id> {};
    for (std::size_t index = 0; index < count; ++index) {
        ids.push_back(game_id {static_cast<std::uint32_t>(index + 1U)});
    }
    return ids;
}

// A fresh rebuild numbers the corpus 1..N; the patched bundle keeps its original (gapped) ids. Zero ("no game") stays zero.
inline auto remap_game_number(std::uint32_t const value, std::vector<game_id> const& ids) -> std::uint32_t
{
    return value == 0U ? 0U : ids.at(value - 1U).value;
}

// Aggregate rows carry two game ids (the lowest and highest game playing a continuation); translate a fresh rebuild's into the patched
// bundle's id space.
inline auto remap_rows(std::vector<opening_stat_agg_row> rows, std::vector<game_id> const& ids) -> std::vector<opening_stat_agg_row>
{
    for (auto& row : rows) {
        row.eco_sample_min = game_id {remap_game_number(row.eco_sample_min.value, ids)};
        row.eco_sample_max = game_id {remap_game_number(row.eco_sample_max.value, ids)};
    }
    return rows;
}

// A merged tree edge of the patched composite against the same edge of a fresh checkpoint. The additive statistics must be equal, and so
// must the non-additive ones: the minimum root ply and the sample game-id bounds stay certified through every edit and every removal (a
// removal carries the exact extrema of the remaining games), with sample ids in the patched bundle's id space. A postings child-frequency
// mode does not trust the cached child frequency, which is resolved from the live postings at presentation.
inline auto edges_equivalent(raw_edge const& got, raw_edge const& want, std::vector<game_id> const& ids, bool const cached_child_frequency)
    -> bool
{
    if (got.key != want.key || !(got.stats == want.stats)) {
        return false;
    }
    if (!got.min_root_ply_valid || !got.sample_bounds_valid || got.min_root_ply != want.min_root_ply
        || got.sample_min != remap_game_number(want.sample_min, ids) || got.sample_max != remap_game_number(want.sample_max, ids))
    {
        return false;
    }
    return !cached_child_frequency || got.child_frequency == want.child_frequency;
}

inline auto nodes_equivalent(std::vector<raw_node> const& got,
                             std::vector<raw_node> const& want,
                             std::vector<game_id> const& ids,
                             bool const cached_child_frequency) -> bool
{
    if (got.size() != want.size()) {
        return false;
    }
    for (std::size_t node = 0; node < got.size(); ++node) {
        auto const& lhs = got[node];
        auto const& rhs = want[node];
        if (lhs.root_hash != rhs.root_hash || lhs.game_count != rhs.game_count || lhs.complete != rhs.complete
            || lhs.edges.size() != rhs.edges.size())
        {
            return false;
        }
        for (std::size_t edge = 0; edge < lhs.edges.size(); ++edge) {
            if (!edges_equivalent(lhs.edges[edge], rhs.edges[edge], ids, cached_child_frequency)) {
                return false;
            }
        }
    }
    return true;
}

// Every query family on the patched bundle must equal a fresh rebuild from the
// canonical data, and the unfiltered/Elo answers must equal an independent oracle.
// One linear sequence of Catch2 assertions per query family; splitting it would scatter the shared fixtures.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
inline void require_equivalent_to_fresh_ids(database_manager& mgr,
                                            std::filesystem::path const& bundle,
                                            std::vector<model_game> const& corpus,
                                            std::vector<game_id> const& ids,
                                            bool const with_tree,
                                            std::uint64_t const seed)
{
    auto rng = std::mt19937_64 {seed};
    temp_dir const fresh_dir {"fresh"};
    auto fresh = build_bundle(fresh_dir.path / "db", corpus, with_tree);

    auto const status = mgr.derived_index_status();
    REQUIRE(status.has_value());
    REQUIRE(status->postings_usable);
    REQUIRE(status->tree_usable == with_tree);

    if (with_tree) {
        // Raw sufficient statistics, node by node, against a fresh checkpoint.
        auto composite = load_chain_tree(bundle, mgr.manifest());
        REQUIRE(composite.has_value());
        auto fresh_index = opening_tree_index::open(fresh_dir.path / "db" / fresh.manifest().tree()->checkpoint.filename);
        REQUIRE(fresh_index.has_value());
        REQUIRE(nodes_equivalent(collect_nodes(*composite),
                                 collect_nodes(*fresh_index),
                                 ids,
                                 mgr.manifest().tree()->child_frequency_mode == tree_child_frequency_mode::checkpoint));
        REQUIRE(composite->source_game_count() == corpus.size());
    }

    REQUIRE(ids.size() == corpus.size());
    auto const& all_ids = ids;
    auto const fresh_all_ids = contiguous_ids(corpus.size());
    auto some_ids = std::vector<game_id> {};
    auto fresh_some_ids = std::vector<game_id> {};
    for (std::size_t index = 0; index < all_ids.size(); index += 2) {
        some_ids.push_back(all_ids[index]);
        fresh_some_ids.push_back(fresh_all_ids[index]);
    }

    for (auto const hash : probe_hashes(corpus, rng)) {
        auto const key = zobrist_hash {hash};
        INFO("hash " << hash);
        auto const got = mgr.query_unfiltered_opening_stats(key);
        auto const want = fresh.query_unfiltered_opening_stats(key);
        REQUIRE(got.has_value());
        REQUIRE(want.has_value());
        REQUIRE(rows_equal(*got, remap_rows(*want, ids)));

        // The independent oracle agrees on the quantities it defines.
        auto const expected_edges = oracle_edges(corpus, hash);
        REQUIRE(expected_edges.size() == got->size());
        for (auto const& row : *got) {
            auto const found = expected_edges.find({row.cont_encoded_move, row.cont_hash.value});
            REQUIRE(found != expected_edges.end());
            auto const& edge = found->second;
            CHECK(std::cmp_equal(row.frequency, edge.frequency));
            CHECK(std::cmp_equal(row.white_wins, edge.white_wins));
            CHECK(std::cmp_equal(row.draws, edge.draws));
            CHECK(std::cmp_equal(row.black_wins, edge.black_wins));
            CHECK(row.avg_white_elo
                  == (edge.white_count == 0
                          ? std::optional<double> {}
                          : std::optional<double> {static_cast<double>(edge.white_sum) / static_cast<double>(edge.white_count)}));
            CHECK(row.avg_black_elo
                  == (edge.black_count == 0
                          ? std::optional<double> {}
                          : std::optional<double> {static_cast<double>(edge.black_sum) / static_cast<double>(edge.black_count)}));
            CHECK(row.elo_weighted_score
                  == (edge.weight2 > 0 ? std::optional<double> {static_cast<double>(edge.numerator2) / static_cast<double>(edge.weight2)}
                                       : std::optional<double> {}));
        }

        auto const filtered_got = mgr.query_filtered_opening_stats(key, some_ids);
        auto const filtered_want = fresh.query_filtered_opening_stats(key, fresh_some_ids);
        REQUIRE(filtered_got.has_value());
        REQUIRE(filtered_want.has_value());
        REQUIRE(rows_equal(*filtered_got, remap_rows(*filtered_want, ids)));

        for (auto const width : {25, 100}) {
            auto const dist_got = mgr.query_elo_distribution(key, search_filter {}, width);
            auto const dist_want = fresh.query_elo_distribution(key, search_filter {}, width);
            REQUIRE(dist_got.has_value());
            REQUIRE(dist_want.has_value());
            REQUIRE(distribution_equal(*dist_got, *dist_want));
            REQUIRE(distribution_equal(*dist_got, oracle_distribution(corpus, hash, width)));
        }
        // A filter whose membership depends on the edited values.
        auto filter = search_filter {};
        filter.result = "1-0";
        auto const filtered_dist_got = mgr.query_elo_distribution(key, filter, 50);
        auto const filtered_dist_want = fresh.query_elo_distribution(key, filter, 50);
        REQUIRE(filtered_dist_got.has_value());
        REQUIRE(filtered_dist_want.has_value());
        REQUIRE(distribution_equal(*filtered_dist_got, *filtered_dist_want));
        filter = search_filter {};
        constexpr auto filter_min_elo = 2000;
        filter.min_elo = filter_min_elo;
        auto const min_got = mgr.query_elo_distribution(key, filter, 50);
        auto const min_want = fresh.query_elo_distribution(key, filter, 50);
        REQUIRE(min_got.has_value());
        REQUIRE(min_want.has_value());
        REQUIRE(distribution_equal(*min_got, *min_want));

        auto const matches_got = mgr.query_position_matches(key);
        auto const matches_want = fresh.query_position_matches(key);
        REQUIRE(matches_got.has_value());
        REQUIRE(matches_want.has_value());
        REQUIRE(same_matches(*matches_got, remap_matches(*matches_want, ids)));
        auto const first_got = mgr.query_position_first_matches(key, all_ids);
        auto const first_want = fresh.query_position_first_matches(key, fresh_all_ids);
        REQUIRE(first_got.has_value());
        REQUIRE(first_want.has_value());
        REQUIRE(same_matches(*first_got, remap_matches(*first_want, ids)));

        auto list_filter = search_filter {};
        list_filter.position = key;
        constexpr auto list_min_elo = 1000;
        list_filter.min_elo = list_min_elo;
        auto const games_got = mgr.find_games(list_filter);
        auto const games_want = fresh.find_games(list_filter);
        REQUIRE(games_got.has_value());
        REQUIRE(games_want.has_value());
        REQUIRE(games_got->total_count == games_want->total_count);
        REQUIRE(games_got->games.size() == games_want->games.size());
        for (std::size_t index = 0; index < games_got->games.size(); ++index) {
            CHECK(games_got->games[index].id == ids.at(games_want->games[index].id.value - 1U));
            CHECK(games_got->games[index].result == games_want->games[index].result);
            CHECK(games_got->games[index].white_elo == games_want->games[index].white_elo);
        }

        auto const summary_got = mgr.position_summary(key);
        auto const summary_want = fresh.position_summary(key);
        REQUIRE(summary_got.has_value());
        REQUIRE(summary_want.has_value());
        REQUIRE(summary_got->has_value() == summary_want->has_value());
        if (summary_got->has_value() && summary_want->has_value()) {
            CHECK(summary_got->value().occurrence_count == summary_want->value().occurrence_count);
            CHECK(summary_got->value().distinct_game_count == summary_want->value().distinct_game_count);
            CHECK(summary_got->value().min_ply == summary_want->value().min_ply);
            CHECK(summary_got->value().max_ply == summary_want->value().max_ply);
        }
    }
}

// Every query family on a bundle whose games kept ids 1..N must equal a fresh rebuild.
inline void require_equivalent_to_fresh(database_manager& mgr,
                                        std::filesystem::path const& bundle,
                                        std::vector<model_game> const& corpus,
                                        bool const with_tree,
                                        std::uint64_t const seed)
{
    require_equivalent_to_fresh_ids(mgr, bundle, corpus, contiguous_ids(corpus.size()), with_tree, seed);
}

}  // namespace motif_test
