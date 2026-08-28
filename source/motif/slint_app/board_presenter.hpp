#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <tl/expected.hpp>

#include "motif/db/types.hpp"
#include "motif/navigator/game_navigator.hpp"
#include "motif/slint_app/error.hpp"

namespace motif::slint_app
{

struct board_error
{
    error_code code;
    std::string message;
};

template<typename T>
using board_result = tl::expected<T, board_error>;

// A single board square. file: 0 = 'a' .. 7 = 'h'. rank: 0 = '1' .. 7 = '8'.
struct board_square
{
    int file {};
    int rank {};

    constexpr auto operator==(board_square const&) const noexcept -> bool = default;
};

// One highlight state per board square, precomputed so Slint only indexes
// a flat array instead of searching selection/legal-target/last-move data.
// Precedence when a square matches more than one condition: selected takes
// priority over legal target, which takes priority over last-move squares.
enum class square_highlight : std::uint8_t
{
    none,
    selected,
    legal_target,
    last_move_from,
    last_move_to,
};

// Toolkit-neutral board/notation state, mirroring the Qt board_model property
// set without any Qt or Slint types. Current data model has no persisted
// comments, NAGs, or variations (motif::db::game exposes only a flat move
// list), so this state has no fields for them; annotation rendering is
// expected to be empty until a future story adds that storage.
struct board_state
{
    bool loaded {false};
    motif::db::game_id game_id {};
    std::string fen;
    // Zobrist hash of the current position. Empty when the loaded move list
    // cannot be replayed; callers must not issue a search from a failed
    // replay.
    std::optional<motif::db::zobrist_hash> current_hash;
    // Flattened 8x8 piece grid derived from fen, index = rank * 8 + file
    // (file 0='a'..7='h', rank 0='1'..7='8'). Empty string for an empty
    // square, else a two-character code such as "wK"/"bP". Slint renders
    // this data directly; it must not walk fen itself.
    std::vector<std::string> square_pieces;
    // Flattened per-square highlight state, same indexing as square_pieces.
    // Precomputed by the presenter; Slint must not derive it by searching
    // selected_square/legal_targets/last_move_from/last_move_to itself.
    std::vector<square_highlight> square_highlights;
    std::size_t current_ply {0};
    std::size_t total_plies {0};
    std::vector<std::string> san_moves;
    std::string current_san;
    std::string white_name;
    std::string black_name;
    std::string result;
    std::string event_name;
    std::string date;
    std::string error_text;
    bool orientation_flipped {false};
    bool panel_visible {true};
    std::optional<board_square> selected_square;
    std::optional<board_square> last_move_from;
    std::optional<board_square> last_move_to;
    std::vector<board_square> legal_targets;
    std::uint64_t load_generation {0};
};

// Toolkit-neutral board/notation presenter. Wraps motif::navigator::game_navigator
// for move storage, FEN, SAN, and ply state; delegates all legality queries
// to motif::chess. Never reimplements replay, SAN, or legal-move logic.
class board_presenter
{
  public:
    board_presenter() noexcept;

    // Applies a game already loaded by another adapter pipeline. This avoids
    // a second database read when browser activation supplies the game.
    void apply_loaded_game(motif::db::game_id game_key, motif::db::game const& game);
    void dismiss_error() noexcept;

    // Resets every board/notation field except the panel visibility
    // preference, which is a workspace layout choice rather than
    // game-specific state.
    void clear() noexcept;

    void advance();
    void retreat();
    void jump_to_start();
    void jump_to_end();
    void navigate_to(std::size_t ply);

    auto select_square(int file, int rank) -> board_result<void>;
    void clear_selection() noexcept;
    void toggle_orientation() noexcept;
    void set_panel_visible(bool visible) noexcept;

    [[nodiscard]] auto state() const noexcept -> board_state const&;

  private:
    motif::navigator::game_navigator navigator_;
    board_state state_;
    void reset_game_state(std::uint64_t generation, bool panel_visible) noexcept;
    void load_game_state(motif::db::game_id game_key, motif::db::game const& game);

    void refresh_navigation(bool refresh_move_list = false);
    void refresh_highlights();
    auto fail(board_error error_value) -> board_result<void>;
};

}  // namespace motif::slint_app
