#include <algorithm>
#include <array>
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

#include "motif/db/types.hpp"

namespace
{

constexpr std::array<std::string_view, 11> known_tag_keys = {
    "White",
    "Black",
    "WhiteElo",
    "BlackElo",
    "WhiteTitle",
    "BlackTitle",
    "Event",
    "Site",
    "Date",
    "Result",
    "ECO",
};

auto is_known_tag(std::string_view key) noexcept -> bool
{
    return std::ranges::any_of(known_tag_keys, [&](std::string_view known) noexcept -> bool { return key == known; });
}

}  // namespace

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

auto pgn_to_game(pgn::game const& pgn_game) -> motif::db::game
{
    auto const& tags = pgn_game.tags;
    auto const white_elo = parse_elo(find_tag(tags, "WhiteElo"));
    auto const black_elo = parse_elo(find_tag(tags, "BlackElo"));
    auto const white_title_raw = find_tag(tags, "WhiteTitle");
    auto const black_title_raw = find_tag(tags, "BlackTitle");
    auto const event_name = find_tag(tags, "Event");
    auto const site_raw = find_tag(tags, "Site");
    auto const date_raw = find_tag(tags, "Date");
    auto const eco_raw = find_tag(tags, "ECO");
    auto const valid_date = (!date_raw.empty() && date_raw != "????.??.??") ? std::optional<std::string> {date_raw} : std::nullopt;

    motif::db::game dbg;
    dbg.white.name = find_tag(tags, "White");
    dbg.white.elo = white_elo ? std::optional<std::int32_t> {*white_elo} : std::nullopt;
    dbg.white.title = white_title_raw.empty() ? std::nullopt : std::optional<std::string> {white_title_raw};

    dbg.black.name = find_tag(tags, "Black");
    dbg.black.elo = black_elo ? std::optional<std::int32_t> {*black_elo} : std::nullopt;
    dbg.black.title = black_title_raw.empty() ? std::nullopt : std::optional<std::string> {black_title_raw};

    if (!event_name.empty()) {
        dbg.event_details = motif::db::event {
            .name = event_name,
            .site = site_raw.empty() ? std::nullopt : std::optional<std::string> {site_raw},
            .date = valid_date,
        };
    }

    dbg.date = valid_date;
    dbg.eco = eco_raw.empty() ? std::nullopt : std::optional<std::string> {eco_raw};
    dbg.result = pgn_result_to_string(pgn_game.result);

    for (auto const& tag : tags) {
        if (!is_known_tag(tag.key)) {
            dbg.extra_tags.emplace_back(tag.key, tag.value);
        }
    }

    return dbg;
}

}  // namespace motif::import
