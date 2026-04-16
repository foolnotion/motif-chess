// NOLINTNEXTLINE(portability-avoid-pragma-once)
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace motif::db {

struct player {
    std::string name;
    std::optional<std::int32_t> elo;
    std::optional<std::string> title;
    std::optional<std::string> country;
};

struct event {
    std::string name;
    std::optional<std::string> site;
    std::optional<std::string> date;
};

struct position {
    std::uint64_t zobrist_hash{};
    std::uint32_t game_id{};
    std::uint16_t ply{};
};

struct game {
    player white;
    player black;
    std::optional<event> event_details;
    std::optional<std::string> date;
    std::string result;
    std::optional<std::string> eco;
    std::vector<std::uint16_t> moves;
    std::vector<std::pair<std::string, std::string>> extra_tags;
};

} // namespace motif::db