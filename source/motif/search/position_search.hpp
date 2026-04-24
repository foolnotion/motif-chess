// NOLINTNEXTLINE(portability-avoid-pragma-once)
#pragma once

#include <cstdint>
#include <vector>

#include "motif/db/types.hpp"
#include "motif/search/error.hpp"

namespace motif::db
{
class database_manager;
}  // namespace motif::db

namespace motif::search::position_search
{

using match_list = std::vector<motif::db::position_match>;

[[nodiscard]] auto find(motif::db::database_manager const& database, std::uint64_t zobrist_hash) -> result<match_list>;

}  // namespace motif::search::position_search
