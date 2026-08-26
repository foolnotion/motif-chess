#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "motif/slint_app/search_presenter.hpp"

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "motif/chess/chess.hpp"
#include "motif/db/database_manager.hpp"
#include "motif/db/types.hpp"
#include "test_helpers.hpp"

namespace
{

constexpr std::size_t heavy_position_game_count {100'000};
constexpr std::size_t performance_sample_count {20};
constexpr auto position_search_limit = std::chrono::milliseconds {100};
constexpr auto opening_stats_limit = std::chrono::milliseconds {500};

void skip_perf_unless_release_build()
{
    if (test_helpers::is_sanitized_build) {
        SKIP("performance checks are skipped in sanitize builds");
    }
#ifndef NDEBUG
    SKIP("performance checks require a Release build");
#endif
}

auto starting_position_hash() -> motif::db::zobrist_hash
{
    return motif::db::zobrist_hash {motif::chess::board {}.hash()};
}

auto make_game(std::vector<std::string> const& sans, std::string result = "1-0") -> motif::db::game
{
    auto board = motif::chess::board {};
    auto moves = std::vector<std::uint16_t> {};
    moves.reserve(sans.size());
    for (auto const& san : sans) {
        auto move = motif::chess::apply_san(board, san);
        REQUIRE(move);
        moves.push_back(*move);
    }

    auto game = motif::db::game {};
    game.white.name = "White Player";
    game.black.name = "Black Player";
    game.result = std::move(result);
    game.event_details = motif::db::event {.name = "Search Open", .site = "Local", .date = "2026.08.26"};
    game.date = "2026.08.26";
    game.eco = "C20";
    game.moves = std::move(moves);
    return game;
}

auto hash_after(std::vector<std::string> const& sans) -> motif::db::zobrist_hash
{
    auto board = motif::chess::board {};
    for (auto const& san : sans) {
        auto move = motif::chess::apply_san(board, san);
        REQUIRE(move);
    }
    return motif::db::zobrist_hash {board.hash()};
}

auto empty_filter() -> motif::db::search_filter
{
    return motif::db::search_filter {};
}

auto result_filter(std::string result) -> motif::db::search_filter
{
    return motif::db::search_filter {
        .player_name = std::nullopt,
        .player_color = motif::db::player_color::either,
        .min_elo = std::nullopt,
        .max_elo = std::nullopt,
        .result = std::move(result),
        .eco_prefix = std::nullopt,
        .position = std::nullopt,
        .offset = 0,
        .limit = motif::db::default_search_limit,
        .sort_column = motif::db::game_sort_column::id,
        .sort_ascending = true,
    };
}

auto load(motif::slint_app::search_presenter& presenter, motif::slint_app::search_query const& query)
    -> motif::slint_app::search_result<bool>
{
    auto page = presenter.execute_query(query);
    if (!page) {
        return presenter.apply_query_error(query.generation, page.error());
    }
    return presenter.apply_query(std::move(*page));
}

}  // namespace

TEST_CASE("search_presenter: empty database has no matches or continuations", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    auto presenter = motif::slint_app::search_presenter {*database};

    CHECK(presenter.state().matches.empty());
    CHECK(presenter.state().continuations.empty());

    auto query = presenter.search(starting_position_hash(), empty_filter());
    REQUIRE(query.has_value());
    REQUIRE(load(presenter, *query).value_or(false));

    CHECK(presenter.state().matches.empty());
    CHECK(presenter.state().total_matches == 0);
    CHECK(presenter.state().continuations.empty());
    CHECK(presenter.state().total_games == 0);
    CHECK(presenter.state().error_text.empty());
}

TEST_CASE("search_presenter: a position reached by a single game", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    REQUIRE(database->insert_game(make_game({"e4", "e5", "Nf3"})).has_value());
    REQUIRE(database->rebuild_position_postings().has_value());
    auto presenter = motif::slint_app::search_presenter {*database};

    auto const hash = hash_after({"e4", "e5"});
    auto query = presenter.search(hash, empty_filter());
    REQUIRE(query.has_value());
    REQUIRE(load(presenter, *query).value_or(false));

    REQUIRE(presenter.state().matches.size() == 1);
    CHECK(presenter.state().matches.front().ply == 2);
    CHECK(presenter.state().total_matches == 1);
    REQUIRE(presenter.state().continuations.size() == 1);
    CHECK(presenter.state().continuations.front().san == "Nf3");
    CHECK(presenter.state().total_games == 1);
}

TEST_CASE("search_presenter: transpositions from different move orders count as one position", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    // Pure knight moves so no pawn move or en-passant state can make the
    // transposed orders diverge; both reach the identical position at ply 4.
    REQUIRE(database->insert_game(make_game({"Nf3", "Nc6", "Nc3", "Nf6"})).has_value());
    REQUIRE(database->insert_game(make_game({"Nc3", "Nf6", "Nf3", "Nc6"})).has_value());
    REQUIRE(database->rebuild_position_postings().has_value());
    auto presenter = motif::slint_app::search_presenter {*database};

    auto const hash = hash_after({"Nf3", "Nc6", "Nc3", "Nf6"});
    REQUIRE(hash == hash_after({"Nc3", "Nf6", "Nf3", "Nc6"}));

    auto query = presenter.search(hash, empty_filter());
    REQUIRE(query.has_value());
    REQUIRE(load(presenter, *query).value_or(false));

    CHECK(presenter.state().matches.size() == 2);
    CHECK(presenter.state().total_matches == 2);
}

TEST_CASE("search_presenter: repeated position within one game keeps the first ply, not the last", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    // Nf3/Ng1 and Nf6/Ng8 shuffle knights out and back, reaching the
    // starting position again at ply 4.
    REQUIRE(database->insert_game(make_game({"Nf3", "Nf6", "Ng1", "Ng8"})).has_value());
    REQUIRE(database->rebuild_position_postings().has_value());
    auto presenter = motif::slint_app::search_presenter {*database};

    auto query = presenter.search(starting_position_hash(), empty_filter());
    REQUIRE(query.has_value());
    REQUIRE(load(presenter, *query).value_or(false));

    REQUIRE(presenter.state().matches.size() == 1);
    CHECK(presenter.state().matches.front().ply == 0);
}

TEST_CASE("search_presenter: filters narrow matches and continuations identically", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    REQUIRE(database->insert_game(make_game({"e4", "e5", "Nf3"}, "1-0")).has_value());
    REQUIRE(database->insert_game(make_game({"e4", "e5", "Bc4"}, "0-1")).has_value());
    REQUIRE(database->rebuild_position_postings().has_value());
    auto presenter = motif::slint_app::search_presenter {*database};

    auto const hash = hash_after({"e4", "e5"});
    auto query = presenter.search(hash, result_filter("1-0"));
    REQUIRE(query.has_value());
    REQUIRE(load(presenter, *query).value_or(false));

    REQUIRE(presenter.state().matches.size() == 1);
    CHECK(presenter.state().matches.front().game.result == "1-0");
    REQUIRE(presenter.state().continuations.size() == 1);
    CHECK(presenter.state().continuations.front().san == "Nf3");
    CHECK(presenter.state().active_filter.result == std::optional<std::string> {"1-0"});
}

TEST_CASE("search_presenter: no matches at an unreached position", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    REQUIRE(database->insert_game(make_game({"e4", "e5"})).has_value());
    REQUIRE(database->rebuild_position_postings().has_value());
    auto presenter = motif::slint_app::search_presenter {*database};

    auto const unreached = hash_after({"d4", "d5", "c4"});
    auto query = presenter.search(unreached, empty_filter());
    REQUIRE(query.has_value());
    REQUIRE(load(presenter, *query).value_or(false));

    CHECK(presenter.state().matches.empty());
    CHECK(presenter.state().total_matches == 0);
    CHECK(presenter.state().continuations.empty());
}

TEST_CASE("search_presenter: a newer query supersedes an older in-flight one", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    REQUIRE(database->insert_game(make_game({"e4", "e5"})).has_value());
    REQUIRE(database->insert_game(make_game({"d4", "d5"})).has_value());
    REQUIRE(database->rebuild_position_postings().has_value());
    auto presenter = motif::slint_app::search_presenter {*database};

    auto stale_query = presenter.search(hash_after({"e4"}), empty_filter());
    REQUIRE(stale_query.has_value());
    auto stale_page = presenter.execute_query(*stale_query);
    REQUIRE(stale_page.has_value());

    auto current_query = presenter.search(hash_after({"d4"}), empty_filter());
    REQUIRE(current_query.has_value());
    REQUIRE(load(presenter, *current_query).value_or(false));
    CHECK(presenter.state().matches.size() == 1);
    CHECK(presenter.state().current_hash == hash_after({"d4"}));

    auto const applied = presenter.apply_query(std::move(*stale_page));
    REQUIRE(applied.has_value());
    CHECK_FALSE(*applied);
    CHECK(presenter.state().current_hash == hash_after({"d4"}));
    CHECK(presenter.state().matches.size() == 1);
}

TEST_CASE("search_presenter: a stale query error cannot replace current state", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    auto presenter = motif::slint_app::search_presenter {*database};

    auto const stale_generation = presenter.search(hash_after({"e4"}), empty_filter())->generation;
    auto const current_query = presenter.search(hash_after({"d4"}), empty_filter());
    REQUIRE(current_query.has_value());
    REQUIRE(load(presenter, *current_query).value_or(false));
    CHECK(presenter.state().error_text.empty());

    auto stale_error = presenter.apply_query_error(
        stale_generation,
        motif::slint_app::search_error {.code = motif::slint_app::error_code::database_failure, .message = "stale failure"});
    REQUIRE(stale_error.has_value());
    CHECK_FALSE(*stale_error);
    CHECK(presenter.state().error_text.empty());
}

TEST_CASE("search_presenter: an accepted query error clears prior result payload", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    REQUIRE(database->insert_game(make_game({"e4", "e5"})).has_value());
    REQUIRE(database->rebuild_position_postings().has_value());
    auto presenter = motif::slint_app::search_presenter {*database};

    auto successful = presenter.search(hash_after({"e4"}), empty_filter());
    REQUIRE(successful.has_value());
    REQUIRE(load(presenter, *successful).value_or(false));
    REQUIRE_FALSE(presenter.state().matches.empty());

    auto failed = presenter.search(hash_after({"d4"}), empty_filter());
    REQUIRE(failed.has_value());
    auto applied = presenter.apply_query_error(
        failed->generation,
        motif::slint_app::search_error {.code = motif::slint_app::error_code::database_failure, .message = "query failed"});

    REQUIRE(applied.has_value());
    CHECK(*applied);
    CHECK(presenter.state().matches.empty());
    CHECK(presenter.state().continuations.empty());
    CHECK(presenter.state().total_matches == 0);
    CHECK(presenter.state().total_games == 0);
    CHECK(presenter.state().current_hash == hash_after({"d4"}));
    CHECK(presenter.state().error_text == "query failed");
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertions are the branches.
TEST_CASE("search_presenter: match rows preserve game identity and ply for row-based activation", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    auto const first_id = database->insert_game(make_game({"e4", "e5", "Nf3"}));
    // Both games reach the identical e4/e5 position (pawns e4/e5, all other
    // pieces home, en passant target active from the just-played e5): the
    // second game detours through a reversible knight shuffle first.
    auto const second_id = database->insert_game(make_game({"Nf3", "Nf6", "Ng1", "Ng8", "e4", "e5"}));
    REQUIRE(first_id.has_value());
    REQUIRE(second_id.has_value());
    REQUIRE(database->rebuild_position_postings().has_value());
    auto presenter = motif::slint_app::search_presenter {*database};

    // Both games reach the e4/e5 position, at different plies.
    auto query = presenter.search(hash_after({"e4", "e5"}), empty_filter());
    REQUIRE(query.has_value());
    REQUIRE(load(presenter, *query).value_or(false));

    REQUIRE(presenter.state().matches.size() == 2);
    for (auto const& match : presenter.state().matches) {
        if (match.game.id == *first_id) {
            CHECK(match.ply == 2);
        } else {
            CHECK(match.game.id == *second_id);
            CHECK(match.ply == 6);
        }
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertions are the branches.
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- builds a real representative corpus and measures P99.
TEST_CASE("search_presenter: position search and opening stats meet interaction targets", "[motif-slint-app][performance]")
{
    skip_perf_unless_release_build();
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());

    REQUIRE(database->writer().begin_transaction().has_value());
    for (std::size_t index = 0; index < heavy_position_game_count; ++index) {
        auto game = make_game({"e4", "e5"}, index % 2 == 0 ? "1-0" : "0-1");
        game.white.name = fmt::format("White {}", index);
        game.black.name = fmt::format("Black {}", index);
        REQUIRE(database->writer().insert(game).has_value());
    }
    REQUIRE(database->writer().commit_transaction().has_value());
    REQUIRE(database->rebuild_position_postings().has_value());

    auto const hash = hash_after({"e4", "e5"});
    auto const filter = empty_filter();
    auto position_samples = std::vector<std::chrono::steady_clock::duration> {};
    auto stats_samples = std::vector<std::chrono::steady_clock::duration> {};
    position_samples.reserve(performance_sample_count);
    stats_samples.reserve(performance_sample_count);

    auto presenter = motif::slint_app::search_presenter {*database};
    // Populate SQLite and immutable-postings pages before sampling the
    // steady-state interaction budget; a cold filesystem read is not an
    // event-loop search latency measurement.
    auto warmup_query = presenter.search(hash, filter);
    REQUIRE(warmup_query.has_value());
    REQUIRE(presenter.execute_query(*warmup_query).has_value());

    for (std::size_t sample = 0; sample < performance_sample_count; ++sample) {
        auto query = presenter.search(hash, filter);
        REQUIRE(query.has_value());

        auto const position_started = std::chrono::steady_clock::now();
        auto games = database->find_games(query->filter);
        REQUIRE(games.has_value());
        auto game_ids = std::vector<motif::db::game_id> {};
        game_ids.reserve(games->games.size());
        for (auto const& game : games->games) {
            game_ids.push_back(game.id);
        }
        auto matches = database->query_position_first_matches(hash, game_ids);
        REQUIRE(matches.has_value());
        CHECK(matches->size() == games->games.size());
        position_samples.push_back(std::chrono::steady_clock::now() - position_started);

        auto const stats_started = std::chrono::steady_clock::now();
        auto page = presenter.execute_query(*query);
        REQUIRE(page.has_value());
        stats_samples.push_back(std::chrono::steady_clock::now() - stats_started);
        CHECK(std::cmp_equal(page->continuations.total_games, heavy_position_game_count));
    }

    std::ranges::sort(position_samples);
    std::ranges::sort(stats_samples);
    auto const p99_index = ((performance_sample_count * 99U + 99U) / 100U) - 1U;
    CHECK(position_samples[p99_index] < position_search_limit);
    CHECK(stats_samples[p99_index] < opening_stats_limit);
}
