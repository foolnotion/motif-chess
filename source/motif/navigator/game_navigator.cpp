#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "motif/navigator/game_navigator.hpp"

#include <tl/expected.hpp>

#include "motif/chess/chess.hpp"
#include "motif/db/types.hpp"

namespace motif::navigator
{

namespace
{

auto find_move(std::vector<motif::chess::move_info> const& legal, std::uint16_t const encoded_move)
    -> std::optional<motif::chess::move_info>
{
    auto const found = std::ranges::find(legal, encoded_move, &motif::chess::move_info::encoded_move);
    if (found == legal.end()) {
        return std::nullopt;
    }
    return *found;
}

}  // namespace

void game_navigator::reset_tree()
{
    nodes_.assign(1, node {});
    main_line_.clear();
    cursor_ = root_node;
    board_ = motif::chess::board {};
    last_move_.reset();
}

void game_navigator::load(motif::db::game const& game)
{
    reset_tree();
    nodes_.reserve(game.moves.size() + 1);
    main_line_.reserve(game.moves.size());

    auto position = motif::chess::board {};
    auto tip = root_node;
    for (auto const encoded_move : game.moves) {
        tip = append_node(tip, encoded_move, motif::chess::san(position, encoded_move));
        motif::chess::apply_encoded_move(position, encoded_move);
    }
}

void game_navigator::clear()
{
    reset_tree();
}

auto game_navigator::append_node(node_id const parent, std::uint16_t const move, std::string san_text) -> node_id
{
    auto const created = static_cast<node_id>(nodes_.size());
    auto const tip = main_line_.empty() ? root_node : main_line_.back();
    nodes_.push_back(node {.move = move, .parent = parent, .depth = nodes_[parent].depth + 1, .san = std::move(san_text), .children = {}});
    nodes_[parent].children.push_back(created);
    if (parent == tip && nodes_[parent].children.size() == 1) {
        main_line_.push_back(created);
    }
    return created;
}

void game_navigator::set_cursor(node_id const target)
{
    cursor_ = target;
    board_ = motif::chess::board {};
    last_move_.reset();
    if (target == root_node) {
        return;
    }

    // Moves from the target back to the root; the last element is the first move.
    auto path = std::vector<std::uint16_t> {};
    path.reserve(nodes_[target].depth);
    for (auto walker = target; walker != root_node; walker = nodes_[walker].parent) {
        path.push_back(nodes_[walker].move);
    }
    for (auto index = path.size(); index-- > 1;) {
        motif::chess::apply_encoded_move(board_, path[index]);
    }
    last_move_ = find_move(motif::chess::legal_moves(board_), path.front());
    motif::chess::apply_encoded_move(board_, path.front());
}

void game_navigator::step_to_child(node_id const child)
{
    auto const encoded_move = nodes_[child].move;
    last_move_ = find_move(motif::chess::legal_moves(board_), encoded_move);
    motif::chess::apply_encoded_move(board_, encoded_move);
    cursor_ = child;
}

void game_navigator::advance()
{
    auto const& next = nodes_[cursor_].children;
    if (!next.empty()) {
        step_to_child(next.front());
    }
}

void game_navigator::retreat()
{
    if (cursor_ != root_node) {
        set_cursor(nodes_[cursor_].parent);
    }
}

void game_navigator::jump_to_start()
{
    set_cursor(root_node);
}

void game_navigator::jump_to_end()
{
    auto end = cursor_;
    while (!nodes_[end].children.empty()) {
        end = nodes_[end].children.front();
    }
    if (end != cursor_) {
        set_cursor(end);
    }
}

void game_navigator::navigate_to(std::size_t const ply)
{
    auto const clamped = std::min(ply, main_line_.size());
    set_cursor(clamped == 0 ? root_node : main_line_[clamped - 1]);
}

auto game_navigator::navigate_to_node(node_id const node) -> bool
{
    if (node >= nodes_.size()) {
        return false;
    }
    set_cursor(node);
    return true;
}

auto game_navigator::is_main_line(node_id const node) const -> bool
{
    if (node >= nodes_.size()) {
        return false;
    }
    auto const level = nodes_[node].depth;
    return level == 0 || (level <= main_line_.size() && main_line_[level - 1] == node);
}

auto game_navigator::commit(motif::chess::move_info const& info) -> applied_move
{
    auto const& existing = nodes_[cursor_].children;
    auto const match = std::ranges::find_if(existing, [&](node_id const child) -> bool { return nodes_[child].move == info.encoded_move; });
    auto result = applied_move {};
    if (match != existing.end()) {
        result.node = *match;
        result.outcome = *match == existing.front() ? move_outcome::followed_line : move_outcome::entered_variation;
    } else {
        result.outcome = existing.empty() ? move_outcome::extended_line : move_outcome::created_variation;
        result.node = append_node(cursor_, info.encoded_move, info.san);
    }
    result.san = nodes_[result.node].san;

    motif::chess::apply_encoded_move(board_, info.encoded_move);
    last_move_ = info;
    cursor_ = result.node;
    return result;
}

auto game_navigator::apply_move(std::string_view const from, std::string_view const target, std::optional<char> const promotion)
    -> tl::expected<applied_move, move_error>
{
    auto const legal = motif::chess::legal_moves(board_);
    auto candidates = std::vector<motif::chess::move_info const*> {};
    for (auto const& info : legal) {
        if (info.from == from && info.to == target) {
            candidates.push_back(&info);
        }
    }
    if (candidates.empty()) {
        return tl::unexpected {move_error::illegal_move};
    }

    auto const lower = [](char const letter) -> char { return static_cast<char>(std::tolower(static_cast<unsigned char>(letter))); };
    auto const wanted = promotion ? std::optional<char> {lower(*promotion)} : std::nullopt;
    auto const chosen = std::ranges::find_if(candidates,
                                             [&](motif::chess::move_info const* info) -> bool
                                             {
                                                 if (!info->promotion) {
                                                     return !wanted.has_value();
                                                 }
                                                 return wanted && info->promotion->size() == 1 && lower((*info->promotion)[0]) == *wanted;
                                             });
    if (chosen == candidates.end()) {
        auto const needs_piece = !wanted && candidates.front()->promotion.has_value();
        return tl::unexpected {needs_piece ? move_error::promotion_required : move_error::illegal_move};
    }
    return commit(**chosen);
}

auto game_navigator::apply_san(std::string_view const san_text) -> tl::expected<applied_move, move_error>
{
    auto probe = board_;
    auto const parsed = motif::chess::apply_san(probe, san_text);
    if (!parsed) {
        return tl::unexpected {move_error::invalid_san};
    }
    auto const info = find_move(motif::chess::legal_moves(board_), *parsed);
    if (!info) {
        return tl::unexpected {move_error::invalid_san};
    }
    return commit(*info);
}

auto game_navigator::current_fen() const -> std::string
{
    return motif::chess::write_fen(board_);
}

auto game_navigator::current_hash() const -> std::optional<motif::db::zobrist_hash>
{
    return motif::db::zobrist_hash {board_.hash()};
}

auto game_navigator::current_san() const -> std::string
{
    return nodes_[cursor_].san;
}

auto game_navigator::move_list() const -> std::vector<std::string>
{
    auto result = std::vector<std::string> {};
    result.reserve(main_line_.size());
    for (auto const node_index : main_line_) {
        result.push_back(nodes_[node_index].san);
    }
    return result;
}

}  // namespace motif::navigator
