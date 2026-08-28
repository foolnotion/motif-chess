#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "motif/slint_app/search_presenter.hpp"

#include <tl/expected.hpp>

#include "motif/chess/chess.hpp"
#include "motif/db/error.hpp"
#include "motif/db/types.hpp"
#include "motif/search/error.hpp"
#include "motif/search/opening_stats.hpp"

namespace motif::slint_app
{

namespace
{

auto has_metadata_filter(motif::db::search_filter const& filter) -> bool
{
    return filter.player_name.has_value() || filter.result.has_value() || filter.eco_prefix.has_value() || filter.min_elo.has_value()
        || filter.max_elo.has_value();
}

}  // namespace

search_presenter::search_presenter(motif::db::database_manager& database)
    : database_(&database)
    , starting_position_hash_ {motif::db::zobrist_hash {motif::chess::board {}.hash()}}
{
}

auto search_presenter::search(motif::db::zobrist_hash const hash, motif::db::search_filter filter)
    -> search_result<std::optional<search_query>>
{
    filter.position = hash;
    auto const cache_hit = hash == starting_position_hash_ && starting_position_cache_ && !has_metadata_filter(filter)
        && filter.offset == starting_position_cache_->filter.offset && filter.limit == starting_position_cache_->filter.limit
        && filter.sort_column == starting_position_cache_->filter.sort_column
        && filter.sort_ascending == starting_position_cache_->filter.sort_ascending;
    if (cache_hit) {
        ++state_.query_generation;
        auto cached_page = *starting_position_cache_;
        cached_page.generation = state_.query_generation;
        cached_page.filter = filter;
        (void)apply_query(std::move(cached_page));
        return std::optional<search_query> {std::nullopt};
    }

    ++state_.query_generation;
    state_.matches.clear();
    state_.total_matches = 0;
    state_.page_index = 0;
    state_.has_previous_page = false;
    state_.has_next_page = false;
    state_.continuations.clear();
    state_.total_games = 0;
    state_.active_filter = filter;
    state_.error_text.clear();
    state_.searching = true;
    state_.current_hash = hash;
    return std::optional<search_query> {search_query {.generation = state_.query_generation, .hash = hash, .filter = std::move(filter)}};
}

auto search_presenter::execute_query(search_query const& query) const -> search_result<search_page>
{
    auto games_result = database_->find_games(query.filter);
    if (!games_result) {
        return tl::unexpected {from_database_error(games_result.error())};
    }

    auto const game_ids = [&games = games_result->games]() -> std::vector<motif::db::game_id>
    {
        auto ids = std::vector<motif::db::game_id> {};
        ids.reserve(games.size());
        for (auto const& game : games) {
            ids.push_back(game.id);
        }
        return ids;
    }();
    auto matches_result = database_->query_position_first_matches(query.hash, game_ids);
    if (!matches_result) {
        return tl::unexpected {from_database_error(matches_result.error())};
    }
    if (matches_result->size() != games_result->games.size()) {
        return tl::unexpected {
            search_error {.code = error_code::database_failure, .message = "Position results changed while search metadata was loaded."}};
    }

    std::ranges::sort(*matches_result, {}, &motif::db::position_match::game_id);
    auto matches = std::vector<search_match> {};
    matches.reserve(games_result->games.size());
    for (auto& entry : games_result->games) {
        auto const found = std::ranges::lower_bound(*matches_result, entry.id, {}, &motif::db::position_match::game_id);
        if (found == matches_result->end() || found->game_id != entry.id) {
            return tl::unexpected {search_error {.code = error_code::database_failure,
                                                 .message = "Position results changed while search metadata was loaded."}};
        }
        matches.push_back(search_match {.game = std::move(entry), .ply = found->ply});
    }

    auto stats_result = motif::search::opening_stats::query(*database_, query.hash, query.filter);
    if (!stats_result) {
        return tl::unexpected {from_search_error(stats_result.error())};
    }

    return search_page {
        .generation = query.generation,
        .hash = query.hash,
        .matches = std::move(matches),
        .total_matches = games_result->total_count,
        .continuations = std::move(*stats_result),
        .filter = query.filter,
    };
}

auto search_presenter::apply_query(search_page page) -> search_result<bool>
{
    if (page.generation != state_.query_generation) {
        return false;
    }
    if (page.hash == starting_position_hash_ && !has_metadata_filter(page.filter)) {
        starting_position_cache_ = page;
    }
    state_.current_hash = page.hash;
    auto const next_offset = page.filter.offset + page.matches.size();
    state_.matches = std::move(page.matches);
    state_.total_matches = page.total_matches;
    state_.page_index = page.filter.limit == 0U ? 0U : page.filter.offset / page.filter.limit;
    state_.has_previous_page = page.filter.offset > 0U;
    state_.has_next_page = page.total_matches > 0 && std::cmp_less(next_offset, page.total_matches);
    state_.total_games = static_cast<std::int64_t>(page.continuations.total_games);
    state_.continuations = std::move(page.continuations.continuations);
    state_.active_filter = std::move(page.filter);
    state_.searching = false;
    state_.error_text.clear();
    return true;
}

void search_presenter::invalidate_starting_position_cache() noexcept
{
    starting_position_cache_.reset();
}

auto search_presenter::apply_query_error(std::uint64_t const generation, search_error query_error) -> search_result<bool>
{
    if (generation != state_.query_generation) {
        return false;
    }
    state_.searching = false;
    state_.error_text = std::move(query_error.message);
    return true;
}

void search_presenter::set_auto_search(bool const enabled) noexcept
{
    state_.auto_search = enabled;
}

void search_presenter::dismiss_error() noexcept
{
    state_.error_text.clear();
}

auto search_presenter::state() const noexcept -> search_state const&
{
    return state_;
}

auto search_presenter::from_database_error(motif::db::error const& database_error) -> search_error
{
    return search_error {
        .code = error_code::database_failure,
        .message = database_error.message.empty() ? std::string {motif::db::to_string(database_error)} : database_error.message,
    };
}

auto search_presenter::from_search_error(motif::search::error const& search_error_value) -> search_error
{
    return search_error {
        .code = search_error_value.code == motif::search::error_code::invalid_argument ? error_code::invalid_argument
                                                                                       : error_code::search_failure,
        .message =
            search_error_value.message.empty() ? std::string {motif::search::to_string(search_error_value)} : search_error_value.message,
    };
}
}  // namespace motif::slint_app
