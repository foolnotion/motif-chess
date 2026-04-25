#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "motif/import/pgn_helpers.hpp"

#include <pgnlib/types.hpp>

namespace motif::import
{

auto find_tag(std::vector<pgn::tag> const& tags, std::string_view key) -> std::string
{
    for (auto const& tag : tags) {
        if (tag.key == key) {
            return tag.value;
        }
    }
    return {};
}

auto parse_elo(std::string const& raw) -> std::optional<std::int16_t>
{
    if (raw.empty() || raw == "?") {
        return std::nullopt;
    }
    int val {};
    auto const* const beg = raw.data();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    auto const* const fin = raw.data() + raw.size();
    auto const parsed = std::from_chars(beg, fin, val);
    if (parsed.ec != std::errc {} || parsed.ptr != fin || val < 0 || val > std::numeric_limits<std::int16_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::int16_t>(val);
}

auto pgn_result_to_string(pgn::result res) noexcept -> std::string
{
    switch (res) {
        case pgn::result::white:
            return "1-0";
        case pgn::result::black:
            return "0-1";
        case pgn::result::draw:
            return "1/2-1/2";
        case pgn::result::unknown:
            return "*";
    }
    return "*";
}

auto pgn_result_to_int8(pgn::result res) noexcept -> std::int8_t
{
    switch (res) {
        case pgn::result::white:
            return 1;
        case pgn::result::black:
            return -1;
        case pgn::result::draw:
        case pgn::result::unknown:
            return 0;
    }
    return 0;
}

}  // namespace motif::import
