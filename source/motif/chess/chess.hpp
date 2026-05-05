#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tl/expected.hpp>

namespace motif::chess
{

enum class error_code : std::uint8_t
{
    invalid_fen,
    invalid_move,
    invalid_ply,
};

template<typename T>
using result = tl::expected<T, error_code>;

class board
{
  public:
    board();
    ~board();

    board(board const& other);
    auto operator=(board const& other) -> board&;

    board(board&& other) noexcept;
    auto operator=(board&& other) noexcept -> board&;

    [[nodiscard]] auto hash() const noexcept -> std::uint64_t;

  private:
    struct impl;
    std::unique_ptr<impl> impl_;

    explicit board(std::unique_ptr<impl> impl) noexcept;

    friend auto parse_fen(std::string_view fen) -> result<board>;
    friend auto write_fen(board const& position) -> std::string;
    friend auto apply_encoded_move(board& position, std::uint16_t encoded_move) -> void;
    friend auto replay(std::span<std::uint16_t const> moves, std::uint16_t ply) -> result<board>;
    friend auto san(board const& position, std::uint16_t encoded_move) -> std::string;
    friend auto apply_san(board& position, std::string_view san_move) -> result<std::uint16_t>;
    friend auto legal_moves(board const& position) -> std::vector<struct move_info>;
    friend auto apply_uci(board& position, std::string_view uci_move) -> result<struct move_info>;
};

struct move_info
{
    std::uint16_t encoded_move {};
    std::string uci;
    std::string san;
    std::string from;
    std::string to;
    std::optional<std::string> promotion;
};

[[nodiscard]] auto parse_fen(std::string_view fen) -> result<board>;
[[nodiscard]] auto write_fen(board const& position) -> std::string;
[[nodiscard]] auto replay(std::span<std::uint16_t const> moves, std::uint16_t ply) -> result<board>;
[[nodiscard]] auto san(board const& position, std::uint16_t encoded_move) -> std::string;
[[nodiscard]] auto apply_san(board& position, std::string_view san_move) -> result<std::uint16_t>;
[[nodiscard]] auto legal_moves(board const& position) -> std::vector<move_info>;
[[nodiscard]] auto apply_uci(board& position, std::string_view uci_move) -> result<move_info>;
auto apply_encoded_move(board& position, std::uint16_t encoded_move) -> void;

}  // namespace motif::chess
