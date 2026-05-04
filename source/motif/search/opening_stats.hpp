#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "motif/search/error.hpp"

namespace motif::db
{
class database_manager;
}  // namespace motif::db

namespace motif::search::opening_stats
{

struct continuation
{
    std::string san;
    std::uint64_t result_hash {};
    std::uint32_t frequency {};
    std::uint32_t white_wins {};
    std::uint32_t draws {};
    std::uint32_t black_wins {};
    std::optional<double> average_white_elo;
    std::optional<double> average_black_elo;
    std::optional<std::string> eco;
    std::optional<std::string> opening_name;
};

struct stats
{
    stats() = default;

    std::uint32_t total_games {};
    std::vector<continuation> continuations;
};

[[nodiscard]] auto query(motif::db::database_manager const& database,
                         std::uint64_t zobrist_hash,
                         std::optional<std::uint64_t> parent_hash = std::nullopt) -> result<stats>;

}  // namespace motif::search::opening_stats
