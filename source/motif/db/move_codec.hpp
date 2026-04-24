// NOLINTNEXTLINE(portability-avoid-pragma-once)
#pragma once

#include <cstdint>

#include <chesslib/board/move_codec.hpp>
#include <chesslib/core/types.hpp>

#include "motif/db/error.hpp"

namespace motif::db
{

using encoded_move = std::uint16_t;

[[nodiscard]] inline auto encode_move(chesslib::move move) noexcept -> result<encoded_move>
{
    return result<encoded_move> {chesslib::codec::encode(move)};
}

[[nodiscard]] inline auto decode_move(encoded_move encoded) noexcept -> result<chesslib::move>
{
    return result<chesslib::move> {chesslib::codec::decode(encoded)};
}

}  // namespace motif::db
