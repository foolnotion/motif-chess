#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tl/expected.hpp>

#include "motif/chess/chess.hpp"
#include "motif/db/types.hpp"

namespace motif::navigator
{

// Pure navigation logic over a move tree: a main line plus nested variations.
// No UI toolkit dependency, fully unit-testable. Every node is one played
// move; a node's first child is the continuation of its line and any further
// children are variations that replace that continuation. The root node is
// the starting position and carries no move.
//
// The board at the cursor is cached, so querying the current position and
// applying a move each cost one legal-move generation instead of a replay
// from the start. Moving the cursor to an arbitrary node replays only the path
// from the root. All legality and SAN come from motif::chess.
class game_navigator
{
  public:
    using node_id = std::uint32_t;

    // The node for the starting position. Node ids stay valid until the next
    // load() or clear(); moves are only ever appended.
    static constexpr node_id root_node {0};

    enum class move_error : std::uint8_t
    {
        illegal_move,
        // A pawn reaches the last rank and the caller named no piece.
        promotion_required,
        invalid_san,
    };

    enum class move_outcome : std::uint8_t
    {
        // The move equals the existing continuation of the cursor's line.
        followed_line,
        // The move equals an existing variation's first move.
        entered_variation,
        // The cursor had no continuation, so the line grew by one node.
        extended_line,
        // The cursor had a continuation and this move differs from it.
        created_variation,
    };

    struct applied_move
    {
        node_id node {};
        std::string san;
        move_outcome outcome {move_outcome::followed_line};
    };

    // Replaces the tree with the game's moves as a single main line and puts
    // the cursor at the start.
    void load(motif::db::game const& game);
    // Empties the tree back to the starting position.
    void clear();

    // Moves along the line the cursor is on: advance follows the first child,
    // retreat returns to the parent (from a variation's first move that is the
    // branch point), jump_to_end follows first children to the end of the line.
    void advance();
    void retreat();
    void jump_to_start();
    void jump_to_end();
    // Addresses the main line by ply, clamped to its length.
    void navigate_to(std::size_t ply);
    // Moves the cursor to any node. Returns false, leaving the cursor where it
    // is, for an id that does not exist.
    auto navigate_to_node(node_id node) -> bool;

    // Plays a move from the cursor by square names ("e2", "e4"). Promotion is
    // the piece letter (q, r, b, n; either case) and is required for a pawn
    // reaching the last rank. A move that matches an existing child only moves
    // the cursor. An error changes no state.
    auto apply_move(std::string_view from, std::string_view target, std::optional<char> promotion = std::nullopt)
        -> tl::expected<applied_move, move_error>;
    // Same effect as apply_move for a move written in SAN.
    auto apply_san(std::string_view san_text) -> tl::expected<applied_move, move_error>;

    [[nodiscard]] auto current_fen() const -> std::string;
    [[nodiscard]] auto current_san() const -> std::string;
    // Zobrist hash of the cursor position. The optional is kept for callers
    // written when replay could fail; the cached board always has a hash now.
    [[nodiscard]] auto current_hash() const -> std::optional<motif::db::zobrist_hash>;
    // SAN of the main line.
    [[nodiscard]] auto move_list() const -> std::vector<std::string>;

    // The cached board at the cursor.
    [[nodiscard]] auto current_board() const -> motif::chess::board const& { return board_; }

    // The from/to squares and metadata of the move that reached the cursor, or
    // std::nullopt at the starting position.
    [[nodiscard]] auto last_move() const -> std::optional<motif::chess::move_info> { return last_move_; }

    [[nodiscard]] auto current_node() const -> node_id { return cursor_; }

    // Depth of the cursor: the number of moves from the starting position along
    // the cursor's own line.
    [[nodiscard]] auto current_ply() const -> std::size_t { return nodes_[cursor_].depth; }

    // Length of the main line.
    [[nodiscard]] auto total_plies() const -> std::size_t { return main_line_.size(); }

    // True once any move exists in the tree.
    [[nodiscard]] auto has_game() const -> bool { return !nodes_[root_node].children.empty(); }

    // Tree access for notation renderers. Ids are valid indices below node_count().
    [[nodiscard]] auto node_count() const -> std::size_t { return nodes_.size(); }

    [[nodiscard]] auto parent(node_id const node) const -> node_id { return nodes_[node].parent; }

    [[nodiscard]] auto children(node_id const node) const -> std::span<node_id const> { return nodes_[node].children; }

    [[nodiscard]] auto san(node_id const node) const -> std::string const& { return nodes_[node].san; }

    [[nodiscard]] auto depth(node_id const node) const -> std::size_t { return nodes_[node].depth; }

    [[nodiscard]] auto is_main_line(node_id node) const -> bool;

  private:
    struct node
    {
        std::uint16_t move {};
        node_id parent {root_node};
        std::size_t depth {};
        std::string san;
        std::vector<node_id> children;
    };

    std::vector<node> nodes_ {node {}};
    std::vector<node_id> main_line_;
    node_id cursor_ {root_node};
    motif::chess::board board_;
    std::optional<motif::chess::move_info> last_move_;

    void reset_tree();
    auto append_node(node_id parent, std::uint16_t move, std::string san_text) -> node_id;
    void set_cursor(node_id target);
    void step_to_child(node_id child);
    auto commit(motif::chess::move_info const& info) -> applied_move;
};
}  // namespace motif::navigator
