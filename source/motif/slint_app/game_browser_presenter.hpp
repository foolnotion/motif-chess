#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <tl/expected.hpp>

#include "motif/db/database_manager.hpp"
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
    motif::db::search_filter filter;
};

struct browser_page
{
    std::uint64_t generation {};
    std::vector<motif::db::game_list_entry> games;
    std::int64_t total_count {};
    std::size_t page_index {};
};

using browser_column_widths = std::array<std::int32_t, browser_column_count>;

struct activation_request
{
    std::uint64_t generation {};
    motif::db::game_id game_id {};
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
    std::int64_t total_count {};
    std::size_t page_index {};
    bool has_previous_page {false};
    bool has_next_page {false};
    std::optional<std::size_t> selected_row;
    motif::db::game_id selected_game_id {};
    bool has_selection {false};
    motif::db::game_id active_game_id {};
    std::optional<motif::db::game> active_game;
    browser_sort_column sort_column {browser_sort_column::white};
    bool sort_ascending {true};
    std::string player_filter;
    std::string result_filter;
    std::string error_text;
    std::uint64_t query_generation {};
    std::uint64_t activation_generation {};
    browser_column_widths column_widths {browser_player_column_width,
                                         browser_player_column_width,
                                         browser_result_column_width,
                                         browser_event_column_width,
                                         browser_date_column_width,
                                         browser_eco_column_width};
};

class game_browser_presenter
{
  public:
    explicit game_browser_presenter(motif::db::database_manager& database) noexcept;

    auto prepare_initial_load() -> browser_result<browser_query>;
    auto set_filters(std::string player_name, std::string result) -> browser_result<browser_query>;
    auto sort_games(std::size_t column, bool ascending) -> browser_result<browser_query>;
    auto set_page(std::size_t page_index) -> browser_result<browser_query>;
    auto next_page() -> browser_result<browser_query>;
    auto previous_page() -> browser_result<browser_query>;

    [[nodiscard]] auto execute_query(browser_query const& query) const -> browser_result<browser_page>;
    auto apply_query(browser_page page) -> browser_result<bool>;
    auto apply_query_error(std::uint64_t generation, browser_error query_error) -> browser_result<bool>;

    auto select_game(std::size_t row) -> browser_result<void>;
    auto move_selection(int delta) -> browser_result<std::size_t>;
    auto prepare_activation() -> browser_result<activation_request>;
    [[nodiscard]] auto execute_activation(activation_request const& request) const -> browser_result<loaded_game>;
    auto apply_activation(loaded_game game) -> browser_result<bool>;
    auto apply_activation_error(std::uint64_t generation, browser_error activation_error) -> browser_result<bool>;
    auto resize_column(std::size_t column, std::int32_t width) -> browser_result<void>;

    void dismiss_error() noexcept;
    [[nodiscard]] auto state() const noexcept -> game_browser_state const&;

  private:
    motif::db::database_manager* database_;
    game_browser_state state_;

    auto make_query(std::size_t page_index) -> browser_result<browser_query>;
    auto fail(browser_error error_value) -> browser_result<void>;
    auto fail_query(browser_error error_value) -> browser_result<browser_query>;
    static auto from_database_error(motif::db::error const& database_error) -> browser_error;
};

}  // namespace motif::slint_app
