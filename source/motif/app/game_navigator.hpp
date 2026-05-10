#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "motif/db/types.hpp"

namespace motif::app
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
    [[nodiscard]] auto move_list() const -> std::vector<std::string>;

    [[nodiscard]] auto current_ply() const -> std::size_t { return ply_; }

    [[nodiscard]] auto total_plies() const -> std::size_t { return moves_.size(); }

    [[nodiscard]] auto has_game() const -> bool { return !moves_.empty(); }

  private:
    std::vector<std::uint16_t> moves_;
    std::size_t ply_ {0};
};

}  // namespace motif::app
