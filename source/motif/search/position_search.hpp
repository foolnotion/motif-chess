#pragma once

#include <cstddef>
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

[[nodiscard]] auto find(motif::db::database_manager const& database,
                        motif::db::zobrist_hash hash,
                        std::size_t limit = 0,
                        std::size_t offset = 0) -> result<match_list>;

}  // namespace motif::search::position_search
