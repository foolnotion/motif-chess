#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <tl/expected.hpp>

#include "motif/db/database_manager.hpp"
#include "motif/db/query_connection.hpp"
#include "motif/db/types.hpp"
#include "motif/slint_app/error.hpp"

namespace motif::slint_app
{

inline constexpr std::size_t browser_page_size {100};
inline constexpr std::int32_t browser_minimum_column_width {70};
inline constexpr std::int32_t browser_maximum_column_width {600};
inline constexpr std::size_t browser_column_count {6};
inline constexpr std::int32_t browser_player_column_width {145};
inline constexpr std::int32_t browser_result_column_width {85};
inline constexpr std::int32_t browser_event_column_width {170};
inline constexpr std::int32_t browser_date_column_width {105};
inline constexpr std::int32_t browser_eco_column_width {70};

enum class browser_sort_column : std::uint8_t
{
    white,
    black,
    result,
    event,
    date,
    eco,
};

struct browser_error
{
    error_code code;
    std::string message;
};

template<typename T>
using browser_result = tl::expected<T, browser_error>;

struct browser_query
{
    std::uint64_t generation {};
    // `filter.position` is set when the list is restricted to a position.
    motif::db::search_filter filter;
    // A position change makes a different list: a selected game that is not in
    // it is deselected, so no hidden selection can be opened.
    bool drop_missing_selection {false};
};

struct browser_page
{
    std::uint64_t generation {};
    std::vector<motif::db::game_list_entry> games;
    std::int64_t total_count {};
    std::size_t page_index {};
    // First ply of the queried position per game, parallel to `games`; empty
    // unless the query was position-restricted.
    std::vector<std::uint16_t> plies;
    bool restricted {false};
    bool drop_missing_selection {false};
};

using browser_column_widths = std::array<std::int32_t, browser_column_count>;

struct activation_request
{
    std::uint64_t generation {};
    motif::db::game_id game_id {};
    // The first ply at which the game reaches the followed position, when the
    // list is restricted to one. The game opens there.
    std::optional<std::size_t> ply;
};

struct loaded_game
{
    std::uint64_t generation {};
    motif::db::game_id game_id {};
    motif::db::game game;
};

struct game_browser_state
{
    std::vector<motif::db::game_list_entry> games;
    // First ply of the followed position per row, parallel to `games`, while
    // `rows_restricted`.
    std::vector<std::uint16_t> row_plies;
    std::optional<motif::db::game> active_game;
    std::string player_filter;
    std::string result_filter;
    std::string error_text;
    std::optional<std::size_t> selected_row;
    // The position the list is, or is about to be, restricted to (Follow board).
    std::optional<motif::db::zobrist_hash> followed_hash;
    // The page that was showing when Follow board went on.
    std::optional<std::size_t> restore_page;
    std::int64_t total_count {};
    std::size_t page_index {};
    motif::db::game_id selected_game_id {};
    motif::db::game_id active_game_id {};
    std::uint64_t query_generation {};
    std::uint64_t activation_generation {};
    // Bumped each time a result lands (rows replaced or dropped), never for
    // selection or activation. A view keys its scroll position on it.
    std::uint64_t result_serial {};
    browser_column_widths column_widths {browser_player_column_width,
                                         browser_player_column_width,
                                         browser_result_column_width,
                                         browser_event_column_width,
                                         browser_date_column_width,
                                         browser_eco_column_width};
    browser_sort_column sort_column {browser_sort_column::white};
    bool sort_ascending {true};
    bool has_previous_page {false};
    bool has_next_page {false};
    bool has_selection {false};
    // Follow board: the list is the games that reach `followed_hash`.
    bool following {false};
    // The rows on screen are the result of a position-restricted query.
    bool rows_restricted {false};
    // A list query has been prepared and has not landed yet.
    bool loading {false};
    // The last list query failed and its rows were dropped.
    bool load_failed {false};
};

// What the empty list says: the failure, or why a followed position has no
// games. Empty when the list needs no explanation.
[[nodiscard]] auto browser_status_text(game_browser_state const& state) -> std::string;

class game_browser_presenter
{
  public:
    explicit game_browser_presenter(motif::db::database_manager& database) noexcept;

