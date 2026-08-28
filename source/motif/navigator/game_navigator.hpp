#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "motif/chess/chess.hpp"
#include "motif/db/types.hpp"

namespace motif::navigator
{

// Pure navigation logic over a loaded game's move list. No Qt dependency — fully unit-testable.
// FEN and SAN strings are computed via motif::chess APIs on each call (O(ply) per query).
class game_navigator
{
  public:
    void load(motif::db::game const& game);
    void clear();

    void advance();
    void retreat();
    void jump_to_start();
    void jump_to_end();
    void navigate_to(std::size_t ply);

    [[nodiscard]] auto current_fen() const -> std::string;
    [[nodiscard]] auto current_san() const -> std::string;
    // Zobrist hash of the current position, or std::nullopt when the move
    // list cannot be replayed (e.g. corrupt stored moves). Callers must not
    // treat a returned hash as valid without checking.
    [[nodiscard]] auto current_hash() const -> std::optional<motif::db::zobrist_hash>;
    [[nodiscard]] auto move_list() const -> std::vector<std::string>;

    // The from/to squares and metadata of the move that reached the current
    // ply, or std::nullopt at ply 0 or when no game is loaded.
    [[nodiscard]] auto last_move() const -> std::optional<motif::chess::move_info>;

    [[nodiscard]] auto current_ply() const -> std::size_t { return ply_; }

    [[nodiscard]] auto total_plies() const -> std::size_t { return moves_.size(); }

    [[nodiscard]] auto has_game() const -> bool { return !moves_.empty(); }

  private:
    std::vector<std::uint16_t> moves_;
    std::size_t ply_ {0};
};
}  // namespace motif::navigator
