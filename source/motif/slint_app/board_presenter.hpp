#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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

// A move chosen to reach the last rank, waiting for the piece to promote to.
struct pending_promotion
{
    board_square from;
    board_square to;
    // Colour of the promoting pawn, so the picker can show matching pieces.
    bool white {true};
};

// One move of the notation in PGN order. The lead, SAN and trail text always
// concatenate (with a space between number and SAN) to the PGN movetext form:
// `open` holds the "(" that start variations before this move, `number` the
// move number ("12." or "12...") when PGN requires one, `close` the ")" that
// end variations after it.
struct notation_token
{
    std::string open;
    std::string number;
    std::string san;
    std::string close;
    // Navigator node the token activates.
    std::uint32_t node {};
    // 0 on the main line, otherwise the variation nesting depth.
    int variation_depth {};

    // The text a view draws before the SAN: opening parentheses then the move
    // number, without a trailing space.
    [[nodiscard]] auto lead() const -> std::string { return number.empty() ? open : open + number; }
};

// Toolkit-neutral board/notation state, mirroring the Qt board_model property
// set without any Qt or Slint types. Stored games load as a single line. Moves
// played on the board add to a tree (main line plus nested variations) held by
// the navigator; `notation` is that tree in PGN order. Comments and NAGs are
// not modelled. Nothing here is persisted: an unsaved game or a variation
// exists only in memory.
struct board_state
{
    // A stored game is on the board.
    bool loaded {false};
    // A game started by playing on the board (or New Game) is on the board.
    // Exclusive with `loaded`.
    bool unsaved {false};
    // Replacing the board would discard the user's input: an unsaved game with
    // at least one move, or a loaded game in which a move was added. Pure
    // navigation never sets it.
    bool edited {false};
    motif::db::game_id game_id {};
    std::string fen;
    // Zobrist hash of the current position. The navigator's cached board always
    // has one, so this holds a value on every presenter state; it stays an
    // optional so callers keep checking it before issuing a search.
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
    // Depth of the cursor along its own line, and the length of the main line.
    // Inside a variation longer than the main line current_ply exceeds
    // total_plies, so do not derive "can advance" from the pair; the
    // navigator controls follow the line the cursor is on.
    std::size_t current_ply {0};
    std::size_t total_plies {0};
    // Characters of the longest notation token (open, number, SAN, close), so
    // the view can size its fixed-width cells to fit it.
    std::size_t notation_chars {0};
    // SAN of the main line only.
    std::vector<std::string> san_moves;
    // The whole tree in PGN order, including variations.
    std::vector<notation_token> notation;
    // Navigator node at the cursor; the token carrying it is the current move.
    std::uint32_t current_node {0};
    std::optional<pending_promotion> promotion;
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
// for the move tree, FEN, SAN, and cursor state; delegates all legality queries
// to motif::chess. Never reimplements replay, SAN, or legal-move logic. The
// board is always playable: with nothing loaded it shows the starting position
// and the first legal move starts an unsaved game.
class board_presenter
{
  public:
    board_presenter();

    // Applies a game already loaded by another adapter pipeline. This avoids
    // a second database read when browser activation supplies the game.
    void apply_loaded_game(motif::db::game_id game_key, motif::db::game const& game);
    void dismiss_error() noexcept;

    // Resets every board/notation field except the panel visibility
    // preference, which is a workspace layout choice rather than
    // game-specific state.
    void clear();

    void advance();
    void retreat();
    void jump_to_start();
    void jump_to_end();
    // Main-line ply, clamped to the main line.
    void navigate_to(std::size_t ply);
    // Any node of the move tree; an unknown id changes nothing.
    void navigate_to_node(std::uint32_t node);

    // Selects a square, or plays the move when a piece is selected and the
    // square is one of its legal targets. A pawn reaching the last rank leaves
    // the move pending in state().promotion until choose_promotion() or
    // cancel_promotion().
    auto select_square(int file, int rank) -> board_result<void>;
    // Resolves a pending promotion with the piece letter (q, r, b, n; either
    // case). An unknown letter or no pending promotion is an error that
    // changes nothing.
    auto choose_promotion(char piece) -> board_result<void>;
    void cancel_promotion() noexcept;
    // Plays a move written in SAN from the cursor, exactly as board input
    // would. This is the entry point for move lists that arrive as text.
    auto play_san(std::string_view san) -> board_result<void>;
    // Discards the board and starts an empty unsaved game at the starting
    // position. Callers confirm first when state().edited.
    void start_new_game();
    void clear_selection() noexcept;
    void toggle_orientation() noexcept;
    void set_panel_visible(bool visible) noexcept;

    [[nodiscard]] auto state() const noexcept -> board_state const&;
    // PGN movetext of the whole tree: main line with variations in
    // parentheses, ending with the result (or "*"). Empty when no game is on
    // the board.
    [[nodiscard]] auto movetext() const -> std::string;

  private:
    motif::navigator::game_navigator navigator_;
    board_state state_;
    // A move was added to the loaded game, so replacing it loses input.
    bool moves_added_ {false};
    void reset_game_state(std::uint64_t generation, bool panel_visible);
    void load_game_state(motif::db::game_id game_key, motif::db::game const& game);
    void begin_unsaved_game();
    auto play_move(board_square from, board_square target, std::optional<char> promotion) -> board_result<void>;
    void finish_move(motif::navigator::game_navigator::move_outcome outcome);
    void rebuild_notation();
    void update_edit_state();
    void select(board_square square);

    // `tree_changed` rebuilds the main-line SAN list and the notation tokens.
    void refresh_navigation(bool tree_changed = false);
    void refresh_highlights();
    auto fail(board_error error_value) -> board_result<void>;
};

}  // namespace motif::slint_app