    [[nodiscard]] auto database_dir() const -> std::filesystem::path const&;

    auto prepare_initial_load() -> browser_result<browser_query>;
    auto set_filters(std::string player_name, std::string result) -> browser_result<browser_query>;
    auto sort_games(std::size_t column, bool ascending) -> browser_result<browser_query>;
    auto set_page(std::size_t page_index) -> browser_result<browser_query>;
    auto next_page() -> browser_result<browser_query>;
    auto previous_page() -> browser_result<browser_query>;

    // Follow board: while on, the list is the games that reach `followed_hash`,
    // combined with the player and result filters. The starting position
    // restricts nothing, because every stored game starts there.
    //
    // set_follow() switches it on or off and always returns the query that
    // refreshes the list. Switching on needs the board's position hash; without
    // one the switch is refused and the list is left alone. Switching off
    // removes only the position restriction and returns to the page that was
    // showing when it went on (the first page once the filters or the sort
    // changed in between).
    //
    // follow_position() reports a new board position. It returns the query for
    // the new list, or nothing when the list is not following or already
    // follows that position.
    auto set_follow(bool enabled, std::optional<motif::db::zobrist_hash> board_hash) -> browser_result<browser_query>;
    auto follow_position(motif::db::zobrist_hash hash) -> browser_result<std::optional<browser_query>>;
    // Reloads the first page. A followed list is brought to `board_hash` first
    // (and drops a selection that is not in the new list when that is a different
    // position); an unfollowed list ignores it.
    auto reload(std::optional<motif::db::zobrist_hash> board_hash) -> browser_result<browser_query>;

    [[nodiscard]] auto execute_query(browser_query const& query) const -> browser_result<browser_page>;
    [[nodiscard]] auto execute_query(browser_query const& query, motif::db::query_connection const& connection) const
        -> browser_result<browser_page>;
    auto apply_query(browser_page page) -> browser_result<bool>;
    auto apply_query_error(std::uint64_t generation, browser_error query_error) -> browser_result<bool>;

    auto select_game(std::size_t row) -> browser_result<void>;
    auto move_selection(int delta) -> browser_result<std::size_t>;
    auto prepare_activation() -> browser_result<activation_request>;
    [[nodiscard]] auto execute_activation(activation_request const& request) const -> browser_result<loaded_game>;
    [[nodiscard]] static auto execute_activation(activation_request const& request, motif::db::query_connection const& connection)
        -> browser_result<loaded_game>;
    auto apply_activation(loaded_game game) -> browser_result<bool>;
    auto apply_activation_error(std::uint64_t generation, browser_error activation_error) -> browser_result<bool>;
    // Activation entry point for callers outside the browser's own row
    // selection (e.g. a search-result row): reuses the identical
    // generation-guarded, single-read activation pipeline as row
    // activation, but does not require or depend on browser row selection.
    auto prepare_external_activation(motif::db::game_id game_id) -> browser_result<activation_request>;
    auto apply_external_activation(loaded_game game) -> browser_result<bool>;
    auto resize_column(std::size_t column, std::int32_t width) -> browser_result<void>;

    void dismiss_error() noexcept;
    [[nodiscard]] auto state() const noexcept -> game_browser_state const&;

  private:
    motif::db::database_manager* database_;
    motif::db::zobrist_hash starting_hash_;
    game_browser_state state_;

    auto make_query(std::size_t page_index, bool drop_missing_selection = false) -> browser_result<browser_query>;
    auto fail(browser_error error_value) -> browser_result<void>;
    auto fail_query(browser_error error_value) -> browser_result<browser_query>;
    static auto from_database_error(motif::db::error const& database_error) -> browser_error;
};

}  // namespace motif::slint_app
