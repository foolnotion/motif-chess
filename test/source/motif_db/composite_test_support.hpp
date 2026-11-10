#pragma once

// Shared fixtures for the PR 6a tree/metadata patch, composite-snapshot and
// equivalence tests: deterministic legal-game generators (including repeated
// positions, late returns to the start position beyond the horizon, zero-move
// games and terminal positions), temp directories, and a loader that rebuilds
// the composite tree from a manifest exactly as the manager does.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "motif/chess/chess.hpp"
#include "motif/db/database_manager.hpp"
#include "motif/db/game_store.hpp"
#include "motif/db/manifest.hpp"
#include "motif/db/opening_tree_composite.hpp"
#include "motif/db/opening_tree_index.hpp"
#include "motif/db/opening_tree_patch.hpp"
#include "motif/db/types.hpp"

namespace motif_test
{

struct temp_dir
{
    std::filesystem::path path;

    explicit temp_dir(std::string_view const tag)
    {
        static auto counter = std::uint64_t {0};
        auto const tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path()
            / ("motif_pr6a_" + std::string {tag} + "_" + std::to_string(tick) + "_" + std::to_string(counter++));
        std::filesystem::create_directories(path);
    }

    ~temp_dir()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    temp_dir(temp_dir const&) = delete;
    auto operator=(temp_dir const&) -> temp_dir& = delete;
    temp_dir(temp_dir&&) = delete;
    auto operator=(temp_dir&&) -> temp_dir& = delete;
};

// The value of an optional the test requires to be engaged. A bare dereference after REQUIRE is invisible to the optional-access analysis
// (Catch2's assertion macros hide the check from it), so an absent value throws instead; Catch2 reports the exception as a failed test.
template<typename T>
[[nodiscard]] auto require_value(std::optional<T>& value) -> T&
{
    if (!value.has_value()) {
        throw std::logic_error {"a required optional value is absent"};
    }
    return *value;
}

template<typename T>
[[nodiscard]] auto require_value(std::optional<T> const& value) -> T const&
{
    if (!value.has_value()) {
        throw std::logic_error {"a required optional value is absent"};
    }
    return *value;
}

template<typename T>
[[nodiscard]] auto require_value(std::optional<T>&& value) -> T
{
    auto taken = std::move(value);
    if (!taken.has_value()) {
        throw std::logic_error {"a required optional value is absent"};
    }
    return std::move(*taken);
}

inline auto start_hash() -> std::uint64_t
{
    return motif::chess::board {}.hash();
}

inline auto uci_moves(std::initializer_list<std::string_view> const moves) -> std::vector<std::uint16_t>
{
    auto board = motif::chess::board {};
    auto encoded = std::vector<std::uint16_t> {};
    for (auto const uci : moves) {
        auto const applied = motif::chess::apply_uci(board, uci);
        REQUIRE(applied.has_value());
        encoded.push_back(applied->encoded_move);
    }
    return encoded;
}

// The position after `moves` from the start.
inline auto hash_after(std::vector<std::uint16_t> const& moves, std::size_t const ply) -> std::uint64_t
{
    auto board = motif::chess::board {};
    for (std::size_t index = 0; index < ply && index < moves.size(); ++index) {
        motif::chess::apply_encoded_move(board, moves[index]);
    }
    return board.hash();
}

// Random legal mainline of at most max_plies plies (shorter at a terminal
// position).
inline auto random_game_moves(std::mt19937_64& rng, std::size_t const max_plies) -> std::vector<std::uint16_t>
{
    auto board = motif::chess::board {};
    auto moves = std::vector<std::uint16_t> {};
    for (std::size_t ply = 0; ply < max_plies; ++ply) {
        auto const legal = motif::chess::legal_moves(board);
        if (legal.empty()) {
            break;
        }
        auto const pick = legal[std::uniform_int_distribution<std::size_t> {0, legal.size() - 1}(rng)].encoded_move;
        motif::chess::apply_encoded_move(board, pick);
        moves.push_back(pick);
    }
    return moves;
}

struct model_game
{
    std::vector<std::uint16_t> moves;
    std::string result;
    std::optional<std::int32_t> white_elo;
    std::optional<std::int32_t> black_elo;
};

inline constexpr auto result_strings = std::array<std::string_view, 4> {"1-0", "0-1", "1/2-1/2", "*"};

// Ratings of the hand-built fixtures; the extremes are the ends of the signed 16-bit domain.
inline constexpr std::int32_t lowest_rating = -32768;
inline constexpr std::int32_t highest_rating = 32767;
inline constexpr std::int32_t club_rating = 1500;
inline constexpr std::int32_t master_rating = 2300;
inline constexpr std::int32_t strong_master_rating = 2500;
// The knight shuffle repeats its four-ply cycle this often (24 plies, past the 20-ply horizon).
inline constexpr auto shuffle_cycles = 6;
// Reused openings keep this many plies of an earlier game, so the corpus shares prefixes.
inline constexpr auto shared_prefix_plies = std::size_t {6};

// Ratings across the whole signed-16-bit domain, including negatives and zero.
inline auto pick_elo(std::mt19937_64& rng) -> std::optional<std::int32_t>
{
    static constexpr auto values =
        std::array<std::optional<std::int32_t>, 10> {std::nullopt, std::nullopt, -32768, -1, 0, 1, 1500, 2300, 2500, 32767};
    return values.at(std::uniform_int_distribution<std::size_t> {0, values.size() - 1}(rng));
}

inline auto pick_result(std::mt19937_64& rng) -> std::string
{
    return std::string {result_strings.at(std::uniform_int_distribution<std::size_t> {0, result_strings.size() - 1}(rng))};
}

// Hand-built fixtures: zero moves, a terminal checkmate, a knight shuffle whose
// start-position returns fall beyond the 20-ply horizon followed by a move out of
// the start position (only the complete start node may capture it), a game that
// repeats one edge within a game, and a 25-move game that crosses the horizon.
inline auto fixture_games() -> std::vector<model_game>
{
    auto games = std::vector<model_game> {};
    games.push_back({.moves = {}, .result = "*", .white_elo = std::nullopt, .black_elo = std::nullopt});
    games.push_back({.moves = uci_moves({"f2f3", "e7e5", "g2g4", "d8h4"}), .result = "0-1", .white_elo = club_rating, .black_elo = -1});
    auto shuffle = std::vector<std::uint16_t> {};
    for (auto cycle = 0; cycle < shuffle_cycles; ++cycle) {
        auto const part = uci_moves({"g1f3", "g8f6", "f3g1", "f6g8"});
        // Moves are position independent encodings, so repeating the cycle from the
        // start position stays legal.
        shuffle.insert(shuffle.end(), part.begin(), part.end());
    }
    auto const out_of_start = uci_moves({"e2e4"});
    auto late_return = shuffle;
    late_return.push_back(out_of_start.front());
    games.push_back({.moves = late_return, .result = "1-0", .white_elo = master_rating, .black_elo = strong_master_rating});
    games.push_back({.moves = shuffle, .result = "1/2-1/2", .white_elo = 0, .black_elo = highest_rating});
    games.push_back({.moves = uci_moves({"e2e4", "e7e5", "g1f3", "b8c6", "f1b5", "a7a6", "b5a4", "g8f6", "e1g1", "f8e7"}),
                     .result = "1-0",
                     .white_elo = lowest_rating,
                     .black_elo = strong_master_rating});
    return games;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- a seed and a game count; every caller passes them in this order.
inline auto make_corpus(std::uint64_t const seed, std::size_t const random_count) -> std::vector<model_game>
{
    auto rng = std::mt19937_64 {seed};
    auto games = fixture_games();
    // A 25-ply game so edges at root ply 20 (child ply 21) occur.
    for (std::size_t index = 0; index < random_count; ++index) {
        auto const length = index % 5 == 0 ? std::size_t {26} : std::uniform_int_distribution<std::size_t> {1, 14}(rng);
        games.push_back(
            {.moves = random_game_moves(rng, length), .result = pick_result(rng), .white_elo = pick_elo(rng), .black_elo = pick_elo(rng)});
    }
    // Make the corpus share prefixes: reuse the openings of earlier games.
    for (std::size_t index = 0; index < random_count && index < games.size(); index += 3) {
        auto reused = games[index];
        if (reused.moves.size() > shared_prefix_plies) {
            reused.moves.resize(shared_prefix_plies);
        }
        reused.result = pick_result(rng);
        reused.white_elo = pick_elo(rng);
        reused.black_elo = pick_elo(rng);
        games.push_back(std::move(reused));
    }
    return games;
}

inline auto to_db_game(model_game const& model, std::size_t const index) -> motif::db::game
{
    return motif::db::game {
        .white = {.name = "White " + std::to_string(index), .elo = model.white_elo, .title = std::nullopt, .country = std::nullopt},
        .black = {.name = "Black " + std::to_string(index), .elo = model.black_elo, .title = std::nullopt, .country = std::nullopt},
        .event_details = std::nullopt,
        .date = std::nullopt,
        .result = model.result,
        .eco = std::nullopt,
        .moves = model.moves,
        .extra_tags = {},
        .provenance = {},
    };
}

// A bundle holding exactly `corpus` (ids 1..N in order) with postings and,
// when requested, a tree checkpoint at the current sequence.
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear setup.
inline auto build_bundle(std::filesystem::path const& dir, std::vector<model_game> const& corpus, bool const with_tree)
    -> motif::db::database_manager
{
    auto created = motif::db::database_manager::create(dir, "pr6a");
    REQUIRE(created.has_value());
    for (std::size_t index = 0; index < corpus.size(); ++index) {
        auto inserted = created->insert_game(to_db_game(corpus[index], index));
        REQUIRE(inserted.has_value());
        REQUIRE(inserted->value == index + 1U);
    }
    REQUIRE(created->rebuild_position_postings().has_value());
    if (with_tree) {
        REQUIRE(created->rebuild_opening_tree_index().has_value());
    }
    return std::move(*created);
}

// Loads the composite tree a manifest describes from its files, the same way
// the manager does at open.
inline auto load_chain_tree(std::filesystem::path const& dir, motif::db::db_manifest const& manifest)
    -> motif::db::result<motif::db::composite_opening_tree>
{
    auto const* tree = manifest.tree();
    REQUIRE(tree != nullptr);
    auto const bundle_id = require_value(motif::db::identity_from_hex(manifest.payload.bundle.id));
    auto runs = std::vector<motif::db::tree_patch_run_ptr> {};
    for (auto const& descriptor : tree->patch_runs) {
        auto run = motif::db::load_tree_patch_run(
            dir / descriptor.artifact.filename,
            descriptor.artifact,
            motif::db::tree_patch_expectation {.identity = {.bundle_id = bundle_id,
                                                            .index_semantics = motif::db::index_semantics_version,
                                                            .canonical_start_hash = start_hash(),
                                                            .max_root_ply = tree->max_root_ply},
                                               .from_exclusive_seq = descriptor.from_exclusive_seq,
                                               .to_inclusive_seq = descriptor.to_inclusive_seq});
        if (!run) {
            return tl::unexpected {run.error()};
        }
        runs.push_back(std::make_shared<motif::db::tree_patch_run const>(std::move(*run)));
    }
    return motif::db::composite_opening_tree::open(dir / tree->checkpoint.filename, std::move(runs), tree->child_frequency_mode);
}

inline auto collect_nodes(motif::db::opening_tree_index const& index) -> std::vector<motif::db::raw_node>
{
    auto stream = index.stream_nodes();
    REQUIRE(stream.has_value());
    auto nodes = std::vector<motif::db::raw_node> {};
    while (true) {
        auto next = stream->next();
        REQUIRE(next.has_value());
        auto& node = *next;
        if (!node) {
            break;
        }
        nodes.push_back(std::move(*node));
    }
    return nodes;
}

inline auto collect_nodes(motif::db::composite_opening_tree const& tree) -> std::vector<motif::db::raw_node>
{
    auto nodes = std::vector<motif::db::raw_node> {};
    auto const visited = tree.for_each_node(
        [&nodes](motif::db::raw_node const& node) -> motif::db::result<void>
        {
            nodes.push_back(node);
            return {};
        });
    REQUIRE(visited.has_value());
    return nodes;
}

// Builds a v6 checkpoint over `corpus` (game ids 1..N) with the production
// builder, without any SQLite store.
inline void write_tree_checkpoint(std::filesystem::path const& path, std::vector<model_game> const& corpus)
{
    auto builder = motif::db::opening_tree_index_builder {path};
    for (std::size_t index = 0; index < corpus.size(); ++index) {
        auto const& game = corpus[index];
        REQUIRE(builder
                    .accumulate(motif::db::replay_game_record {.id = motif::db::game_id {static_cast<std::uint32_t>(index + 1U)},
                                                               .result = game.result,
                                                               .white_elo = game.white_elo,
                                                               .black_elo = game.black_elo,
                                                               .moves = game.moves})
                    .has_value());
    }
    REQUIRE(builder.finalize().has_value());
}

inline auto tree_state_of(model_game const& game, std::uint32_t const document) -> motif::db::tree_game_state
{
    return motif::db::tree_game_state {.id = motif::db::game_id {document},
                                       .moves = game.moves,
                                       .result = motif::db::encode_result_code(game.result),
                                       .white_elo = game.white_elo,
                                       .black_elo = game.black_elo};
}

inline auto patch_identity(std::uint16_t const horizon = motif::db::opening_tree_index_default_max_root_ply)
    -> motif::db::tree_patch_identity
{
    return motif::db::tree_patch_identity {.bundle_id = require_value(motif::db::identity_from_hex("0123456789abcdef0123456789abcdef")),
                                           .index_semantics = motif::db::index_semantics_version,
                                           .canonical_start_hash = start_hash(),
                                           .max_root_ply = horizon};
}

inline auto same_match(motif::db::position_match const& left, motif::db::position_match const& right) -> bool
{
    return left.game_id == right.game_id && left.ply == right.ply && left.result == right.result && left.white_elo == right.white_elo
        && left.black_elo == right.black_elo;
}

inline auto same_matches(std::vector<motif::db::position_match> const& left, std::vector<motif::db::position_match> const& right) -> bool
{
    return left.size() == right.size()
        && std::ranges::equal(left, right, [](auto const& lhs, auto const& rhs) -> bool { return same_match(lhs, rhs); });
}

}  // namespace motif_test
