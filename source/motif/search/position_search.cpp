#include <cstddef>

#include "motif/search/position_search.hpp"

#include <tl/expected.hpp>

#include "motif/db/database_manager.hpp"
#include "motif/search/error.hpp"

namespace motif::search::position_search
{

auto find(motif::db::database_manager const& database,
          motif::db::zobrist_hash const hash,
          std::size_t const limit,
          std::size_t const offset) -> result<match_list>
{
    auto query = database.positions().query_by_zobrist(hash, limit, offset);
    if (!query) {
        return tl::unexpected {error_code::io_failure};
    }

    return *query;
}

}  // namespace motif::search::position_search
