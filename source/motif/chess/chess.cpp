#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "motif/chess/chess.hpp"

#include <chesslib/board/board.hpp>
#include <chesslib/board/move_codec.hpp>
#include <chesslib/board/move_generator.hpp>
#include <chesslib/util/fen.hpp>
#include <chesslib/util/san.hpp>
#include <chesslib/util/uci.hpp>
#include <tl/expected.hpp>

namespace motif::chess
{

namespace
{

constexpr auto uci_promotion_length = std::size_t {5};

auto squares_from_uci(std::string const& uci) -> std::pair<std::string, std::string>
{
    if (uci.size() < 4U) {
        return {};
    }

    return {uci.substr(0, 2), uci.substr(2, 2)};
}

auto promotion_from_uci(std::string const& uci) -> std::optional<std::string>
{
    if (uci.size() < uci_promotion_length) {
        return std::nullopt;
    }

    return std::string {uci[4]};
}

// NOLINTNEXTLINE(misc-include-cleaner) — chesslib::move is transitively included via move_codec.hpp
auto describe_move(chesslib::board const& position, chesslib::move const move) -> move_info
{
    auto const encoded_move = chesslib::codec::encode(move);
    auto const uci = chesslib::uci::to_string(move);
    auto const san_move = chesslib::san::to_string(position, move);
    auto [from_square, to_square] = squares_from_uci(uci);

    return move_info {
        .encoded_move = encoded_move,
        .uci = uci,
        .san = san_move,
        .from = std::move(from_square),
        .to = std::move(to_square),
        .promotion = promotion_from_uci(uci),
    };
}

}  // namespace

struct board::impl
{
    chesslib::board native;
};

board::board()
    : impl_ {std::make_unique<impl>()}
{
}

board::board(std::unique_ptr<impl> impl) noexcept
    : impl_ {std::move(impl)}
{
}

void board::ensure_impl()
{
    if (impl_ == nullptr) {
        impl_ = std::make_unique<impl>();
    }
}

board::~board() = default;

board::board(board const& other)
    : impl_ {other.impl_ != nullptr ? std::make_unique<impl>(*other.impl_) : std::make_unique<impl>()}
{
}

auto board::operator=(board const& other) -> board&
{
    if (this != &other) {
        ensure_impl();
        if (other.impl_ != nullptr) {
            *impl_ = *other.impl_;
        } else {
            *impl_ = impl {};
        }
    }
    return *this;
}

board::board(board&& other) noexcept = default;

auto board::operator=(board&& other) noexcept -> board& = default;

auto board::hash() const noexcept -> std::uint64_t
{
    return impl_ != nullptr ? impl_->native.hash() : chesslib::board {}.hash();
}

auto parse_fen(std::string_view const fen) -> result<board>
{
    auto parsed = chesslib::fen::read(fen);
    if (!parsed) {
        return tl::unexpected {error_code::invalid_fen};
    }

    auto impl = std::make_unique<board::impl>();
    impl->native = *parsed;
    return board {std::move(impl)};
}

auto write_fen(board const& position) -> std::string
{
    return chesslib::fen::write(position.impl_->native);
}

auto replay(std::span<std::uint16_t const> const moves, std::uint16_t const ply) -> result<board>
{
    if (ply > moves.size()) {
        return tl::unexpected {error_code::invalid_ply};
    }

    auto position = board {};
    for (std::size_t index = 0; index < ply; ++index) {
        apply_encoded_move(position, moves[index]);
    }

    return position;
}

auto san(board const& position, std::uint16_t const encoded_move) -> std::string
{
    return chesslib::san::to_string(position.impl_->native, chesslib::codec::decode(encoded_move));
}

auto apply_san(board& position, std::string_view const san_move) -> result<std::uint16_t>
{
    auto move = chesslib::san::from_string(position.impl_->native, san_move);
    if (!move) {
        return tl::unexpected {error_code::invalid_move};
    }

    auto const encoded_move = chesslib::codec::encode(*move);
    chesslib::move_maker {position.impl_->native, *move}.make();
    return encoded_move;
}

auto legal_moves(board const& position) -> std::vector<move_info>
{
    auto const moves = chesslib::legal_moves(position.impl_->native);
    auto result_moves = std::vector<move_info> {};
    result_moves.reserve(moves.size());
    for (auto const& move : moves) {
        result_moves.push_back(describe_move(position.impl_->native, move));
    }

    return result_moves;
}

auto apply_uci(board& position, std::string_view const uci_move) -> result<move_info>
{
    auto move = chesslib::uci::from_string(position.impl_->native, uci_move);
    if (!move) {
        return tl::unexpected {error_code::invalid_move};
    }

    auto info = describe_move(position.impl_->native, *move);
    chesslib::move_maker {position.impl_->native, *move}.make();
    return info;
}

auto apply_encoded_move(board& position, std::uint16_t const encoded_move) -> void
{
    chesslib::move_maker {position.impl_->native, chesslib::codec::decode(encoded_move)}.make();
}

}  // namespace motif::chess
