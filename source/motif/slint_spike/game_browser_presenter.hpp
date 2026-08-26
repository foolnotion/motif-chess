#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <tl/expected.hpp>

#include "motif/db/database_manager.hpp"
#include "motif/db/types.hpp"
#include "motif/navigator/game_navigator.hpp"

namespace motif::slint_spike
{

enum class error_code : std::uint8_t
{
    database_failure,
    invalid_argument,
};

struct error
{
    error_code code;
    std::string message;
};

template<typename T>
using result = tl::expected<T, error>;

inline constexpr std::size_t sort_column_count {6};

struct game_browser_state
{
    std::vector<motif::db::game_list_entry> games;
    std::size_t selected_row {};
    bool has_selection {false};
    std::string selected_white;
    std::string selected_black;
    std::string selected_event;
    std::string selected_date;
    std::string selected_result;
    std::vector<std::string> san_moves;
    std::string current_fen;
    std::size_t current_ply {};
    std::string error_text;
};

class game_browser_presenter
{
  public:
    explicit game_browser_presenter(motif::db::database_manager& database) noexcept;
    game_browser_presenter(game_browser_presenter const&) = delete;
    auto operator=(game_browser_presenter const&) -> game_browser_presenter& = delete;
    game_browser_presenter(game_browser_presenter&&) = delete;
    auto operator=(game_browser_presenter&&) -> game_browser_presenter& = delete;
    ~game_browser_presenter() = default;

    auto load_games() -> result<void>;
    auto select_game(std::size_t row) -> result<void>;
    auto sort_games(std::size_t column, bool ascending) -> result<void>;
    auto advance() -> result<void>;
    auto retreat() -> result<void>;

    [[nodiscard]] auto state() const noexcept -> game_browser_state const&;

  private:
    motif::db::database_manager* database_;
    motif::navigator::game_navigator navigator_;
    game_browser_state state_;

    void update_navigation_state();
    auto fail(error error_value) -> result<void>;
    auto fail_database(motif::db::error const& database_error) -> result<void>;
};

}  // namespace motif::slint_spike
