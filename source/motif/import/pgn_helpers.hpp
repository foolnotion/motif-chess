#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <pgnlib/types.hpp>

namespace motif::import
{

auto find_tag(std::vector<pgn::tag> const& tags, std::string_view key) -> std::string;

auto parse_elo(std::string const& raw) -> std::optional<std::int16_t>;

auto pgn_result_to_string(pgn::result res) noexcept -> std::string;

auto pgn_result_to_int8(pgn::result res) noexcept -> std::int8_t;

}  // namespace motif::import
