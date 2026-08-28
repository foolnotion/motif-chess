#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "motif/navigator/game_navigator.hpp"

#include "motif/chess/chess.hpp"
#include "motif/db/types.hpp"

namespace motif::navigator
{

void game_navigator::load(motif::db::game const& game)
{
    moves_ = game.moves;
    ply_ = 0;
}

void game_navigator::clear()
{
    moves_.clear();
    ply_ = 0;
}

void game_navigator::advance()
{
    if (ply_ < moves_.size()) {
        ++ply_;
    }
}

void game_navigator::retreat()
{
    if (ply_ > 0) {
        --ply_;
    }
}

void game_navigator::jump_to_start()
{
    ply_ = 0;
}

void game_navigator::jump_to_end()
{
    ply_ = moves_.size();
}

void game_navigator::navigate_to(std::size_t const ply)
{
    ply_ = std::min(ply, moves_.size());
}

auto game_navigator::current_fen() const -> std::string
{
    auto result = motif::chess::replay(moves_, static_cast<std::uint16_t>(ply_));
    if (!result) {
        return {};
    }
    return motif::chess::write_fen(*result);
}

auto game_navigator::current_hash() const -> std::optional<motif::db::zobrist_hash>
{
    auto result = motif::chess::replay(moves_, static_cast<std::uint16_t>(ply_));
    if (!result) {
        return std::nullopt;
    }
    return motif::db::zobrist_hash {result->hash()};
}

auto game_navigator::current_san() const -> std::string
{
    if (ply_ == 0 || moves_.empty()) {
        return {};
    }
    auto board = motif::chess::replay(moves_, static_cast<std::uint16_t>(ply_ - 1));
    if (!board) {
        return {};
    }
    return motif::chess::san(*board, moves_[ply_ - 1]);
}

auto game_navigator::move_list() const -> std::vector<std::string>
{
    auto result = std::vector<std::string> {};
    result.reserve(moves_.size());

    auto pos = motif::chess::board {};
    for (auto const encoded_move : moves_) {
        result.push_back(motif::chess::san(pos, encoded_move));
        motif::chess::apply_encoded_move(pos, encoded_move);
    }
    return result;
}

auto game_navigator::last_move() const -> std::optional<motif::chess::move_info>
{
    if (ply_ == 0) {
        return std::nullopt;
    }
    auto board_before = motif::chess::replay(moves_, static_cast<std::uint16_t>(ply_ - 1));
    if (!board_before) {
        return std::nullopt;
    }
    auto const legal = motif::chess::legal_moves(*board_before);
    auto const played = moves_[ply_ - 1];
    auto const found = std::ranges::find(legal, played, &motif::chess::move_info::encoded_move);
    if (found == legal.end()) {
        return std::nullopt;
    }
    return *found;
}

}  // namespace motif::navigator
